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
#include <cctype>
#include <chrono>
#include <array>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <thread>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/dualsense.h>
#include <rex/input/flags.h>
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

SDLInputDriver::~SDLInputDriver() {}

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
      // Racing wheel force feedback. Without it wheels still drive.
      if (SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        sdl_haptic_initialized_ = true;
      } else {
        REXLOG_WARN("SDL: haptic subsystem unavailable, no wheel force feedback: {}",
                    SDL_GetError());
      }
      if (SteamInputHidesDualSense()) {
        REXLOG_INFO(
            "SDL pad: Steam Input hides a DualSense from the game (it is listed in "
            "SDL_GAMECONTROLLER_IGNORE_DEVICES); its triggers, lightbar and motion stay with "
            "Steam until Steam Input is off for the game.");
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
        const int loaded = LoadUserMappings(user_mappings);
        REXLOG_INFO("SDL: loaded {} user mappings.", loaded);
      }
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
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
        CloseWheelHapticLocked(controller);
        SDL_CloseGamepad(controller.sdl);
      }
      controllers_.clear();
    }
    if (shifter_) {
      SDL_CloseJoystick(shifter_);
      shifter_ = nullptr;
    }
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
    info.force_feedback = controller.haptic_capable;
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
    // An H-pattern shifter's gear, where the title reads it for a wheel.
    if (controller->caps.sub_type == 0x02) {
      if (const auto gear = ReadShifterLocked()) {
        pad.thumb_rx = *gear;
      }
    }
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
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // A wheel under force feedback gets the motors as vibration through the
  // force model; SDL's own rumble would fight the haptic effects for the
  // device.
  if (controller->haptic && REXCVAR_GET(hid_wheel_ffb)) {
    return X_ERROR_SUCCESS;
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
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(controller->state.gamepad.buttons)) |
                   AnalogToKeyfield(controller->state.gamepad))
                : uint64_t(0);
  KeystrokeState& last = controller->keystroke;

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
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
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
    int haptics = 0;
    if (sdl_haptic_initialized_)
      SDL_free(SDL_GetHaptics(&haptics));
    state.haptic_capable =
        sdl_haptic_initialized_ && (SDL_IsJoystickHaptic(joystick) || haptics > 0);
    REXLOG_INFO("SDL OnControllerDeviceAdded: wheel force feedback {}.",
                state.haptic_capable ? "available" : "not available");
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
  CloseWheelHapticLocked(controllers_.at(*idx));
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

// The damper, vibration and gain change at most this often (25 a second),
// and only by a felt step: a T248's driver stopped honouring a condition
// effect retuned up to 100 times a second while still reporting it playing.
constexpr uint64_t kWheelForceIntervalNs = 40'000'000;
// How often a running wheel's effects are checked (or, without status
// support, simply started again), and how often they are started again in
// any case, since a driver can drop an effect and still report it playing.
constexpr uint64_t kWheelEffectCheckNs = 250'000'000;
constexpr uint64_t kWheelEffectRefreshNs = 2'000'000'000;
// The constant force (the title's steering torque) changes faster: road
// noise and the torque's fall past the tyres' peak are in it.
constexpr uint64_t kWheelConstantIntervalNs = 10'000'000;
constexpr float kWheelConstantStep = 0.004f;

SDL_HapticEffect WheelCondition(uint16_t type, float coefficient) {
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.condition.type = type;
  effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.condition.length = SDL_HAPTIC_INFINITY;
  const auto coeff =
      static_cast<int16_t>(std::lround(std::clamp(coefficient, 0.0f, 1.0f) * 32767.0f));
  effect.condition.right_coeff[0] = coeff;
  effect.condition.left_coeff[0] = coeff;
  effect.condition.right_sat[0] = 0xFFFF;
  effect.condition.left_sat[0] = 0xFFFF;
  return effect;
}

