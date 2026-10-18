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

#include "SoundLibrary.h"
#include "SoundDS3D.h"

#if _PATCHCONFIG_ENGINEPATCHES

void CSoundLibPatch::P_Listen(CSoundListener &sl)
{
  // Ignore sound listener
  if (_EnginePatches.GetNoListening()) return;

  // Original function code
  if (sl.sli_lnInActiveListeners.IsLinked()) {
    sl.sli_lnInActiveListeners.Remove();
  }

  sl_lhActiveListeners.AddTail(sl.sli_lnInActiveListeners);
};

void CSoundObjPatch::P_Update3DEffects(void)
{
  // Not a 3D sound
  if (!(so_slFlags & SOF_3D)) return;

  static CSymbolPtr pfDopplerSoundSpeed("snd_fDopplerSoundSpeed");
  static CSymbolPtr pfEarsDistance("snd_fEarsDistance");
  static CSymbolPtr pfDelaySoundSpeed("snd_fDelaySoundSpeed");
  static CSymbolPtr pfPanStrength("snd_fPanStrength");
  static CSymbolPtr pfLRFilter("snd_fLRFilter");
  static CSymbolPtr pfBFilter("snd_fBFilter");
  static CSymbolPtr pfUFilter("snd_fUFilter");
  static CSymbolPtr pfDFilter("snd_fDFilter");

  // Total parameters (accounting for all listeners)
  FLOAT fTLVolume = 0.0f;
  FLOAT fTRVolume = 0.0f;
  FLOAT fTLFilter = UpperLimit(0.0f);
  FLOAT fTRFilter = UpperLimit(0.0f);
  FLOAT fTLDelay  = UpperLimit(0.0f);
  FLOAT fTRDelay  = UpperLimit(0.0f);
  FLOAT fTPitchShift = 0;

  // Get sound position parameters
  FLOAT3D vPosition(0.0f, 0.0f, 0.0f);
  FLOAT3D vSpeed(0.0f, 0.0f, 0.0f);

  if (so_penEntity != NULL) {
    vPosition = so_penEntity->en_plPlacement.pl_PositionVector;

    if (so_penEntity->en_ulPhysicsFlags & EPF_MOVABLE) {
      CMovableEntity *penMovable = (CMovableEntity *)so_penEntity;
      vSpeed = penMovable->en_vCurrentTranslationAbsolute;
    }
  }

  // Go through listeners
  INDEX ctEffectiveListeners = 0;

  FOREACHINLIST(CSoundListener, sli_lnInActiveListeners, _pSound->sl_lhActiveListeners, itsli) {
    CSoundListener &sli = *itsli;

    // [Cecil] Fixed local sounds not being played for predicted listeners
    if (so_slFlags & SOF_LOCAL) {
      // Skip if no listener
      if (sli.sli_penEntity == NULL) continue;

      // Skip if listener doesn't match
      if (so_penEntity != sli.sli_penEntity->GetPredictionTail()) continue;
    }

    // Calculated parameters for this listener
    FLOAT fLVolume, fRVolume, fLFilter, fRFilter, fLDelay, fRDelay, fPitchShift;

    // Calculate distance from the listener
    FLOAT3D vAbsDelta = vPosition - sli.sli_vPosition;
    FLOAT fAbsDelta = vAbsDelta.Length();

    // Too far away
    if (fAbsDelta > so_sp3.sp3_fFalloff) continue;

    // Calculate distance for fall-off factor
    FLOAT fDistanceFactor = 1.0f;

    if (fAbsDelta > so_sp3.sp3_fHotSpot) {
      fDistanceFactor = (so_sp3.sp3_fFalloff - fAbsDelta)
                      / (so_sp3.sp3_fFalloff - so_sp3.sp3_fHotSpot);
    }

    ASSERT(fDistanceFactor >= 0.0f && fDistanceFactor <= 1.0f);

    // Calculate volumetric influence
    // NOTE: decoded sounds must be treated as volumetric
    FLOAT fNonVolumetric = 1.0f;
    FLOAT fNonVolumetricAdvanced = 1.0f;

    if ((so_slFlags & SOF_VOLUMETRIC) || so_psdcDecoder != NULL) {
      fNonVolumetric = 1.0f - fDistanceFactor;
      fNonVolumetricAdvanced = 0.0f;
    }

    ASSERT(fNonVolumetric >= 0.0f && fNonVolumetric <= 1.0f);

    // Find doppler effect pitch shift
    fPitchShift = 1.0f;

    if (fAbsDelta > 0.001f) {
      FLOAT3D vObjectDirection = vAbsDelta / fAbsDelta;
      FLOAT fObjectSpeed = vSpeed % vObjectDirection; // Negative towards listener
      FLOAT fListenerSpeed = sli.sli_vSpeed % vObjectDirection; // Positive towards object

      fPitchShift = (pfDopplerSoundSpeed.GetFloat() + fListenerSpeed * fNonVolumetricAdvanced)
                  / (pfDopplerSoundSpeed.GetFloat() + fObjectSpeed * fNonVolumetricAdvanced);
    }

    // Find position of sound relative to viewer orientation
    FLOAT3D vRelative = vAbsDelta * !sli.sli_mRotation;

    // Find distances from left and right ear
    FLOAT fLDistance = (FLOAT3D(-pfEarsDistance.GetFloat() * fNonVolumetricAdvanced * 0.5f, 0.0f, 0.0f) - vRelative).Length();
    FLOAT fRDistance = (FLOAT3D(+pfEarsDistance.GetFloat() * fNonVolumetricAdvanced * 0.5f, 0.0f, 0.0f) - vRelative).Length();

    // Calculate sound delay to each ear
    fLDelay = fLDistance / pfDelaySoundSpeed.GetFloat();
    fRDelay = fRDistance / pfDelaySoundSpeed.GetFloat();

    // Calculate relative sound directions
    FLOAT fLRFactor = 0.0f; // Positive right
    FLOAT fFBFactor = 0.0f; // Positive front
    FLOAT fUDFactor = 0.0f; // Positive up

    if (fAbsDelta > 0.001f) {
      FLOAT3D vDir = vRelative / fAbsDelta;
      fLRFactor = +vDir(1);
      fFBFactor = -vDir(3);
      fUDFactor = +vDir(2);
    }

    ASSERT(fLRFactor >= -1.1f && fLRFactor <= +1.1f);
    ASSERT(fFBFactor >= -1.1f && fFBFactor <= +1.1f);
    ASSERT(fUDFactor >= -1.1f && fUDFactor <= +1.1f);

    // Calculate panning influence factor
    FLOAT fPanningFactor = fNonVolumetric * pfPanStrength.GetFloat();
    ASSERT(fPanningFactor >= 0.0f && fPanningFactor <= 1.0f);

    // Calculate volume for left and right channels
    FLOAT fVolume = so_sp3.sp3_fMaxVolume * fDistanceFactor;

    if (fLRFactor > 0.0f) {
      fLVolume = (1.0f - fLRFactor * fPanningFactor) * fVolume;
      fRVolume = fVolume;

    } else {
      fLVolume = fVolume;
      fRVolume = (1.0f + fLRFactor * fPanningFactor) * fVolume;
    }

    // Calculate filters
    FLOAT fListenerFilter = sli.sli_fFilter;

    if (so_slFlags & SOF_NOFILTER) {
      fListenerFilter = 0.0f;
    }

    fLFilter = fRFilter = fListenerFilter + 1.0f;

    if (fLRFactor > 0.0f) {
      fLFilter += fLRFactor * pfLRFilter.GetFloat() * fNonVolumetricAdvanced;

    } else {
      fRFilter -= fLRFactor * pfLRFilter.GetFloat() * fNonVolumetricAdvanced;
    }

    if (fFBFactor < 0.0f) {
      fLFilter -= pfBFilter.GetFloat() * fFBFactor * fNonVolumetricAdvanced;
      fRFilter -= pfBFilter.GetFloat() * fFBFactor * fNonVolumetricAdvanced;
    }

    if (fUDFactor > 0.0f) {
      fLFilter += pfUFilter.GetFloat() * fUDFactor * fNonVolumetricAdvanced;
      fRFilter += pfUFilter.GetFloat() * fUDFactor * fNonVolumetricAdvanced;

    } else {
      fLFilter -= pfDFilter.GetFloat() * fUDFactor * fNonVolumetricAdvanced;
      fRFilter -= pfDFilter.GetFloat() * fUDFactor * fNonVolumetricAdvanced;
    }

    // Adjust calculated volume to the one of the listener
    fLVolume *= sli.sli_fVolume;
    fRVolume *= sli.sli_fVolume;

    // Update parameters
    fTLVolume = Max(fTLVolume, fLVolume);
    fTRVolume = Max(fTRVolume, fRVolume);
    fTLDelay  = Min(fTLDelay, fLDelay);
    fTRDelay  = Min(fTRDelay, fRDelay);
    fTLFilter = Min(fTLFilter, fLFilter);
    fTRFilter = Min(fTRFilter, fRFilter);
    fTPitchShift += fPitchShift;

    ctEffectiveListeners++;
  }

  fTPitchShift /= ctEffectiveListeners;

  // Calculate 2D parameters
  FLOAT fPitchShift = fTPitchShift * so_sp3.sp3_fPitch;
  FLOAT fPhaseShift = fTLDelay - fTRDelay;
  FLOAT fDelay = Min(fTRDelay, fTLDelay);

  // Set sound parameters
  fTLVolume = Clamp(fTLVolume, SL_VOLUME_MIN, SL_VOLUME_MAX);
  fTRVolume = Clamp(fTRVolume, SL_VOLUME_MIN, SL_VOLUME_MAX);
  SetVolume(fTLVolume, fTRVolume);

  if (fTLVolume > 0.0f || fTRVolume > 0.0f) {
    // Do safety clamping
    fTLFilter   = ClampDn(fTLFilter, 1.0f);
    fTRFilter   = ClampDn(fTRFilter, 1.0f);
    fDelay      = ClampDn(fDelay, 0.0f);
    fPitchShift = ClampDn(fPitchShift, 0.001f);
    fPhaseShift = Clamp(fPhaseShift, -1.0f, +1.0f);

    SetFilter(fTLFilter, fTRFilter);
    SetDelay(fDelay);
    SetPitch(fPitchShift);
    SetPhase(fPhaseShift);
  }

#if SE1_VER == SE1_107 && defined(NDEBUG)
  // 08 s3/08 s4 - store-only spatial snapshot for shadowed sounds (no COM calls;
  // the P_UpdateSounds tail runs the driver). Called under the caller's sl_csSound.
  DS3D_StoreSpatial(this);
#endif
};

