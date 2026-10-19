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
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/dualsense.h>
#include <rex/input/ffb_trace.h>
#include <rex/input/flags.h>
#include <rex/input/pad_remap.h>
#include <rex/input/sdl/hid_direct.h>
#include <rex/input/sdl/joystick_mapping.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/sdl/wheel.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");
REXCVAR_DEFINE_BOOL(pad_gyro_steering, false, "Input",
                    "Steer by tilting a controller with motion sensors (DualSense, DualShock 4, "
                    "Switch Pro) like a wheel; adds to the left stick. The touchpad button (or "
                    "the capture button) re-centres it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pad_gyro_sensitivity, 100, "Input",
                     "Tilt steering sensitivity in percent: 100 reaches full lock at 45 degrees "
                     "of tilt, 200 at 22.5")
    .range(25, 400)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Racing wheels (FB-2). The guest sees one XInput wheel: steering on the left
// stick X, throttle on the right trigger, brake on the left trigger.
// Off by default: wheels then go through the SDL gamepad path (hid_wheel_*).
REXCVAR_DEFINE_BOOL(wheel_enabled, false, "Input",
                    "Read racing wheels raw, each axis and button bound per device (wheel_*), "
                    "as one XInput wheel (subtype 2) that plays the title's own force feedback; "
                    "off uses the SDL gamepad path (hid_wheel_*)");
REXCVAR_DEFINE_STRING(wheel_device, "", "Input",
                      "The wheel base: empty for the first device SDL reports as a wheel, "
                      "\"vvvv:pppp\" (USB IDs in hex) or part of its name");
REXCVAR_DEFINE_INT32(wheel_steering_axis, 0, "Input", "Steering axis on the wheel base")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_steering_invert, false, "Input", "Reverse the steering axis")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_steering_scale, 100, "Input",
                     "Steering sensitivity in percent; 200 reaches full lock at half the "
                     "wheel's rotation")
    .range(10, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_steering_deadzone, 0, "Input",
                     "Steering deadzone in percent of half travel")
    .range(0, 50)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_throttle, "", "Input",
                      "Throttle axis: \"[device|][+|-]axisN\"; device as in wheel_device "
                      "(empty: the wheel base), + or - forces the pedal direction, which is "
                      "otherwise detected from its resting value")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_brake, "", "Input", "Brake axis, as wheel_throttle")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_clutch, "", "Input",
                      "Clutch axis, as wheel_throttle. A wheel's clutch is analog on the right "
                      "stick's vertical axis, where FH1's wheel layouts read it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_clutch_button, "", "Input",
                      "Pad button the clutch pedal also presses past half travel; empty for none. "
                      "In FH1's wheel layout 1 LB shifts down, so leave it empty there")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_pedal_deadzone, 3, "Input", "Pedal deadzone in percent of travel")
    .range(0, 50)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#define REX_WHEEL_PEDAL_DEADZONES(pedal, Pedal)                                                    \
  REXCVAR_DEFINE_INT32(wheel_##pedal##_deadzone_bottom, -1, "Input",                               \
                       Pedal                                                                       \
                       " dead zone at rest, in percent of travel; -1 uses "                        \
                       "wheel_pedal_deadzone")                                                     \
      .range(-1, 50)                                                                               \
      .lifecycle(rex::cvar::Lifecycle::kHotReload);                                                \
  REXCVAR_DEFINE_INT32(wheel_##pedal##_deadzone_top, 0, "Input",                                   \
                       Pedal " dead zone at full travel, in percent: past it reads fully pressed") \
      .range(0, 50)                                                                                \
      .lifecycle(rex::cvar::Lifecycle::kHotReload);
REX_WHEEL_PEDAL_DEADZONES(throttle, "Throttle")
REX_WHEEL_PEDAL_DEADZONES(brake, "Brake")
REX_WHEEL_PEDAL_DEADZONES(clutch, "Clutch")
#undef REX_WHEEL_PEDAL_DEADZONES
REXCVAR_DEFINE_STRING(wheel_buttons, "UP=h0.up,DOWN=h0.down,LEFT=h0.left,RIGHT=h0.right", "Input",
                      "Wheel buttons as pad controls: \"NAME=[device|]N\" pairs separated by "
                      "commas, N a button number or hN.up/down/left/right for a hat; names as "
                      "in pad_remap (A, B, X, Y, LB, RB, LT, RT, LS, RS, BACK, START, UP, ...), "
                      "or GEAR1 to GEAR7 and REVERSE for an H-shifter's gates")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_ffb_mode, "difference", "Input",
                      "Force feedback: difference (the title's two motor values as a signed "
                      "constant force, right minus left), left-signed or right-signed (one "
                      "motor alone, 0x8000 = no force), rumble, or off")
    .allowed({"difference", "left-signed", "right-signed", "rumble", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_strength, 25, "Input",
                     "Force feedback strength in percent of what the title asks for")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_ffb_invert, false, "Input", "Reverse the force feedback direction")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_centering, 0, "Input",
                     "Host centring spring in percent, added to the title's forces")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(wheel_ffb_device, "", "Input",
                      "Device that receives force feedback, as wheel_device; empty for the "
                      "steering device (for EmuWheel/vJoy, the vJoy device relays it)");
REXCVAR_DEFINE_BOOL(wheel_hide_gamepads, true, "Input",
                    "While a racing wheel is connected, hide other SDL gamepads from the title "
                    "so none of them takes player one");
REXCVAR_DEFINE_BOOL(
    wheel_900_degree, true, "Input",
    "Report the wheel as a 900-degree wheel (Gamepad.sThumbLX = 900 in its "
    "capabilities), so FH1 uses its linear 900-degree steering instead of the default "
    "wheel mapping, which cuts steering at speed. Set the base to 900 degrees. Read when "
    "the wheel connects");
REXCVAR_DEFINE_INT32(wheel_ffb_min_force, 0, "Input",
                     "Lifts any non-zero steering force to at least this percent, so small forces "
                     "get past a geared or belted wheel's friction (a direct-drive base needs 0)")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_spring, 100, "Input",
                     "Strength in percent of the title's centring spring effect")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_damper, 100, "Input",
                     "Strength in percent of the title's damper effect")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_friction, 100, "Input",
                     "Strength in percent of the title's friction effect")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_inertia, 100, "Input",
                     "Strength in percent of the title's inertia effect")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_ffb_effects, true, "Input",
                    "Play the title's own wheel force feedback effects (XInputdFF: spring, damper, "
                    "friction, inertia and constant force) on the wheel")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    wheel_ffb_rumble, 50, "Input",
    "Strength in percent of the title's wheel rumble (kerbs, impacts), played as a "
    "vibration on top of the effects")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// FH1's motor census (pinyon-shift DS-2) and a T248 trace: the right motor carries the engine,
// rising with RPM through each gear, and the left the bumps, kerbs and impacts.
REXCVAR_DEFINE_INT32(wheel_ffb_rumble_engine, 100, "Input",
                     "Share in percent of wheel_ffb_rumble for the title's right motor, which "
                     "in Forza Horizon carries the engine and rises towards the limiter")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_rumble_road, 100, "Input",
                     "Share in percent of wheel_ffb_rumble for the title's left motor, which "
                     "in Forza Horizon carries bumps, kerbs and impacts")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_magnitude_max, 10000, "Input",
                     "Full-scale value of the title's effect magnitudes and coefficients "
                     "(DirectInput's 10000); the FFB trace shows the values it sends")
    .range(1, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wheel_ffb_saturation_max, 0, "Input",
                     "Full-scale value of the title's effect saturations and dead bands; 0 decides "
                     "per effect (255 when every value fits a byte, else 10000)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wheel_log_inputs, false, "Input",
                    "Log wheel axis, button and hat changes and the title's motor values, to "
                    "find the numbers for the wheel bindings")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::input::sdl {

namespace {

// SDL clamps to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

// Sensor events arrive at 250-1000 Hz; while nothing drains the queue (the
// title is not polling), keep only the newest.
constexpr size_t kMaxPendingEvents = 4096;

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

PadFamily FamilyOf(SDL_GamepadType type) {
  switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return PadFamily::kXbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return PadFamily::kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return PadFamily::kNintendo;
    default:
      return PadFamily::kUnknown;
  }
}

const char* FamilyName(PadFamily family) {
  switch (family) {
    case PadFamily::kXbox:
      return "xbox";
    case PadFamily::kPlayStation:
      return "playstation";
    case PadFamily::kNintendo:
      return "nintendo";
    default:
      return "unknown";
  }
}

std::string HexBytes(const uint8_t* data, size_t size) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(size * 3);
  for (size_t i = 0; i < size; ++i) {
    if (i) text.push_back(' ');
    text.push_back(kDigits[data[i] >> 4]);
    text.push_back(kDigits[data[i] & 15]);
  }
  return text;
}

}  // namespace

bool SteamInputHidesDualSense() {
  // Steam sets this for games it launches with Steam Input on, listing the
  // real pads it wraps so SDL in the game skips them.
  const char* ignored = SDL_getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
  if (!ignored) return false;
  std::string text = ignored;
  for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text.find("0x054c/0x0ce6") != std::string::npos ||
         text.find("0x054c/0x0df2") != std::string::npos;
}

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_() {}

