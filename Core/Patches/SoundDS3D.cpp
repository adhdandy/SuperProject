/* Copyright (c) 2022-2026 Dreamy Cecil
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation


This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "StdH.h"
#include <Engine/Base/Shell.h>
#include <Engine/Base/Timer.h>
#include <Engine/Sound/SoundLibrary.h>
#include <Engine/Sound/SoundObject.h>
#include <Engine/Sound/SoundData.h>
#include "SoundDS3D.h"

// 08 s1: per-sound DirectSound3D patch subsystem, Steam TSE 1.07 release only.
#if _PATCHCONFIG_ENGINEPATCHES && SE1_VER == SE1_107 && defined(NDEBUG)

// 08 s2 - cvars; snd_bDS3DSpatial is the mode switch, read only in game-thread
// hooks (07 sG8) - the mixer-thread hook reads the precomputed bBypass flag.
INDEX _snd_bDS3DSpatial = 0;

// 99 #14 - guid3DAlgorithm 0/1/2 = DEFAULT/HRTF_LIGHT/HRTF_FULL
INDEX _snd_iEmulationType = 0;

// 99 #15 - voice budget (clamped 4..64 at read time)
INDEX _snd_iMaxVoices = 30;

// 99 #9 - per-CSoundData volume budget (clamped 1..99 at read time)
INDEX _snd_iMaxSameSounds = 5;

// 99 #13 - EAX DirectHF gate
INDEX _snd_bUseEnvironmentFX = 1;

// 07 s16 - post-curve flat gain offset in dB (deviation from beta parity; applied after
// the 99 #10 curve at 08 s4 step 6, ungated branch only). Clamped -30..0 at read time.
// Default -6.0 (user-tuned, finalized 2026-10-18; -4.0 and -9.0 were earlier cuts).
FLOAT _snd_fDS3DGain = -6.0f;

// 08 s6 - refusal state. Cleared if any hook in ICorePatches::SoundLibrary() fails its
// install check (dtor symbol missing, MixSound prologue/module mismatch). Step 3
// registration gates on this: a partial install is never safe (shadows without the
// MixSound skip = double audio; skip without shadows = silent zombies).
BOOL _bDS3DHooksOk = TRUE;

// 08 s2/G6 - snd_iEmulationType post: handler (definition below, with the SetFormat
// hooks). The shell grammar type-checks it as void(INDEX) but passes the ADDRESS of
// the changed variable (Parser.y base call site; stock precedent SndPostFunc(void*) -
// SoundLibrary.cpp:173; the patch precedent ignores the argument the same way -
// Core\Base\ObserverCamera.cpp:513). We read the cvar global itself.
static void DS3D_EmulationTypeChanged(INDEX);

// 08 s2/s11 step 1 - declared once here; called from ICorePatches::SoundLibrary()
// (EnginePatches.cpp, 06 s5 pattern). Default snd_bDS3DSpatial = 0: nothing in this
// subsystem runs, behavior is vanilla.
void SoundDS3D_Init(void) {
  // 08 s2/07 s12.4 - declare the function symbol first; the post: tag references it by
  // name (precedent Core\Base\ObserverCamera.cpp:532-533)
  _pShell->DeclareSymbol("void DS3D_EmulationTypeChanged(INDEX);", &DS3D_EmulationTypeChanged);

  _pShell->DeclareSymbol("persistent user INDEX snd_bDS3DSpatial;",      &_snd_bDS3DSpatial);
  _pShell->DeclareSymbol("persistent user INDEX snd_iEmulationType post:DS3D_EmulationTypeChanged;", &_snd_iEmulationType);
  _pShell->DeclareSymbol("persistent user INDEX snd_iMaxVoices;",        &_snd_iMaxVoices);
  _pShell->DeclareSymbol("persistent user INDEX snd_iMaxSameSounds;",    &_snd_iMaxSameSounds);
  _pShell->DeclareSymbol("persistent user INDEX snd_bUseEnvironmentFX;", &_snd_bUseEnvironmentFX);
  _pShell->DeclareSymbol("persistent user FLOAT snd_fDS3DGain;",        &_snd_fDS3DGain);
}

// 08 s3 P_MixSound / AGENTS invariant 3 - the raw address is patched only if the loaded
// module identity and the prologue byte pattern both match.
BOOL SoundDS3D_CheckMixSound(void) {
  void *pvTarget = (void *)(ADDR_ENGINE + 0x7AF80); // 08 s3 (RVA per 07 s13 item 9)

  // Module identity: the address must map inside the loaded Engine.dll image
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(pvTarget, &mbi, sizeof(mbi)) == 0) return FALSE;
  if (mbi.AllocationBase != HMODULE_ENGINE) return FALSE;

  // Full rewritten prologue: 55 8b ec 83 ec 20 = push ebp; mov ebp,esp; sub esp,20h
  // (6 bytes - covers every byte CreateOpcodePatch overwrites; verified against the
  // Steam binary 0x6013af80, 2026-10-17). MixSound is __cdecl (caller push ecx +
  // call + add esp,4 at 0x6013a02b; epilogue pop ebp; ret at 0x6013bbb9).
  static const unsigned char pubPrologue[6] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x20 };
  return memcmp(pvTarget, pubPrologue, sizeof(pubPrologue)) == 0;
}

// 08 s8 - registry (P_MixSound membership test / driver source) and the template cache
// (se1::map is C++98-safe - Extras/XGizmo/Objects/MapStructure.h, precedent IPatches.cpp:78)
static se1::map<CSoundObject *, DS3DShadow *> _mapShadows;
static se1::map<CSoundData *, DS3DTemplate> _mapTemplates;

// 99 #9 - same-sounds budget (07 s13 item 26): per-frame running sums of the
// master-scaled volumes, keyed by the opaque psdKey (item 12), reset every driver
// tail; over-budget entries stay committed at volume 0 (beta parity - deliberately
// NOT routed through the D8 silence gate)
static se1::map<CSoundData *, FLOAT> _mapSameRunning;

// 08 s9 - listener parameters applied once per (re)initialized device
static BOOL _bDS3DListenerApplied = FALSE;

// G6 - a SetFormat pre-release marks the device as dying; the post section re-applies
// the listener params (they are ours, 99 #1) or drops every shadow when the listener
// is gone. Only armed when our params had been applied (mode really active) - keeps
// the cvar-off path structurally stock.
static BOOL _bDS3DReapplyAfterFormat = FALSE;

// 08 s6/G9 - failure notices: once per cause per session
static BOOL _bDS3DNoticeCommit   = FALSE; // G9.1 creation failure at commit
static BOOL _bDS3DNoticeRuntime  = FALSE; // G9.2 runtime HRESULT failure
static BOOL _bDS3DNoticeLost     = FALSE; // G9.3 buffer lost / terminated
static BOOL _bDS3DNoticeDisabled = FALSE; // G2 listener loss - forced mode disable

static void DS3D_NoticeOnce(BOOL &bFlag, const char *strMsg) {
  if (bFlag) return;
  bFlag = TRUE;
  CPrintF("%s", strMsg);
};

// 07 s13 item 16 - high-precision timestamp of the previous driver tail (frame time
// for the so_fDelayed advance of skipped shadows)
static __int64 _llDS3DLastTail = 0;

// 07 s13 item 28 - game tick of the previous driver tail (the D8 silent-since timer
// advances on the game clock for game sounds: frozen while the game does not tick)
static TIME _fDS3DLastTick = 0.0f;

// 08 s3 - GUIDs. DSOUND.H / EAX.h declare them via DEFINE_GUID, which requires
// dxguid.lib; Core does not link it, so keep local const copies (values:
// DSOUND.H:910 IID_IDirectSound3DBuffer, DSOUND.H:1185 IID_IKsPropertySet,
// DSOUND.H:2262-2271 DS3DALG_*, EAX.h:240 DSPROPSETID_EAX20_BufferProperties)
static const GUID _guidDS3DDefault     = { 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };
static const GUID _guidDS3DHRTFLight   = { 0xc2413342, 0x1c1b, 0x11d2, { 0x94, 0xf5, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca } };
static const GUID _guidDS3DHRTFFull    = { 0xc2413340, 0x1c1b, 0x11d2, { 0x94, 0xf5, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca } };
static const GUID _iidDS3DBuffer       = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID _iidKsPropertySet    = { 0x31efac30, 0x515c, 0x11d0, { 0xa9, 0xaa, 0x00, 0xaa, 0x00, 0x61, 0xbe, 0x93 } };
static const GUID _guidEAXBufferProps  = { 0x0306a6a7, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };

// 08 s4 - registry lookup (P_MixSound membership test in step 3)
static DS3DShadow *DS3D_Find(CSoundObject *pso) {
  se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.find(pso);
  return (it != _mapShadows.end()) ? it->second : NULL;
};

// 08 s6/G2 - registration gate: cvar + full hook set + live DS listener (state, not
// snd_iInterface; re-evaluated at every registration, so a later SetFormat that
// restores the listener re-enables while the cvar is 1)
static BOOL DS3D_ModeOn(void) {
  return _snd_bDS3DSpatial != 0 && _bDS3DHooksOk
      && _pSound != NULL && _pSound->sl_pDS != NULL && _pSound->sl_pDSListener != NULL;
};

// 08 s5 - registration-time eligibility, on the Play_internal arguments (decoder
// sounds are excluded via SDF_ENCODED; SOF_VOLUMETRIC sounds ARE shadowed)
static BOOL DS3D_IsEligible(CSoundData *psd, SLONG slFlags) {
  if (psd == NULL) return FALSE;
  if (!(slFlags & SOF_3D)) return FALSE;
  if (slFlags & SOF_MUSIC) return FALSE;
  if (psd->sd_ulFlags & SDF_ENCODED) return FALSE;
  if (psd->sd_wfeFormat.nChannels != 1) return FALSE;
  if (psd->sd_wfeFormat.wBitsPerSample != 16) return FALSE;
  return TRUE;
};

// 08 s9 / 99 #1 - beta listener parameters (the stock init is rolloff 1.0 / doppler
// 1.0); IMMEDIATE, applied on mode-enable and re-applied after every SetFormat
static void DS3D_ApplyListenerParams(void) {
  if (_bDS3DListenerApplied) return;

  LPDIRECTSOUND3DLISTENER pListener = _pSound->sl_pDSListener;
  if (pListener == NULL) return;

  pListener->SetPosition(0.0f, 0.0f, 0.0f, DS3D_IMMEDIATE);
  pListener->SetOrientation(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, DS3D_IMMEDIATE);
  pListener->SetVelocity(0.0f, 0.0f, 0.0f, DS3D_IMMEDIATE);
  pListener->SetRolloffFactor(0.0f, DS3D_IMMEDIATE);
  pListener->SetDopplerFactor(1.1f, DS3D_IMMEDIATE);

  _bDS3DListenerApplied = TRUE;
  CPrintF("DS3D: snd_bDS3DSpatial active - listener parameters applied\n");
};

// 08 s8 - release the shadow's DS objects (force stop + release). Idempotent;
// failure of Stop is ignorable. The non-forced orphan path is DS3D_DetachToOrphan.
static void DS3D_ReleaseBuffer(DS3DShadow *psh) {
  if (psh->pKsProps != NULL)  { psh->pKsProps->Release();  psh->pKsProps = NULL; }
  if (psh->pBuffer3D != NULL) { psh->pBuffer3D->Release(); psh->pBuffer3D = NULL; }
  if (psh->pBuffer != NULL) {
    psh->pBuffer->Stop();
    psh->pBuffer->Release();
    psh->pBuffer = NULL;
  }
  psh->bStarted = FALSE;
};

// 08 s8 - orphan entry: interfaces detached from a removed/re-played shadow. No
// CSoundData*/owner pointer (never dereferenced); the buffer plays out (99 #17
// non-forced, beta FUN_10082780 semantics) and the per-frame scan releases it once done
struct DS3DOrphan {
  LPDIRECTSOUNDBUFFER pBuffer;
  LPDIRECTSOUND3DBUFFER pBuffer3D;
  LPKSPROPERTYSET pKsProps;
};
static std::list<DS3DOrphan> _lstOrphans;

static void DS3D_ReleaseOrphan(DS3DOrphan &orph) {
  if (orph.pKsProps != NULL)  { orph.pKsProps->Release();  orph.pKsProps = NULL; }
  if (orph.pBuffer3D != NULL) { orph.pBuffer3D->Release(); orph.pBuffer3D = NULL; }
  if (orph.pBuffer != NULL) {
    orph.pBuffer->Stop(); // ignorable - already stopped counts as fine
    orph.pBuffer->Release();
    orph.pBuffer = NULL;
  }
};

// 99 #17 / 08 s3 - detach a live buffer to the orphan list instead of force-stopping
// it (re-play in P_Play_internal, explicit Stop_internal): it plays out and the tail
// scan releases it later. A buffer that is not actually playing is plain-released
// right away (beta 0x82780: GetStatus; not playing -> release now). Looping buffers are
// force-released: the buffer carries DSBPLAY_LOOPING, so "playing out" would never end
// - the scan would never release it (07 s15: deliberate deviation, stop means stop).
// bWasLooping is passed separately: on a re-play, bLooping already holds the NEW play's
// flag (OnPlayPre snapshots the old one into bReplayLooping).
static void DS3D_DetachToOrphan(DS3DShadow *psh, BOOL bWasLooping) {
  if (psh->pBuffer == NULL) return;

  DWORD dwStatus = 0;
  const BOOL bPlaying = SUCCEEDED(psh->pBuffer->GetStatus(&dwStatus))
                     && (dwStatus & DSBSTATUS_PLAYING) != 0;

  if (!bPlaying || bWasLooping) {
    DS3D_ReleaseBuffer(psh);
    return;
  }

  DS3DOrphan orph;
  orph.pBuffer = psh->pBuffer;
  orph.pBuffer3D = psh->pBuffer3D;
  orph.pKsProps = psh->pKsProps;
  _lstOrphans.push_back(orph);

  psh->pBuffer = NULL;
  psh->pBuffer3D = NULL;
  psh->pKsProps = NULL;
  psh->bStarted = FALSE;
};

// 08 s8 - orphan scan: release entries whose buffer is done (GetStatus failed or not
// playing). Runs every driver tail under sl_csSound; the SetFormat pre-pass frees the
// whole list while the device is still alive (08 s3 - "mark generation", 07 sG6)
static void DS3D_ScanOrphans(void) {
  std::list<DS3DOrphan>::iterator it = _lstOrphans.begin();

  while (it != _lstOrphans.end()) {
    DWORD dwStatus = 0;

    if (FAILED(it->pBuffer->GetStatus(&dwStatus)) || !(dwStatus & DSBSTATUS_PLAYING)) {
      DS3D_ReleaseOrphan(*it);
      it = _lstOrphans.erase(it);
    } else {
      it++;
    }
  }
};

// 08 s8 - registry entry removal (release + erase). Game thread, under sl_csSound.
// Plain release (no position sync): dtor, driver finish, !SOF_PLAY drops; the
// explicit Stop_internal path detaches the buffer to the orphan list first (99 #17)
static void DS3D_RemoveEntry(CSoundObject *pso) {
  se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.find(pso);
  if (it == _mapShadows.end()) return;

  DS3DShadow *psh = it->second;
  DS3D_ReleaseBuffer(psh);
  _mapShadows.erase(it);
  delete psh;
};

// 07 sG4d - position-preserving release: the DS play cursor is written back into the
// engine offsets (bytes -> source samples) plus the past-delay sentinel
// (SoundMixer.cpp:597; bStarted implies the delay already elapsed). bStarted-guarded:
// a delay-pending sound has no DS position (sync would cancel its partial delay) and
// is intentionally inert without a buffer (e.g. DropToSoftware after
// ReleaseForRecommit). No mixer-rate conversion across SetFormat (stock-parity
// imprecision accepted).
static void DS3D_SyncPositionToEngine(DS3DShadow *psh) {
  if (!psh->bStarted || psh->pBuffer == NULL || psh->iBytesPerFrame <= 0) return;

  CSoundObject *pso = psh->psoOwner;
  pso->so_fDelayed = 9999.9999f; // past-delay sentinel (07 s13 item 16)

  DWORD dwPlay = 0;
  if (FAILED(psh->pBuffer->GetCurrentPosition(&dwPlay, NULL))) return; // offsets stay; sentinel kept

  const FLOAT fOfs = (FLOAT)dwPlay / (FLOAT)psh->iBytesPerFrame;
  pso->so_fLeftOffset = fOfs;
  pso->so_fRightOffset = fOfs;
};

// 07 s13 item 14 ReleaseForRecommit - position sync + release, entry KEPT: bStarted
// and bSpatialValid are reset (the recommit routes through the item-18 Play-latency
// guard instead of reissuing Play against stale state), bBypass stays true. Sites:
// SOF_PAUSED gate, voice steal (step 5 - plus bBypass=false), P_SetFormat pre-release,
// P_ClearBuffer owner-alive, 10 s silent release (step 5).
static void DS3D_ReleaseForRecommit(DS3DShadow *psh) {
  DS3D_SyncPositionToEngine(psh);
  DS3D_ReleaseBuffer(psh); // releases 3D/KS + buffer, bStarted = false
  psh->bSpatialValid = FALSE;
};

// 07 s13 item 14 DropToSoftware - position sync + release + remove the table entry;
// the stock mixer owns the sound again and resumes it from the synced offset on its
// next tick (one ~1-block fade at most, real spNew/sp values - D4). Sites: G9
// failures, forced mode disable (listener loss), listener missing after SetFormat.
static void DS3D_DropToSoftware(DS3DShadow *psh) {
  DS3D_SyncPositionToEngine(psh);
  DS3D_RemoveEntry(psh->psoOwner);
};

// 08 s8 - new entry with the stored fields initialized to parked (02 s1; 07 s13 item 18)
static DS3DShadow *DS3D_NewShadow(CSoundObject *pso) {
  DS3DShadow *psh = new DS3DShadow;
  psh->psoOwner = pso;
  psh->psdKey = NULL;
  psh->pBuffer = NULL;
  psh->pBuffer3D = NULL;
  psh->pKsProps = NULL;
  psh->bLooping = FALSE;
  psh->bStarted = FALSE;
  psh->bDelayPending = FALSE;
  psh->bSpatialValid = FALSE;
  psh->bBypass = TRUE;         // 07 s13 item 19: true at registration
  psh->bGateStopped = FALSE;
  psh->bReplay = FALSE;
  psh->bReplayLooping = FALSE;
  psh->vPos = FLOAT3D(0.0f, 0.0f, 10000.0f);
  psh->vVel = FLOAT3D(0.0f, 0.0f, 0.0f);
  psh->fLastVolume = 0.0f;
  psh->fLastFilter = 0.0f;
  psh->iRate = 0;
  psh->iChannels = 0;
  psh->iBytesPerFrame = 0;
  psh->iFailFrames = 0;
  psh->tmSilentSince = 0.0;
  return psh;
};

// 99 #14 - guid3DAlgorithm from snd_iEmulationType (0/1/2 = DEFAULT/HRTF_LIGHT/HRTF_FULL)
static const GUID *DS3D_GetAlgorithm(void) {
  switch (_snd_iEmulationType) {
    case 1:  return &_guidDS3DHRTFLight;
    case 2:  return &_guidDS3DHRTFFull;
    default: return &_guidDS3DDefault;
  }
};

// 07 sG1 - build the cached template buffer for a CSoundData: create with the mono 3D
// template flags, copy the PCM data once via Lock/Unlock (the template is never
// re-locked; duplicates share its memory - 07 s13 item 13)
static BOOL DS3D_BuildTemplate(CSoundData *psd, DS3DTemplate &tpl) {
  tpl.pbuf = NULL;
  tpl.pswPCM = NULL;
  tpl.iBytes = 0;
  tpl.iRate = 0;
  tpl.iChannels = 0;

  if (psd->sd_pswBuffer == NULL || psd->sd_slBufferSampleSize <= 0) return FALSE;

  const INDEX iChannels = psd->sd_wfeFormat.nChannels;
  const INDEX iRate = psd->sd_wfeFormat.nSamplesPerSec;
  const INDEX iBytes = psd->sd_slBufferSampleSize * iChannels * (psd->sd_wfeFormat.wBitsPerSample / 8);
  if (iBytes <= 0) return FALSE;

  DSBUFFERDESC dsbd;
  memset(&dsbd, 0, sizeof(dsbd));
  dsbd.dwSize = sizeof(dsbd);
  // 99 #14 - mono 3D template flags
  dsbd.dwFlags = DSBCAPS_CTRL3D|DSBCAPS_CTRLFREQUENCY|DSBCAPS_CTRLVOLUME
               | DSBCAPS_MUTE3DATMAXDISTANCE|DSBCAPS_LOCDEFER;
  dsbd.dwBufferBytes = iBytes;
  dsbd.lpwfxFormat = &psd->sd_wfeFormat;
  dsbd.guid3DAlgorithm = *DS3D_GetAlgorithm();

  LPDIRECTSOUNDBUFFER pbuf = NULL;
  if (FAILED(_pSound->sl_pDS->CreateSoundBuffer(&dsbd, &pbuf, NULL)) || pbuf == NULL) {
    CPrintF("DS3D: sound buffer template creation failed\n");
    return FALSE;
  }

  void *pv1 = NULL;
  void *pv2 = NULL;
  DWORD dw1 = 0;
  DWORD dw2 = 0;

  if (FAILED(pbuf->Lock(0, (DWORD)iBytes, &pv1, &dw1, &pv2, &dw2, 0))) {
    pbuf->Release();
    return FALSE;
  }
  memcpy(pv1, psd->sd_pswBuffer, dw1);
  if (pv2 != NULL) memcpy(pv2, ((UBYTE *)psd->sd_pswBuffer) + dw1, dw2);
  pbuf->Unlock(pv1, dw1, pv2, dw2);

  tpl.pbuf = pbuf;
  tpl.pswPCM = psd->sd_pswBuffer;
  tpl.iBytes = iBytes;
  tpl.iRate = iRate;
  tpl.iChannels = iChannels;
  return TRUE;
};

// 08 s8 - template lookup; validates against the live CSoundData and rebuilds on
// mismatch (07 s13 item 13)
static DS3DTemplate *DS3D_GetTemplate(CSoundData *psd) {
  if (_pSound == NULL || _pSound->sl_pDS == NULL) return NULL;

  se1::map<CSoundData *, DS3DTemplate>::iterator it = _mapTemplates.find(psd);

  if (it != _mapTemplates.end()) {
    DS3DTemplate &tpl = it->second;

    const INDEX iChannels = psd->sd_wfeFormat.nChannels;
    const INDEX iBytes = psd->sd_slBufferSampleSize * iChannels * (psd->sd_wfeFormat.wBitsPerSample / 8);

    if (tpl.pswPCM == psd->sd_pswBuffer
     && tpl.iBytes == iBytes
     && tpl.iRate == (INDEX)psd->sd_wfeFormat.nSamplesPerSec
     && tpl.iChannels == iChannels) {
      return &tpl; // still valid
    }
    if (tpl.pbuf != NULL) tpl.pbuf->Release();
    _mapTemplates.erase(it);
  }

  DS3DTemplate tplNew;
  if (!DS3D_BuildTemplate(psd, tplNew)) return NULL;
  return &_mapTemplates.insert(std::make_pair(psd, tplNew)).first->second;
};

// 08 s4 step 3 / 99 #16 - duplicate the cached template, configure 3D mode and start
// at the current engine-side sample offset. Caller holds sl_csSound and has validated
// SOF_PLAY (+ refreshed the cached format where required - 07 s13 item 17).
static BOOL DS3D_CommitShadow(DS3DShadow *psh) {
  CSoundObject *pso = psh->psoOwner;
  CSoundData *psd = pso->so_pCsdLink; // live while SOF_PLAY; never psdKey (07 s13 item 12)
  if (psd == NULL || _pSound == NULL || _pSound->sl_pDS == NULL) return FALSE;

  DS3DTemplate *ptpl = DS3D_GetTemplate(psd);
  if (ptpl == NULL) return FALSE;

  // cached format refresh (99 #11 input, bytes<->samples conversion - 08 s8)
  psh->iRate = ptpl->iRate;
  psh->iChannels = ptpl->iChannels;
  psh->iBytesPerFrame = ptpl->iChannels * 2; // 16-bit samples (eligibility, 08 s5)

  LPDIRECTSOUNDBUFFER pbuf = NULL;
  if (FAILED(_pSound->sl_pDS->DuplicateSoundBuffer(ptpl->pbuf, &pbuf)) || pbuf == NULL) {
    return FALSE;
  }

  LPDIRECTSOUND3DBUFFER pbuf3D = NULL;
  if (FAILED(pbuf->QueryInterface(_iidDS3DBuffer, (void **)&pbuf3D)) || pbuf3D == NULL) {
    pbuf->Release();
    return FALSE;
  }

  // the EAX property set is optional (non-fatal - 08 s4 step 9 gates the DirectHF set)
  LPKSPROPERTYSET pKsProps = NULL;
  if (_pSound->sl_bUsingEAX) {
    if (FAILED(pbuf->QueryInterface(_iidKsPropertySet, (void **)&pKsProps))) pKsProps = NULL;
  }

  // 99 #16 - HEADRELATIVE (positions are precomputed in listener space), immediate
  if (FAILED(pbuf3D->SetMode(DS3DMODE_HEADRELATIVE, DS3D_IMMEDIATE))) {
    if (pKsProps != NULL) pKsProps->Release();
    pbuf3D->Release();
    pbuf->Release();
    return FALSE;
  }

  // start at the current engine offset (= 0 for a fresh play; synced offset or live
  // mixer-advanced offset for recommits - 08 s4 step 3); the byte cursor lives on the
  // plain buffer interface
  DWORD dwStart = 0;
  if (pso->so_fLeftOffset > 0.0f) {
    FLOAT fOfs = pso->so_fLeftOffset * (FLOAT)psh->iBytesPerFrame;
    if (fOfs > 0.0f) dwStart = (DWORD)FloatToInt(fOfs);
  }
  if (FAILED(pbuf->SetCurrentPosition(dwStart))) {
    if (pKsProps != NULL) pKsProps->Release();
    pbuf3D->Release();
    pbuf->Release();
    return FALSE;
  }

  psh->pBuffer = pbuf;
  psh->pBuffer3D = pbuf3D;
  psh->pKsProps = pKsProps;
  psh->bStarted = FALSE;
  return TRUE;
};

// 08 s3 / 08 s6 - registration + release paths for the hook bodies in
// Patches/SoundLibrary.cpp (step 3: every eligible sound - 08 s11 step 3)
void DS3D_OnPlayPre(CSoundObject *pso, CSoundData *psd, SLONG slFlags) {
  const BOOL bWillShadow = DS3D_IsEligible(psd, slFlags) && DS3D_ModeOn();

  DS3DShadow *psh = DS3D_Find(pso);

  if (!bWillShadow) {
    // the engine object switched to data/mode we don't shadow - a stale entry would
    // keep playing the old sound forever, so release it (software takes over)
    if (psh != NULL) DS3D_RemoveEntry(pso);
    return;
  }

  if (psh == NULL) {
    psh = DS3D_NewShadow(pso);
    _mapShadows.insert(std::make_pair(pso, psh));
    DS3D_ApplyListenerParams(); // 08 s9 - first registration enables the mode
  } else {
    // re-play: the original's inlined Stop_internal won't pass through P_Stop_internal
    // (05 s1.3) - release the stale buffer right after it (DS3D_OnPlayPost). Snapshot
    // the OLD loop state first: the registration block below overwrites bLooping with
    // the new play's, and the detach decision needs the old buffer's (07 s15)
    psh->bReplay = TRUE;
    psh->bReplayLooping = psh->bLooping;
  }

  // (re)play registration - key + cached fields from the play arguments (08 s8)
  psh->psdKey = psd;
  psh->bLooping = (slFlags & SOF_LOOP) != 0;
  psh->iRate = psd->sd_wfeFormat.nSamplesPerSec;
  psh->iChannels = psd->sd_wfeFormat.nChannels;
  psh->iBytesPerFrame = psh->iChannels * (psd->sd_wfeFormat.wBitsPerSample / 8);
  psh->bSpatialValid = FALSE;  // 07 s13 item 18 - a fresh store gates the Play
  psh->tmSilentSince = 0.0;    // 07 s13 item 28 - the D8 silent timer restarts
  psh->bBypass = TRUE;         // 07 s13 item 27 - registration: the tail decides fallback
};

void DS3D_OnPlayPost(CSoundObject *pso) {
  DS3DShadow *psh = DS3D_Find(pso);
  if (psh == NULL) return;

  if (psh->bReplay) {
    psh->bReplay = FALSE;
    // 99 #17 - the object was re-played while our shadow buffer was playing: non-forced
    // release for non-looping buffers (detach, plays out; a not-playing buffer is just
    // released), force release for looping ones (07 s15). bReplayLooping: the OLD
    // buffer's loop state (bLooping is already the new play's). The tail commits the
    // fresh one this frame
    DS3D_DetachToOrphan(psh, psh->bReplayLooping);
  }
  // the (re)commit itself is lazy - the DS3D_DriverTail commit (08 s4 step 3)
};

void DS3D_OnStop(CSoundObject *pso) {
  // explicit Stop/Stop_internal only (Play_internal inlines Stop_internal - 05 s1.3)
  // and the original already ran (08 s3): detach per DS3D_DetachToOrphan (non-looping:
  // orphan play-out; looping: force release - 07 s15) and drop the entry
  if (_pSound == NULL) return;
  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE); // recursive - Stop() already holds it

  DS3DShadow *psh = DS3D_Find(pso);
  if (psh == NULL) return;

  DS3D_DetachToOrphan(psh, psh->bLooping);
  DS3D_RemoveEntry(pso);
};

