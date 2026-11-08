// l80 (KI-013): MIN/MAX source blend factor emulation decision
// (d3d12_blend_minmax_fix).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/graphics/util/blend_min_max.h>
#include <rex/graphics/xenos.h>

using rex::graphics::GetMinMaxSourceScale;
using rex::graphics::GetMinMaxSourceScaleAllTargets;
using rex::graphics::MinMaxSourceScale;
using rex::graphics::PackMinMaxSourceScale;
using rex::graphics::UnpackMinMaxAlphaScale;
using rex::graphics::UnpackMinMaxColorScale;
using F = rex::graphics::xenos::BlendFactor;
using Op = rex::graphics::xenos::BlendOp;

namespace {

constexpr uint32_t BlendControl(F color_src, Op color_op, F color_dst, F alpha_src, Op alpha_op,
                                F alpha_dst) {
  return uint32_t(color_src) | (uint32_t(color_op) << 5) | (uint32_t(color_dst) << 8) |
         (uint32_t(alpha_src) << 16) | (uint32_t(alpha_op) << 21) | (uint32_t(alpha_dst) << 24);
}

// Default "no blending": ONE, ZERO, ADD for both.
constexpr uint32_t kOpaque = BlendControl(F::kOne, Op::kAdd, F::kZero, F::kOne, Op::kAdd, F::kZero);

MinMaxSourceScale Color(uint32_t blendcontrol) {
  return UnpackMinMaxColorScale(GetMinMaxSourceScale(blendcontrol));
}
MinMaxSourceScale Alpha(uint32_t blendcontrol) {
  return UnpackMinMaxAlphaScale(GetMinMaxSourceScale(blendcontrol));
}

}  // namespace

TEST_CASE("MinMax blend: pack and unpack", "[graphics][blend_min_max]") {
  for (uint32_t c = 0; c < 4; ++c) {
    for (uint32_t a = 0; a < 4; ++a) {
      uint32_t packed = PackMinMaxSourceScale(MinMaxSourceScale(c), MinMaxSourceScale(a));
      CHECK(packed < 16);
      CHECK(UnpackMinMaxColorScale(packed) == MinMaxSourceScale(c));
      CHECK(UnpackMinMaxAlphaScale(packed) == MinMaxSourceScale(a));
    }
  }
}

TEST_CASE("MinMax blend: non-min/max blending is unchanged", "[graphics][blend_min_max]") {
  CHECK(GetMinMaxSourceScale(kOpaque) == 0);
  CHECK(GetMinMaxSourceScale(0) == 0);
  // Classic alpha blending.
  CHECK(GetMinMaxSourceScale(BlendControl(F::kSrcAlpha, Op::kAdd, F::kOneMinusSrcAlpha, F::kOne,
                                          Op::kAdd, F::kZero)) == 0);
  // Additive and subtractive blending.
  CHECK(GetMinMaxSourceScale(
            BlendControl(F::kSrcAlpha, Op::kAdd, F::kOne, F::kSrcAlpha, Op::kAdd, F::kOne)) == 0);
  CHECK(GetMinMaxSourceScale(BlendControl(F::kSrcAlpha, Op::kRevSubtract, F::kOne, F::kSrcAlpha,
                                          Op::kSubtract, F::kOne)) == 0);
}

TEST_CASE("MinMax blend: the KI-013 case max(src * src.a, dst)", "[graphics][blend_min_max]") {
  uint32_t bc = BlendControl(F::kSrcAlpha, Op::kMax, F::kOne, F::kOne, Op::kAdd, F::kZero);
  CHECK(Color(bc) == MinMaxSourceScale::kSrcAlpha);
  CHECK(Alpha(bc) == MinMaxSourceScale::kNone);
  // Same with min.
  bc = BlendControl(F::kSrcAlpha, Op::kMin, F::kOne, F::kOne, Op::kAdd, F::kZero);
  CHECK(Color(bc) == MinMaxSourceScale::kSrcAlpha);
}

