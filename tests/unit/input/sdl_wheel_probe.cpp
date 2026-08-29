#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct HapticFeature {
  Uint32 bit;
  const char* name;
};

constexpr std::array<HapticFeature, 16> kHapticFeatures = {{
    {SDL_HAPTIC_CONSTANT, "constant"},
    {SDL_HAPTIC_SINE, "sine"},
    {SDL_HAPTIC_SQUARE, "square"},
    {SDL_HAPTIC_TRIANGLE, "triangle"},
    {SDL_HAPTIC_SAWTOOTHUP, "sawtooth-up"},
    {SDL_HAPTIC_SAWTOOTHDOWN, "sawtooth-down"},
    {SDL_HAPTIC_RAMP, "ramp"},
    {SDL_HAPTIC_SPRING, "spring"},
    {SDL_HAPTIC_DAMPER, "damper"},
    {SDL_HAPTIC_INERTIA, "inertia"},
    {SDL_HAPTIC_FRICTION, "friction"},
    {SDL_HAPTIC_LEFTRIGHT, "left-right"},
    {SDL_HAPTIC_CUSTOM, "custom"},
    {SDL_HAPTIC_GAIN, "gain"},
    {SDL_HAPTIC_AUTOCENTER, "autocenter"},
    {SDL_HAPTIC_STATUS, "status"},
}};

const char* Safe(const char* value) {
  return value ? value : "";
}

bool IsWheel(SDL_JoystickID id) {
  const uint16_t vendor_id = SDL_GetJoystickVendorForID(id);
  const uint16_t product_id = SDL_GetJoystickProductForID(id);
  const SDL_JoystickType type = SDL_GetJoystickTypeForID(id);
  return type == SDL_JOYSTICK_TYPE_WHEEL ||
         (vendor_id == 0x044F &&
          (product_id == 0xB66D || product_id == 0xB66E || product_id == 0xB66F));
}

