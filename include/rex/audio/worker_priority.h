/**
 * @file        rex/audio/worker_priority.h
 * @brief       l80 (KI-060): host priority of the APU worker threads.
 *
 * apu_worker_priority (rex/system/short_sleep.h: ApuWorkerPriority) applies
 * to the Audio Worker (runs the guest audio client callbacks) and the XMA
 * Decoder host threads. "normal" leaves the thread untouched.
 */

#pragma once

#include <string_view>

namespace rex::thread {
class Thread;
}  // namespace rex::thread

namespace rex::audio {

void ApplyApuWorkerPriority(rex::thread::Thread* thread, std::string_view thread_name);

}  // namespace rex::audio