SDL_HapticEffect WheelVibration(float level, uint16_t period_ms) {
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.periodic.type = SDL_HAPTIC_SINE;
  effect.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.periodic.length = SDL_HAPTIC_INFINITY;
  effect.periodic.period = period_ms ? period_ms : 50;
  effect.periodic.magnitude =
      static_cast<int16_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 32767.0f));
  return effect;
}

// Racing wheel makers by USB vendor, as their force feedback devices are
// named.
const char* WheelMaker(uint16_t vendor_id) {
  switch (vendor_id) {
    case 0x044F:
      return "thrustmaster";
    case 0x046D:
      return "logitech";
    case 0x0EB7:
      return "fanatec";
    case 0x346E:
      return "moza";
    case 0x16D0:
      return "simucube";
    case 0x11FF:
      return "pxn";
    default:
      return nullptr;
  }
}

std::string Lower(std::string text) {
  for (char& c : text)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// SDL hides a joystick's haptic side once it has a gamepad mapping
// (SDL_IsJoystickHaptic is false for gamepads), but its haptic device list
// still has it. Pick the wheel's entry from that list: the name chosen in
// hid_wheel_ffb_device, else the wheel maker's, else the only one.
SDL_HapticID FindWheelHapticId(uint16_t vendor_id, const std::vector<SDL_HapticID>& in_use) {
  int count = 0;
  SDL_HapticID* ids = SDL_GetHaptics(&count);
  if (!ids)
    return 0;
  std::vector<std::pair<SDL_HapticID, std::string>> free;
  for (int i = 0; i < count; ++i) {
    if (std::find(in_use.begin(), in_use.end(), ids[i]) != in_use.end())
      continue;
    const char* name = SDL_GetHapticNameForID(ids[i]);
    free.emplace_back(ids[i], Lower(name ? name : ""));
  }
  SDL_free(ids);
  const std::string wanted = Lower(REXCVAR_GET(hid_wheel_ffb_device));
  const char* maker = WheelMaker(vendor_id);
  for (const std::string& needle : {wanted, std::string(maker ? maker : "")}) {
    if (needle.empty())
      continue;
    for (const auto& [id, name] : free) {
      if (name.find(needle) != std::string::npos)
        return id;
    }
  }
  return free.size() == 1 && wanted.empty() ? free.front().first : 0;
}

SDL_HapticEffect WheelConstant(float level) {
  SDL_HapticEffect effect;
  SDL_zero(effect);
  effect.constant.type = SDL_HAPTIC_CONSTANT;
  effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
  effect.constant.length = SDL_HAPTIC_INFINITY;
  effect.constant.level =
      static_cast<int16_t>(std::lround(std::clamp(level, -1.0f, 1.0f) * 32767.0f));
  return effect;
}

bool ForcesClose(const wheel_ffb::Forces& a, const wheel_ffb::Forces& b) {
  constexpr float kStep = 0.03f;
  return a.active == b.active && std::fabs(a.gain - b.gain) < kStep &&
         std::fabs(a.damper - b.damper) < kStep && std::fabs(a.vibration - b.vibration) < kStep &&
         a.vibration_period_ms == b.vibration_period_ms;
}

}  // namespace