SDLInputDriver::~SDLInputDriver() {
  if (xinputd_ffb::GetSink() == this) {
    xinputd_ffb::SetSink(nullptr);
  }
}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Before the PS5 driver opens a pad, so the LEDs never flash on.
      ApplyPlayerLedHint();

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;
      if (SteamInputHidesDualSense()) {
        REXLOG_INFO(
            "SDL pad: Steam Input hides a DualSense from the game (it is listed in "
            "SDL_GAMECONTROLLER_IGNORE_DEVICES); its triggers, lightbar and motion stay with "
            "Steam until Steam Input is off for the game.");
      }

      // Force feedback for wheels. Optional: without it wheels still steer.
      if (SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        sdl_haptic_initialized_ = true;
      } else {
        REXLOG_WARN("SDL: haptic subsystem unavailable, wheels get no force feedback: {}",
                    SDL_GetError());
      }

      // Load custom controller mappings if available
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      REXCVAR_GET(hid_mappings_file));
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         REXCVAR_GET(hid_mappings_file), mappings_result);
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      // The player's own mappings (the mapping assistant) replace the
      // database's for the same controller.
      const std::string user_mappings = REXCVAR_GET(hid_user_mappings_file);
      if (!user_mappings.empty() && std::filesystem::exists(rex::to_path(user_mappings))) {
        const int loaded = SDL_AddGamepadMappingsFromFile(user_mappings.c_str());
        if (loaded < 0) {
          REXLOG_ERROR("SDL: error loading user mappings '{}': {}", user_mappings, SDL_GetError());
        } else {
          REXLOG_INFO("SDL: loaded {} user mappings.", loaded);
        }
      }
      xinputd_ffb::SetSink(this);
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (xinputd_ffb::GetSink() == this) {
    xinputd_ffb::SetSink(nullptr);
  }
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    feedback_released_ = true;
    {
      auto guard = DrainAndLock();
      for (auto& controller : controllers_) {
        // SDL's PS5 driver leaves the triggers as they are when it closes a
        // pad; it waits up to 30 ms for this report to go out.
        ReleaseFeedbackLocked(controller);
        SDL_CloseGamepad(controller.sdl);
      }
      controllers_.clear();
    }
    CloseWheelLocked();
    for (auto& joystick : wheel_joysticks_) {
      SDL_CloseJoystick(joystick.sdl);
    }
    wheel_joysticks_.clear();
    if (sdl_haptic_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_HAPTIC);
      sdl_haptic_initialized_ = false;
    }
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  focused_ = false;
  if (!SDL_Gamepad_initialized_) return;
  auto guard = DrainAndLock();
  for (auto& controller : controllers_) {
    ReleaseFeedbackLocked(controller);
  }
}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  // The next InputSystem update sends whatever the triggers should hold.
  focused_ = true;
}

void SDLInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Polling can start before the window exists and the subsystems are up.
  if (!sdl_events_initialized_ || !SDL_Gamepad_initialized_) {
    return;
  }

  auto guard = DrainAndLock();

  // The wheel enumerates first so it is the first device to take a guest
  // user, and other pads can be hidden so none of them becomes player one.
  if (wheel_) {
    DeviceInfo info;
    info.id = wheel_->id;
    for (const auto& joystick : wheel_joysticks_) {
      if (joystick.instance_id == wheel_->steering) {
        info.name = joystick.name;
        char guid_text[33] = {};
        SDL_GUIDToString(SDL_GetJoystickGUID(joystick.sdl), guid_text,
                         static_cast<int>(sizeof(guid_text)));
        info.guid = guid_text;
      }
    }
    info.synthetic = false;
    out.push_back(info);
    if (REXCVAR_GET(wheel_hide_gamepads)) {
      return;
    }
  }

  for (const auto& controller : controllers_) {
    DeviceInfo info;
    info.id = controller.id;
    const char* name = SDL_GetGamepadName(controller.sdl);
    info.name = name ? name : "";
    char guid_text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller.sdl)), guid_text,
                     static_cast<int>(sizeof(guid_text)));
    info.guid = guid_text;
    info.synthetic = false;
    info.family = controller.family;
    info.vendor_id = controller.vendor_id;
    info.product_id = controller.product_id;
    info.steam_virtual = controller.steam_virtual;
    info.adaptive_triggers = controller.dualsense;
    info.lightbar = controller.lightbar;
    info.motion = controller.motion;
    out.push_back(info);
  }
}

X_RESULT SDLInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    if (wheel_ && wheel_->id == id) {
      std::memcpy(out_caps, &wheel_->caps, sizeof(*out_caps));
      return X_ERROR_SUCCESS;
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller && wheel_ && wheel_->id == id) {
    RefreshWheelStateLocked();
    WheelState& wheel = *wheel_;
    if ((is_active != wheel.is_active) || (is_active && wheel.state_changed)) {
      wheel.state.packet_number++;
      wheel.is_active = is_active;
      wheel.state_changed = false;
    }
    std::memcpy(out_state, &wheel.state, sizeof(*out_state));
    if (!is_active) {
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
    }
    return X_ERROR_SUCCESS;
  }
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  UpdateSensorsLocked(*controller);
  if (controller->sensors_on && controller->tilt_valid) {
    // Tilting the pad like a wheel: clockwise (right side down) steers right.
    const float full_lock_rad =
        float(0.25 * 3.14159265358979) * 100.0f /
        float(std::clamp(REXCVAR_GET(pad_gyro_sensitivity), 25, 400));
    float tilt = controller->tilt_centre_rad - controller->tilt_rad;
    // A small dead band so a pad held still does not creep.
    constexpr float kDeadBand = 0.035f;  // about 2 degrees
    tilt = std::abs(tilt) < kDeadBand ? 0.0f : tilt - std::copysign(kDeadBand, tilt);
    const float steer = std::clamp(tilt / full_lock_rad, -1.0f, 1.0f);
    auto& pad = out_state->gamepad;
    const int32_t sum = int32_t(int16_t(pad.thumb_lx)) + int32_t(std::lround(steer * 32767.0f));
    pad.thumb_lx = int16_t(std::clamp(sum, -32768, 32767));
  }
  if (controller->wheel_type || IsListedWheel(controller->guid)) {
    // Shaped on read so the settings apply live; menus (keystrokes) keep the
    // raw input.
    const WheelShaping shaping = CurrentWheelShaping();
    auto& pad = out_state->gamepad;
    pad.thumb_lx = ShapeWheelSteering(static_cast<int16_t>(pad.thumb_lx), shaping);
    pad.left_trigger = ShapeWheelPedal(pad.left_trigger, shaping);
    pad.right_trigger = ShapeWheelPedal(pad.right_trigger, shaping);
  }
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller && wheel_ && wheel_->id == id) {
    return vibration ? SetWheelVibrationLocked(*vibration) : X_ERROR_SUCCESS;
  }
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // XInput vibration holds until the guest changes it, but SDL rumble expires,
  // and a zero duration expires on the next SDL_UpdateJoysticks. Arm it for
  // SDL's maximum instead; each call cancels the previous effect anyway.
  return SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                           vibration->right_motor_speed, kRumbleDurationMs)
             ? X_ERROR_SUCCESS
             : X_ERROR_FUNCTION_FAILED;
}