void PrintHapticFeatures(Uint32 features) {
  std::printf("SDL_WHEEL_PROBE_HAPTIC_FEATURES mask=%08X names=", features);
  bool first = true;
  for (const auto& feature : kHapticFeatures) {
    if ((features & feature.bit) == 0) {
      continue;
    }
    std::printf("%s%s", first ? "" : ",", feature.name);
    first = false;
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  uint32_t duration_seconds = 30;
  uint32_t spring_gain_percent = 10;
  bool list_only = false;
  bool spring_test = false;
  bool condition_isolation_test = false;
  bool transient_test = false;
  bool mixed_transient_test = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list-only") == 0) {
      list_only = true;
    } else if (std::strcmp(argv[i], "--spring-test") == 0) {
      spring_test = true;
    } else if (std::strcmp(argv[i], "--condition-isolation-test") == 0) {
      condition_isolation_test = true;
    } else if (std::strcmp(argv[i], "--transient-test") == 0) {
      transient_test = true;
    } else if (std::strcmp(argv[i], "--mixed-transient-test") == 0) {
      mixed_transient_test = true;
    } else if (std::strncmp(argv[i], "--seconds=", 10) == 0) {
      duration_seconds = static_cast<uint32_t>(std::strtoul(argv[i] + 10, nullptr, 10));
      if (duration_seconds == 0 || duration_seconds > 300) {
        std::fprintf(stderr, "--seconds must be in 1..300\n");
        return 64;
      }
    } else if (std::strncmp(argv[i], "--gain=", 7) == 0) {
      spring_gain_percent = static_cast<uint32_t>(std::strtoul(argv[i] + 7, nullptr, 10));
      if (spring_gain_percent == 0 || spring_gain_percent > 100) {
        std::fprintf(stderr, "--gain must be in 1..100\n");
        return 64;
      }
    } else {
      std::fprintf(stderr,
                   "usage: sdl_wheel_probe [--list-only | --spring-test | "
                   "--condition-isolation-test | --transient-test | --mixed-transient-test] "
                   "[--seconds=1..300] [--gain=1..100]\n");
      return 64;
    }
  }
  if (static_cast<int>(list_only) + static_cast<int>(spring_test) +
          static_cast<int>(condition_isolation_test) + static_cast<int>(transient_test) +
          static_cast<int>(mixed_transient_test) >
      1) {
    std::fprintf(stderr, "probe modes are mutually exclusive\n");
    return 64;
  }

  SDL_SetMainReady();
  if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
    std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
    return 1;
  }

  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  if (!ids) {
    std::fprintf(stderr, "SDL joystick enumeration failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  std::printf("SDL_WHEEL_PROBE_ENUM v=1 devices=%d\n", count);
  SDL_JoystickID selected_id = 0;
  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID id = ids[i];
    SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
    char guid_text[33] = {};
    SDL_GUIDToString(guid, guid_text, sizeof(guid_text));
    std::printf(
        "SDL_WHEEL_PROBE_DEVICE index=%d id=%u name=\"%s\" path=\"%s\" "
        "guid=%s vendor=%04X product=%04X version=%04X type=%d gamepad=%d player=%d wheel=%d\n",
        i, id, Safe(SDL_GetJoystickNameForID(id)), Safe(SDL_GetJoystickPathForID(id)), guid_text,
        SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id),
        SDL_GetJoystickProductVersionForID(id), static_cast<int>(SDL_GetJoystickTypeForID(id)),
        SDL_IsGamepad(id) ? 1 : 0, SDL_GetJoystickPlayerIndexForID(id), IsWheel(id) ? 1 : 0);
    if (!selected_id && IsWheel(id)) {
      selected_id = id;
    }
  }
  SDL_free(ids);

  if (!selected_id) {
    std::printf("SDL_WHEEL_PROBE_RESULT status=NO_WHEEL\n");
    SDL_Quit();
    return 2;
  }

  SDL_Joystick* joystick = SDL_OpenJoystick(selected_id);
  if (!joystick) {
    std::fprintf(stderr, "SDL wheel open failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  const int axes = SDL_GetNumJoystickAxes(joystick);
  const int buttons = SDL_GetNumJoystickButtons(joystick);
  const int hats = SDL_GetNumJoystickHats(joystick);
  std::printf("SDL_WHEEL_PROBE_OPEN id=%u axes=%d buttons=%d hats=%d\n", selected_id, axes, buttons,
              hats);
  SDL_PumpEvents();
  for (int axis = 0; axis < axes; ++axis) {
    Sint16 initial = 0;
    const bool has_initial = SDL_GetJoystickAxisInitialState(joystick, axis, &initial);
    std::printf("SDL_WHEEL_PROBE_AXIS_INITIAL axis=%d value=%d known=%d current=%d\n", axis,
                static_cast<int>(initial), has_initial ? 1 : 0,
                static_cast<int>(SDL_GetJoystickAxis(joystick, axis)));
  }
  for (int hat = 0; hat < hats; ++hat) {
    std::printf("SDL_WHEEL_PROBE_HAT_INITIAL hat=%d value=%u\n", hat,
                static_cast<unsigned>(SDL_GetJoystickHat(joystick, hat)));
  }

  SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick);
  if (haptic) {
    const Uint32 features = SDL_GetHapticFeatures(haptic);
    std::printf(
        "SDL_WHEEL_PROBE_HAPTIC status=OPEN name=\"%s\" axes=%d effects=%d playing=%d "
        "rumble=%d\n",
        Safe(SDL_GetHapticName(haptic)), SDL_GetNumHapticAxes(haptic),
        SDL_GetMaxHapticEffects(haptic), SDL_GetMaxHapticEffectsPlaying(haptic),
        SDL_HapticRumbleSupported(haptic) ? 1 : 0);
    PrintHapticFeatures(features);
  } else {
    std::printf("SDL_WHEEL_PROBE_HAPTIC status=UNAVAILABLE error=\"%s\"\n", SDL_GetError());
  }

  if (spring_test) {
    if (!haptic || (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_SPRING) == 0) {
      std::fprintf(stderr, "SDL spring effect is unavailable\n");
      if (haptic) {
        SDL_CloseHaptic(haptic);
      }
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
    SDL_HapticEffect effect = {};
    effect.type = SDL_HAPTIC_SPRING;
    effect.condition.length = duration_seconds * 1000;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = 1;
    effect.condition.right_sat[0] = static_cast<uint16_t>((65535u * spring_gain_percent) / 100u);
    effect.condition.left_sat[0] = effect.condition.right_sat[0];
    effect.condition.right_coeff[0] = 0x6000;
    effect.condition.left_coeff[0] = 0x6000;
    const SDL_HapticEffectID effect_id = SDL_CreateHapticEffect(haptic, &effect);
    if (effect_id < 0) {
      std::fprintf(stderr, "SDL spring creation failed: %s\n", SDL_GetError());
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
    std::printf("SDL_WHEEL_PROBE_SPRING action=start seconds=%u gain_percent=%u\n",
                duration_seconds, spring_gain_percent);
    if (!SDL_RunHapticEffect(haptic, effect_id, 1)) {
      std::fprintf(stderr, "SDL spring start failed: %s\n", SDL_GetError());
      SDL_DestroyHapticEffect(haptic, effect_id);
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
    SDL_Delay(duration_seconds * 1000);
    SDL_StopHapticEffect(haptic, effect_id);
    SDL_DestroyHapticEffect(haptic, effect_id);
    std::printf("SDL_WHEEL_PROBE_SPRING action=stop\n");
  } else if (condition_isolation_test) {
    constexpr Uint32 kRequiredFeatures =
        SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER | SDL_HAPTIC_CONSTANT;
    if (!haptic || (SDL_GetHapticFeatures(haptic) & kRequiredFeatures) != kRequiredFeatures) {
      std::fprintf(stderr, "SDL spring/damper/constant effects are unavailable\n");
      if (haptic) {
        SDL_CloseHaptic(haptic);
      }
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    auto play_phase = [&](const char* label, SDL_HapticEffect& effect, uint32_t length_ms) {
      std::printf("SDL_WHEEL_PROBE_CONDITION label=%s action=prepare pause_ms=2000\n", label);
      SDL_Delay(2000);
      const SDL_HapticEffectID effect_id = SDL_CreateHapticEffect(haptic, &effect);
      if (effect_id < 0 || !SDL_RunHapticEffect(haptic, effect_id, 1)) {
        std::fprintf(stderr, "SDL condition phase failed for %s: %s\n", label, SDL_GetError());
        if (effect_id >= 0) {
          SDL_DestroyHapticEffect(haptic, effect_id);
        }
        return false;
      }
      std::printf("SDL_WHEEL_PROBE_CONDITION label=%s action=start length_ms=%u\n", label,
                  length_ms);
      SDL_Delay(length_ms);
      SDL_StopHapticEffect(haptic, effect_id);
      SDL_DestroyHapticEffect(haptic, effect_id);
      std::printf("SDL_WHEEL_PROBE_CONDITION label=%s action=stop\n", label);
      return true;
    };

    SDL_HapticEffect spring = {};
    spring.type = SDL_HAPTIC_SPRING;
    spring.condition.length = 5000;
    spring.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    spring.condition.direction.dir[0] = 1;
    spring.condition.right_coeff[0] = 0x5000;
    spring.condition.left_coeff[0] = 0x5000;
    spring.condition.right_sat[0] = 0x4000;
    spring.condition.left_sat[0] = 0x4000;
    std::printf(
        "PHASE A: turn the wheel about 60 degrees, release it, and observe whether it "
        "returns toward center.\n");
    if (!play_phase("positive-spring", spring, spring.condition.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    SDL_HapticEffect damper = {};
    damper.type = SDL_HAPTIC_DAMPER;
    damper.condition.length = 5000;
    damper.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    damper.condition.direction.dir[0] = 1;
    damper.condition.right_coeff[0] = 0x7000;
    damper.condition.left_coeff[0] = 0x7000;
    damper.condition.right_sat[0] = 0xC000;
    damper.condition.left_sat[0] = 0xC000;
    std::printf(
        "PHASE B: move the wheel left/right repeatedly and observe whether motion is "
        "resisted without a one-sided pull.\n");
    if (!play_phase("positive-damper", damper, damper.condition.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    SDL_HapticEffect positive = {};
    positive.type = SDL_HAPTIC_CONSTANT;
    positive.constant.length = 2500;
    positive.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    positive.constant.direction.dir[0] = 1;
    positive.constant.level = 15000;
    std::printf(
        "PHASE C: keep hands ready and note which direction the low positive constant "
        "force pulls.\n");
    if (!play_phase("positive-constant", positive, positive.constant.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    SDL_HapticEffect negative = positive;
    negative.constant.level = -5000;
    std::printf(
        "PHASE D: keep hands ready and confirm the low negative constant force pulls "
        "the opposite way.\n");
    if (!play_phase("negative-constant", negative, negative.constant.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
  } else if (transient_test) {
    if (!haptic || (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_SQUARE) == 0 ||
        (SDL_GetHapticFeatures(haptic) & SDL_HAPTIC_CONSTANT) == 0) {
      std::fprintf(stderr, "SDL square/constant transient effects are unavailable\n");
      if (haptic) {
        SDL_CloseHaptic(haptic);
      }
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    auto play_transient = [&](const char* label, SDL_HapticEffect& effect, uint32_t length_ms) {
      const SDL_HapticEffectID effect_id = SDL_CreateHapticEffect(haptic, &effect);
      if (effect_id < 0) {
        std::fprintf(stderr, "SDL transient creation failed for %s: %s\n", label, SDL_GetError());
        return false;
      }
      std::printf("SDL_WHEEL_PROBE_TRANSIENT label=%s length_ms=%u action=start\n", label,
                  length_ms);
      const bool started = SDL_RunHapticEffect(haptic, effect_id, 1);
      if (!started) {
        std::fprintf(stderr, "SDL transient start failed for %s: %s\n", label, SDL_GetError());
      }
      SDL_Delay(length_ms + 100);
      SDL_StopHapticEffect(haptic, effect_id);
      SDL_DestroyHapticEffect(haptic, effect_id);
      std::printf("SDL_WHEEL_PROBE_TRANSIENT label=%s action=stop\n", label);
      SDL_Delay(1500);
      return started;
    };

    SDL_HapticEffect square = {};
    square.type = SDL_HAPTIC_SQUARE;
    square.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
    square.periodic.direction.dir[0] = 1;
    square.periodic.period = 75;
    square.periodic.magnitude = 10536;
    square.periodic.fade_length = 20;
    square.periodic.fade_level = 2569;
    square.periodic.length = 150;
    if (!play_transient("square-150", square, square.periodic.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
    square.periodic.length = 400;
    if (!play_transient("square-400", square, square.periodic.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    SDL_HapticEffect constant = {};
    constant.type = SDL_HAPTIC_CONSTANT;
    constant.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    constant.constant.direction.dir[0] = 1;
    constant.constant.level = 19866;
    constant.constant.length = 150;
    if (!play_transient("constant-150", constant, constant.constant.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
    constant.constant.length = 400;
    if (!play_transient("constant-400", constant, constant.constant.length)) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
  } else if (mixed_transient_test) {
    constexpr Uint32 kRequiredFeatures =
        SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER | SDL_HAPTIC_CONSTANT | SDL_HAPTIC_SQUARE;
    if (!haptic || (SDL_GetHapticFeatures(haptic) & kRequiredFeatures) != kRequiredFeatures) {
      std::fprintf(stderr, "SDL mixed transient effect set is unavailable\n");
      if (haptic) {
        SDL_CloseHaptic(haptic);
      }
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    std::vector<SDL_HapticEffectID> baseline_ids;
    auto start_baseline = [&](const char* label, SDL_HapticEffect& effect) {
      const SDL_HapticEffectID effect_id = SDL_CreateHapticEffect(haptic, &effect);
      if (effect_id < 0 || !SDL_RunHapticEffect(haptic, effect_id, 1)) {
        std::fprintf(stderr, "SDL mixed baseline failed for %s: %s\n", label, SDL_GetError());
        if (effect_id >= 0) {
          SDL_DestroyHapticEffect(haptic, effect_id);
        }
        return false;
      }
      baseline_ids.push_back(effect_id);
      std::printf("SDL_WHEEL_PROBE_MIXED_BASELINE label=%s action=start\n", label);
      return true;
    };

    SDL_HapticEffect spring = {};
    spring.type = SDL_HAPTIC_SPRING;
    spring.condition.length = SDL_HAPTIC_INFINITY;
    spring.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    spring.condition.direction.dir[0] = 1;
    spring.condition.right_coeff[0] = 22962;
    spring.condition.left_coeff[0] = 22962;
    spring.condition.right_sat[0] = 45746;
    spring.condition.left_sat[0] = 45746;

    SDL_HapticEffect damper = {};
    damper.type = SDL_HAPTIC_DAMPER;
    damper.condition.length = SDL_HAPTIC_INFINITY;
    damper.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    damper.condition.direction.dir[0] = 1;
    damper.condition.right_coeff[0] = 18060;
    damper.condition.left_coeff[0] = 18060;
    damper.condition.right_sat[0] = 65535;
    damper.condition.left_sat[0] = 65535;

    SDL_HapticEffect steering = {};
    steering.type = SDL_HAPTIC_CONSTANT;
    steering.constant.length = SDL_HAPTIC_INFINITY;
    steering.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    steering.constant.direction.dir[0] = 1;
    steering.constant.level = 6708;

    SDL_HapticEffect engine = {};
    engine.type = SDL_HAPTIC_SQUARE;
    engine.periodic.length = SDL_HAPTIC_INFINITY;
    engine.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
    engine.periodic.direction.dir[0] = 1;
    engine.periodic.period = 100;
    engine.periodic.magnitude = 976;

    if (!start_baseline("spring", spring) || !start_baseline("damper", damper) ||
        !start_baseline("steering-constant", steering) ||
        !start_baseline("engine-square", engine)) {
      for (const auto effect_id : baseline_ids) {
        SDL_StopHapticEffect(haptic, effect_id);
        SDL_DestroyHapticEffect(haptic, effect_id);
      }
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }

    SDL_Delay(2000);
    auto play_mixed_transient = [&](const char* label, SDL_HapticEffect& effect,
                                    bool update_after_start) {
      const SDL_HapticEffectID effect_id = SDL_CreateHapticEffect(haptic, &effect);
      if (effect_id < 0 || !SDL_RunHapticEffect(haptic, effect_id, 1)) {
        std::fprintf(stderr, "SDL mixed transient failed for %s: %s\n", label, SDL_GetError());
        if (effect_id >= 0) {
          SDL_DestroyHapticEffect(haptic, effect_id);
        }
        return false;
      }
      std::printf("SDL_WHEEL_PROBE_MIXED_TRANSIENT label=%s update_after_start=%d action=start\n",
                  label, update_after_start ? 1 : 0);
      if (update_after_start && !SDL_UpdateHapticEffect(haptic, effect_id, &effect)) {
        std::fprintf(stderr, "SDL mixed transient update failed for %s: %s\n", label,
                     SDL_GetError());
        SDL_StopHapticEffect(haptic, effect_id);
        SDL_DestroyHapticEffect(haptic, effect_id);
        return false;
      }
      SDL_Delay(300);
      SDL_StopHapticEffect(haptic, effect_id);
      SDL_DestroyHapticEffect(haptic, effect_id);
      SDL_Delay(1500);
      return true;
    };

    SDL_HapticEffect curb = {};
    curb.type = SDL_HAPTIC_SQUARE;
    curb.periodic.length = 150;
    curb.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
    curb.periodic.direction.dir[0] = 1;
    curb.periodic.period = 75;
    curb.periodic.magnitude = 10536;
    curb.periodic.fade_length = 20;
    curb.periodic.fade_level = 2569;

    SDL_HapticEffect collision = {};
    collision.type = SDL_HAPTIC_CONSTANT;
    collision.constant.length = 150;
    collision.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    collision.constant.direction.dir[0] = 1;
    collision.constant.level = 19866;

    const bool curb_ok = play_mixed_transient("curb-square-150", curb, true);
    const bool collision_ok =
        curb_ok && play_mixed_transient("collision-constant-150", collision, false);
    for (const auto effect_id : baseline_ids) {
      SDL_StopHapticEffect(haptic, effect_id);
      SDL_DestroyHapticEffect(haptic, effect_id);
    }
    if (!collision_ok) {
      SDL_CloseHaptic(haptic);
      SDL_CloseJoystick(joystick);
      SDL_Quit();
      return 3;
    }
  } else if (!list_only) {
    std::printf("SDL_WHEEL_PROBE_CAPTURE seconds=%u action=move-wheel-pedals-hat-buttons\n",
                duration_seconds);
    std::vector<Sint16> previous_axes(static_cast<size_t>(axes));
    std::vector<bool> previous_buttons(static_cast<size_t>(buttons));
    std::vector<Uint8> previous_hats(static_cast<size_t>(hats));
    for (int axis = 0; axis < axes; ++axis) {
      previous_axes[axis] = SDL_GetJoystickAxis(joystick, axis);
    }
    for (int button = 0; button < buttons; ++button) {
      previous_buttons[button] = SDL_GetJoystickButton(joystick, button);
    }
    for (int hat = 0; hat < hats; ++hat) {
      previous_hats[hat] = SDL_GetJoystickHat(joystick, hat);
    }
    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(duration_seconds) * 1000;
    while (SDL_GetTicks() < deadline) {
      SDL_PumpEvents();
      for (int axis = 0; axis < axes; ++axis) {
        const Sint16 value = SDL_GetJoystickAxis(joystick, axis);
        if (value != previous_axes[axis]) {
          std::printf("SDL_WHEEL_PROBE_AXIS axis=%d value=%d\n", axis, static_cast<int>(value));
          previous_axes[axis] = value;
        }
      }
      for (int button = 0; button < buttons; ++button) {
        const bool down = SDL_GetJoystickButton(joystick, button);
        if (down != previous_buttons[button]) {
          std::printf("SDL_WHEEL_PROBE_BUTTON button=%d down=%d\n", button, down ? 1 : 0);
          previous_buttons[button] = down;
        }
      }
      for (int hat = 0; hat < hats; ++hat) {
        const Uint8 value = SDL_GetJoystickHat(joystick, hat);
        if (value != previous_hats[hat]) {
          std::printf("SDL_WHEEL_PROBE_HAT hat=%d value=%u\n", hat, static_cast<unsigned>(value));
          previous_hats[hat] = value;
        }
      }
      SDL_Delay(5);
    }
  }

  if (haptic) {
    SDL_CloseHaptic(haptic);
  }
  SDL_CloseJoystick(joystick);
  SDL_Quit();
  std::printf("SDL_WHEEL_PROBE_RESULT status=PASS\n");
  return 0;
}
