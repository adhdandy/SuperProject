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

// 08 s6 - refusal state. Cleared if any hook in ICorePatches::SoundLibrary() fails its
// install check (dtor symbol missing, MixSound prologue/module mismatch). Step 3
// registration gates on this: a partial install is never safe (shadows without the
// MixSound skip = double audio; skip without shadows = silent zombies).
BOOL _bDS3DHooksOk = TRUE;

// 08 s2/s11 step 1 - declared once here; called from ICorePatches::SoundLibrary()
// (EnginePatches.cpp, 06 s5 pattern). Default snd_bDS3DSpatial = 0: nothing in this
// subsystem runs, behavior is vanilla.
void SoundDS3D_Init(void) {
  _pShell->DeclareSymbol("persistent user INDEX snd_bDS3DSpatial;",      &_snd_bDS3DSpatial);
  _pShell->DeclareSymbol("persistent user INDEX snd_iEmulationType;",    &_snd_iEmulationType);
  _pShell->DeclareSymbol("persistent user INDEX snd_iMaxVoices;",        &_snd_iMaxVoices);
  _pShell->DeclareSymbol("persistent user INDEX snd_iMaxSameSounds;",    &_snd_iMaxSameSounds);
  _pShell->DeclareSymbol("persistent user INDEX snd_bUseEnvironmentFX;", &_snd_bUseEnvironmentFX);
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

// 08 s9 - listener parameters applied once per (re)initialized device
static BOOL _bDS3DListenerApplied = FALSE;

// step-2/3 interim failure policy (the full DropToSoftware primitive lands in step 4)
static BOOL _bDS3DCommitFailed = FALSE;

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

// 08 s8 - release the shadow's DS objects (force; the orphan play-out list lands in
// step 5, so the buffer is stopped now). Idempotent; failure of Stop is ignorable.
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

// 08 s8 - registry entry removal (release + erase). Game thread, under sl_csSound.
static void DS3D_RemoveEntry(CSoundObject *pso) {
  se1::map<CSoundObject *, DS3DShadow *>::iterator it = _mapShadows.find(pso);
  if (it == _mapShadows.end()) return;

  DS3DShadow *psh = it->second;
  DS3D_ReleaseBuffer(psh);
  _mapShadows.erase(it);
  delete psh;
};

// step-2/3 interim failure policy (08 s6): drop the entry, notice once per session
// (the full DropToSoftware primitive with position sync lands in step 4)
static void DS3D_CommitFailed(CSoundObject *pso) {
  if (!_bDS3DCommitFailed) {
    _bDS3DCommitFailed = TRUE;
    CPrintF("DS3D: shadow buffer commit failed - sound left to the software mixer\n");
  }
  DS3D_RemoveEntry(pso);
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
// Patches/SoundLibrary.cpp (step 2 scope: looping ambients only, 08 s11 step 2)
void DS3D_OnPlayPre(CSoundObject *pso, CSoundData *psd, SLONG slFlags) {
  // step-2 scope: looping ambients only (extended to every eligible sound in step 3)
  const BOOL bWillShadow = DS3D_IsEligible(psd, slFlags)
                        && ((slFlags & SOF_LOOP) != 0)
                        && DS3D_ModeOn();

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
    // (05 s1.3) - release the stale buffer right after it (DS3D_OnPlayPost)
    psh->bReplay = TRUE;
  }

  // (re)play registration - key + cached fields from the play arguments (08 s8)
  psh->psdKey = psd;
  psh->bLooping = (slFlags & SOF_LOOP) != 0;
  psh->iRate = psd->sd_wfeFormat.nSamplesPerSec;
  psh->iChannels = psd->sd_wfeFormat.nChannels;
  psh->iBytesPerFrame = psh->iChannels * (psd->sd_wfeFormat.wBitsPerSample / 8);
  psh->bSpatialValid = FALSE; // 07 s13 item 18 - a fresh store gates the Play
};

void DS3D_OnPlayPost(CSoundObject *pso) {
  DS3DShadow *psh = DS3D_Find(pso);
  if (psh == NULL) return;

  if (psh->bReplay) {
    psh->bReplay = FALSE;
    // 08 s3 step 3 - the object was re-played: release the stale buffer (orphan
    // play-out lands in step 5)
    DS3D_ReleaseBuffer(psh);
  }

  // step-2 immediate commit for looping ambients (the lazy tail commit lands in step 3;
  // the buffer plays audibly centered at DS defaults until the driver exists)
  if (psh->bLooping && (pso->so_slFlags & SOF_PLAY)) {
    if (!DS3D_CommitShadow(psh)) DS3D_CommitFailed(pso);
  }
};

void DS3D_OnStop(CSoundObject *pso) {
  // explicit Stop/Stop_internal only (Play_internal inlines Stop_internal - 05 s1.3);
  // force release, orphan play-out lands in step 5
  if (_pSound == NULL) return;
  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE); // recursive - Stop() already holds it
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
  if (it == _mapTemplates.end()) return;

  if (it->second.pbuf != NULL) it->second.pbuf->Release();
  _mapTemplates.erase(it);
};

void DS3D_OnSetFormatPre(void) {
  if (_pSound == NULL) return;
  // the original may be entered without the caller holding sl_csSound
  // (UpdateSounds hwnd-change path, SoundLibrary.cpp:992-994) - take it explicitly
  CTSingleLock slSounds(&_pSound->sl_csSound, TRUE);

  // step-2/3 interim: the device is about to be recreated - release every shadow and
  // drop the entries (software takes over; ReleaseForRecommit + recommit-at-synced-
  // offset lands in step 4/6). All buffers under our control must go before sl_pDS.
  while (!_mapShadows.empty()) {
    DS3D_RemoveEntry(_mapShadows.begin()->first);
  }

  // belt-and-braces beyond the P_ClearBuffer hooks that run during the original's
  // ClearBuffer-all pass
  while (!_mapTemplates.empty()) {
    if (_mapTemplates.begin()->second.pbuf != NULL) _mapTemplates.begin()->second.pbuf->Release();
    _mapTemplates.erase(_mapTemplates.begin());
  }

  _bDS3DListenerApplied = FALSE; // 08 s9 - re-apply on the fresh device
};

#endif // _PATCHCONFIG_ENGINEPATCHES && SE1_VER == SE1_107 && defined(NDEBUG)