void DS3D_OnDtor(CSoundObject *pso) {
  // 07 s12.1 - the destructor inlines Stop_internal, so a shadowed object being
  // destroyed never passes through P_Stop_internal; release before the original
  if (_pSound == NULL) return;
  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE); // recursive - any caller context
  DS3D_RemoveEntry(pso);
};

void DS3D_OnClearBuffer(CSoundData *psd) {
  // 07 sG1 - the single template invalidation point (all ClearBuffer paths hold
  // sl_csSound: Clear/Reload via CSoundData::Clear, CSoundLibrary::Clear)
  se1::map<CSoundData *, DS3DTemplate>::iterator it = _mapTemplates.find(psd);
  if (it != _mapTemplates.end()) {
    if (it->second.pbuf != NULL) it->second.pbuf->Release();
    _mapTemplates.erase(it);
  }

  // 07 s13 item 14 / G1 - shadows of this data: the owner is still playing -> hold the
  // entry via ReleaseForRecommit (Reload re-reads the PCM; the next tail recommits at
  // the synced offset and refreshes psdKey + the cached format, item 17); otherwise
  // the owner is gone (defensive - the dtor hook already removed the entry) -> drop.
  se1::map<CSoundObject *, DS3DShadow *>::iterator itSh = _mapShadows.begin();
  while (itSh != _mapShadows.end()) {
    DS3DShadow *psh = itSh->second;

    if (psh->psdKey != psd) {
      itSh++;
      continue;
    }

    if (itSh->first->so_slFlags & SOF_PLAY) {
      DS3D_ReleaseForRecommit(psh);
      itSh++;

    } else {
      // owner not playing (paused owners keep SOF_PLAY - they take the branch above)
      CSoundObject *pso = itSh->first;
      DS3D_RemoveEntry(pso);
      itSh = _mapShadows.begin(); // the map was mutated; restart
    }
  }
};

