/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/audio_route_audit.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/audio_config.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/frame_audit.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

REXCVAR_DEFINE_STRING(audio_device, "default", "Audio",
                      "Playback device name (default = operating-system default)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(audio_volume, 1.0, "Audio", "Host output volume (0.0 = silent, 1.0 = full)")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioDeviceID requested_device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  const std::string_view configured_device = REXCVAR_GET(audio_device);
  if (configured_device != "default") {
    int playback_device_count = 0;
    SDL_AudioDeviceID* playback_devices = SDL_GetAudioPlaybackDevices(&playback_device_count);
    if (!playback_devices) {
      REXAPU_ERROR("SDL_GetAudioPlaybackDevices() failed: {}", SDL_GetError());
      return false;
    }
    std::vector<PlaybackDeviceConfigEntry> device_entries;
    device_entries.reserve(static_cast<size_t>(playback_device_count));
    for (int i = 0; i < playback_device_count; ++i) {
      const char* device_name = SDL_GetAudioDeviceName(playback_devices[i]);
      if (device_name) {
        device_entries.push_back({playback_devices[i], device_name});
      }
    }
    requested_device =
        static_cast<SDL_AudioDeviceID>(FindPlaybackDeviceByName(configured_device, device_entries));
    SDL_free(playback_devices);
    if (!requested_device) {
      REXAPU_ERROR("Configured audio playback device is unavailable");
      return false;
    }
  }
  output_volume_ = static_cast<float>(REXCVAR_GET(audio_volume));

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(requested_device, &desired_spec, SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  if (obtained_spec.channels == 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(requested_device, &desired_spec, SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (REXCVAR_GET(sdl_audio_stability_audit)) {
    const char* physical_device_name = SDL_GetAudioDeviceName(sdl_device);
    if (!physical_device_name) {
      REXAPU_ERROR("SDL_GetAudioDeviceName() failed for audio stability monitoring: {}",
                   SDL_GetError());
      return false;
    }
    physical_device_name_ = physical_device_name;
    if (!SDL_AddEventWatch(SDLEventWatch, this)) {
      REXAPU_ERROR("SDL_AddEventWatch() failed for audio device monitoring: {}", SDL_GetError());
      return false;
    }
    sdl_event_watch_registered_ = true;
    RecordAudioStabilityDriver(true, sdl_device_channels_);
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  RecordAudioRouteSubmittedFrame(input_frame, frame_samples_);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    RecordAudioRouteQueueDepth(frames_queued_.size());
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

bool SDLAudioDriver::Pause() {
  if (!sdl_stream_ || stream_paused_) {
    return sdl_stream_ != nullptr;
  }
  const bool success = SDL_PauseAudioStreamDevice(sdl_stream_);
  if (success) {
    stream_paused_ = true;
  } else {
    REXAPU_ERROR("SDL_PauseAudioStreamDevice() failed: {}", SDL_GetError());
  }
  return success;
}

bool SDLAudioDriver::Resume() {
  if (!sdl_stream_ || !stream_paused_) {
    return sdl_stream_ != nullptr;
  }
  const bool success = SDL_ResumeAudioStreamDevice(sdl_stream_);
  if (success) {
    stream_paused_ = false;
  } else {
    REXAPU_ERROR("SDL_ResumeAudioStreamDevice() failed: {}", SDL_GetError());
  }
  return success;
}

bool SDLAudioDriver::PollDefaultDeviceChange() {
  if (!sdl_stream_ || !REXCVAR_GET(sdl_audio_stability_audit)) {
    return false;
  }
  const SDL_AudioDeviceID logical_device = SDL_GetAudioStreamDevice(sdl_stream_);
  const char* physical_device_name =
      logical_device ? SDL_GetAudioDeviceName(logical_device) : nullptr;
  if (!physical_device_name || physical_device_name_ == physical_device_name) {
    return false;
  }
  physical_device_name_ = physical_device_name;
  RecordAudioStabilityDeviceEvent(AudioDeviceEvent::kMigrated);
  return true;
}

void SDLAudioDriver::Shutdown() {
  if (sdl_event_watch_registered_) {
    SDL_RemoveEventWatch(SDLEventWatch, this);
    sdl_event_watch_registered_ = false;
  }
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
  physical_device_name_.clear();
}

bool SDLCALL SDLAudioDriver::SDLEventWatch(void* userdata, SDL_Event* event) {
  if (!userdata || !event) {
    return false;
  }
  switch (event->type) {
    case SDL_EVENT_AUDIO_DEVICE_ADDED:
      if (event->adevice.recording) {
        break;
      }
      RecordAudioStabilityDeviceEvent(AudioDeviceEvent::kAdded);
      break;
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
      if (event->adevice.recording) {
        break;
      }
      RecordAudioStabilityDeviceEvent(AudioDeviceEvent::kRemoved);
      break;
    case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
      if (event->adevice.recording) {
        break;
      }
      RecordAudioStabilityDeviceEvent(AudioDeviceEvent::kFormatChanged);
      break;
    default:
      break;
  }
  return false;
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      RecordAudioRouteStarvation();
      rex::perf::RecordAudioUnderrun();
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
        ApplyOutputVolume(data, static_cast<size_t>(sample_count), driver->output_volume_);
      }
      const bool submitted = SDL_PutAudioStreamData(stream, data, len);
      RecordAudioRouteDeviceFrame(data, sample_count, driver->sdl_device_channels_, submitted);
      if (!submitted) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
