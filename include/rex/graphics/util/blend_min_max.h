/**
 * @file        rex/graphics/util/blend_min_max.h
 * @brief       l80 (KI-013): source blend factor emulation for MIN/MAX blending.
 *
 * Direct3D 12 (like Direct3D 9 on the PC, OpenGL and Vulkan) ignores both
 * blend factors when the blend operation is MIN or MAX:
 *   result = min/max(src, dst).
 * The Xbox 360 GPU is reported to apply them (the GL state tracker in Mesa
 * also forces the factors to ONE for MIN/MAX for the same family of ATI
 * hardware), so a title that draws with, for example,
 *   RB_BLENDCONTROL: color = MAX, src = SRC_ALPHA, dst = ONE
 * gets max(src * src.a, dst) on the console, but max(src, dst) on the host -
 * every low-alpha highlight is drawn at full strength.
 *
 * With host render targets, the source factor can be applied in the pixel
 * shader (after alpha test, alpha to coverage and the exponent bias) when it
 * depends only on the source and the destination factor is ONE (which the host
 * min/max already implements). This header holds the pure decision: given an
 * RB_BLENDCONTROL value, which scale the translated pixel shader has to apply
 * to the color (RGB) and to the alpha output of one render target. Cases that
 * can't be emulated exactly (destination factor other than ONE, factors
 * depending on the destination or the blend constant, SRC_COLOR for RGB) are
 * left unchanged (MinMaxSourceScale::kNone), keeping the old behavior.
 *
 * Gated by the d3d12_blend_minmax_fix cvar (default on since l81, after the
 * owner PASS on l80).
 * Own design from the problem description (no third-party code). Header-only,
 * unit-tested (tests/unit/graphics/blend_min_max_test.cpp).
 */

#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Scale applied to the pixel shader output component(s) before the host
// MIN/MAX blending. Two bits.
enum class MinMaxSourceScale : uint32_t {
  kNone = 0,
  // Multiply by the source alpha.
  kSrcAlpha = 1,
  // Multiply by (1 - source alpha).
  kInvSrcAlpha = 2,
  // Multiply by zero.
  kZero = 3,
};

constexpr uint32_t kMinMaxSourceScaleBitsPerRenderTarget = 4;

// Per render target: bits 0:1 - RGB scale, bits 2:3 - alpha scale.
constexpr uint32_t PackMinMaxSourceScale(MinMaxSourceScale color, MinMaxSourceScale alpha) {
  return uint32_t(color) | (uint32_t(alpha) << 2);
}
constexpr MinMaxSourceScale UnpackMinMaxColorScale(uint32_t rt_bits) {
  return MinMaxSourceScale(rt_bits & 0b11);
}
constexpr MinMaxSourceScale UnpackMinMaxAlphaScale(uint32_t rt_bits) {
  return MinMaxSourceScale((rt_bits >> 2) & 0b11);
}