X_RESULT SDLInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  const X_INPUT_GAMEPAD* gamepad = nullptr;
  KeystrokeState* last_state = nullptr;
  if (controller) {
    gamepad = &controller->state.gamepad;
    last_state = &controller->keystroke;
  } else if (wheel_ && wheel_->id == id) {
    RefreshWheelStateLocked();
    gamepad = &wheel_->state.gamepad;
    last_state = &wheel_->keystroke;
  } else {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(gamepad->buttons)) |
                   AnalogToKeyfield(*gamepad))
                : uint64_t(0);
  KeystrokeState& last = *last_state;

  // Handle repeating
  auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
  static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
  if (last.repeat_state == RepeatState::Waiting &&
      (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
    last.repeat_state = RepeatState::Repeating;
  }
  if (last.repeat_state == RepeatState::Repeating &&
      (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
    last.repeat_time = guest_now;
    rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
    assert_true(vk != rex::ui::VirtualKey::kNone);
    out_keystroke->virtual_key = uint16_t(vk);
    out_keystroke->unicode = 0;
    // InputSystem stamps the guest user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  auto butts_changed = curr_butts ^ last.buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behavior when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      auto fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      rex::ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == rex::ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        // up
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        last.buttons &= ~fbutton;
        last.repeat_state = RepeatState::Idle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        // down
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        last.buttons |= fbutton;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_butt_idx = i;
        last.repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  if (pending_events_.size() >= kMaxPendingEvents &&
      event.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
    return;
  }
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
      OnControllerSensorLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_ADDED:
      OnJoystickAddedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_REMOVED:
      OnJoystickRemovedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  // A wheel base, pedal set or shifter is read raw for the wheel; as a
  // gamepad it would steer with a stick-shaped axis and get plain rumble.
  if (IsWheelPart(event.gdevice.which)) {
    REXLOG_INFO("SDL: \"{}\" is used by the racing wheel, not as a gamepad.",
                SDL_GetGamepadNameForID(event.gdevice.which));
    return;
  }
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));

  // SDL_GetGamepadPlayerIndex is deliberately ignored: on Windows it reports
  // the XInput user index the OS assigned, which is unrelated to connection
  // order and leaves non-XInput pads unnumbered.
  ControllerState state = {};
  state.sdl = controller;
  state.id = AllocateDeviceId();
  state.state_changed = true;  // XInput starts with packet_number = 1
  SDL_Joystick* joystick = SDL_GetGamepadJoystick(controller);
  state.wheel_type = SDL_GetJoystickType(joystick) == SDL_JOYSTICK_TYPE_WHEEL;
  char guid_text[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid_text, static_cast<int>(sizeof(guid_text)));
  state.guid = guid_text;
  if (state.wheel_type || IsListedWheel(state.guid)) {
    REXLOG_INFO("SDL OnControllerDeviceAdded: racing wheel, steering and pedals are shaped.");
  }
  // Identity (DS-0.1). Steam Input's virtual pad reports the real pad's type
  // on some platforms, so the Steam handle decides, not the type.
  state.family = FamilyOf(SDL_GetGamepadType(controller));
  state.vendor_id = SDL_GetGamepadVendor(controller);
  state.product_id = SDL_GetGamepadProduct(controller);
  const char* pad_name = SDL_GetGamepadName(controller);
  state.steam_virtual = SDL_GetGamepadSteamHandle(controller) != 0 ||
                        (pad_name && std::string_view(pad_name) == "Steam Virtual Gamepad");
  const SDL_PropertiesID properties = SDL_GetGamepadProperties(controller);
  // SDL's PS5 driver reports its RGB LED capability only once the pad is in
  // enhanced mode, which a Bluetooth pad enters on the first effect; the
  // vendor and product decide.
  state.dualsense =
      !state.steam_virtual && dualsense::IsDualSense(state.vendor_id, state.product_id);
  state.lightbar = state.dualsense || (!state.steam_virtual &&
                                       SDL_GetBooleanProperty(
                                           properties, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false));
  state.motion = !state.steam_virtual && SDL_GamepadHasSensor(controller, SDL_SENSOR_GYRO) &&
                 SDL_GamepadHasSensor(controller, SDL_SENSOR_ACCEL);
  const auto connection = SDL_GetJoystickConnectionState(joystick);
  if (const char* path = SDL_GetGamepadPath(controller)) {
    state.hid_path = path;
  }
  state.bluetooth = connection == SDL_JOYSTICK_CONNECTION_WIRELESS;
  REXLOG_INFO(
      "SDL pad: \"{}\" family {} vendor 0x{:04X} product 0x{:04X} {} steam_virtual {} "
      "adaptive_triggers {} lightbar {} motion {}",
      pad_name ? pad_name : "", FamilyName(state.family), state.vendor_id, state.product_id,
      connection == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless"
      : connection == SDL_JOYSTICK_CONNECTION_WIRED  ? "wired"
                                                     : "unknown-link",
      state.steam_virtual, state.dualsense, state.lightbar, state.motion);
  if (state.steam_virtual) {
    REXLOG_INFO(
        "SDL pad: Steam Input stands between the game and this pad; turn Steam Input off for "
        "the game to use a DualSense's own features.");
  }
  UpdateXCapabilities(state);
  controllers_.push_back(state);

  const int ordinal = static_cast<int>(controllers_.size()) - 1;
  SDL_SetGamepadPlayerIndex(controller, ordinal);
  if (state.dualsense) {
    // A pad keeps its trigger effects until told otherwise, also after a
    // previous run crashed; start from slack triggers.
    ReleaseFeedbackLocked(controllers_.back());
  }

  REXLOG_INFO("SDL OnControllerDeviceAdded: connection order {}, device {}.", ordinal,
              static_cast<uint64_t>(state.id));
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored. Unknown device.");
    return;
  }
  SDL_CloseGamepad(controllers_.at(*idx).sdl);
  controllers_.erase(controllers_.begin() + static_cast<ptrdiff_t>(*idx));

  // LEDs only. Guest user assignment does not shift, because InputSystem holds
  // each device's ordinal.
  for (size_t i = 0; i < controllers_.size(); i++) {
    SDL_SetGamepadPlayerIndex(controllers_[i].sdl, static_cast<int>(i));
  }
  REXLOG_INFO("SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons:
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles:
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    // The pad can be removed between the event being posted and drained.
    return;
  }
  auto& controller = controllers_.at(*idx);

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  // The touchpad (PlayStation) or capture button (Switch) re-centres tilt
  // steering while it is on, and is not passed to the title then.
  if (controller.sensors_on && (event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD ||
                                event.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1)) {
    if (event.gbutton.down && controller.tilt_valid) {
      controller.tilt_centre_rad = controller.tilt_rad;
      REXLOG_INFO("SDL pad: tilt steering re-centred at {:.1f} degrees",
                  controller.tilt_rad * 57.2957795f);
    }
    return;
  }
  // Pressed or released?
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

void SDLInputDriver::OnControllerSensorLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gsensor.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (!controller.sensors_on) {
    return;
  }
  const float* data = event.gsensor.data;
  if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
    // SDL's gamepad axes: +X right, +Y up out of the face, +Z toward the
    // player. Held like a wheel, gravity moves between Y and X as the pad
    // rolls; atan2 gives the counter-clockwise roll seen from the player.
    if (std::abs(data[0]) + std::abs(data[1]) < 2.0f) {
      return;  // pointing at the floor or the sky; roll is undefined
    }
    const float accel_roll = std::atan2(data[0], data[1]);
    if (!controller.tilt_valid) {
      controller.tilt_rad = accel_roll;
      controller.tilt_centre_rad = 0.0f;
      controller.tilt_valid = true;
    } else {
      // Complementary filter: the gyro carries quick turns, gravity pulls
      // the estimate back so it never drifts.
      controller.tilt_rad += 0.02f * (accel_roll - controller.tilt_rad);
    }
  } else if (event.gsensor.sensor == SDL_SENSOR_GYRO && controller.tilt_valid) {
    const uint64_t timestamp = event.gsensor.sensor_timestamp;
    if (controller.gyro_timestamp_ns && timestamp > controller.gyro_timestamp_ns) {
      const float dt = std::min(0.05f, float(timestamp - controller.gyro_timestamp_ns) * 1e-9f);
      // Rotation about Z, counter-clockwise positive (right-hand rule).
      controller.tilt_rad += data[2] * dt;
    }
    controller.gyro_timestamp_ns = timestamp;
  }
}

void SDLInputDriver::UpdateSensorsLocked(ControllerState& controller) {
  const bool wanted = controller.motion && REXCVAR_GET(pad_gyro_steering);
  if (wanted == controller.sensors_on) {
    return;
  }
  const bool ok = SDL_SetGamepadSensorEnabled(controller.sdl, SDL_SENSOR_ACCEL, wanted) &&
                  SDL_SetGamepadSensorEnabled(controller.sdl, SDL_SENSOR_GYRO, wanted);
  if (wanted && !ok) {
    REXLOG_WARN("SDL pad: cannot enable motion sensors: {}", SDL_GetError());
    controller.motion = false;  // do not retry every poll
    return;
  }
  controller.sensors_on = wanted;
  controller.tilt_valid = false;
  controller.gyro_timestamp_ns = 0;
  REXLOG_INFO("SDL pad: motion sensors {}", wanted ? "on" : "off");
}

void SDLInputDriver::ApplyPlayerLedHint() {
  const int wanted = REXCVAR_GET(pad_ds_player_leds) ? 1 : 0;
  if (wanted == player_led_hint_) {
    return;
  }
  player_led_hint_ = wanted;
  // SDL's PS5 driver watches the hint and updates open pads at once.
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, wanted ? "1" : "0");
}

bool SDLInputDriver::SetDeviceTriggerEffects(DeviceId id,
                                             const dualsense::TriggerEffects& effects) {
  if (!SDL_Gamepad_initialized_) {
    return false;
  }
  auto guard = DrainAndLock();
  auto controller = FindController(id);
  if (!controller || !controller->dualsense) {
    return false;
  }
  if (feedback_released_) {
    return true;  // quitting or crashing: the triggers stay slack
  }
  // Held at slack while another window has focus (the release already went
  // out in OnLostFocus).
  const dualsense::TriggerEffects wanted = focused_ ? effects : dualsense::TriggerEffects{};
  const uint64_t now = SteadyNs();
  dualsense::EffectPayload payload;
  if (!controller->triggers.Update(wanted, now, &payload)) {
    return true;
  }
  const bool sent =
      SDL_SendGamepadEffect(controller->sdl, payload.data(), int(payload.size()));
  if (!sent) {
    REXLOG_WARN("SDL pad: trigger effect report failed: {}", SDL_GetError());
    controller->triggers.Invalidate();
  }
  if (REXCVAR_GET(pad_ds_log_reports)) {
    REXLOG_INFO("DualSense report device {}: R2 {} | L2 {}", static_cast<uint64_t>(id),
                HexBytes(wanted.right.data(), wanted.right.size()),
                HexBytes(wanted.left.data(), wanted.left.size()));
    if (now - log_window_start_ns_ >= 1'000'000'000ull) {
      if (log_window_start_ns_) {
        REXLOG_INFO("DualSense reports in the last {:.2f} s: {}",
                    double(now - log_window_start_ns_) * 1e-9, log_window_reports_);
      }
      log_window_start_ns_ = now;
      log_window_reports_ = 0;
    }
    ++log_window_reports_;
  }
  return sent;
}

