#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xam/xmp_audit.h>

using rex::kernel::xam::IsXmpKnownMessage;
using rex::kernel::xam::IsXmpPlaybackMessage;
using rex::kernel::xam::IsXmpQueryMessage;
using rex::kernel::xam::IsXmpStateConsistent;
using rex::kernel::xam::XmpFallbackState;

TEST_CASE("XMP fallback recognizes the MCLA message surface", "[kernel][xmp]") {
  CHECK(IsXmpKnownMessage(0x00070009));
  CHECK(IsXmpKnownMessage(0x00070025));
  CHECK(IsXmpKnownMessage(0x0007003B));
  CHECK(IsXmpKnownMessage(0x0007003D));
}

TEST_CASE("XMP fallback classifies playback and query messages", "[kernel][xmp]") {
  CHECK(IsXmpPlaybackMessage(0x00070002));
  CHECK(IsXmpPlaybackMessage(0x00070006));
  CHECK(IsXmpPlaybackMessage(0x0007003D));
  CHECK_FALSE(IsXmpPlaybackMessage(0x00070009));
  CHECK(IsXmpQueryMessage(0x00070009));
  CHECK(IsXmpQueryMessage(0x00070025));
  CHECK(IsXmpQueryMessage(0x0007003B));
  CHECK_FALSE(IsXmpQueryMessage(0x0007000C));
}

TEST_CASE("XMP metadata-only fallback remains idle", "[kernel][xmp]") {
  CHECK(IsXmpStateConsistent(XmpFallbackState{}));
  XmpFallbackState title_idle{0, 1, 1, 0, false};
  CHECK(IsXmpStateConsistent(title_idle));
  title_idle.state = 1;
  CHECK_FALSE(IsXmpStateConsistent(title_idle));
  title_idle.state = 2;
  CHECK_FALSE(IsXmpStateConsistent(title_idle));
}

TEST_CASE("XMP decoder-backed active state requires songs", "[kernel][xmp]") {
  XmpFallbackState playing{1, 1, 1, 1, true};
  CHECK(IsXmpStateConsistent(playing));
  playing.active_song_count = 0;
  CHECK_FALSE(IsXmpStateConsistent(playing));
  playing.active_song_count = 1;
  playing.playback_client = 2;
  CHECK_FALSE(IsXmpStateConsistent(playing));
  playing.playback_client = 1;
  playing.state = 3;
  CHECK_FALSE(IsXmpStateConsistent(playing));
}