// 08 s3 - DirectSound3D wrapper hooks (Steam TSE 1.07 release only, 08 s1)
#if SE1_VER == SE1_107 && defined(NDEBUG)

// 06 s10.1/s10.3 - trampoline variables (static storage duration, AGENTS invariant 4;
// static-typed PMFs hold the concrete address on MSVC x86 - 06 s10.3). CreatePatch()
// in ICorePatches::SoundLibrary() (EnginePatches.cpp) rewrites each variable to hold
// the trampoline; the dtor and MixSound addresses are filled at registration time.
void (CSoundObject::*pDS_Play_internal)(CSoundData *, SLONG) = &CSoundObject::Play_internal;
void (CSoundObject::*pDS_Stop_internal)(void) = &CSoundObject::Stop_internal;
void (CSoundObject::*pDS_dtor)(void) = NULL;                               // filled with the 0x7E150 export at registration
void (CSoundLibrary::*pDS_UpdateSounds)(void) = &CSoundLibrary::UpdateSounds;
CSoundLibrary::SoundFormat (CSoundLibrary::*pDS_SetFormat)(CSoundLibrary::SoundFormat, BOOL) = &CSoundLibrary::SetFormat;
void (CSoundLibrary::*pDS_Mute)(void) = &CSoundLibrary::Mute;
void (CSoundData::*pDS_ClearBuffer)(void) = &CSoundData::ClearBuffer;
void (*pDS_MixSound)(CSoundObject *) = NULL;                               // filled with ADDR_ENGINE + 0x7AF80 at registration

