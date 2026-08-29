#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>

// Input/HID configuration flags
REXCVAR_DECLARE(bool, guide_button);
REXCVAR_DECLARE(std::string, hid_mappings_file);
REXCVAR_DECLARE(std::string, input_backend);
REXCVAR_DECLARE(int32_t, wheel_steering_axis);
REXCVAR_DECLARE(int32_t, wheel_brake_axis);
REXCVAR_DECLARE(int32_t, wheel_accelerator_axis);
REXCVAR_DECLARE(bool, wheel_steering_inverted);
REXCVAR_DECLARE(bool, wheel_brake_inverted);
REXCVAR_DECLARE(bool, wheel_accelerator_inverted);
REXCVAR_DECLARE(bool, wheel_force_feedback);
REXCVAR_DECLARE(int32_t, wheel_force_feedback_gain);
REXCVAR_DECLARE(int32_t, wheel_button_left_shoulder);
REXCVAR_DECLARE(int32_t, wheel_button_right_shoulder);
REXCVAR_DECLARE(int32_t, wheel_button_y);
REXCVAR_DECLARE(int32_t, wheel_button_x);
REXCVAR_DECLARE(int32_t, wheel_button_b);
REXCVAR_DECLARE(int32_t, wheel_button_a);
REXCVAR_DECLARE(int32_t, wheel_button_back);
REXCVAR_DECLARE(int32_t, wheel_button_start);
REXCVAR_DECLARE(int32_t, wheel_button_left_thumb);
REXCVAR_DECLARE(int32_t, wheel_button_right_thumb);
REXCVAR_DECLARE(int32_t, wheel_button_guide);
REXCVAR_DECLARE(int32_t, wheel_button_dpad_up);
REXCVAR_DECLARE(int32_t, wheel_button_dpad_down);
REXCVAR_DECLARE(int32_t, wheel_button_dpad_left);
REXCVAR_DECLARE(int32_t, wheel_button_dpad_right);