TEST_CASE("MinMax blend: factors that the host already implements", "[graphics][blend_min_max]") {
  // ONE / ONE is what the host does - nothing to apply.
  CHECK(GetMinMaxSourceScale(
            BlendControl(F::kOne, Op::kMax, F::kOne, F::kOne, Op::kMax, F::kOne)) == 0);
  CHECK(GetMinMaxSourceScale(
            BlendControl(F::kOne, Op::kMin, F::kOne, F::kOne, Op::kMin, F::kOne)) == 0);
}

TEST_CASE("MinMax blend: supported source factors", "[graphics][blend_min_max]") {
  CHECK(Color(BlendControl(F::kOneMinusSrcAlpha, Op::kMax, F::kOne, F::kOne, Op::kAdd, F::kZero)) ==
        MinMaxSourceScale::kInvSrcAlpha);
  CHECK(Color(BlendControl(F::kZero, Op::kMax, F::kOne, F::kOne, Op::kAdd, F::kZero)) ==
        MinMaxSourceScale::kZero);
  // Alpha, with the color modes acting as alpha modes.
  CHECK(Alpha(BlendControl(F::kOne, Op::kMax, F::kOne, F::kSrcAlpha, Op::kMax, F::kOne)) ==
        MinMaxSourceScale::kSrcAlpha);
  CHECK(Alpha(BlendControl(F::kOne, Op::kMax, F::kOne, F::kSrcColor, Op::kMax, F::kOne)) ==
        MinMaxSourceScale::kSrcAlpha);
  CHECK(Alpha(BlendControl(F::kOne, Op::kMax, F::kOne, F::kOneMinusSrcColor, Op::kMin, F::kOne)) ==
        MinMaxSourceScale::kInvSrcAlpha);
  CHECK(Alpha(BlendControl(F::kOne, Op::kMax, F::kOne, F::kOneMinusSrcAlpha, Op::kMin, F::kOne)) ==
        MinMaxSourceScale::kInvSrcAlpha);
  CHECK(Alpha(BlendControl(F::kOne, Op::kMax, F::kOne, F::kZero, Op::kMax, F::kOne)) ==
        MinMaxSourceScale::kZero);
  // Both at once.
  uint32_t bc = BlendControl(F::kSrcAlpha, Op::kMax, F::kOne, F::kSrcAlpha, Op::kMax, F::kOne);
  CHECK(Color(bc) == MinMaxSourceScale::kSrcAlpha);
  CHECK(Alpha(bc) == MinMaxSourceScale::kSrcAlpha);
}

TEST_CASE("MinMax blend: unsupported cases keep the old behavior", "[graphics][blend_min_max]") {
  // Destination factor other than ONE can't be done on the host.
  CHECK(GetMinMaxSourceScale(BlendControl(F::kSrcAlpha, Op::kMax, F::kOneMinusSrcAlpha, F::kOne,
                                          Op::kAdd, F::kZero)) == 0);
  CHECK(GetMinMaxSourceScale(
            BlendControl(F::kSrcAlpha, Op::kMax, F::kZero, F::kSrcAlpha, Op::kMax, F::kZero)) == 0);
  // Per-component, destination-dependent, constant and saturate factors.
  for (F f : {F::kSrcColor, F::kOneMinusSrcColor, F::kDstColor, F::kOneMinusDstColor, F::kDstAlpha,
              F::kOneMinusDstAlpha, F::kConstantColor, F::kOneMinusConstantColor, F::kConstantAlpha,
              F::kOneMinusConstantAlpha, F::kSrcAlphaSaturate}) {
    CHECK(Color(BlendControl(f, Op::kMax, F::kOne, F::kOne, Op::kAdd, F::kZero)) ==
          MinMaxSourceScale::kNone);
  }
  for (F f : {F::kDstColor, F::kOneMinusDstColor, F::kDstAlpha, F::kOneMinusDstAlpha,
              F::kConstantColor, F::kOneMinusConstantColor, F::kConstantAlpha,
              F::kOneMinusConstantAlpha, F::kSrcAlphaSaturate}) {
    CHECK(Alpha(BlendControl(F::kOne, Op::kMax, F::kOne, f, Op::kMax, F::kOne)) ==
          MinMaxSourceScale::kNone);
  }
}