std::optional<int16_t> SDLInputDriver::ReadShifterLocked() {
  const std::string wanted = REXCVAR_GET(hid_shifter_guid);
  if (wanted.empty()) {
    if (shifter_) {
      SDL_CloseJoystick(shifter_);
      shifter_ = nullptr;
    }
    return std::nullopt;
  }
  if (shifter_ && (wanted != shifter_guid_ || !SDL_JoystickConnected(shifter_))) {
    SDL_CloseJoystick(shifter_);
    shifter_ = nullptr;
  }
  const uint64_t now = SteadyNs();
  if (!shifter_ && now - shifter_checked_ns_ >= 1'000'000'000ull) {
    // Look for it at most once a second: it may be plugged in later.
    shifter_checked_ns_ = now;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count && !shifter_; ++i) {
      char guid[33] = {};
      SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, int(sizeof(guid)));
      if (wanted == guid) {
        shifter_ = SDL_OpenJoystick(ids[i]);
        shifter_guid_ = wanted;
        if (shifter_) {
          REXLOG_INFO("Shifter: opened \"{}\" ({} buttons)", SDL_GetJoystickName(shifter_),
                      SDL_GetNumJoystickButtons(shifter_));
        }
      }
    }
    SDL_free(ids);
  }
  if (!shifter_) {
    return std::nullopt;
  }
  uint64_t down = 0;
  for (int b = 0, n = std::min(SDL_GetNumJoystickButtons(shifter_), 64); b < n; ++b) {
    if (SDL_GetJoystickButton(shifter_, b))
      down |= uint64_t(1) << b;
  }
  const int16_t gear = ShifterGearValue(ParseShifterGears(REXCVAR_GET(hid_shifter_gears)), down);
  if (gear != shifter_gear_) {
    REXLOG_INFO("Shifter: gear {} (buttons 0x{:X})",
                gear < 0    ? std::string("R")
                : gear == 0 ? std::string("N")
                            : std::to_string(gear),
                down);
    shifter_gear_ = gear;
  }
  return gear;
}

bool SDLInputDriver::OpenWheelHapticLocked(ControllerState& controller) {
  if (controller.haptic)
    return true;
  SDL_Joystick* joystick = SDL_GetGamepadJoystick(controller.sdl);
  SDL_Haptic* haptic = nullptr;
  if (joystick && SDL_IsJoystickHaptic(joystick)) {
    haptic = SDL_OpenHapticFromJoystick(joystick);
  } else {
    std::vector<SDL_HapticID> in_use;
    for (const auto& other : controllers_) {
      if (other.haptic)
        in_use.push_back(SDL_GetHapticID(other.haptic));
    }
    const SDL_HapticID id = FindWheelHapticId(controller.vendor_id, in_use);
    if (id) {
      haptic = SDL_OpenHaptic(id);
      if (haptic) {
        REXLOG_INFO("Wheel FFB: using haptic device \"{}\" for device {}",
                    SDL_GetHapticName(haptic) ? SDL_GetHapticName(haptic) : "",
                    static_cast<uint64_t>(controller.id));
      }
    } else {
      SDL_SetError("no force feedback device matches this wheel (set hid_wheel_ffb_device)");
    }
  }
  if (!haptic) {
    REXLOG_WARN("Wheel FFB: cannot open the haptic device: {}", SDL_GetError());
    controller.haptic_capable = false;
    return false;
  }
  const uint32_t features = SDL_GetHapticFeatures(haptic);
  controller.haptic = haptic;
  controller.ffb_damper = controller.ffb_vibration = controller.ffb_constant = -1;
  if (features & SDL_HAPTIC_CONSTANT) {
    SDL_HapticEffect effect = WheelConstant(0.0f);
    controller.ffb_constant = SDL_CreateHapticEffect(haptic, &effect);
  }
  if (features & SDL_HAPTIC_DAMPER) {
    SDL_HapticEffect effect = WheelCondition(SDL_HAPTIC_DAMPER, 0.0f);
    controller.ffb_damper = SDL_CreateHapticEffect(haptic, &effect);
  }
  if (features & SDL_HAPTIC_SINE) {
    SDL_HapticEffect effect = WheelVibration(0.0f, 50);
    controller.ffb_vibration = SDL_CreateHapticEffect(haptic, &effect);
  }
  if (features & SDL_HAPTIC_AUTOCENTER) {
    // The title's torque centres the wheel; the device's own centring would
    // add to it.
    SDL_SetHapticAutocenter(haptic, 0);
  }
  controller.ffb_running = false;
  controller.ffb_vibrating = false;
  controller.ffb_sent = {};
  controller.ffb_status = (features & SDL_HAPTIC_STATUS) != 0;
  controller.ffb_checked_ns = 0;
  controller.ffb_restarts = 0;
  controller.ffb_constant_running = false;
  controller.ffb_constant_sent = 0.0f;
  REXLOG_INFO("Wheel FFB: opened device {} (features 0x{:X}): constant {} damper {} vibration {}",
              static_cast<uint64_t>(controller.id), features, controller.ffb_constant,
              controller.ffb_damper, controller.ffb_vibration);
  // The title's torque needs a constant force; without one the wheel stays free.
  return controller.ffb_constant >= 0;
}

