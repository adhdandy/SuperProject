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

// 08 s6 - FALSE if any hook failed its install check; step 3 registration gates on it
extern BOOL _bDS3DHooksOk;

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
