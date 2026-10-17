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

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/dualsense.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  /// The user's merged physical pads for host-drawn UI: live even while the
  /// active callback blocks the guest's input, and without keyboard-and-mouse
  /// emulation (host UI reads the keyboard through window events).
  X_RESULT GetHostPadState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  /// Every device the drivers have open, in ordinal order.
  std::vector<DeviceInfo> Devices();
  /// The device a guest user last moved, else the user's first device.
  std::optional<DeviceInfo> ActiveDevice(uint32_t user_index);

  /// The player's car this frame, for DualSense trigger effects and the RPM
  /// lightbar (guest user 0). Treated as gone 250 ms after the last call.
  void SetCarTelemetry(const dualsense::CarTelemetry& telemetry);
  /// False while the player is not driving (menus, loading): the triggers go
  /// slack. True by default, so a game that never calls it still gets them.
  void SetFeedbackDriving(bool driving);
  /// Triggers slack and lightbars back to the player colour on every pad, for
  /// good: for quitting and crashing, nothing is sent after it.
  void ReleaseFeedback(bool crashing = false);
  /// ReleaseFeedback on the live input system, for crash handlers.
  static void ReleaseAllFeedback(bool crashing);
  /// The input system the runtime created, or null before and after it.
  static InputSystem* Live();

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  X_RESULT GetMergedState(uint32_t user_index, bool host_pads_only, X_INPUT_STATE* out_state);
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;
  /// Runs the trigger model and the lightbar for one guest read.
  void UpdateFeedbackLocked(uint32_t user_index, const std::vector<DeviceId>& ids,
                            const X_INPUT_STATE& guest_state);

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  // The guest's polling threads and host UI on the UI thread share the device
  // list, the assignment and the active-device tracker.
  std::mutex devices_mutex_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  std::function<bool()> active_callback_;

  // DualSense feedback, per guest user.
  std::array<dualsense::TriggerModel, kMaxGuestUsers> trigger_models_;
  std::array<std::chrono::steady_clock::time_point, kMaxGuestUsers> last_feedback_tick_ = {};
  std::array<X_INPUT_VIBRATION, kMaxGuestUsers> last_vibration_ = {};
  std::mutex telemetry_mutex_;
  dualsense::CarTelemetry telemetry_;
  std::chrono::steady_clock::time_point telemetry_time_ = {};
  std::atomic<bool> feedback_driving_{true};
  std::chrono::steady_clock::time_point last_wheel_log_ = {};
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
