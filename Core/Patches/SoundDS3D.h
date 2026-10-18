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

#ifndef CECIL_INCL_PATCHES_SOUNDDS3D_H
#define CECIL_INCL_PATCHES_SOUNDDS3D_H

#ifdef PRAGMA_ONCE
  #pragma once
#endif

// 08 s1: per-sound DirectSound3D patch subsystem, Steam TSE 1.07 release only.
// Gate per AGENTS.md invariant 2: SE1_VER == SE1_107 && defined(NDEBUG) && _PATCHCONFIG_ENGINEPATCHES.
#if _PATCHCONFIG_ENGINEPATCHES && SE1_VER == SE1_107 && defined(NDEBUG)

// Engine.h does not include SoundData.h (and neither do SoundObject.h/SoundLibrary.h),
// so fetch the full CSoundData definition ourselves. The CSoundObject/CSoundLibrary
// full definitions must already have been pulled in by the enclosing TU (Engine.h).
#include <Engine/Sound/SoundData.h>

class CSoundLibDSPatch : public CSoundLibrary {
  public:
    void P_UpdateSounds(void);                                  // 08 s3
    SoundFormat P_SetFormat(SoundFormat EsfNew, BOOL bReload);  // 08 s3
    void P_Mute(void);                                          // 08 s3
};

class CSoundObjDSPatch : public CSoundObject {
  public:
    void P_Play_internal(CSoundData *pCsdLink, SLONG slFlags);  // 08 s3
    void P_Stop_internal(void);                                 // 08 s3 (ForceRewrite(7))
    void P_dtor(void);                                          // 08 s3 (via mangled-name symbol)
};

class CSoundDataDSPatch : public CSoundData {
  public:
    void P_ClearBuffer(void);                                   // 08 s3
};

// 08 s2 - cvars (definitions in SoundDS3D.cpp, declared once in SoundDS3D_Init)
extern INDEX _snd_bDS3DSpatial;      // snd_bDS3DSpatial, default 0 - the mode switch
extern INDEX _snd_iEmulationType;    // snd_iEmulationType, default 0 (99 #14)
extern INDEX _snd_iMaxVoices;        // snd_iMaxVoices, default 30, clamp 4..64 (99 #15)
extern INDEX _snd_iMaxSameSounds;    // snd_iMaxSameSounds, default 5, clamp 1..99 (99 #9)
extern INDEX _snd_bUseEnvironmentFX; // snd_bUseEnvironmentFX, default 1 (99 #13)
extern FLOAT _snd_fDS3DGain;         // snd_fDS3DGain, default -6.0 dB, clamp -30..0 (07 s16)

// 08 s6 - FALSE if any hook failed its install check; step 3 registration gates on it
extern BOOL _bDS3DHooksOk;

// 08 s8 - cached DS template per CSoundData (07 sG1: built lazily from sd_pswBuffer
// via Lock/Unlock, PCM copied once; flags per 99 #14 + guid3DAlgorithm from
// snd_iEmulationType). Invalidated by P_ClearBuffer, freed on P_SetFormat. Lookup
// validates the fields against the live CSoundData and rebuilds on mismatch (07 s13
// item 13 - guards CSoundData address reuse if a ClearBuffer path is ever missed).
struct DS3DTemplate {
  LPDIRECTSOUNDBUFFER pbuf;
  SWORD *pswPCM;         // sd_pswBuffer at build time (validation only)
  INDEX iBytes;          // sd_slBufferSampleSize * channels * 2
  INDEX iRate;           // sd_wfeFormat.nSamplesPerSec
  INDEX iChannels;       // sd_wfeFormat.nChannels
};

