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

// Hook bodies for CSoundLibDSPatch / CSoundObjDSPatch / CSoundDataDSPatch
// (declared in SoundDS3D.h, 08 s3) live in Patches/SoundLibrary.cpp next to the
// existing patch classes (08 s1); hook installation goes into
// ICorePatches::SoundLibrary() in Core/API/EnginePatches.cpp.

#endif // _PATCHCONFIG_ENGINEPATCHES && SE1_VER == SE1_107 && defined(NDEBUG)