bool SDLInputDriver::SetDeviceLightbar(DeviceId id, const dualsense::Rgb* colour) {
  if (!SDL_Gamepad_initialized_) {
    return false;
  }
  ApplyPlayerLedHint();
  auto guard = DrainAndLock();
  for (size_t i = 0; i < controllers_.size(); ++i) {
    auto& controller = controllers_[i];
    if (controller.id != id) {
      continue;
    }
    if (!controller.lightbar) {
      return false;
    }
    if (feedback_released_) {
      return true;
    }
    const bool want_colour = colour && focused_;
    if (want_colour == controller.has_colour && (!want_colour || *colour == controller.colour)) {
      return true;
    }
    if (want_colour) {
      SDL_SetGamepadLED(controller.sdl, colour->r, colour->g, colour->b);
      controller.colour = *colour;
    } else {
      // Setting the player index again restores SDL's player colour.
      SDL_SetGamepadPlayerIndex(controller.sdl, static_cast<int>(i));
    }
    controller.has_colour = want_colour;
    return true;
  }
  return false;
}

namespace {

// Slack triggers as a raw DualSense output report: USB report 0x02, or
// Bluetooth report 0x31 with its CRC, as SDL's PS5 driver frames them.
bool WriteSlackTriggersDirect(const std::string& path, bool bluetooth) {
#if defined(_WIN32)
  const auto payload = dualsense::BuildEffectPayload(dualsense::TriggerEffects{});
  std::array<uint8_t, 78> report = {};
  size_t size = 0;
  if (bluetooth) {
    report[0] = 0x31;
    report[1] = 0x00;  // tag and sequence
    report[2] = 0x10;
    std::copy(payload.begin(), payload.end(), report.begin() + 3);
    const uint8_t header = 0xA2;  // the HIDP header is part of the CRC
    uint32_t crc = SDL_crc32(0, &header, 1);
    crc = SDL_crc32(crc, report.data(), report.size() - 4);
    for (int i = 0; i < 4; ++i) report[report.size() - 4 + i] = uint8_t(crc >> (8 * i));
    size = report.size();
  } else {
    report[0] = 0x02;
    std::copy(payload.begin(), payload.end(), report.begin() + 1);
    size = 48;
  }
  return WriteHidOutputReport(path, report.data(), size);
#else
  (void)path;
  (void)bluetooth;
  return false;
#endif
}

}  // namespace

namespace {

bool NameHas(std::string_view name, std::string_view word) {
  std::string lower(name);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find(word) != std::string::npos;
}

std::string JoystickNameForID(SDL_JoystickID instance_id) {
  const char* name = SDL_GetJoystickNameForID(instance_id);
  return name ? name : "";
}

std::string JoystickPathForID(SDL_JoystickID instance_id) {
  const char* path = SDL_GetJoystickPathForID(instance_id);
  return path ? path : "";
}

}  // namespace

bool SDLInputDriver::IsSteeringCandidate(SDL_JoystickID instance_id) {
  if (!REXCVAR_GET(wheel_enabled)) {
    return false;
  }
  const std::string name = JoystickNameForID(instance_id);
  const uint16_t vendor = SDL_GetJoystickVendorForID(instance_id);
  const uint16_t product = SDL_GetJoystickProductForID(instance_id);
  const std::string& match = REXCVAR_GET(wheel_device);
  if (!match.empty()) {
    return WheelDeviceMatches(match, name, vendor, product, JoystickPathForID(instance_id));
  }
  if (SDL_GetJoystickTypeForID(instance_id) == SDL_JOYSTICK_TYPE_WHEEL) {
    return true;
  }
  // Bases SDL does not type as a wheel; their pedal sets and shifters share
  // the vendor ID, so those are left to the bindings.
  return IsKnownWheelVendor(vendor) && !NameHas(name, "pedal") && !NameHas(name, "shifter") &&
         !NameHas(name, "handbrake");
}

bool SDLInputDriver::IsWheelPart(SDL_JoystickID instance_id) {
  if (!REXCVAR_GET(wheel_enabled)) {
    return false;
  }
  if (IsSteeringCandidate(instance_id)) {
    return true;
  }
  RefreshWheelBindingsLocked();
  const std::string name = JoystickNameForID(instance_id);
  const uint16_t vendor = SDL_GetJoystickVendorForID(instance_id);
  const uint16_t product = SDL_GetJoystickProductForID(instance_id);
  const std::string path = JoystickPathForID(instance_id);
  auto names = [&](const std::string& device) {
    return !device.empty() && WheelDeviceMatches(device, name, vendor, product, path);
  };
  if (names(REXCVAR_GET(wheel_ffb_device))) {
    return true;
  }
  const auto& b = wheel_bindings_;
  if ((b.throttle && names(b.throttle->device)) || (b.brake && names(b.brake->device)) ||
      (b.clutch && names(b.clutch->device))) {
    return true;
  }
  return std::any_of(b.buttons.begin(), b.buttons.end(),
                     [&](const WheelButtonBinding& button) { return names(button.device); });
}

void SDLInputDriver::OnJoystickAddedLocked(const SDL_Event& event) {
  const SDL_JoystickID which = event.jdevice.which;
  if (!IsWheelPart(which)) {
    return;
  }
  for (const auto& joystick : wheel_joysticks_) {
    if (joystick.instance_id == which) {
      return;
    }
  }
  SDL_Joystick* sdl = SDL_OpenJoystick(which);
  if (!sdl) {
    REXLOG_WARN("SDL wheel: could not open \"{}\": {}", JoystickNameForID(which), SDL_GetError());
    return;
  }
  WheelJoystick joystick;
  joystick.sdl = sdl;
  joystick.instance_id = which;
  joystick.name = JoystickNameForID(which);
  joystick.path = JoystickPathForID(which);
  joystick.vendor = SDL_GetJoystickVendor(sdl);
  joystick.product = SDL_GetJoystickProduct(sdl);
  REXLOG_INFO(
      "SDL wheel: opened \"{}\" ({:04x}:{:04x}), type {}, {} axes, {} buttons, {} hats, path {}.",
      joystick.name, joystick.vendor, joystick.product, static_cast<int>(SDL_GetJoystickType(sdl)),
      SDL_GetNumJoystickAxes(sdl), SDL_GetNumJoystickButtons(sdl), SDL_GetNumJoystickHats(sdl),
      joystick.path);
  wheel_joysticks_.push_back(std::move(joystick));

  if (wheel_) {
    // A separate force feedback device can arrive after the steering base.
    if (!wheel_->haptic && IsFfbTarget(wheel_joysticks_.back())) {
      OpenWheelHapticLocked(*wheel_, wheel_joysticks_.back());
      SyncGameEffectsLocked();
    }
    return;
  }
  if (!IsSteeringCandidate(which)) {
    return;
  }
  WheelState wheel;
  wheel.id = AllocateDeviceId();
  wheel.steering = which;
  wheel.state = {};
  wheel.keystroke = {};
  auto& c = wheel.caps;
  c = {};
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x02;  // XINPUT_DEVSUBTYPE_WHEEL
  // The title takes its wheel path only for a wheel that reports force
  // feedback (FEATURE_BACKLOG FB-2), so the flag is set even when the base
  // has no haptic support; its forces are then dropped.
  c.flags = X_INPUT_CAPS_FFB_SUPPORTED;
  c.gamepad.buttons = 0xF3FF;
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  // FH1 reads sThumbLX == 900 in a wheel's capabilities as its 900-degree flag
  // (pad +32) and then steers through WheelMapping900Degree: linear and not
  // reduced at speed. The default wheel mapping keeps about a third of the
  // steering at speed. Found by Ty Crowther (pinyon-shift PR #605).
  c.gamepad.thumb_lx = REXCVAR_GET(wheel_900_degree) ? int16_t(900) : static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  // A wheel whose capabilities give thumb_rx other than 0 or 1 has an
  // H-shifter: FH1 then reads the gear from the raw thumb_rx (0 neutral).
  // Reported only when gears are bound; paddles and buttons still shift.
  RefreshWheelBindingsLocked();
  const bool shifter =
      std::any_of(wheel_bindings_.buttons.begin(), wheel_bindings_.buttons.end(),
                  [](const WheelButtonBinding& binding) { return binding.gear != 0; });
  c.gamepad.thumb_rx = shifter ? static_cast<int16_t>(0xFFFFu) : 0;
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
  for (const auto& candidate : wheel_joysticks_) {
    if (candidate.instance_id == which ? REXCVAR_GET(wheel_ffb_device).empty()
                                       : IsFfbTarget(candidate)) {
      OpenWheelHapticLocked(wheel, candidate);
      break;
    }
  }
  wheel_ = std::move(wheel);
  SyncGameEffectsLocked();
  REXLOG_INFO("SDL wheel: \"{}\" is the guest's racing wheel, device {}.",
              wheel_joysticks_.back().name, static_cast<uint64_t>(wheel_->id));
  if (REXCVAR_GET(wheel_throttle).empty() || REXCVAR_GET(wheel_brake).empty()) {
    REXLOG_INFO(
        "SDL wheel: set wheel_throttle and wheel_brake (and wheel_clutch, wheel_buttons) for the "
        "pedals and buttons; wheel_log_inputs prints the numbers as you press them.");
  }
}