// 08 s8 - one registered shadow per CSoundObject.
// psdKey is an OPAQUE map key only - never dereference it (07 s13 item 12). The live
// data pointer is always psoOwner->so_pCsdLink (valid while SOF_PLAY); refresh order
// at (re)commit: !SOF_PLAY validation -> read so_pCsdLink -> refresh the cached fields
// (07 s13 item 17). Stored spatial fields initialize to parked (02 s1) as the
// bSpatialValid backstop.
struct DS3DShadow {
  CSoundObject *psoOwner;
  CSoundData *psdKey;            // opaque key (07 s13 item 12)
  LPDIRECTSOUNDBUFFER pBuffer;
  LPDIRECTSOUND3DBUFFER pBuffer3D;
  LPKSPROPERTYSET pKsProps;      // NULL when EAX is unavailable (non-fatal - 08 s4 step 9)
  BOOL bLooping;                 // SOF_LOOP at registration (99 #16)
  BOOL bStarted;                 // Play issued on the current buffer (99 #17)
  BOOL bDelayPending;            // sp_fDelay not yet elapsed (07 s13 item 16)
  BOOL bSpatialValid;            // set by the P_Update3DEffects store (07 s13 item 18)
  BOOL bBypass;                  // P_MixSound skips only when set (07 s13 item 19)
  BOOL bGateStopped;             // step-4 gate consumed by the re-Play (07 s13 item 20)
  BOOL bReplay;                  // re-play: stale buffer pending detach (07 s15: loops force-released)
  BOOL bReplayLooping;           // re-play: OLD buffer's loop state for the detach (bLooping is already the new play's)
  FLOAT3D vPos;                  // stored listener-space position, engine space (02 s2)
  FLOAT3D vVel;                  // stored listener-space velocity (02 s5)
  FLOAT fLastVolume;             // stored engine volume 0..1, pre-master (08 s4 step 6)
  FLOAT fLastFilter;             // stored hf factor (DirectHF source, 99 #13)
  INDEX iRate;                   // cached from the play args (08 s8, 99 #11)
  INDEX iChannels;
  INDEX iBytesPerFrame;
  INDEX iFailFrames;             // G9.3 lost-buffer Restore-once tracker
  DOUBLE tmSilentSince;          // step-5 D8 silent-since timer (07 s13 item 24)
};

// 08 s3 - hook bodies for CSoundObjDSPatch / CSoundDataDSPatch / CSoundLibDSPatch
// (definitions in SoundDS3D.cpp, called from Patches/SoundLibrary.cpp). Game thread,
// all table mutations under sl_csSound (inherited from the hook's caller or taken
// explicitly; OPTEX is recursive - 08 s7).
void DS3D_OnPlayPre(CSoundObject *pso, CSoundData *psd, SLONG slFlags);
void DS3D_OnPlayPost(CSoundObject *pso);
void DS3D_OnStop(CSoundObject *pso); // detach: non-looping -> orphan play-out; looping -> force release (07 s15)
void DS3D_OnDtor(CSoundObject *pso);
void DS3D_OnClearBuffer(CSoundData *psd);
void DS3D_OnSetFormatPre(void);
void DS3D_OnSetFormatPost(void); // 08 s3 G6 - listener re-apply or forced disable

// 08 s11 step 4 - the Mute window hook body (P_Mute original first, then Stop every
// playing shadow + bGateStopped; buffers kept - 07 sG7 companion)
void DS3D_OnMute(void);

// 08 s11 step 3 - the P_MixSound skip (D1: table membership && bBypass), the spatial
// store called from P_Update3DEffects and the per-frame driver tail called from
// P_UpdateSounds (08 s4 steps 4-9 + one CommitDeferredSettings per frame)
BOOL DS3D_IsShadowBypassed(CSoundObject *pso);
void DS3D_StoreSpatial(CSoundObject *pso);
void DS3D_DriverTail(void);

// 08 s11 step 1 - declares the cvars; called from ICorePatches::SoundLibrary()
void SoundDS3D_Init(void);

// 06 s10.1/s10.3 - trampoline variables (static storage duration, AGENTS invariant 4);
// definitions in SoundLibrary.cpp, each rewritten by CreatePatch to point at the trampoline
extern void (CSoundObject::*pDS_Play_internal)(CSoundData *, SLONG);   // 08 s3
extern void (CSoundObject::*pDS_Stop_internal)(void);                  // 08 s3 (ForceRewrite(7) at registration)
extern void (CSoundObject::*pDS_dtor)(void);                           // 08 s3 (0x7E150 export, filled at registration)
extern void (CSoundLibrary::*pDS_UpdateSounds)(void);                  // 08 s3
extern CSoundLibrary::SoundFormat (CSoundLibrary::*pDS_SetFormat)(CSoundLibrary::SoundFormat, BOOL); // 08 s3
extern void (CSoundLibrary::*pDS_Mute)(void);                          // 08 s3
extern void (CSoundData::*pDS_ClearBuffer)(void);                      // 08 s3
extern void (*pDS_MixSound)(CSoundObject *);                           // 08 s3 (raw ADDR_ENGINE + 0x7AF80)

// 08 s3 P_MixSound - free-function wrapper (MixSound is a free function, SoundLibrary.h:47)
void PDS_MixSound(CSoundObject *pso);

// 08 s3 P_MixSound / AGENTS invariant 3 - module identity + prologue byte-compare;
// CreateOpcodePatch failure is fatal in release (06 s3), so the gate runs BEFORE patching
BOOL SoundDS3D_CheckMixSound(void);

#endif // _PATCHCONFIG_ENGINEPATCHES && SE1_VER == SE1_107 && defined(NDEBUG)

#endif
