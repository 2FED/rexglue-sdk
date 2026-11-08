/**
 * @file        rex/system/guest_thread_priority.h
 * @brief       l81 (KI-060): host priority boost for guest threads by start address.
 *
 * The l80 Deck analysis (docs/evidence/M11-l80-deck-analysis-20261108.md
 * section 1) showed that the heavy audio thread is not the SDK Audio Worker
 * (apu_worker_priority) but a guest-created thread of the title's audio
 * library. Guest priorities are ignored (ignore_thread_priorities), so
 * guest_audio_thread_priority raises the host thread of every guest thread
 * whose start address is in guest_thread_priority_boost_start.
 *
 * The default list holds the two XAudio engine thread entries of Midnight
 * Club: Los Angeles (sub_823ECC60 creates one thread per hardware thread with
 * entry 0x823EC8C8 or 0x823EC990 and raises each to base priority 15 with
 * KeSetBasePriorityThread). In another title these addresses do not match
 * any thread and nothing changes.
 *
 * Everything here is pure (unit-tested in
 * tests/unit/system/guest_thread_priority_test.cpp); the boost is applied in
 * XThread::Create (src/system/xthread.cpp).
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace rex::system {

// Parses a comma/space separated list of hexadecimal guest addresses
// ("823EC8C8, 0x823EC990"). An empty list is valid. Returns nullopt on any
// malformed entry (non-hex digits, more than 8 digits, or zero).
inline std::optional<std::vector<uint32_t>> ParseGuestThreadStartList(std::string_view text) {
  std::vector<uint32_t> out;
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == ',' || c == ' ' || c == '\t' || c == ';') {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < text.size() && text[end] != ',' && text[end] != ' ' && text[end] != '\t' &&
           text[end] != ';') {
      ++end;
    }
    std::string_view token = text.substr(i, end - i);
    i = end;
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
      token.remove_prefix(2);
    }
    if (token.empty() || token.size() > 8) {
      return std::nullopt;
    }
    uint32_t value = 0;
    for (const char d : token) {
      uint32_t digit;
      if (d >= '0' && d <= '9') {
        digit = uint32_t(d - '0');
      } else if (d >= 'a' && d <= 'f') {
        digit = uint32_t(d - 'a' + 10);
      } else if (d >= 'A' && d <= 'F') {
        digit = uint32_t(d - 'A' + 10);
      } else {
        return std::nullopt;
      }
      value = (value << 4) | digit;
    }
    if (value == 0) {
      return std::nullopt;
    }
    out.push_back(value);
  }
  return out;
}

inline bool GuestThreadStartMatches(const std::vector<uint32_t>& list, uint32_t start_address) {
  return start_address != 0 && std::find(list.begin(), list.end(), start_address) != list.end();
}

}  // namespace rex::system