void SDLInputDriver::OnJoystickRemovedLocked(const SDL_Event& event) {
  const SDL_JoystickID which = event.jdevice.which;
  for (size_t i = 0; i < wheel_joysticks_.size(); i++) {
    if (wheel_joysticks_[i].instance_id != which) {
      continue;
    }
    if (wheel_ && wheel_->steering == which) {
      CloseWheelLocked();
      REXLOG_INFO("SDL wheel: the wheel base was disconnected.");
    } else if (wheel_ && wheel_->haptic && wheel_->haptic_source == which) {
      CloseWheelHapticLocked(*wheel_);
      REXLOG_INFO("SDL wheel: the force feedback device was disconnected.");
    }
    SDL_CloseJoystick(wheel_joysticks_[i].sdl);
    wheel_joysticks_.erase(wheel_joysticks_.begin() + static_cast<ptrdiff_t>(i));
    return;
  }
}

SDLInputDriver::WheelJoystick* SDLInputDriver::FindWheelJoystick(std::string_view device_match,
                                                                 bool steering_default) {
  for (auto& joystick : wheel_joysticks_) {
    if (device_match.empty()) {
      if (steering_default && wheel_ && joystick.instance_id == wheel_->steering) {
        return &joystick;
      }
      continue;
    }
    if (WheelDeviceMatches(device_match, joystick.name, joystick.vendor, joystick.product,
                           joystick.path)) {
      return &joystick;
    }
  }
  return nullptr;
}

bool SDLInputDriver::IsFfbTarget(const WheelJoystick& joystick) const {
  const std::string& match = REXCVAR_GET(wheel_ffb_device);
  return !match.empty() &&
         WheelDeviceMatches(match, joystick.name, joystick.vendor, joystick.product, joystick.path);
}

void SDLInputDriver::OpenWheelHapticLocked(WheelState& wheel, const WheelJoystick& joystick) {
  if (!sdl_haptic_initialized_) {
    return;
  }
  if (!SDL_IsJoystickHaptic(joystick.sdl)) {
    REXLOG_INFO("SDL wheel: \"{}\" has no force feedback; rumble only.", joystick.name);
    return;
  }
  wheel.haptic = SDL_OpenHapticFromJoystick(joystick.sdl);
  if (!wheel.haptic) {
    REXLOG_WARN("SDL wheel: could not open force feedback on \"{}\": {}", joystick.name,
                SDL_GetError());
    return;
  }
  wheel.haptic_source = joystick.instance_id;
  wheel.constant_effect = -1;
  wheel.spring_effect = -1;
  wheel.last_force = 0;
  wheel.last_spring = 0;
  wheel.haptic_features = SDL_GetHapticFeatures(wheel.haptic);
  if (wheel.haptic_features & SDL_HAPTIC_GAIN) {
    SDL_SetHapticGain(wheel.haptic, 100);
  }
  wheel.rumble_effect = -1;
  const char* haptic_name = SDL_GetHapticName(wheel.haptic);
  REXLOG_INFO("SDL wheel: force feedback on \"{}\" (\"{}\"), constant force {}, spring {}.",
              joystick.name, haptic_name ? haptic_name : "",
              (wheel.haptic_features & SDL_HAPTIC_CONSTANT) ? "yes" : "no",
              (wheel.haptic_features & SDL_HAPTIC_SPRING) ? "yes" : "no");
}

void SDLInputDriver::CloseWheelHapticLocked(WheelState& wheel) {
  if (!wheel.haptic) {
    return;
  }
  SDL_StopHapticEffects(wheel.haptic);
  for (auto& [id, game] : game_effects_) {
    if (game.sdl_id >= 0) {
      SDL_DestroyHapticEffect(wheel.haptic, game.sdl_id);
    }
    game.sdl_id = -1;
    game.sdl_running = false;
  }
  if (wheel.rumble_effect >= 0) {
    SDL_DestroyHapticEffect(wheel.haptic, wheel.rumble_effect);
    wheel.rumble_effect = -1;
  }
  if (wheel.constant_effect >= 0) {
    SDL_DestroyHapticEffect(wheel.haptic, wheel.constant_effect);
  }
  if (wheel.spring_effect >= 0) {
    SDL_DestroyHapticEffect(wheel.haptic, wheel.spring_effect);
  }
  SDL_CloseHaptic(wheel.haptic);
  wheel.haptic = nullptr;
  wheel.haptic_source = 0;
  wheel.haptic_features = 0;
  wheel.constant_effect = -1;
  wheel.spring_effect = -1;
  wheel.last_force = 0;
  wheel.last_spring = 0;
}

void SDLInputDriver::CloseWheelLocked() {
  if (!wheel_) {
    return;
  }
  CloseWheelHapticLocked(*wheel_);
  wheel_.reset();
}

void SDLInputDriver::RefreshWheelBindingsLocked() {
  auto& b = wheel_bindings_;
  const std::string& throttle = REXCVAR_GET(wheel_throttle);
  if (throttle != b.throttle_text) {
    b.throttle_text = throttle;
    b.throttle = ParseWheelAxisBinding(throttle);
  }
  const std::string& brake = REXCVAR_GET(wheel_brake);
  if (brake != b.brake_text) {
    b.brake_text = brake;
    b.brake = ParseWheelAxisBinding(brake);
  }
  const std::string& clutch = REXCVAR_GET(wheel_clutch);
  if (clutch != b.clutch_text) {
    b.clutch_text = clutch;
    b.clutch = ParseWheelAxisBinding(clutch);
  }
  const std::string& buttons = REXCVAR_GET(wheel_buttons);
  if (buttons != b.buttons_text) {
    b.buttons_text = buttons;
    b.buttons = ParseWheelButtonBindings(buttons);
  }
}

uint8_t SDLInputDriver::ReadWheelPedalLocked(const std::optional<WheelAxisBinding>& binding) {
  return ReadWheelPedalLocked(binding, -1, 0);
}

uint8_t SDLInputDriver::ReadWheelPedalLocked(const std::optional<WheelAxisBinding>& binding,
                                             int bottom_percent, int top_percent) {
  if (!binding) {
    return 0;
  }
  WheelJoystick* joystick = FindWheelJoystick(binding->device, true);
  if (!joystick || binding->axis >= SDL_GetNumJoystickAxes(joystick->sdl)) {
    return 0;
  }
  int16_t rest = -32768;
  if (binding->direction == AxisDirection::kAuto &&
      !SDL_GetJoystickAxisInitialState(joystick->sdl, binding->axis, &rest)) {
    rest = -32768;
  }
  const AxisDirection direction = ResolveAxisDirection(binding->direction, rest);
  if (bottom_percent < 0) {
    bottom_percent = REXCVAR_GET(wheel_pedal_deadzone);
  }
  return PedalFromAxis(SDL_GetJoystickAxis(joystick->sdl, binding->axis), direction, bottom_percent,
                       top_percent);
}

void SDLInputDriver::RefreshWheelStateLocked() {
  if (!wheel_) {
    return;
  }
  RefreshWheelBindingsLocked();
  X_INPUT_GAMEPAD pad = {};

  if (WheelJoystick* base = FindWheelJoystick({}, true)) {
    const int axis = REXCVAR_GET(wheel_steering_axis);
    if (axis < SDL_GetNumJoystickAxes(base->sdl)) {
      pad.thumb_lx =
          SteeringFromAxis(SDL_GetJoystickAxis(base->sdl, axis), REXCVAR_GET(wheel_steering_invert),
                           REXCVAR_GET(wheel_steering_scale), REXCVAR_GET(wheel_steering_deadzone));
    }
  }

  const auto& b = wheel_bindings_;
  uint8_t right_trigger =
      ReadWheelPedalLocked(b.throttle, REXCVAR_GET(wheel_throttle_deadzone_bottom),
                           REXCVAR_GET(wheel_throttle_deadzone_top));
  uint8_t left_trigger = ReadWheelPedalLocked(b.brake, REXCVAR_GET(wheel_brake_deadzone_bottom),
                                              REXCVAR_GET(wheel_brake_deadzone_top));
  uint16_t buttons = 0;
  int16_t gear = 0;  // neutral unless a shifter gate is held
  // FH1's wheel layouts read the clutch from |thumb_ry| (a split deadzone
  // applied radially with thumb_rx), and layout 1 shifts on LB/RB, so the
  // clutch pedal is an axis, not a button (found by Ty Crowther,
  // pinyon-shift PR #605).
  const uint8_t clutch = ReadWheelPedalLocked(b.clutch, REXCVAR_GET(wheel_clutch_deadzone_bottom),
                                              REXCVAR_GET(wheel_clutch_deadzone_top));
  pad.thumb_ry = static_cast<int16_t>(int32_t(clutch) * 32767 / 255);
  if (clutch >= 128) {
    buttons |= PadChordButtons(REXCVAR_GET(wheel_clutch_button));
  }
  for (const auto& binding : b.buttons) {
    WheelJoystick* joystick = FindWheelJoystick(binding.device, true);
    if (!joystick) {
      continue;
    }
    bool pressed = false;
    if (binding.axis >= 0) {
      WheelAxisBinding axis;
      axis.device = binding.device;
      axis.axis = binding.axis;
      axis.direction = binding.axis_direction;
      pressed = ReadWheelPedalLocked(axis) >= 128;
    } else if (binding.button >= 0) {
      pressed = binding.button < SDL_GetNumJoystickButtons(joystick->sdl) &&
                SDL_GetJoystickButton(joystick->sdl, binding.button);
    } else if (binding.hat >= 0 && binding.hat < SDL_GetNumJoystickHats(joystick->sdl)) {
      pressed = (SDL_GetJoystickHat(joystick->sdl, binding.hat) & binding.hat_mask) != 0;
    }
    if (!pressed) {
      continue;
    }
    if (binding.gear != 0) {
      gear = binding.gear;
    }
    buttons |= binding.xbuttons;
    if (binding.left_trigger)
      left_trigger = 0xFF;
    if (binding.right_trigger)
      right_trigger = 0xFF;
  }
  if (!REXCVAR_GET(guide_button)) {
    buttons &= ~uint16_t(X_INPUT_GAMEPAD_GUIDE);
  }
  pad.buttons = buttons;
  pad.left_trigger = left_trigger;
  pad.right_trigger = right_trigger;
  // With a shifter reported (below), FH1 reads the gear from the raw thumb_rx:
  // -1 reverse, 0 neutral, 1 and up the gear (pinyon-shift PR #605).
  pad.thumb_rx = gear;
  ffb_trace::NoteWheelInput(pad.thumb_lx, right_trigger, left_trigger, buttons);

  if (std::memcmp(&pad, &wheel_->state.gamepad, sizeof(pad)) != 0) {
    wheel_->state.gamepad = pad;
    wheel_->state_changed = true;
  }
  UpdateWheelSpringLocked();
  FlushGameEffectsLocked();
  if (REXCVAR_GET(wheel_log_inputs)) {
    LogWheelInputsLocked();
  }
}