void SDLInputDriver::StopWheelForcesLocked(ControllerState& controller) {
  if (!controller.haptic)
    return;
  SDL_StopHapticEffects(controller.haptic);
  controller.ffb_running = false;
  controller.ffb_vibrating = false;
  controller.ffb_constant_running = false;
  controller.ffb_sent = {};
}

void SDLInputDriver::CloseWheelHapticLocked(ControllerState& controller) {
  if (!controller.haptic)
    return;
  StopWheelForcesLocked(controller);
  for (int effect : {controller.ffb_damper, controller.ffb_vibration, controller.ffb_constant}) {
    if (effect >= 0)
      SDL_DestroyHapticEffect(controller.haptic, effect);
  }
  SDL_CloseHaptic(controller.haptic);
  controller.haptic = nullptr;
  controller.ffb_damper = controller.ffb_vibration = controller.ffb_constant = -1;
  REXLOG_INFO("Wheel FFB: closed device {}", static_cast<uint64_t>(controller.id));
}

bool SDLInputDriver::SetDeviceWheelForces(DeviceId id, const wheel_ffb::Forces& forces) {
  if (!SDL_Gamepad_initialized_ || !sdl_haptic_initialized_) {
    return false;
  }
  auto guard = DrainAndLock();
  auto controller = FindController(id);
  if (!controller || !controller->haptic_capable) {
    return false;
  }
  if (feedback_released_) {
    return true;  // quitting or crashing: the wheel stays free
  }
  // Free while another window has focus (the release went out in
  // OnLostFocus).
  const wheel_ffb::Forces wanted = focused_ ? forces : wheel_ffb::Forces{};
  if (!wanted.active) {
    if (controller->haptic) {
      if (controller->ffb_running)
        StopWheelForcesLocked(*controller);
      // Turned off: hand the device back, with its own centring.
      if (!REXCVAR_GET(hid_wheel_ffb))
        CloseWheelHapticLocked(*controller);
    }
    return true;
  }
  if (!controller->haptic && !OpenWheelHapticLocked(*controller)) {
    return false;
  }
  const uint64_t now = SteadyNs();
  SDL_Haptic* haptic = controller->haptic;
  if (controller->ffb_running && now - controller->ffb_checked_ns >= kWheelEffectCheckNs) {
    // When SDL re-acquires the device after DirectInput reported it lost
    // (seen on a T248 on rough surfaces and in crashes), every effect is
    // stopped while our updates keep succeeding, so the force went dead until
    // a menu restarted it. Start the effects again when they stopped.
    controller->ffb_checked_ns = now;
    const bool playing =
        controller->ffb_status && SDL_GetHapticEffectStatus(haptic, controller->ffb_constant);
    const bool refresh = now - controller->ffb_refreshed_ns >= kWheelEffectRefreshNs;
    if (refresh && REXCVAR_GET(hid_wheel_ffb_log)) {
      REXLOG_INFO("Wheel FFB: device {} constant force reported {}",
                  static_cast<uint64_t>(controller->id),
                  !controller->ffb_status ? "unknown"
                  : playing               ? "playing"
                                          : "stopped");
    }
    if (!playing || refresh) {
      controller->ffb_refreshed_ns = now;
      if (controller->ffb_damper >= 0) {
        SDL_RunHapticEffect(haptic, controller->ffb_damper, 1);
      }
      if (controller->ffb_vibrating && controller->ffb_vibration >= 0) {
        SDL_RunHapticEffect(haptic, controller->ffb_vibration, 1);
      }
      if (controller->ffb_constant_running && controller->ffb_constant >= 0) {
        SDL_RunHapticEffect(haptic, controller->ffb_constant, 1);
      }
      if (controller->ffb_status && !playing && controller->ffb_restarts++ < 20) {
        REXLOG_INFO("Wheel FFB: effects had stopped on device {}; started them again",
                    static_cast<uint64_t>(controller->id));
      }
    }
  }
  bool ok = true;
  if (controller->ffb_constant >= 0 &&
      (!controller->ffb_constant_running ||
       (now - controller->ffb_constant_ns >= kWheelConstantIntervalNs &&
        std::fabs(wanted.constant - controller->ffb_constant_sent) >= kWheelConstantStep))) {
    SDL_HapticEffect effect = WheelConstant(wanted.constant);
    ok &= SDL_UpdateHapticEffect(haptic, controller->ffb_constant, &effect);
    if (!controller->ffb_constant_running) {
      ok &= SDL_RunHapticEffect(haptic, controller->ffb_constant, 1);
      controller->ffb_constant_running = true;
    }
    controller->ffb_constant_sent = wanted.constant;
    controller->ffb_constant_ns = now;
  }
  if (controller->ffb_running && (now - controller->ffb_sent_ns < kWheelForceIntervalNs ||
                                  ForcesClose(wanted, controller->ffb_sent))) {
    if (!ok) {
      REXLOG_WARN("Wheel FFB: constant force update failed: {}", SDL_GetError());
    }
    return ok;
  }
  if (!controller->ffb_running || std::fabs(wanted.gain - controller->ffb_sent.gain) >= 0.01f) {
    ok &= SDL_SetHapticGain(haptic, static_cast<int>(std::lround(wanted.gain * 100.0f)));
  }
  if (controller->ffb_damper >= 0) {
    SDL_HapticEffect effect = WheelCondition(SDL_HAPTIC_DAMPER, wanted.damper);
    ok &= SDL_UpdateHapticEffect(haptic, controller->ffb_damper, &effect);
  }
  if (!controller->ffb_running) {
    if (controller->ffb_damper >= 0) {
      ok &= SDL_RunHapticEffect(haptic, controller->ffb_damper, 1);
    }
    controller->ffb_running = true;
  }
  if (controller->ffb_vibration >= 0) {
    if (wanted.vibration > 0.0f) {
      SDL_HapticEffect effect = WheelVibration(wanted.vibration, wanted.vibration_period_ms);
      ok &= SDL_UpdateHapticEffect(haptic, controller->ffb_vibration, &effect);
      if (!controller->ffb_vibrating) {
        ok &= SDL_RunHapticEffect(haptic, controller->ffb_vibration, 1);
        controller->ffb_vibrating = true;
      }
    } else if (controller->ffb_vibrating) {
      ok &= SDL_StopHapticEffect(haptic, controller->ffb_vibration);
      controller->ffb_vibrating = false;
    }
  }
  if (!ok) {
    REXLOG_WARN("Wheel FFB: effect update failed: {}", SDL_GetError());
  }
  controller->ffb_sent = wanted;
  controller->ffb_sent_ns = now;
  return ok;
}

bool SDLInputDriver::ReleaseFeedbackLocked(ControllerState& controller, bool stop_rumble) {
  // A pad keeps its last rumble when the process dies without closing it.
  bool sent = !stop_rumble || SDL_RumbleGamepad(controller.sdl, 0, 0, 0);
  StopWheelForcesLocked(controller);
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
  // With the wheel subtype the title also checks the force feedback flag
  // before it takes its wheel path (FB-2.1).
  if (c.sub_type == 0x02) {
    cap_flags |= X_INPUT_CAPS_FFB_SUPPORTED;
  }
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  if (c.sub_type == 0x02) {
    // A wheel reporting 900 in its capabilities' left stick X is a
    // 900-degree wheel to FH1, which then maps steering linearly with no
    // reduction at speed. Its default wheel mapping keeps 35 % of the
    // steering by 30 m/s and halves small inputs, made for wheels with a
    // short rotation (sub_82D2FD30; the caps value is read in sub_82BFBA90).
    c.gamepad.thumb_lx = 900;
  }
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
