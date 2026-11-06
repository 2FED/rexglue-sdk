/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/audio/audio_system.h>
#include <rex/audio/speaker_config.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

namespace {

// Resolved once: audio_speaker_config requires a restart, and the title may poll the config.
uint32_t ResolveGuestSpeakerConfig() {
  audio::SpeakerConfigMode mode = audio::SpeakerConfigMode::kAuto;
  const std::string configured = REXCVAR_GET(audio_speaker_config);
  if (!audio::ParseSpeakerConfigMode(configured, &mode)) {
    REXAPU_WARN("[audio] speaker config: unknown audio_speaker_config '{}', using auto",
                configured);
    mode = audio::SpeakerConfigMode::kAuto;
  }
  // Probe the host playback device directly so the answer does not depend on whether the title
  // registers its render client (which opens the SDL stream) before or after this call.
  int device_channels = 0;
  auto* emulator = REX_KERNEL_STATE()->emulator();
  if (auto* audio_system =
          emulator ? static_cast<audio::AudioSystem*>(emulator->audio_system()) : nullptr) {
    device_channels = audio_system->QueryHostOutputChannels();
  }
  const audio::SpeakerConfigAnswer answer = audio::ResolveSpeakerConfig(mode, device_channels);
  REXAPU_INFO("[audio] speaker config: cvar={} device_channels={} answer=0x{:08X}",
              audio::SpeakerConfigModeName(mode), device_channels, answer.value);
  if (answer.device_unknown) {
    REXAPU_WARN(
        "[audio] speaker config: host device channel count unknown; auto keeps surround "
        "(0x{:08X})",
        answer.value);
  }
  return answer.value;
}

}  // namespace

u32 XAudioGetSpeakerConfig_entry(mapped_u32 config_ptr) {
  static const uint32_t speaker_config = ResolveGuestSpeakerConfig();
  *config_ptr = speaker_config;
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolumeChangeMask_entry(mapped_void driver_ptr, mapped_u32 out_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  rex::thread::Sleep(std::chrono::microseconds(1));

  // Checking these bits to see if any voice volume changed.
  // I think.
  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolume_entry(u32 unk, mapped_f32 out_ptr) {
  // Expects a floating point single. Volume %?
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}

u32 XAudioEnableDucker_entry(u32 unk) {
  return X_ERROR_SUCCESS;
}

u32 XAudioRegisterRenderDriverClient_entry(mapped_u32 callback_ptr, mapped_u32 driver_ptr) {
  REXKRNL_DEBUG("XAudioRegisterRenderDriverClient called! callback_ptr={:08X} driver_ptr={:08X}",
                callback_ptr.guest_address(), driver_ptr.guest_address());
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  uint32_t callback_arg = callback_ptr[1];

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());

  size_t index;
  auto result = audio_system->RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    return result;
  }

  assert_true(!(index & ~0x0000FFFF));
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioUnregisterRenderDriverClient_entry(mapped_void driver_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioSubmitRenderDriverFrame_entry(mapped_void driver_ptr, mapped_void samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  static uint32_t submit_krnl_count = 0;
  if (submit_krnl_count < 10) {
    REXKRNL_DEBUG("XAudioSubmitRenderDriverFrame: driver={:08X} samples={:08X}",
                  driver_ptr.guest_address(), samples_ptr.guest_address());
    submit_krnl_count++;
  }

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples_ptr.guest_address());

  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XAudioGetSpeakerConfig, rex::kernel::xboxkrnl::XAudioGetSpeakerConfig_entry)
REX_EXPORT(__imp__XAudioGetVoiceCategoryVolumeChangeMask,
           rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolumeChangeMask_entry)
REX_EXPORT(__imp__XAudioGetVoiceCategoryVolume,
           rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolume_entry)
REX_EXPORT(__imp__XAudioEnableDucker, rex::kernel::xboxkrnl::XAudioEnableDucker_entry)
REX_EXPORT(__imp__XAudioRegisterRenderDriverClient,
           rex::kernel::xboxkrnl::XAudioRegisterRenderDriverClient_entry)
REX_EXPORT(__imp__XAudioUnregisterRenderDriverClient,
           rex::kernel::xboxkrnl::XAudioUnregisterRenderDriverClient_entry)
REX_EXPORT(__imp__XAudioSubmitRenderDriverFrame,
           rex::kernel::xboxkrnl::XAudioSubmitRenderDriverFrame_entry)

REX_EXPORT_STUB(__imp__XAudioRenderDriverInitialize);
REX_EXPORT_STUB(__imp__XAudioRenderDriverLock);
REX_EXPORT_STUB(__imp__XAudioSetVoiceCategoryVolume);
REX_EXPORT_STUB(__imp__XAudioBeginDigitalBypassMode);
REX_EXPORT_STUB(__imp__XAudioEndDigitalBypassMode);
REX_EXPORT_STUB(__imp__XAudioSubmitDigitalPacket);
REX_EXPORT_STUB(__imp__XAudioQueryDriverPerformance);
REX_EXPORT_STUB(__imp__XAudioGetRenderDriverThread);
REX_EXPORT_STUB(__imp__XAudioSetSpeakerConfig);
REX_EXPORT_STUB(__imp__XAudioOverrideSpeakerConfig);
REX_EXPORT_STUB(__imp__XAudioSuspendRenderDriverClients);
REX_EXPORT_STUB(__imp__XAudioRegisterRenderDriverMECClient);
REX_EXPORT_STUB(__imp__XAudioUnregisterRenderDriverMECClient);
REX_EXPORT_STUB(__imp__XAudioCaptureRenderDriverFrame);
REX_EXPORT_STUB(__imp__XAudioGetRenderDriverTic);
REX_EXPORT_STUB(__imp__XAudioSetDuckerLevel);
REX_EXPORT_STUB(__imp__XAudioIsDuckerEnabled);
REX_EXPORT_STUB(__imp__XAudioGetDuckerLevel);
REX_EXPORT_STUB(__imp__XAudioGetDuckerThreshold);
REX_EXPORT_STUB(__imp__XAudioSetDuckerThreshold);
REX_EXPORT_STUB(__imp__XAudioGetDuckerAttackTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerAttackTime);
REX_EXPORT_STUB(__imp__XAudioGetDuckerReleaseTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerReleaseTime);
REX_EXPORT_STUB(__imp__XAudioGetDuckerHoldTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerHoldTime);
REX_EXPORT_STUB(__imp__XAudioGetUnderrunCount);
REX_EXPORT_STUB(__imp__XAudioSetProcessFrameCallback);