// 08 s3 G6 - the device is about to be recreated (or the algorithm GUID changed): every
// template is a buffer on the dying device. Called from the P_SetFormat pre pass and
// from the snd_iEmulationType post: handler (08 s2); the belt-and-braces pass also
// covers ClearBuffer paths we may miss
static void DS3D_FreeTemplates(void) {
  while (!_mapTemplates.empty()) {
    if (_mapTemplates.begin()->second.pbuf != NULL) _mapTemplates.begin()->second.pbuf->Release();
    _mapTemplates.erase(_mapTemplates.begin());
  }
};

void DS3D_OnSetFormatPre(void) {
  if (_pSound == NULL) return;
  // the original may be entered without the caller holding sl_csSound
  // (UpdateSounds hwnd-change path, SoundLibrary.cpp:992-994) - take it explicitly
  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE);

  // 08 s3 G6 - the device is about to be recreated: every template is a buffer on the
  // dying device (the belt-and-braces pass also covers ClearBuffer paths we may miss)
  DS3D_FreeTemplates();

  // 08 s3 G6 - orphan interfaces die with the device too; release while it is still
  // alive (step-5 orphan play-out list - "mark generation", 07 sG6)
  while (!_lstOrphans.empty()) {
    DS3D_ReleaseOrphan(_lstOrphans.back());
    _lstOrphans.pop_back();
  }

  // 08 s11 step 4 / G6 - release ALL shadows via ReleaseForRecommit: the entries are
  // KEPT (bStarted/bSpatialValid reset, bBypass stays true); the next P_UpdateSounds
  // tail recommits them at the synced offsets (RECREATE - one silent frame accepted,
  // item 15 - this makes the snd_iEmulationType toggle observable on running sounds)
  for (se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.begin();
       it != _mapShadows.end(); it++) {
    DS3D_ReleaseForRecommit(it->second);
  }

  // 08 s9 - our listener params die with the device; the post section re-applies them
  // (only when they had been applied - mode really active - else vanilla stays vanilla)
  _bDS3DReapplyAfterFormat = _bDS3DListenerApplied;
  _bDS3DListenerApplied = FALSE;
};