namespace blend_min_max_detail {

constexpr bool IsMinMax(xenos::BlendOp op) {
  return op == xenos::BlendOp::kMin || op == xenos::BlendOp::kMax;
}

// Scale for the RGB source factor; returns false if the factor
// can't be applied in the shader as a scalar multiplication.
constexpr bool ColorFactorToScale(xenos::BlendFactor factor, MinMaxSourceScale& scale_out) {
  switch (factor) {
    case xenos::BlendFactor::kOne:
      scale_out = MinMaxSourceScale::kNone;
      return true;
    case xenos::BlendFactor::kZero:
      scale_out = MinMaxSourceScale::kZero;
      return true;
    case xenos::BlendFactor::kSrcAlpha:
      scale_out = MinMaxSourceScale::kSrcAlpha;
      return true;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      scale_out = MinMaxSourceScale::kInvSrcAlpha;
      return true;
    default:
      // SRC_COLOR is per-component, the destination and constant factors
      // aren't available in the shader, SRC_ALPHA_SATURATE needs the
      // destination alpha.
      return false;
  }
}

// Same for the alpha source factor - color modes act as alpha modes for
// alpha, like in the pipeline state description.
constexpr bool AlphaFactorToScale(xenos::BlendFactor factor, MinMaxSourceScale& scale_out) {
  switch (factor) {
    case xenos::BlendFactor::kOne:
      scale_out = MinMaxSourceScale::kNone;
      return true;
    case xenos::BlendFactor::kZero:
      scale_out = MinMaxSourceScale::kZero;
      return true;
    case xenos::BlendFactor::kSrcColor:
    case xenos::BlendFactor::kSrcAlpha:
      scale_out = MinMaxSourceScale::kSrcAlpha;
      return true;
    case xenos::BlendFactor::kOneMinusSrcColor:
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      scale_out = MinMaxSourceScale::kInvSrcAlpha;
      return true;
    default:
      return false;
  }
}

// Whether the RGB blending (when not MIN/MAX, thus with factors) reads the
// source alpha, which the alpha scaling would change.
constexpr bool ColorFactorReadsSourceAlpha(xenos::BlendFactor factor) {
  return factor == xenos::BlendFactor::kSrcAlpha ||
         factor == xenos::BlendFactor::kOneMinusSrcAlpha ||
         factor == xenos::BlendFactor::kSrcAlphaSaturate;
}

}  // namespace blend_min_max_detail

// Returns the packed per-render-target scale bits (PackMinMaxSourceScale) for
// an RB_BLENDCONTROL value. The RGB scale is computed with the unmodified
// source alpha, the alpha scale is applied after it.
constexpr uint32_t GetMinMaxSourceScale(uint32_t rb_blendcontrol) {
  using namespace blend_min_max_detail;
  const auto color_src = xenos::BlendFactor(rb_blendcontrol & 0x1F);
  const auto color_op = xenos::BlendOp((rb_blendcontrol >> 5) & 0x7);
  const auto color_dst = xenos::BlendFactor((rb_blendcontrol >> 8) & 0x1F);
  const auto alpha_src = xenos::BlendFactor((rb_blendcontrol >> 16) & 0x1F);
  const auto alpha_op = xenos::BlendOp((rb_blendcontrol >> 21) & 0x7);
  const auto alpha_dst = xenos::BlendFactor((rb_blendcontrol >> 24) & 0x1F);

  MinMaxSourceScale color_scale = MinMaxSourceScale::kNone;
  if (IsMinMax(color_op) && color_dst == xenos::BlendFactor::kOne) {
    MinMaxSourceScale scale = MinMaxSourceScale::kNone;
    if (ColorFactorToScale(color_src, scale)) {
      color_scale = scale;
    }
  }

  MinMaxSourceScale alpha_scale = MinMaxSourceScale::kNone;
  if (IsMinMax(alpha_op) && alpha_dst == xenos::BlendFactor::kOne &&
      (IsMinMax(color_op) ||
       (!ColorFactorReadsSourceAlpha(color_src) && !ColorFactorReadsSourceAlpha(color_dst)))) {
    MinMaxSourceScale scale = MinMaxSourceScale::kNone;
    if (AlphaFactorToScale(alpha_src, scale)) {
      alpha_scale = scale;
    }
  }

  return PackMinMaxSourceScale(color_scale, alpha_scale);
}

// Packs the scales of all four render targets, with the render targets that
// don't write any component (normalized color mask, 4 bits per target) left
// at zero so they don't create redundant shader modifications.
inline uint32_t GetMinMaxSourceScaleAllTargets(const uint32_t rb_blendcontrol[4],
                                               uint32_t normalized_color_mask) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((normalized_color_mask >> (i * 4)) & 0xF)) {
      continue;
    }
    result |= GetMinMaxSourceScale(rb_blendcontrol[i])
              << (i * kMinMaxSourceScaleBitsPerRenderTarget);
  }
  return result;
}

}  // namespace rex::graphics