// 08 s3 P_MixSound (free function, SoundLibrary.h:47) - step 3: shadowed entries
// (table membership && bBypass) are skipped here (D1); everything else mixed by the
// stock mixer. Timer thread: one table lookup + a precomputed flag read, no cvars (08 s7)
void PDS_MixSound(CSoundObject *pso) {
  if (DS3D_IsShadowBypassed(pso)) return;

  pDS_MixSound(pso);
};

// 08 s3 P_Play_internal - registration happens BEFORE the original (Play holds
// sl_csSound, the first mix block can never see an unregistered playing sound - 07 s7.4);
// the inlined Stop_internal inside the original never passes through P_Stop_internal
// (05 s1.3), so the re-play stale-buffer release runs after it (08 s3 step 3)
void CSoundObjDSPatch::P_Play_internal(CSoundData *pCsdLink, SLONG slFlags) {
  DS3D_OnPlayPre(this, pCsdLink, slFlags);

  (this->*pDS_Play_internal)(pCsdLink, slFlags);

  DS3D_OnPlayPost(this);
};

// 08 s3 P_Stop_internal - explicit stops only; force release (orphan play-out in step 5)
void CSoundObjDSPatch::P_Stop_internal(void) {
  DS3D_OnStop(this);
  (this->*pDS_Stop_internal)();
};