void SDLInputDriver::LogWheelInputsLocked() {
  for (auto& joystick : wheel_joysticks_) {
    const int axes = SDL_GetNumJoystickAxes(joystick.sdl);
    const int buttons = SDL_GetNumJoystickButtons(joystick.sdl);
    if (joystick.logged_axes.size() != size_t(std::max(axes, 0))) {
      joystick.logged_axes.assign(size_t(std::max(axes, 0)), 0);
      for (int i = 0; i < axes; i++) {
        joystick.logged_axes[size_t(i)] = SDL_GetJoystickAxis(joystick.sdl, i);
      }
      joystick.logged_buttons.assign(size_t(std::max(buttons, 0)), false);
    }
    for (int i = 0; i < axes; i++) {
      const int16_t value = SDL_GetJoystickAxis(joystick.sdl, i);
      if (std::abs(int32_t(value) - int32_t(joystick.logged_axes[size_t(i)])) >= 8192) {
        REXLOG_INFO("SDL wheel input: \"{}\" axis{} = {}", joystick.name, i, value);
        joystick.logged_axes[size_t(i)] = value;
      }
    }
    for (int i = 0; i < buttons && size_t(i) < joystick.logged_buttons.size(); i++) {
      const bool down = SDL_GetJoystickButton(joystick.sdl, i);
      if (down != joystick.logged_buttons[size_t(i)]) {
        REXLOG_INFO("SDL wheel input: \"{}\" button {} {}", joystick.name, i, down ? "down" : "up");
        joystick.logged_buttons[size_t(i)] = down;
      }
    }
  }
}

X_RESULT SDLInputDriver::SetWheelVibrationLocked(const X_INPUT_VIBRATION& vibration) {
  WheelState& wheel = *wheel_;
  const uint16_t left = vibration.left_motor_speed;
  const uint16_t right = vibration.right_motor_speed;
  if (REXCVAR_GET(wheel_log_inputs) && (left != wheel.logged_left || right != wheel.logged_right)) {
    REXLOG_INFO("SDL wheel: title motors left {} right {}", left, right);
    wheel.logged_left = left;
    wheel.logged_right = right;
  }
  // Once the title drives the wheel with its own effects (XInputdFF), its
  // motor values are rumble and arrive through SetRumble; turning them into a
  // constant force as well would fight the title's.
  if (!game_effects_.empty() && GameEffectsEnabled()) {
    return X_ERROR_SUCCESS;
  }

  const WheelFfbMode mode = ParseWheelFfbMode(REXCVAR_GET(wheel_ffb_mode));
  const int strength = REXCVAR_GET(wheel_ffb_strength);
  WheelJoystick* base = FindWheelJoystick({}, true);

  if (mode == WheelFfbMode::kRumble || !(wheel.haptic_features & SDL_HAPTIC_CONSTANT)) {
    if (wheel.constant_effect >= 0 && wheel.last_force != 0) {
      SDL_StopHapticEffect(wheel.haptic, wheel.constant_effect);
      wheel.last_force = 0;
    }
    ffb_trace::NoteWheelOutput(0, mode == WheelFfbMode::kOff ? "off" : "rumble");
    if (mode == WheelFfbMode::kOff || !base) {
      return X_ERROR_SUCCESS;
    }
    // A base with neither constant force nor rumble still accepts the call.
    SDL_RumbleJoystick(base->sdl, uint16_t(uint32_t(left) * uint32_t(strength) / 100),
                       uint16_t(uint32_t(right) * uint32_t(strength) / 100), kRumbleDurationMs);
    return X_ERROR_SUCCESS;
  }

  const int16_t level =
      ConstantForceForMode(mode, left, right, strength, REXCVAR_GET(wheel_ffb_invert));
  ffb_trace::NoteWheelOutput(level, WheelFfbModeName(mode));
  if (level == wheel.last_force) {
    return X_ERROR_SUCCESS;
  }
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.type = SDL_HAPTIC_CONSTANT;
  effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.constant.length = SDL_HAPTIC_INFINITY;
  effect.constant.level = level;
  if (wheel.constant_effect < 0) {
    wheel.constant_effect = SDL_CreateHapticEffect(wheel.haptic, &effect);
    if (wheel.constant_effect < 0) {
      REXLOG_WARN("SDL wheel: could not create the constant force: {}", SDL_GetError());
      wheel.haptic_features &= ~uint32_t(SDL_HAPTIC_CONSTANT);
      return X_ERROR_FUNCTION_FAILED;
    }
    SDL_RunHapticEffect(wheel.haptic, wheel.constant_effect, 1);
  } else {
    if (!SDL_UpdateHapticEffect(wheel.haptic, wheel.constant_effect, &effect)) {
      return X_ERROR_FUNCTION_FAILED;
    }
    if (wheel.last_force == 0) {
      SDL_RunHapticEffect(wheel.haptic, wheel.constant_effect, 1);
    }
  }
  if (level == 0) {
    SDL_StopHapticEffect(wheel.haptic, wheel.constant_effect);
  }
  wheel.last_force = level;
  return X_ERROR_SUCCESS;
}

void SDLInputDriver::UpdateWheelSpringLocked() {
  WheelState& wheel = *wheel_;
  const int strength = REXCVAR_GET(wheel_ffb_strength);
  const bool invert = REXCVAR_GET(wheel_ffb_invert);
  const int condition_gains = REXCVAR_GET(wheel_ffb_spring) * 1000003 +
                              REXCVAR_GET(wheel_ffb_damper) * 1009 +
                              REXCVAR_GET(wheel_ffb_friction) * 31 + REXCVAR_GET(wheel_ffb_inertia);
  // A mode or strength change in F6 applies to running effects at once.
  if (!game_effects_.empty() &&
      (strength != applied_effect_strength_ || invert != applied_effect_invert_ ||
       condition_gains != applied_condition_gains_ ||
       GameEffectsEnabled() != applied_effects_enabled_)) {
    applied_condition_gains_ = condition_gains;
    SyncGameEffectsLocked();
  }
  if (!wheel.haptic || !(wheel.haptic_features & SDL_HAPTIC_SPRING)) {
    return;
  }
  const int percent = std::clamp(REXCVAR_GET(wheel_ffb_centering), 0, 100);
  if (percent == wheel.last_spring) {
    return;
  }
  wheel.last_spring = percent;
  if (percent == 0) {
    if (wheel.spring_effect >= 0) {
      SDL_StopHapticEffect(wheel.haptic, wheel.spring_effect);
    }
    return;
  }
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.type = SDL_HAPTIC_SPRING;
  effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.condition.length = SDL_HAPTIC_INFINITY;
  const uint16_t saturation = uint16_t(0xFFFF * percent / 100);
  const int16_t coefficient = int16_t(0x7FFF * percent / 100);
  effect.condition.right_sat[0] = saturation;
  effect.condition.left_sat[0] = saturation;
  effect.condition.right_coeff[0] = coefficient;
  effect.condition.left_coeff[0] = coefficient;
  if (wheel.spring_effect < 0) {
    wheel.spring_effect = SDL_CreateHapticEffect(wheel.haptic, &effect);
    if (wheel.spring_effect < 0) {
      REXLOG_WARN("SDL wheel: could not create the centring spring: {}", SDL_GetError());
      wheel.haptic_features &= ~uint32_t(SDL_HAPTIC_SPRING);
      return;
    }
  } else {
    SDL_UpdateHapticEffect(wheel.haptic, wheel.spring_effect, &effect);
  }
  SDL_RunHapticEffect(wheel.haptic, wheel.spring_effect, 1);
}

// ---------------------------------------------------------------------------
// The title's XInputdFF effects (xinputd_ffb::Sink). Calls come from guest
// threads; the haptic device belongs to the wheel and is used under the
// controllers lock like the rest of it.