void DS3D_OnSetFormatPost(void) {
  if (!_bDS3DReapplyAfterFormat) return; // mode never applied - nothing to restore/drop
  _bDS3DReapplyAfterFormat = FALSE;
  if (_pSound == NULL) return;

  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE); // OPTEX-recursive (08 s7)

  if (_pSound->sl_pDSListener != NULL) {
    // G6 - re-apply the beta listener params on the fresh device (99 #1); the next
    // tail recommits the kept entries (item 15)
    DS3D_ApplyListenerParams();
    return;
  }

  // G2/G6 - the listener is gone: forced mode disable (DropToSoftware all - the only
  // mass DropToSoftware; registrations stay refused while sl_pDSListener == NULL and
  // re-enable on a later SetFormat that restores the listener - 08 s6)
  DS3D_NoticeOnce(_bDS3DNoticeDisabled, "DS3D: DS3D listener lost - spatial mode disabled, sounds dropped to the software mixer\n");

  while (!_mapShadows.empty()) {
    DS3D_DropToSoftware(_mapShadows.begin()->second);
  }
};

// 08 s2/G6 (07 s12.4) - the snd_iEmulationType change handler. guid3DAlgorithm is baked
// into every template buffer, so a change takes the re-init path (the beta declares the
// cvar post: with a SetFormat trigger, 04 s5): free the templates (the P_SetFormat pre
// pass repeats this sweep under its own lock - belt and braces per 08 s2) and call
// SetFormat(sl_EsfFormat). The call lands on the patched entry: pre frees orphans and
// holds every running shadow (ReleaseForRecommit, entries kept, bBypass stays true),
// post re-applies the listener params (or disables the mode when the listener is gone),
// and the next driver tail recommits at the synced offsets with the new GUID (RECREATE
// - 07 s13 item 15). This is what makes the toggle observable on running sounds
// (99 s4 #6, 08 s11 step 6).
static void DS3D_EmulationTypeChanged(INDEX) {
  if (_pSound == NULL) return;

  {
    // own lock for the cache sweep only - the SetFormat call must acquire tm_csHooks
    // BEFORE sl_csSound (stock order, SoundLibrary.cpp:952-954) to keep the lock order
    // consistent with the timer thread (07 sG8)
    CTSingleLock slSounds(&_pSound->sl_csSound, TRUE);
    DS3D_FreeTemplates();
  }

  _pSound->SetFormat(_pSound->sl_EsfFormat);
};