// 08 s3/07 s12.1 P_dtor - release the shadow BEFORE the original (its Stop_internal
// is inlined, so a destroyed shadowed object would otherwise dangle in the table)
void CSoundObjDSPatch::P_dtor(void) {
  DS3D_OnDtor(this);
  (this->*pDS_dtor)();
};

// 08 s3 P_UpdateSounds - original first (its loop calls P_Update3DEffects per object,
// filling the shadow store); then the s4 driver tail under its own sl_csSound (the
// original released the lock on return - SoundLibrary.cpp:1001)
void CSoundLibDSPatch::P_UpdateSounds(void) {
  (this->*pDS_UpdateSounds)();

  DS3D_DriverTail();
};

// 08 s3 P_SetFormat - pre: free templates + release ALL shadows via ReleaseForRecommit
// (entries kept); post: re-apply the beta listener params on the fresh device (the next
// P_UpdateSounds tail recommits at the synced offsets - RECREATE, 07 s13 item 15) or,
// when the listener is gone, DropToSoftware all + forced mode disable (08 s11 step 4)
CSoundLibrary::SoundFormat CSoundLibDSPatch::P_SetFormat(CSoundLibrary::SoundFormat EsfNew, BOOL bReload) {
  DS3D_OnSetFormatPre();
  CSoundLibrary::SoundFormat EsfRet = (this->*pDS_SetFormat)(EsfNew, bReload);
  DS3D_OnSetFormatPost();
  return EsfRet;
};

// 08 s3 P_Mute - original (zeroes mix buffers, sets _bMuted); then Stop every playing
// shadow + bGateStopped, buffers kept (07 sG7 companion - resume from the DS position)
void CSoundLibDSPatch::P_Mute(void) {
  (this->*pDS_Mute)();
  DS3D_OnMute();
};

// 08 s3 P_ClearBuffer - invalidate the cached template (07 sG1 single invalidation point)
void CSoundDataDSPatch::P_ClearBuffer(void) {
  (this->*pDS_ClearBuffer)();

  DS3D_OnClearBuffer(this);
};

#endif // SE1_VER == SE1_107 && defined(NDEBUG)

#endif // _PATCHCONFIG_ENGINEPATCHES