namespace {

uint16_t SdlEffectType(uint32_t type) {
  switch (type) {
    case xinputd_ffb::kConstant:
      return SDL_HAPTIC_CONSTANT;
    case xinputd_ffb::kSpring:
      return SDL_HAPTIC_SPRING;
    case xinputd_ffb::kDamper:
      return SDL_HAPTIC_DAMPER;
    case xinputd_ffb::kInertia:
      return SDL_HAPTIC_INERTIA;
    case xinputd_ffb::kFriction:
      return SDL_HAPTIC_FRICTION;
    default:
      return 0;
  }
}

// Whether a condition differs from what the wheel has by less than a felt
// step: 3 % of the gain and of the effect's own scale (bytes, as FH1 sends,
// or DirectInput's 10000).
bool ConditionClose(const xinputd_ffb::Effect& a, const xinputd_ffb::Effect& b) {
  const int32_t fields[][2] = {{a.magnitude, b.magnitude},
                               {a.positive_coefficient, b.positive_coefficient},
                               {a.negative_coefficient, b.negative_coefficient},
                               {a.positive_saturation, b.positive_saturation},
                               {a.negative_saturation, b.negative_saturation},
                               {a.dead_band, b.dead_band}};
  int32_t largest = 0;
  for (const auto& f : fields) {
    largest = std::max({largest, std::abs(f[0]), std::abs(f[1])});
  }
  const int32_t step = largest <= 255 ? 4 : 300;
  if (a.type != b.type || std::abs(int32_t(a.gain) - int32_t(b.gain)) >= 8) {
    return false;
  }
  for (const auto& f : fields) {
    if (std::abs(f[0] - f[1]) >= step) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool SDLInputDriver::ReleaseFeedbackLocked(ControllerState& controller, bool stop_rumble) {
  // A pad keeps its last rumble when the process dies without closing it.
  bool sent = !stop_rumble || SDL_RumbleGamepad(controller.sdl, 0, 0, 0);
  if (controller.dualsense) {
    const dualsense::TriggerEffects slack;
    const auto payload = dualsense::BuildEffectPayload(slack);
    sent = SDL_SendGamepadEffect(controller.sdl, payload.data(), int(payload.size())) && sent;
    controller.triggers.Sent(slack, SteadyNs());
  }
  if (controller.has_colour) {
    for (size_t i = 0; i < controllers_.size(); ++i) {
      if (&controllers_[i] == &controller) {
        SDL_SetGamepadPlayerIndex(controller.sdl, static_cast<int>(i));
      }
    }
    controller.has_colour = false;
  }
  return sent;
}

void SDLInputDriver::ReleaseFeedback(bool crashing) {
  if (!SDL_Gamepad_initialized_) {
    return;
  }
  // The title's threads keep polling until the process is gone; nothing may
  // send effects again after this.
  feedback_released_ = true;
  if (!crashing) {
    auto guard = DrainAndLock();
    for (auto& controller : controllers_) {
      ReleaseFeedbackLocked(controller);
    }
    return;
  }
  // From a crash handler: the faulting thread may hold our lock, so work on
  // a fresh thread and give up on the lock after a while. SDL writes queued
  // reports one at a time (about every 10 ms) on its rumble thread, and a pad
  // busy with rumble can keep a release in that queue until the process is
  // gone, so the slack triggers are also written straight to the device,
  // first and once more after SDL's queue has had time to drain.
  struct Shared {
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;
  };
  auto shared = std::make_shared<Shared>();
  std::thread worker([this, shared] {
    std::unique_lock<std::mutex> lock(controllers_mutex_, std::defer_lock);
    for (int i = 0; i < 40 && !lock.try_lock(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto write_direct = [this] {
      for (auto& controller : controllers_) {
        if (controller.dualsense) {
          WriteSlackTriggersDirect(controller.hid_path, controller.bluetooth);
        }
      }
    };
    write_direct();
    // SDL's own calls are thread-safe, so a lock held by a dead thread does
    // not stop these.
    for (auto& controller : controllers_) {
      ReleaseFeedbackLocked(controller, /*stop_rumble=*/true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    write_direct();
    if (lock.owns_lock()) {
      lock.unlock();
    }
    std::lock_guard<std::mutex> done_lock(shared->mutex);
    shared->done = true;
    shared->done_cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> wait_lock(shared->mutex);
    shared->done_cv.wait_for(wait_lock, std::chrono::milliseconds(400),
                             [&] { return shared->done; });
  }
  worker.detach();
}

bool SDLInputDriver::GameEffectsEnabled() const {
  // "rumble" keeps only the title's vibration (XInputdFFSetRumble), so its
  // steering forces and condition effects stay off there as in "off".
  const WheelFfbMode mode = ParseWheelFfbMode(REXCVAR_GET(wheel_ffb_mode));
  return REXCVAR_GET(wheel_ffb_effects) && mode != WheelFfbMode::kOff &&
         mode != WheelFfbMode::kRumble;
}

WheelEffectScale SDLInputDriver::EffectScaleLocked() const {
  WheelEffectScale scale;
  scale.strength_percent = REXCVAR_GET(wheel_ffb_strength);
  scale.magnitude_max = REXCVAR_GET(wheel_ffb_magnitude_max);
  scale.saturation_max = REXCVAR_GET(wheel_ffb_saturation_max);
  // The title's steering force takes the sign of the steering it opposes;
  // DirectInput's (and SDL's) constant force runs the other way, so it is
  // negated, and wheel_ffb_invert reverses that for a wheel that differs
  // (pinyon-shift PR #605 found the same on a Thrustmaster T248).
  scale.invert = !REXCVAR_GET(wheel_ffb_invert);
  scale.min_force_percent = REXCVAR_GET(wheel_ffb_min_force);
  return scale;
}

void SDLInputDriver::StopGameEffectLocked(GameEffect& game) {
  if (wheel_ && wheel_->haptic && game.sdl_id >= 0 && game.sdl_running) {
    SDL_StopHapticEffect(wheel_->haptic, game.sdl_id);
  }
  game.sdl_running = false;
}

void SDLInputDriver::ApplyGameEffectLocked(GameEffect& game) {
  if (!wheel_ || !wheel_->haptic) {
    return;
  }
  if (!GameEffectsEnabled()) {
    StopGameEffectLocked(game);
    return;
  }
  SDL_Haptic* haptic = wheel_->haptic;
  const xinputd_ffb::Effect& e = game.effect;
  const uint16_t sdl_type = SdlEffectType(e.type);
  if (!sdl_type || !(wheel_->haptic_features & sdl_type)) {
    if (!game.unsupported) {
      REXLOG_WARN("SDL wheel: the wheel has no {} effect; the title's effect #{} is left out.",
                  xinputd_ffb::EffectTypeName(e.type), e.id);
      game.unsupported = true;
    }
    return;
  }
  WheelEffectScale scale = EffectScaleLocked();
  SDL_HapticEffect effect;
  SDL_zero(effect);
  if (e.type == xinputd_ffb::kConstant) {
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    effect.constant.level = EffectConstantLevel(e, scale);
    ffb_trace::NoteWheelOutput(effect.constant.level, "effects");
  } else {
    if (scale.saturation_max <= 0) {
      const bool bytes =
          e.positive_saturation <= 255 && e.negative_saturation <= 255 && e.dead_band <= 255;
      scale.saturation_max = bytes ? 255 : 10000;
    }
    switch (e.type) {
      case xinputd_ffb::kSpring:
        scale.condition_percent = REXCVAR_GET(wheel_ffb_spring);
        break;
      case xinputd_ffb::kDamper:
        scale.condition_percent = REXCVAR_GET(wheel_ffb_damper);
        break;
      case xinputd_ffb::kFriction:
        scale.condition_percent = REXCVAR_GET(wheel_ffb_friction);
        break;
      case xinputd_ffb::kInertia:
        scale.condition_percent = REXCVAR_GET(wheel_ffb_inertia);
        break;
      default:
        break;
    }
    const WheelCondition c = EffectCondition(e, scale);
    effect.type = sdl_type;
    effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    effect.condition.right_sat[0] = c.right_sat;
    effect.condition.left_sat[0] = c.left_sat;
    effect.condition.right_coeff[0] = c.right_coeff;
    effect.condition.left_coeff[0] = c.left_coeff;
    effect.condition.deadband[0] = c.deadband;
    effect.condition.center[0] = c.center;
  }
  if (game.sdl_id < 0) {
    game.sdl_id = SDL_CreateHapticEffect(haptic, &effect);
    if (game.sdl_id < 0) {
      REXLOG_WARN("SDL wheel: could not create the title's {} effect #{}: {}",
                  xinputd_ffb::EffectTypeName(e.type), e.id, SDL_GetError());
      game.unsupported = true;
      return;
    }
    game.sdl_running = false;
  } else if (!SDL_UpdateHapticEffect(haptic, game.sdl_id, &effect)) {
    return;
  }
  game.dirty = false;
  game.applied_ms = SDL_GetTicks();
  game.sent = e;
  if (game.running && !game.sdl_running) {
    game.sdl_running = SDL_RunHapticEffect(haptic, game.sdl_id, 1);
  } else if (!game.running && game.sdl_running) {
    StopGameEffectLocked(game);
  }
}

void SDLInputDriver::FlushGameEffectsLocked() {
  if (!wheel_ || !wheel_->haptic || game_effects_.empty()) {
    return;
  }
  const uint64_t now = SDL_GetTicks();
  for (auto& [id, game] : game_effects_) {
    const uint64_t interval = game.effect.type == xinputd_ffb::kConstant ? 10 : 40;
    if (game.dirty && now - game.applied_ms >= interval) {
      ApplyGameEffectLocked(game);
    }
  }
  // A T248's driver can drop an effect and still report it playing, so the
  // status check below misses it: start every running effect again every two
  // seconds regardless (pinyon-shift PR #605 does the same for its forces).
  if (now >= next_effect_refresh_ms_ && GameEffectsEnabled()) {
    next_effect_refresh_ms_ = now + 2000;
    for (auto& [id, game] : game_effects_) {
      if (game.running && game.sdl_running && game.sdl_id >= 0) {
        SDL_RunHapticEffect(wheel_->haptic, game.sdl_id, 1);
      }
    }
  }
  // Some bases drop a running effect (a Thrustmaster T248 stopped them on
  // reacquiring, pinyon-shift PR #605): start again what should be running.
  if (now >= next_effect_check_ms_ && (wheel_->haptic_features & SDL_HAPTIC_STATUS)) {
    next_effect_check_ms_ = now + 500;
    for (auto& [id, game] : game_effects_) {
      if (game.running && game.sdl_running && game.sdl_id >= 0 && GameEffectsEnabled() &&
          SDL_GetHapticEffectStatus(wheel_->haptic, game.sdl_id) == 0) {
        SDL_RunHapticEffect(wheel_->haptic, game.sdl_id, 1);
      }
    }
  }
}

void SDLInputDriver::SyncGameEffectsLocked() {
  applied_effect_strength_ = REXCVAR_GET(wheel_ffb_strength);
  applied_effect_invert_ = REXCVAR_GET(wheel_ffb_invert);
  applied_effects_enabled_ = GameEffectsEnabled();
  for (auto& [id, game] : game_effects_) {
    ApplyGameEffectLocked(game);
  }
  ApplyRumbleLocked();
}

void SDLInputDriver::ApplyRumbleLocked() {
  if (!wheel_ || !wheel_->haptic || !(wheel_->haptic_features & SDL_HAPTIC_SINE)) {
    return;
  }
  WheelState& wheel = *wheel_;
  const int64_t left =
      int64_t(game_rumble_left_) * std::clamp(REXCVAR_GET(wheel_ffb_rumble_road), 0, 100) / 100;
  const int64_t right =
      int64_t(game_rumble_right_) * std::clamp(REXCVAR_GET(wheel_ffb_rumble_engine), 0, 100) / 100;
  const int64_t strongest = std::max(left, right);
  int64_t magnitude = strongest * 32767 / 65535;
  magnitude = magnitude * std::clamp(REXCVAR_GET(wheel_ffb_rumble), 0, 100) / 100;
  magnitude = magnitude * std::clamp(REXCVAR_GET(wheel_ffb_strength), 0, 100) / 100;
  // The title's vibration plays in every mode but "off", "rumble" included.
  if (!REXCVAR_GET(wheel_ffb_effects) ||
      ParseWheelFfbMode(REXCVAR_GET(wheel_ffb_mode)) == WheelFfbMode::kOff) {
    magnitude = 0;
  }
  if (magnitude == 0) {
    if (wheel.rumble_effect >= 0) {
      SDL_StopHapticEffect(wheel.haptic, wheel.rumble_effect);
    }
    return;
  }
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.type = SDL_HAPTIC_SINE;
  effect.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.periodic.length = SDL_HAPTIC_INFINITY;
  // The right (small) motor buzzes, the left (large) one rumbles.
  effect.periodic.period = right > left ? 25 : 60;
  effect.periodic.magnitude = static_cast<int16_t>(magnitude);
  if (wheel.rumble_effect < 0) {
    wheel.rumble_effect = SDL_CreateHapticEffect(wheel.haptic, &effect);
    if (wheel.rumble_effect < 0) {
      wheel.haptic_features &= ~uint32_t(SDL_HAPTIC_SINE);
      return;
    }
  } else {
    SDL_UpdateHapticEffect(wheel.haptic, wheel.rumble_effect, &effect);
  }
  SDL_RunHapticEffect(wheel.haptic, wheel.rumble_effect, 1);
}

bool SDLInputDriver::HasForceFeedback() {
  auto guard = DrainAndLock();
  return wheel_ && wheel_->haptic;
}

void SDLInputDriver::SetEffect(const xinputd_ffb::Effect& effect) {
  auto guard = DrainAndLock();
  GameEffect& game = game_effects_[effect.id];
  if (game.sdl_id >= 0 && game.effect.type != effect.type && wheel_ && wheel_->haptic) {
    SDL_DestroyHapticEffect(wheel_->haptic, game.sdl_id);
    game.sdl_id = -1;
    game.sdl_running = false;
  }
  game.effect = effect;
  game.unsupported = false;
  ApplyGameEffectLocked(game);
}

void SDLInputDriver::UpdateEffect(const xinputd_ffb::Effect& effect, uint32_t) {
  auto guard = DrainAndLock();
  auto it = game_effects_.find(effect.id);
  if (it == game_effects_.end()) {
    // An update for an effect never set: treat it as set.
    GameEffect& game = game_effects_[effect.id];
    game.effect = effect;
    ApplyGameEffectLocked(game);
    return;
  }
  GameEffect& game = it->second;
  game.effect = effect;
  // A T248's driver stops honouring a condition effect retuned many times a
  // second while still reporting it playing; FH1 retunes its spring's gain
  // with speed, up to 30 times a second. Conditions change only by a felt step.
  if (effect.type != xinputd_ffb::kConstant && game.sdl_id >= 0 &&
      ConditionClose(effect, game.sent)) {
    game.dirty = false;
    return;
  }
  // The title updates its constant force every frame (60-120 times a second).
  // Some bases drop effects retuned that often (pinyon-shift PR #605: a T248 at
  // 100 Hz), so the constant force goes at most every 10 ms and the others every
  // 40 ms; the latest value is kept and sent by FlushGameEffectsLocked.
  const uint64_t interval = effect.type == xinputd_ffb::kConstant ? 10 : 40;
  if (SDL_GetTicks() - game.applied_ms >= interval) {
    ApplyGameEffectLocked(game);
  } else {
    game.dirty = true;
  }
}

void SDLInputDriver::EffectOperation(uint32_t id, uint32_t op) {
  auto guard = DrainAndLock();
  auto it = game_effects_.find(id);
  if (it == game_effects_.end()) {
    return;
  }
  it->second.running = op != 3;
  ApplyGameEffectLocked(it->second);
}

void SDLInputDriver::DeviceControl(uint32_t command) {
  auto guard = DrainAndLock();
  SDL_Haptic* haptic = wheel_ ? wheel_->haptic : nullptr;
  switch (command) {
    case xinputd_ffb::kStopAll:
    case xinputd_ffb::kDisableActuators:
      for (auto& [id, game] : game_effects_) {
        game.running = false;
        StopGameEffectLocked(game);
      }
      break;
    case xinputd_ffb::kReset:
      for (auto& [id, game] : game_effects_) {
        if (haptic && game.sdl_id >= 0) {
          SDL_DestroyHapticEffect(haptic, game.sdl_id);
        }
      }
      game_effects_.clear();
      break;
    case xinputd_ffb::kPause:
      if (haptic && (wheel_->haptic_features & SDL_HAPTIC_PAUSE))
        SDL_PauseHaptic(haptic);
      break;
    case xinputd_ffb::kContinue:
      if (haptic && (wheel_->haptic_features & SDL_HAPTIC_PAUSE))
        SDL_ResumeHaptic(haptic);
      break;
    default:
      break;
  }
}

void SDLInputDriver::SetDeviceGain(uint32_t gain) {
  auto guard = DrainAndLock();
  if (!wheel_ || !wheel_->haptic || !(wheel_->haptic_features & SDL_HAPTIC_GAIN)) {
    return;
  }
  const int percent =
      gain <= 255 ? int(gain * 100 / 255) : int(std::min<uint32_t>(gain, 10000) / 100);
  SDL_SetHapticGain(wheel_->haptic, percent);
}

void SDLInputDriver::SetRumble(uint16_t left, uint16_t right) {
  auto guard = DrainAndLock();
  if (left == game_rumble_left_ && right == game_rumble_right_) {
    return;
  }
  game_rumble_left_ = left;
  game_rumble_right_ = right;
  ApplyRumbleLocked();
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::FindController(DeviceId id) {
  if (id == DeviceId::kInvalid) {
    return nullptr;
  }
  for (auto& controller : controllers_) {
    if (controller.id == id) {
      return &controller;
    }
  }
  return nullptr;
}

DeviceId SDLInputDriver::AllocateDeviceId() {
  return static_cast<DeviceId>(next_device_id_++);
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_DEBUG("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;  // XINPUT_DEVTYPE_GAMEPAD
  // XINPUT_DEVSUBTYPE_WHEEL (2) for a wheel when asked, GAMEPAD (1) otherwise:
  // a title may read the subtype to pick its steering model (after
  // xenia-canary 103257c5aa); the wheel detection is the one the steering
  // shaping uses.
  c.sub_type = (REXCVAR_GET(input_report_wheel_subtype) &&
                (state.wheel_type || IsListedWheel(state.guid)))
                   ? 0x02
                   : 0x01;
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date, at most every
  // 4 ms: the title polls from several threads many times a frame, and a pump
  // queued again as soon as the last ended kept the UI thread busy for a whole
  // core on Android, where every pump goes through the activity lifecycle
  // semaphores. 250 pumps a second still outpace any display.
  constexpr int64_t kMinimumPumpIntervalNs = 4000000;
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - sdl_pumpevents_last_ns_.load(std::memory_order_relaxed) < kMinimumPumpIntervalNs) {
    return;
  }
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    sdl_pumpevents_last_ns_.store(now, std::memory_order_relaxed);
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