// 08 s3/08 s4 step 6 (07 sG7 companion) - the Mute window: the original zeroed the mix
// buffers and set _bMuted; the game loop is about to block (no UpdateSounds clears
// _bMuted until the operation finishes) while DS keeps mixing. Stop every PLAYING
// shadow (buffers kept - DS preserves the play cursor) and mark it gate-stopped; the
// first driver frame after the window resumes it from the DS position (step 7).
void DS3D_OnMute(void) {
  if (_mapShadows.size() == 0) return;
  if (_pSound == NULL) return;
  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE);

  se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.begin();
  while (it != _mapShadows.end()) {
    DS3DShadow *psh = it->second;

    if (psh->pBuffer == NULL) {
      it++;
      continue;
    }

    // only actually-playing buffers: a finished-but-undriven non-looping buffer must
    // NOT get bGateStopped (step 7 would spuriously re-Play it after the window);
    // lost/terminated buffers are left to the driver's G9 policy (inaudible anyway)
    DWORD dwStatus = 0;
    if (FAILED(psh->pBuffer->GetStatus(&dwStatus))
     || !(dwStatus & DSBSTATUS_PLAYING)) {
      it++;
      continue;
    }

    if (FAILED(psh->pBuffer->SetVolume(DSBVOLUME_MIN))) {
      // G9.2 - runtime failure: drop to software
      DS3D_NoticeOnce(_bDS3DNoticeRuntime, "DS3D: sound call failed - shadow dropped to the software mixer\n");
      DS3D_DropToSoftware(psh);
      it = _mapShadows.begin(); // the map was mutated; restart
      continue;
    }

    psh->pBuffer->Stop(); // failure ignorable (not in the G9 trigger list)
    psh->bGateStopped = TRUE; // 07 s13 item 20 - the step-7 re-Play consumes it
    it++;
  }
};

// 08 s3 / D1 - the P_MixSound membership test (timer thread, under MixSounds'
// sl_csSound - one table lookup + the precomputed bBypass read; no cvars, 08 s7)
BOOL DS3D_IsShadowBypassed(CSoundObject *pso) {
  DS3DShadow *psh = DS3D_Find(pso);
  return psh != NULL && psh->bBypass;
};

