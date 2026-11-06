/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::audio::conversion {

// Guest frames are planar big-endian float, 6 channels in the default XAudio 5.1 order
// FL FR FC LFE BL BR.
// https://docs.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping
struct StereoDownmixWeights {
  float front;     // FL -> L, FR -> R
  float center;    // FC -> L and R
  float surround;  // BL -> L, BR -> R
};

// ITU-R BS.775 Lo/Ro: L = FL + 0.7071 FC + 0.7071 BL (R likewise), LFE dropped, normalised by
// 1 / (1 + 2 * 0.7071) so full-scale FL + FC + BL cannot exceed 1.0.
inline constexpr StereoDownmixWeights kItuStereoDownmix = {0.41421356f, 0.29289322f, 0.29289322f};
// Weights inherited from Xenia: (FL + BL + 0.5 FC) / 2.5, LFE dropped. Kept for A/B
// (audio_downmix_itu=false).
inline constexpr StereoDownmixWeights kLegacyStereoDownmix = {0.4f, 0.2f, 0.4f};

inline void sequential_6_BE_to_interleaved_6_LE_scalar(float* output, const float* input,
                                                       size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    for (size_t channel = 0; channel < 6; channel++) {
      output[sample * 6 + channel] = rex::byte_swap(input[channel * ch_sample_count + sample]);
    }
  }
}

inline void sequential_6_BE_to_interleaved_2_LE_scalar(float* output, const float* input,
                                                       size_t ch_sample_count,
                                                       const StereoDownmixWeights& weights) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    const float fl = rex::byte_swap(input[0 * ch_sample_count + sample]);
    const float fr = rex::byte_swap(input[1 * ch_sample_count + sample]);
    const float fc = rex::byte_swap(input[2 * ch_sample_count + sample]);
    const float bl = rex::byte_swap(input[4 * ch_sample_count + sample]);
    const float br = rex::byte_swap(input[5 * ch_sample_count + sample]);
    const float center = fc * weights.center;
    output[sample * 2] = fl * weights.front + bl * weights.surround + center;
    output[sample * 2 + 1] = fr * weights.front + br * weights.surround + center;
  }
}

#if REX_ARCH_AMD64
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  const uint32_t* in = reinterpret_cast<const uint32_t*>(input);
  uint32_t* out = reinterpret_cast<uint32_t*>(output);
  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    __m128i sample0 =
        _mm_set_epi32(in[3 * ch_sample_count + sample], in[2 * ch_sample_count + sample],
                      in[1 * ch_sample_count + sample], in[0 * ch_sample_count + sample]);
    uint32_t sample1 = in[4 * ch_sample_count + sample];
    uint32_t sample2 = in[5 * ch_sample_count + sample];
    sample0 = _mm_shuffle_epi8(sample0, byte_swap_shuffle);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[sample * 6]), sample0);
    sample1 = rex::byte_swap(sample1);
    out[sample * 6 + 4] = sample1;
    sample2 = rex::byte_swap(sample2);
    out[sample * 6 + 5] = sample2;
  }
}

inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count,
                                                const StereoDownmixWeights& weights) {
  assert_true(ch_sample_count % 4 == 0);

  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  const __m128 front_weight = _mm_set1_ps(weights.front);
  const __m128 center_weight = _mm_set1_ps(weights.center);
  const __m128 surround_weight = _mm_set1_ps(weights.surround);

  // Centre to both sides, discard low frequency.
  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    // load 4 samples from 6 channels each
    __m128 fl = _mm_loadu_ps(&input[0 * ch_sample_count + sample]);
    __m128 fr = _mm_loadu_ps(&input[1 * ch_sample_count + sample]);
    __m128 fc = _mm_loadu_ps(&input[2 * ch_sample_count + sample]);
    __m128 bl = _mm_loadu_ps(&input[4 * ch_sample_count + sample]);
    __m128 br = _mm_loadu_ps(&input[5 * ch_sample_count + sample]);
    // byte swap
    fl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fl), byte_swap_shuffle));
    fr = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fr), byte_swap_shuffle));
    fc = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fc), byte_swap_shuffle));
    bl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(bl), byte_swap_shuffle));
    br = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(br), byte_swap_shuffle));

    // Same operation order as the scalar path: front + surround + centre.
    const __m128 center = _mm_mul_ps(fc, center_weight);
    __m128 left = _mm_add_ps(_mm_mul_ps(fl, front_weight), _mm_mul_ps(bl, surround_weight));
    __m128 right = _mm_add_ps(_mm_mul_ps(fr, front_weight), _mm_mul_ps(br, surround_weight));
    left = _mm_add_ps(left, center);
    right = _mm_add_ps(right, center);
    _mm_storeu_ps(&output[sample * 2], _mm_unpacklo_ps(left, right));
    _mm_storeu_ps(&output[(sample + 2) * 2], _mm_unpackhi_ps(left, right));
  }
}
#else
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  sequential_6_BE_to_interleaved_6_LE_scalar(output, input, ch_sample_count);
}
inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count,
                                                const StereoDownmixWeights& weights) {
  sequential_6_BE_to_interleaved_2_LE_scalar(output, input, ch_sample_count, weights);
}
#endif

}  // namespace rex::audio::conversion