TEST_CASE("MinMax blend: alpha scale doesn't break color factors reading alpha",
          "[graphics][blend_min_max]") {
  // Color uses classic alpha blending with the source alpha - scaling the
  // alpha in the shader would change the color result, so skip it.
  CHECK(Alpha(BlendControl(F::kSrcAlpha, Op::kAdd, F::kOneMinusSrcAlpha, F::kSrcAlpha, Op::kMax,
                           F::kOne)) == MinMaxSourceScale::kNone);
  CHECK(Alpha(BlendControl(F::kOne, Op::kAdd, F::kOneMinusSrcAlpha, F::kSrcAlpha, Op::kMax,
                           F::kOne)) == MinMaxSourceScale::kNone);
  CHECK(Alpha(BlendControl(F::kSrcAlphaSaturate, Op::kAdd, F::kOne, F::kSrcAlpha, Op::kMax,
                           F::kOne)) == MinMaxSourceScale::kNone);
  // Color factors not reading the source alpha - fine.
  CHECK(Alpha(BlendControl(F::kOne, Op::kAdd, F::kOne, F::kSrcAlpha, Op::kMax, F::kOne)) ==
        MinMaxSourceScale::kSrcAlpha);
  CHECK(Alpha(BlendControl(F::kDstAlpha, Op::kAdd, F::kZero, F::kSrcAlpha, Op::kMax, F::kOne)) ==
        MinMaxSourceScale::kSrcAlpha);
  // Color min/max ignores its factors on the host, and its own scale uses the
  // unmodified alpha.
  CHECK(Alpha(BlendControl(F::kSrcAlpha, Op::kMin, F::kOne, F::kSrcAlpha, Op::kMax, F::kOne)) ==
        MinMaxSourceScale::kSrcAlpha);
}

TEST_CASE("MinMax blend: all render targets and the color mask", "[graphics][blend_min_max]") {
  const uint32_t ki013 = BlendControl(F::kSrcAlpha, Op::kMax, F::kOne, F::kOne, Op::kAdd, F::kZero);
  const uint32_t both =
      BlendControl(F::kOneMinusSrcAlpha, Op::kMin, F::kOne, F::kZero, Op::kMax, F::kOne);
  const uint32_t bc[4] = {ki013, kOpaque, both, ki013};
  // Only RT 0 and 2 written.
  uint32_t r = GetMinMaxSourceScaleAllTargets(bc, 0x0F0F);
  CHECK((r & 0xF) == PackMinMaxSourceScale(MinMaxSourceScale::kSrcAlpha, MinMaxSourceScale::kNone));
  CHECK(((r >> 4) & 0xF) == 0);
  CHECK(((r >> 8) & 0xF) ==
        PackMinMaxSourceScale(MinMaxSourceScale::kInvSrcAlpha, MinMaxSourceScale::kZero));
  CHECK(((r >> 12) & 0xF) == 0);
  // All written, a partial mask counts as written.
  r = GetMinMaxSourceScaleAllTargets(bc, 0x8F1F);
  CHECK(((r >> 12) & 0xF) ==
        PackMinMaxSourceScale(MinMaxSourceScale::kSrcAlpha, MinMaxSourceScale::kNone));
  CHECK(r < (uint32_t(1) << 16));
  // Nothing written.
  CHECK(GetMinMaxSourceScaleAllTargets(bc, 0) == 0);
}

TEST_CASE("MinMax blend: padding bits are ignored", "[graphics][blend_min_max]") {
  const uint32_t bc = BlendControl(F::kSrcAlpha, Op::kMax, F::kOne, F::kOne, Op::kAdd, F::kZero);
  CHECK(GetMinMaxSourceScale(bc | (uint32_t(0x7) << 13) | (uint32_t(0x7) << 29)) ==
        GetMinMaxSourceScale(bc));
}