// 08 s4 (store, 07 sG10c) - compute and store the shadow's spatial values from the
// same listener walk as the vanilla math (own loop; the [Cecil] predicted-listener
// gate is reused so shadow and software listeners agree). Store-only: no COM calls.
void DS3D_StoreSpatial(CSoundObject *pso) {
  DS3DShadow *psh = DS3D_Find(pso);
  if (psh == NULL) return;

  FLOAT3D vPosition(0.0f, 0.0f, 0.0f);
  FLOAT3D vSpeed(0.0f, 0.0f, 0.0f);

  if (pso->so_penEntity != NULL) {
    vPosition = pso->so_penEntity->en_plPlacement.pl_PositionVector;

    if (pso->so_penEntity->en_ulPhysicsFlags & EPF_MOVABLE) {
      CMovableEntity *penMovable = (CMovableEntity *)pso->so_penEntity;
      vSpeed = penMovable->en_vCurrentTranslationAbsolute;
    }
  }

  // accumulators (02 s2): weighted sums over contributors
  FLOAT fSum = 0.0f;
  FLOAT3D vPosAcc(0.0f, 0.0f, 0.0f);
  FLOAT3D vVelAcc(0.0f, 0.0f, 0.0f);
  FLOAT fVolAcc = 0.0f;
  FLOAT fFilterAcc = 0.0f;

  if (pso->so_penEntity != NULL) {
    const BOOL bVolumetric = ((pso->so_slFlags & SOF_VOLUMETRIC) != 0) || (pso->so_psdcDecoder != NULL);
    // 07 s14.3 item 2: the beta's mask 0x58 = LOCAL|MUSIC|VOICE; Steam has no voice
    // flag, so the mapping is LOCAL|MUSIC = 0x50
    const BOOL bNoFilter = (pso->so_slFlags & (SOF_LOCAL|SOF_MUSIC)) != 0;

    FOREACHINLIST(CSoundListener, sli_lnInActiveListeners, _pSound->sl_lhActiveListeners, itsli) {
      CSoundListener &sli = *itsli;

      // [Cecil] gate - same listener set as the vanilla math (07 sG10c)
      if (pso->so_slFlags & SOF_LOCAL) {
        if (sli.sli_penEntity == NULL) continue;
        if (pso->so_penEntity != sli.sli_penEntity->GetPredictionTail()) continue;
      }

      FLOAT3D vAbsDelta = vPosition - sli.sli_vPosition;
      FLOAT fAbsDelta = vAbsDelta.Length();

      if (fAbsDelta > pso->so_sp3.sp3_fFalloff) continue; // 02 s2 - no contribution at all

      // distance factor w and volumetric bias (02 s2/s3, 99 #2/#5)
      FLOAT fW = 1.0f;
      FLOAT fVolBias = 0.0f;

      if (fAbsDelta > pso->so_sp3.sp3_fHotSpot) {
        fW = (pso->so_sp3.sp3_fFalloff - fAbsDelta)
           / (pso->so_sp3.sp3_fFalloff - pso->so_sp3.sp3_fHotSpot);
        fVolBias = ClampUp((fAbsDelta - pso->so_sp3.sp3_fHotSpot)
                         / (pso->so_sp3.sp3_fFalloff - pso->so_sp3.sp3_fHotSpot) * 3.0f, 1.0f);
      }

      // rotated delta and relative velocity in listener space (02 s2/s5, engine z)
      FLOAT3D vDirR = vAbsDelta * !sli.sli_mRotation;
      FLOAT3D vVelR = (vSpeed - sli.sli_vSpeed) * !sli.sli_mRotation;

      // volumetric near-field blend (02 s3): bias 0 -> pinned in front, static;
      // 0 < bias < 1 -> axis-lerp rotation of the front basis; bias >= 1 -> plain
      if (bVolumetric) {
        if (fVolBias <= 0.0f) {
          vDirR = FLOAT3D(0.0f, 0.0f, -1.0f);
          vVelR = FLOAT3D(0.0f, 0.0f, 0.0f);

        } else if (fVolBias < 1.0f && fAbsDelta > 0.001f) {
          const FLOAT3D vFront(0.0f, 0.0f, -1.0f);
          FLOAT3D vN = vDirR / fAbsDelta;
          FLOAT fDot = -vN(3); // dot(n, front) - 99 #5 / 07 s14.4 (ACos(n.z) is a misread)

          FLOAT3D vPlane = vN * vFront; // rotation axis in the horizontal plane
          FLOAT fPlaneLen = vPlane.Length();
          if (fPlaneLen > 0.001f) {
            vPlane /= fPlaneLen;
          } else {
            vPlane = FLOAT3D(0.0f, 1.0f, 0.0f); // exactly front/back - any vertical axis
          }

          FLOAT3D vOrt2 = vFront * vPlane;
          FLOAT fT = Clamp(fDot, 0.0f, 1.0f); // lerp toward +Y for downward directions
          FLOAT3D vOrt1 = vOrt2 * (1.0f - fT) + FLOAT3D(0.0f, 1.0f, 0.0f) * fT;

          FLOAT fAngle = (FLOAT)acos((double)Clamp(fDot, -1.0f, 1.0f)) * fVolBias;
          vDirR = vFront * (FLOAT)cos((double)fAngle) + vOrt1 * (FLOAT)sin((double)fAngle);
          vVelR *= fVolBias; // attenuated speed
        }
      }

      fSum += fW;
      vPosAcc += vDirR * fW;
      vVelAcc += vVelR * fW;
      fVolAcc += sli.sli_fVolume * pso->so_sp3.sp3_fMaxVolume * fW * fW;
      fFilterAcc += (bNoFilter ? 0.0f : sli.sli_fFilter) * fW;
    }
  }

  if (fSum > 0.001f) {
    psh->vPos = vPosAcc / fSum;                          // 02 s2 - weighted average
    psh->vVel = vVelAcc / fSum;                          // 02 s5
    psh->fLastVolume = Clamp(fVolAcc / fSum, 0.0f, 1.0f); // 08 s4, G10c - clamp01, pre-master
    psh->fLastFilter = fFilterAcc / fSum;                // 99 #13 DirectHF source
    // 07 s13 item 16 - sp_new delay test in the store; the tail advances so_fDelayed
    psh->bDelayPending = (pso->so_spNew.sp_fDelay > 0.0f && pso->so_fDelayed < pso->so_spNew.sp_fDelay);

  } else {
    // parked (02 s1/08 s4): no entity or no listener within falloff
    psh->vPos = FLOAT3D(0.0f, 0.0f, 10000.0f);
    psh->vVel = FLOAT3D(0.0f, 0.0f, 0.0f);
    psh->fLastVolume = 0.0f;
    psh->fLastFilter = 0.0f;
    psh->bDelayPending = FALSE;
  }

  psh->bSpatialValid = TRUE; // 07 s13 item 18 - store ran; the driver's Play may follow
};

// 08 s4 - what the driver tail must do with a driven entry
enum EDS3DDriveResult {
  DS3D_KEEP,      // normal - entry stays committed
  DS3D_FINISHED,  // step 8 fired - caller releases the buffer and drops the entry
  DS3D_DROPPED,   // failure policy removed the entry (DropToSoftware) - caller rescans
};

// G9.2/G9.3 - runtime DS failure handling for a committed shadow.
// BUFFERLOST: Restore() once; still lost on the next frame -> drop. Every other
// failure (TERMINATED handled by the caller): drop immediately. Returns DS3D_KEEP
// (restored - no further DS calls this frame) or DS3D_DROPPED (entry removed).
static int DS3D_FailShadow(DS3DShadow *psh, HRESULT hr) {
  if (hr == DSERR_BUFFERLOST) {
    if (psh->iFailFrames > 0 || FAILED(psh->pBuffer->Restore())) {
      DS3D_NoticeOnce(_bDS3DNoticeLost, "DS3D: shadow buffer lost - dropped to the software mixer\n");
      DS3D_DropToSoftware(psh);
      return DS3D_DROPPED;
    }
    psh->iFailFrames = 1; // G9.3 - one Restore per loss episode
    return DS3D_KEEP;     // skip the rest of this frame's drive
  }

  DS3D_NoticeOnce(_bDS3DNoticeRuntime, "DS3D: sound call failed - shadow dropped to the software mixer\n");
  DS3D_DropToSoftware(psh);
  return DS3D_DROPPED;
};

