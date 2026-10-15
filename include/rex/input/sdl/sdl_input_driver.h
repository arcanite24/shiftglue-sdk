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

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/input/input_driver.h>

#include <string>

#include <SDL3/SDL.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pad_gyro_steering);
REXCVAR_DECLARE(int32_t, pad_gyro_sensitivity);

#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

/// Steam launched the game with Steam Input wrapping a DualSense: SDL is told
/// to skip the real pad, and the game sees Steam's virtual Xbox pad instead.
bool SteamInputHidesDualSense();

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;
  bool SetDeviceTriggerEffects(DeviceId id, const dualsense::TriggerEffects& effects) override;
  bool SetDeviceLightbar(DeviceId id, const dualsense::Rgb* colour) override;
  void ReleaseFeedback(bool crashing) override;

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    // Per pad rather than per guest user, so it survives reassignment.
    KeystrokeState keystroke;
    // A racing wheel: SDL's joystick type, or listed in hid_wheel_guids.
    bool wheel_type;
    std::string guid;
    // Identity (DS-0.1), read once at open.
    PadFamily family = PadFamily::kUnknown;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    bool steam_virtual = false;
    bool dualsense = false;  // trigger effects allowed
    bool lightbar = false;
    bool motion = false;
    dualsense::EffectChannel triggers;
    // The HID device behind SDL's HIDAPI driver, for the crash path's
    // direct release, and whether it is a Bluetooth link.
    std::string hid_path;
    bool bluetooth = false;
    // Lightbar: has_colour false = SDL's player colour.
    bool has_colour = false;
    dualsense::Rgb colour;
    // Motion steering (DS-6): sensors on, the tilt estimate and its centre.
    bool sensors_on = false;
    bool tilt_valid = false;
    float tilt_rad = 0.0f;  // counter-clockwise roll seen from the player
    float tilt_centre_rad = 0.0f;
    uint64_t gyro_timestamp_ns = 0;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);
  void OnControllerSensorLocked(const SDL_Event& event);
  void UpdateSensorsLocked(ControllerState& controller);
  // Slack triggers and the player colour; `stop_rumble` also stops the
  // motors (the crash path, where SDL never closes the pad).
  bool ReleaseFeedbackLocked(ControllerState& controller, bool stop_rumble = false);
  // The H-pattern shifter's gear value for a wheel's thumb_rx, or nullopt
  // without a configured, connected shifter (hid_shifter_guid).
  std::optional<int16_t> ReadShifterLocked();
  void ApplyPlayerLedHint();

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* FindController(DeviceId id);
  DeviceId AllocateDeviceId();
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  SDL_Joystick* shifter_ = nullptr;
  std::string shifter_guid_;
  uint64_t shifter_checked_ns_ = 0;
  int16_t shifter_gear_ = 0;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  // steady_clock nanoseconds of the last queued pump (QueueControllerUpdate).
  std::atomic<int64_t> sdl_pumpevents_last_ns_{0};
  // Appended in connection order, never re-sorted, and unbounded: the
  // assignment decides how many the guest sees.
  std::vector<ControllerState> controllers_;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  // Never rewound, so a handle held past a disconnect resolves to nothing.
  uint64_t next_device_id_ = 1;
  // Window focus: trigger effects and lightbar colours are held back while
  // another window has it.
  std::atomic<bool> focused_{true};
  // Set by ReleaseFeedback and on close: no effect is sent after it.
  std::atomic<bool> feedback_released_{false};
  int player_led_hint_ = -1;
  uint64_t log_window_start_ns_ = 0;
  uint64_t log_window_reports_ = 0;
};

}  // namespace rex::input::sdl