// 08 s4 steps 4-9 - drive one committed shadow (05-step-4 split: gates + failure policy).
// Caller holds sl_csSound, has validated SOF_PLAY and refreshed the cached format.
// fVolEff = stored volume with the master factor already applied (07 sG10b) - the D8
// gate consumed the same value in the tail (08 s4 step 2).
static int DS3D_DriveShadow(DS3DShadow *psh, BOOL bGamePaused, FLOAT fVolEff) {
  CSoundObject *pso = psh->psoOwner;
  LPDIRECTSOUNDBUFFER pbuf = psh->pBuffer;

  // 4. position/velocity - DEFERRED (99 #6), z-negated into DS3D space (02 s5)
  HRESULT hrPos = psh->pBuffer3D->SetPosition(psh->vPos(1), psh->vPos(2), -psh->vPos(3), DS3D_DEFERRED);
  if (FAILED(hrPos)) return DS3D_FailShadow(psh, hrPos);

  HRESULT hrVel = psh->pBuffer3D->SetVelocity(psh->vVel(1), psh->vVel(2), -psh->vVel(3), DS3D_DEFERRED);
  if (FAILED(hrVel)) return DS3D_FailShadow(psh, hrVel);

  // 5. current status (finish rule + EAX gate + G9.3 lost handling)
  DWORD dwStatus = 0;
  HRESULT hrStatus = pbuf->GetStatus(&dwStatus);
  if (FAILED(hrStatus)) return DS3D_FailShadow(psh, hrStatus);

  if (dwStatus & DSBSTATUS_TERMINATED) {
    // G9.3 - TERMINATED -> drop (the buffer is going away)
    DS3D_NoticeOnce(_bDS3DNoticeLost, "DS3D: shadow buffer lost - dropped to the software mixer\n");
    DS3D_DropToSoftware(psh);
    return DS3D_DROPPED;
  }
  if (dwStatus & DSBSTATUS_BUFFERLOST) return DS3D_FailShadow(psh, DSERR_BUFFERLOST);
  psh->iFailFrames = 0; // healthy this frame

  BOOL bPlaying = (dwStatus & DSBSTATUS_PLAYING) != 0;

  // 6. pause/mute gates (08 s4 step 6, 07 s13 item 20): game pause, SOF_NONGAME exempt.
  // The Mute window is handled at Mute()-call time (07 sG7 companion - the original
  // just cleared _bMuted during UpdateSounds:996, so it is unreadable here). Gated
  // frames skip the volume/frequency sets entirely (beta order, 07 s14.1 check 1).
  BOOL bGated = bGamePaused && !(pso->so_slFlags & SOF_NONGAME);

  if (bGated) {
    if (bPlaying) {
      pbuf->Stop(); // DS Stop preserves the play cursor (99 #20); failure ignorable
      bPlaying = FALSE;
      psh->bGateStopped = TRUE; // consumed by the step-7 re-Play
    }

  } else {
    // volume + frequency - IMMEDIATE (99 #6). fVolEff is the master-applied stored
    // volume (07 sG10b). Same-sounds budget (99 #9, 07 s13 item 26): per-frame running
    // sum per CSoundData; an entry keeps its volume only if volume + running <=
    // snd_iMaxSameSounds, else volume 0 (committed, still counted as a voice).
    FLOAT fVol = fVolEff;

    if (fVol > 0.005f) {
      FLOAT &fRunning = _mapSameRunning[psh->psdKey]; // opaque key only (item 12)
      const FLOAT fMax = (FLOAT)Clamp((LONG)_snd_iMaxSameSounds, 1L, 99L);

      if (fRunning + fVol <= fMax) {
        fRunning += fVol;
      } else {
        fVol = 0.0f;
      }
    }

    SLONG slCb;
    if (fVol <= 0.005f) {
      slCb = DSBVOLUME_MIN; // 99 #10
    } else {
      if (fVol > 1.0f) fVol = 1.0f; // log10 input guard (DS range is [-10000, 0])
      FLOAT fCb = 3000.0f * (fVol * (fVol - 1.0f) + (1.0f - fVol) * (FLOAT)log10((double)fVol));
      slCb = (SLONG)Clamp((LONG)FloatToInt(fCb), (LONG)DSBVOLUME_MIN, (LONG)0);

      // 07 §16, 08 §4 step 6 - post-curve flat user gain offset (deliberate deviation from
      // beta parity; the 99 #10 curve itself is unchanged). This branch only runs for
      // fVol > 0.005, so the v <= 0.005 sentinel above stays exactly -10000. The D8 silence
      // test, voice budget and same-sounds budget above keep the pre-curve volume.
      FLOAT fGain = Clamp(_snd_fDS3DGain, -30.0f, 0.0f);
      slCb = (SLONG)Clamp((LONG)slCb + (LONG)FloatToInt(fGain * 100.0f), (LONG)DSBVOLUME_MIN, (LONG)0);
    }

    HRESULT hrVol = pbuf->SetVolume(slCb);
    if (FAILED(hrVol)) return DS3D_FailShadow(psh, hrVol);

    const FLOAT fPitch = pso->so_sp3.sp3_fPitch;
    const SLONG slFreq = (fPitch == 1.0f) ? 0 : FloatToInt(psh->iRate * fPitch); // 99 #11
    HRESULT hrFreq = pbuf->SetFrequency(slFreq);
    if (FAILED(hrFreq)) return DS3D_FailShadow(psh, hrFreq);
  }

  // 7. Play (resume) once the store ran and the delay elapsed (07 s13 items 16/18/20);
  // bGateStopped covers a paused/muted buffer stopped while DS preserved its cursor
  if (!bPlaying && !bGated && !psh->bDelayPending && psh->bSpatialValid
   && (!psh->bStarted || psh->bGateStopped)) {
    HRESULT hrPlay = pbuf->Play(0, 0, psh->bLooping ? DSBPLAY_LOOPING : 0);
    if (FAILED(hrPlay)) return DS3D_FailShadow(psh, hrPlay);

    psh->bStarted = TRUE;
    psh->bGateStopped = FALSE; // 07 s13 item 20 - cleared when Play is issued
    bPlaying = TRUE;           // just started - the finish rule may not fire now
  }

  // 8. finish (pure beta rule, 08 s4 step 8; gates active block it, so a gated Stop is
  // never misread as a natural finish) - a started non-looping shadow that DS reports
  // not playing has finished naturally (or the device stopped it). SOF_PLAY is ours to
  // clear, under sl_csSound; the engine unlinks the object on the next UpdateSounds.
  if (psh->bStarted && !bPlaying && !psh->bLooping && !bGated) {
    psh->bStarted = FALSE;
    pso->so_slFlags &= ~SOF_PLAY;
    pso->so_fDelayed = 0.0f; // mixer parity (07 s13 item 16)
    return DS3D_FINISHED;    // the tail releases the buffer and drops the entry
  }

  // 9. EAX DirectHF (DEFERRED) - 99 #13
  if (psh->pKsProps != NULL && _snd_bUseEnvironmentFX != 0
   && (pso->so_slFlags & SOF_3D) && bPlaying) {
    LONG lHF = FloatToInt(-100.0f * psh->fLastFilter - 0.5f); // round() for negatives
    lHF = Clamp(lHF, (LONG)-10000, (LONG)0);
    psh->pKsProps->Set(_guidEAXBufferProps,
      0x80000003 /* DEFERRED | DSPROPERTY_EAXBUFFER_DIRECTHF, EAX.h:254 */,
      NULL, 0, &lHF, sizeof(LONG));
  }

  return DS3D_KEEP;
};

// 99 #15 - live voices = shadows holding a buffer (the channel table is the live-shadow
// list itself - 08 s8). Silent entries never reach a commit, so they never consume a
// voice (07 s13 item 25); parked (held+silent) buffers still occupy their channel for
// the <= 10 s window of item 24.
static INDEX DS3D_CountLiveVoices(void) {
  INDEX ctLive = 0;

  for (se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.begin();
       it != _mapShadows.end(); it++) {
    if (it->second->pBuffer != NULL) ctLive++;
  }
  return ctLive;
};

// 99 #15 - steal candidate: the live channel with the lowest stored volume (the
// master factor scales all candidates equally, so the comparison is order-safe)
static DS3DShadow *DS3D_FindLowestLive(void) {
  DS3DShadow *pstLowest = NULL;
  FLOAT fLowest = 2.0f; // above any stored 0..1 volume

  for (se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.begin();
       it != _mapShadows.end(); it++) {
    DS3DShadow *psh = it->second;
    if (psh->pBuffer == NULL) continue;

    if (psh->fLastVolume < fLowest) {
      fLowest = psh->fLastVolume;
      pstLowest = psh;
    }
  }
  return pstLowest;
};

// 04 s3/s4 - beta per-frame drive order (PlayBuffers): stored volume descending, then
// distance ascending (comparator FUN_10081f00 / LC SoundLibrary.cpp:1018-1042)
struct DS3DDriveEntry {
  CSoundObject *pso;
  FLOAT fVolume;
  FLOAT fDistance;
};

static int DS3D_CompareDriveEntries(const void *p1, const void *p2) {
  const DS3DDriveEntry &e1 = *(const DS3DDriveEntry *)p1;
  const DS3DDriveEntry &e2 = *(const DS3DDriveEntry *)p2;

  if (e1.fVolume > e2.fVolume) return -1;
  if (e1.fVolume < e2.fVolume) return 1;
  if (e1.fDistance < e2.fDistance) return -1;
  if (e1.fDistance > e2.fDistance) return 1;
  return 0;
};

// 08 s4 - the per-frame driver tail, called after the original UpdateSounds (which
// released sl_csSound on return, SoundLibrary.cpp:1001): table validation, SOF_PAUSED
// gate, delay advance, D8 silence gate (step 2), lazy commit with voice budget/steal
// (step 3), driver steps 4-9 per shadow in beta (sorted) order, orphan scan, then ONE
// CommitDeferredSettings
void DS3D_DriverTail(void) {
  if (_mapShadows.size() == 0 && _lstOrphans.empty()) return; // fast path: mode off / nothing registered
  if (_pSound == NULL || _pSound->sl_pDS == NULL || _pSound->sl_pDSListener == NULL) return;

  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE); // OPTEX-recursive (08 s7)

  // frame time for the so_fDelayed advance (07 s13 item 16); clamped, first call safe
  CTimerValue tvNow = _pTimer->GetHighPrecisionTimer();
  CTimerValue tvLast(_llDS3DLastTail);
  _llDS3DLastTail = tvNow.tv_llValue;
  const FLOAT fFrameSecs = Clamp((FLOAT)(tvNow - tvLast).GetSeconds(), 0.0f, 0.25f);

  // 07 s13 item 24 - the D8 silent-since timer runs on the beta's clock: game tick
  // (frozen while the game does not advance), real time for SOF_NONGAME sounds
  const TIME tmTick = _pTimer->CurrentTick();
  const FLOAT fTickSecs = Clamp((FLOAT)(tmTick - _fDS3DLastTick), 0.0f, 0.25f);
  _fDS3DLastTick = tmTick;

  // 99 #9 - same-sounds budget sums restart every frame (07 s13 item 26)
  _mapSameRunning.clear();

  // 08 s4 step 6 - the game-pause gate uses the stock MixSounds expression
  // (SoundLibrary.cpp:1284). _pNetwork==NULL counts as not paused (IsPaused()
  // returns TRUE for this==NULL by design - Network.cpp:1389).
  BOOL bGamePaused = FALSE;
  if (_pNetwork != NULL) {
    bGamePaused = _pNetwork->IsPaused() || (_pNetwork->IsServer() && _pNetwork->GetLocalPause());
  }

  // 99 #15 - voice budget (clamped 4..64) and the per-tail master factor for the D8
  // gate ("storedVol x snd_fSoundVolume < 0.005" - 07 s13 item 21)
  const INDEX iMaxVoices = (INDEX)Clamp((LONG)_snd_iMaxVoices, 4L, 64L);
  static CSymbolPtr pfSoundVolume("snd_fSoundVolume");
  const FLOAT fMaster = pfSoundVolume.GetFloat();

  BOOL bAnyBuffer = FALSE;

  // 04 s3/s4 + 08 s8 - beta per-frame order: drive the table sorted by stored volume
  // descending, then distance ascending (required for the voice steal). Snapshot the
  // owner pointers; each is re-validated against the table because this pass removes
  // entries (freed shadows must not be dereferenced).
  const INDEX ctSorted = (INDEX)_mapShadows.size();
  DS3DDriveEntry *aSorted = NULL;

  if (ctSorted > 0) {
    aSorted = new DS3DDriveEntry[ctSorted];

    INDEX iSorted = 0;
    for (se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.begin();
         it != _mapShadows.end(); it++) {
      aSorted[iSorted].pso = it->first;
      aSorted[iSorted].fVolume = it->second->fLastVolume;
      aSorted[iSorted].fDistance = it->second->vPos.Length();
      iSorted++;
    }
    if (iSorted > 1) qsort(aSorted, iSorted, sizeof(DS3DDriveEntry), DS3D_CompareDriveEntries);
  }

  for (INDEX i = 0; i < ctSorted; i++) {
    CSoundObject *pso = aSorted[i].pso;
    DS3DShadow *psh = DS3D_Find(pso);
    if (psh == NULL) continue; // removed earlier in this pass

    // validation: !SOF_PLAY means the engine is done with the sound (Stop clears it,
    // the stock mixer clears it for fallback entries) - plain release, no sync
    if (!(pso->so_slFlags & SOF_PLAY)) {
      DS3D_RemoveEntry(pso);
      continue;
    }

    // 07 s13 item 17 - refresh the cached format from the live data AFTER the check
    // above; psdKey stays an opaque key and is never dereferenced
    CSoundData *psd = pso->so_pCsdLink;
    if (psd != NULL) {
      psh->psdKey = psd;
      psh->iRate = psd->sd_wfeFormat.nSamplesPerSec;
      psh->iChannels = psd->sd_wfeFormat.nChannels;
      psh->iBytesPerFrame = psh->iChannels * (psd->sd_wfeFormat.wBitsPerSample / 8);
    }

    // 1. SOF_PAUSED gate (08 s4 step 1) - release via ReleaseForRecommit (sync + entry
    // kept; bBypass stays true); recommit on resume at the synced offset (99 #18).
    // Paused entries do not advance so_fDelayed either - the stock mixer skips them
    // (05 s2.1 SOF_PAUSED test).
    if (pso->so_slFlags & SOF_PAUSED) {
      DS3D_ReleaseForRecommit(psh); // idempotent after the first frame
      continue;
    }

    // delay advance for pending skipped shadows - the stock mixer no longer advances
    // them (07 s13 item 16: advances only while bDelayPending && bBypass)
    if (psh->bDelayPending && psh->bBypass) {
      pso->so_fDelayed += fFrameSecs;
      psh->bDelayPending = (pso->so_sp.sp_fDelay > 0.0f && pso->so_fDelayed < pso->so_sp.sp_fDelay);
    }

    // 2. D8 silence gate (07 s13 items 21-25): post-master, pre-budget
    const FLOAT fVolEff = psh->fLastVolume * fMaster;
    const BOOL bSilent = (fVolEff < 0.005f); // beta const 0.005 (0x101b95cc)

    if (bSilent) {
      if (psh->pBuffer == NULL) {
        // item 22 - not held: no commit attempt, no voice slot, no DS calls at all;
        // audible-software-not (silent) fallback via the stock mixer; recommit on a
        // later tail once audible again (item 23)
        psh->bBypass = FALSE;
        continue;
      }

      // item 24 - held + silent: keep parked (steps 4-9 below set the sentinel
      // position + SetVolume(-10000)); after > 10 s of silence release to
      // software-silent, recommit at the live offset once audible again
      psh->tmSilentSince += ((pso->so_slFlags & SOF_NONGAME) ? fFrameSecs : fTickSecs);

      if (psh->tmSilentSince > 10.0) { // beta const 10.0 (0x101b95c8)
        DS3D_ReleaseForRecommit(psh);
        psh->bBypass = FALSE;
        continue;
      }

    } else {
      psh->tmSilentSince = 0.0; // audible - timer cleared (beta 0x10082b1a-0x10082b21)
    }

    // 3. ensure committed (08 s4 step 3; silent entries never reach this point, so
    // they never consume a voice)
    if (psh->pBuffer == NULL) {
      DS3DShadow *pstVictim = NULL;

      if (DS3D_CountLiveVoices() >= iMaxVoices) {
        // voice budget full - steal the lowest-volume live channel only if the new
        // sound is clearly louder (99 #15, beta const 0.333 / 0x101b95bc)
        pstVictim = DS3D_FindLowestLive();

        if (pstVictim == NULL || fVolEff - pstVictim->fLastVolume * fMaster <= 0.333f) {
          // budget denied this frame - audible software fallback, retried every tail
          // (07 s13 item 19 - deviation from 99 #15's silence, by design)
          psh->bBypass = FALSE;
          continue;
        }
      }

      if (!DS3D_CommitShadow(psh)) {
        // G9.1 - creation failure: DropToSoftware (sync inert, nothing was poisoned)
        DS3D_NoticeOnce(_bDS3DNoticeCommit, "DS3D: shadow buffer creation failed - sound left to the software mixer\n");
        DS3D_DropToSoftware(psh);
        continue;
      }

      psh->bBypass = TRUE; // 07 s13 item 19 - back to the shadow path on success

      if (pstVictim != NULL) {
        // steal victim: sync at steal time, then audible software continues from the
        // synced offset (07 sG4d/99 #15; re-commit at the then-live offset)
        DS3D_ReleaseForRecommit(pstVictim);
        pstVictim->bBypass = FALSE;
      }
    }

    const int eDrive = DS3D_DriveShadow(psh, bGamePaused, fVolEff);

    if (eDrive == DS3D_KEEP) {
      bAnyBuffer = TRUE;
      continue;
    }
    if (eDrive == DS3D_FINISHED) {
      // step 8 fired - plain release + drop (the engine unlinks on the next update)
      DS3D_RemoveEntry(pso);
      continue;
    }
    // DS3D_DROPPED - DropToSoftware already erased the entry
  }

  delete[] aSorted;

  // 99 #17 - orphan scan: buffers detached from stopped/re-played shadows play out
  // and are released here once DS reports them done
  DS3D_ScanOrphans();

  // 99 #6 - one commit per frame, on the listener (the DS object that owns it)
  if (bAnyBuffer) _pSound->sl_pDSListener->CommitDeferredSettings();
};

#endif // _PATCHCONFIG_ENGINEPATCHES && SE1_VER == SE1_107 && defined(NDEBUG)
