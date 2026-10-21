/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>
#include <string>

#include <fmt/format.h>

#include <rex/input/ffb_trace.h>
#include <rex/input/xinputd_ffb.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

u32 HidReadKeys_entry(u32 unk1, u32 unk2, u32 unk3) {
  /* TODO(gibbed):
   * Games check for the following errors:
   *   0xC000009D - translated to 0x48F  - ERROR_DEVICE_NOT_CONNECTED
   *   0x103      - translated to 0x10D2 - ERROR_EMPTY
   * Other errors appear to be ignored?
   *
   * unk1 is 0
   * unk2 is a pointer to &unk3[2], possibly a 6-byte buffer
   * unk3 is a pointer to a 20-byte buffer
   */
  return 0xC000009D;
}

u32 HidGetCapabilities_entry(u32 unk1, u32 caps_ptr) {
  // HidGetCapabilities - ordinal 0x01EA
  // Returns capabilities for HID device (keyboard/mouse)
  // Not supported in rexglue - return unsuccessful
  return X_STATUS_UNSUCCESSFUL;
}

u32 HidGetLastInputTime_entry(mapped_u32 time_ptr) {
  // HidGetLastInputTime - ordinal 0x01F1
  // Returns the last time any HID input was received
  if (time_ptr) {
    *time_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}

u32 HidReadMouseChanges_entry(u32 unk1, u32 unk2) {
  // HidReadMouseChanges - ordinal 0x0273
  // Reads mouse input changes - not supported in rexglue
  return X_STATUS_UNSUCCESSFUL;
}

// ---------------------------------------------------------------------------
// Wheel force feedback (XInputdFF*). A title that finds an FFB wheel
// (subtype 2, flags bit 0) drives it through these instead of
// XamInputSetState. Forza Horizon's wheel set-up (its input module's state
// machine) asks GetDeviceInfo for at least five effect slots, resets the
// device (DeviceControl 3), downloads five effects (inertia, friction,
// damper, spring, constant force), starts them, then every frame updates the
// constant force and the speed-dependent spring and sends rumble through
// SetRumble. Every call is asynchronous: it completes an XOVERLAPPED the
// title polls until its status is no longer ERROR_IO_PENDING (997). The stubs
// left that and the device info unwritten, so the title retried the set-up
// forever and never drew a frame.
//
// The guest's descriptors go to the wheel driver (rex/input/xinputd_ffb.h),
// which plays them on the host wheel. Which of the effect exports is which
// ordinal was read from the title's use, not documented, so set, update and
// operation calls are also recognised by their arguments.

namespace {

namespace ffb = rex::input::xinputd_ffb;
namespace ffb_trace = rex::input::ffb_trace;

constexpr uint32_t kOverlappedSize = 28;

uint32_t GuestCaller() {
  auto* thread_state = rex::runtime::ThreadState::Get();
  return thread_state ? static_cast<uint32_t>(thread_state->context()->lr) : 0;
}

bool GuestRange(uint32_t address, uint32_t size) {
  if (!address || size == 0 || address + size < address) {
    return false;
  }
  auto* memory = REX_KERNEL_MEMORY();
  return memory->LookupHeap(address) && memory->LookupHeap(address + size - 1);
}

const uint8_t* GuestBytes(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
}

uint32_t LoadGuest32(uint32_t address) {
  const uint8_t* p = GuestBytes(address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreGuest32(uint32_t address, uint32_t value) {
  uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

bool OverlappedOrNull(uint32_t address) {
  return address == 0 || GuestRange(address, kOverlappedSize);
}

void CompleteOverlapped(uint32_t overlapped) {
  if (overlapped && GuestRange(overlapped, kOverlappedSize)) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
  }
}

// The first few calls of each kind go to the runtime log as well as the trace.
bool LogThis(std::atomic<uint32_t>& count, uint32_t limit = 12) {
  return count.fetch_add(1, std::memory_order_relaxed) < limit;
}

bool PlausibleDescriptor(uint32_t address) {
  if (!GuestRange(address, ffb::kDescriptorSize)) {
    return false;
  }
  const uint32_t id = LoadGuest32(address);
  const uint32_t type = LoadGuest32(address + 4);
  return id >= 1 && id <= 64 && type >= 1 && type <= 12;
}

bool PlausibleOperations(uint32_t address, uint32_t count) {
  if (count == 0 || count > 64 || !GuestRange(address, count * 8)) {
    return false;
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t id = LoadGuest32(address + i * 8);
    const uint32_t op = LoadGuest32(address + i * 8 + 4);
    if (id < 1 || id > 64 || op < 1 || op > 3) {
      return false;
    }
  }
  return true;
}

enum class EffectCall { kSet, kUpdate, kOperation };

// set:       (handle, descriptors, count, overlapped)
// update:    (handle, descriptor, flags, overlapped)   flags 0x100 / 0x400 in Forza Horizon
// operation: (handle, {id, op} pairs, count, flag, overlapped)
bool Fits(EffectCall kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  switch (kind) {
    case EffectCall::kSet:
      return b >= 1 && b <= 64 && PlausibleDescriptor(a) && OverlappedOrNull(c) &&
             (b == 1 || GuestRange(a, b * ffb::kDescriptorSize));
    case EffectCall::kUpdate:
      return (b == 0 || b >= 0x40) && PlausibleDescriptor(a) && OverlappedOrNull(c);
    case EffectCall::kOperation:
      return c <= 16 && PlausibleOperations(a, b) && OverlappedOrNull(d);
  }
  return false;
}

std::atomic<uint32_t> g_log_set{0}, g_log_update{0}, g_log_op{0}, g_log_unknown{0};

void DispatchEffectCall(EffectCall kind, uint32_t caller, ffb::Sink* sink, uint32_t a, uint32_t b,
                        uint32_t c, uint32_t d);

uint32_t HandleEffectCall(EffectCall named, const char* export_name, uint32_t handle, uint32_t a,
                          uint32_t b, uint32_t c, uint32_t d) {
  const uint32_t caller = GuestCaller();
  EffectCall kind = named;
  if (!Fits(kind, a, b, c, d)) {
    bool found = false;
    for (EffectCall other : {EffectCall::kSet, EffectCall::kUpdate, EffectCall::kOperation}) {
      if (other != named && Fits(other, a, b, c, d)) {
        kind = other;
        found = true;
        break;
      }
    }
    if (!found) {
      const std::string detail = fmt::format("{} args {:08X} {:08X} {:08X} {:08X} {:08X}",
                                             export_name, handle, a, b, c, d);
      if (LogThis(g_log_unknown)) {
        REXKRNL_WARN("Wheel FFB: unrecognised call: {} (from {:08X})", detail, caller);
      }
      ffb_trace::RecordWheelCall("ffb_unknown", caller, a, b, c, 0, detail);
      // No overlapped can be identified to complete, so fail the call rather
      // than leave a caller waiting on one that never signals.
      return X_STATUS_INVALID_PARAMETER;
    }
    REXKRNL_INFO("Wheel FFB: {} called with the arguments of another effect call; handled as such",
                 export_name);
  }

  // The sink stays registered until the call is done (the driver clears it on exit).
  ffb::WithSink([&](ffb::Sink* sink) { DispatchEffectCall(kind, caller, sink, a, b, c, d); });
  return X_STATUS_SUCCESS;
}

void DispatchEffectCall(EffectCall kind, uint32_t caller, ffb::Sink* sink, uint32_t a, uint32_t b,
                        uint32_t c, uint32_t d) {
  switch (kind) {
    case EffectCall::kSet: {
      for (uint32_t i = 0; i < b; i++) {
        const ffb::Effect effect = ffb::DecodeEffect(GuestBytes(a + i * ffb::kDescriptorSize));
        if (LogThis(g_log_set, 32)) {
          REXKRNL_INFO("Wheel FFB: set effect {}", ffb::DescribeEffect(effect));
        }
        ffb_trace::RecordWheelCall("ffb_set_effect", caller, effect.id, effect.type, b, 0,
                                   ffb::DescribeEffect(effect) + " | " + ffb::HexWords(effect));
        if (sink)
          sink->SetEffect(effect);
      }
      CompleteOverlapped(c);
      break;
    }
    case EffectCall::kUpdate: {
      const ffb::Effect effect = ffb::DecodeEffect(GuestBytes(a));
      if (LogThis(g_log_update, 8)) {
        REXKRNL_INFO("Wheel FFB: update effect (flags {:#x}) {}", b, ffb::DescribeEffect(effect));
      }
      ffb_trace::RecordWheelCall("ffb_update_effect", caller, effect.id, effect.type, b, 0,
                                 ffb::DescribeEffect(effect) + " | " + ffb::HexWords(effect));
      if (sink)
        sink->UpdateEffect(effect, b);
      CompleteOverlapped(c);
      break;
    }
    case EffectCall::kOperation: {
      for (uint32_t i = 0; i < b; i++) {
        const uint32_t id = LoadGuest32(a + i * 8);
        const uint32_t op = LoadGuest32(a + i * 8 + 4);
        if (LogThis(g_log_op, 32)) {
          REXKRNL_INFO("Wheel FFB: effect #{} operation {} (flag {})", id, op, c);
        }
        ffb_trace::RecordWheelCall("ffb_effect_op", caller, id, op, c, 0, "");
        if (sink)
          sink->EffectOperation(id, op);
      }
      CompleteOverlapped(d);
      break;
    }
  }
}

std::atomic<uint32_t> g_log_info{0}, g_log_control{0}, g_log_gain{0}, g_log_rumble{0},
    g_log_cancel{0};

}  // namespace

// (handle, info): Forza Horizon needs the word at +4 (effect slots) to be at
// least 5. Only the first 8 bytes are written; its buffer sits 16 bytes below
// another local.
u32 XInputdFFGetDeviceInfo_entry(u32 handle, u32 info) {
  constexpr uint32_t kEffectSlots = 16;
  if (GuestRange(info, 8)) {
    StoreGuest32(info, 0);
    StoreGuest32(info + 4, kEffectSlots);
  }
  if (LogThis(g_log_info, 4)) {
    bool host_ffb = false;
    ffb::WithSink([&](ffb::Sink* sink) { host_ffb = sink && sink->HasForceFeedback(); });
    REXKRNL_INFO(
        "Wheel FFB: device info requested (handle {:08X}); {} effect slots, host force "
        "feedback {}",
        (uint32_t)handle, kEffectSlots, host_ffb ? "yes" : "no");
  }
  ffb_trace::RecordWheelCall("ffb_get_info", GuestCaller(), handle, info, kEffectSlots, 0, "");
  return X_STATUS_SUCCESS;
}

u32 XInputdFFSetEffect_entry(u32 handle, u32 a, u32 b, u32 c, u32 d) {
  return HandleEffectCall(EffectCall::kSet, "XInputdFFSetEffect", handle, a, b, c, d);
}

u32 XInputdFFUpdateEffect_entry(u32 handle, u32 a, u32 b, u32 c, u32 d) {
  return HandleEffectCall(EffectCall::kUpdate, "XInputdFFUpdateEffect", handle, a, b, c, d);
}

u32 XInputdFFEffectOperation_entry(u32 handle, u32 a, u32 b, u32 c, u32 d) {
  return HandleEffectCall(EffectCall::kOperation, "XInputdFFEffectOperation", handle, a, b, c, d);
}

// (handle, command, overlapped): PID device control; Forza Horizon sends 3 (stop all) at set-up.
u32 XInputdFFDeviceControl_entry(u32 handle, u32 command, u32 overlapped) {
  if (LogThis(g_log_control)) {
    REXKRNL_INFO("Wheel FFB: device control {}", (uint32_t)command);
  }
  ffb_trace::RecordWheelCall("ffb_device_control", GuestCaller(), command, overlapped, 0, 0, "");
  ffb::WithSink([&](ffb::Sink* sink) {
    if (sink)
      sink->DeviceControl(command);
  });
  CompleteOverlapped(overlapped);
  return X_STATUS_SUCCESS;
}

u32 XInputdFFSetDeviceGain_entry(u32 handle, u32 gain, u32 overlapped) {
  if (LogThis(g_log_gain)) {
    REXKRNL_INFO("Wheel FFB: device gain {}", (uint32_t)gain);
  }
  ffb_trace::RecordWheelCall("ffb_set_gain", GuestCaller(), gain, overlapped, 0, 0, "");
  ffb::WithSink([&](ffb::Sink* sink) {
    if (sink)
      sink->SetDeviceGain(gain);
  });
  // Forza Horizon never calls this and its arguments are unconfirmed, so a
  // possible overlapped argument is left alone rather than written.
  return X_STATUS_SUCCESS;
}

u32 XInputdFFCancelIo_entry(u32 handle, u32 a, u32 b) {
  if (LogThis(g_log_cancel, 4)) {
    REXKRNL_INFO("Wheel FFB: cancel I/O ({:08X} {:08X})", (uint32_t)a, (uint32_t)b);
  }
  ffb_trace::RecordWheelCall("ffb_cancel_io", GuestCaller(), a, b, 0, 0, "");
  return X_STATUS_SUCCESS;
}

// (handle, XINPUT_VIBRATION*, overlapped): the title's rumble, scaled by the
// profile vibration level, sent alongside the effects.
u32 XInputdFFSetRumble_entry(u32 handle, u32 vibration, u32 overlapped) {
  uint16_t left = 0, right = 0;
  if (GuestRange(vibration, 4)) {
    const uint8_t* p = GuestBytes(vibration);
    left = uint16_t((p[0] << 8) | p[1]);
    right = uint16_t((p[2] << 8) | p[3]);
  }
  if (LogThis(g_log_rumble, 4)) {
    REXKRNL_INFO("Wheel FFB: rumble {} {}", left, right);
  }
  ffb_trace::RecordWheelCall("ffb_rumble", GuestCaller(), left, right, overlapped, 0, "");
  ffb::WithSink([&](ffb::Sink* sink) {
    if (sink)
      sink->SetRumble(left, right);
  });
  CompleteOverlapped(overlapped);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
REX_EXPORT(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
REX_EXPORT(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
REX_EXPORT(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)

// XInputd stubs
REX_EXPORT_STUB(__imp__XInputdGetCapabilities);
REX_EXPORT_STUB(__imp__XInputdReadState);
REX_EXPORT_STUB(__imp__XInputdWriteState);
REX_EXPORT_STUB(__imp__XInputdNotify);
REX_EXPORT_STUB(__imp__XInputdRawState);
REX_EXPORT_STUB(__imp__XInputdGetDeviceStats);
REX_EXPORT_STUB(__imp__XInputdResetDevice);
REX_EXPORT_STUB(__imp__XInputdSetRingOfLight);
REX_EXPORT_STUB(__imp__XInputdSetRFPowerMode);
REX_EXPORT_STUB(__imp__XInputdSetRadioFrequency);
REX_EXPORT_STUB(__imp__XInputdPassThroughRFCommand);
REX_EXPORT_STUB(__imp__XInputdPowerDownDevice);
REX_EXPORT_STUB(__imp__XInputdReadTextKeystroke);
REX_EXPORT_STUB(__imp__XInputdSendStayAliveRequest);
REX_EXPORT(__imp__XInputdFFGetDeviceInfo, rex::kernel::xboxkrnl::XInputdFFGetDeviceInfo_entry)
REX_EXPORT(__imp__XInputdFFSetEffect, rex::kernel::xboxkrnl::XInputdFFSetEffect_entry)
REX_EXPORT(__imp__XInputdFFUpdateEffect, rex::kernel::xboxkrnl::XInputdFFUpdateEffect_entry)
REX_EXPORT(__imp__XInputdFFEffectOperation, rex::kernel::xboxkrnl::XInputdFFEffectOperation_entry)
REX_EXPORT(__imp__XInputdFFDeviceControl, rex::kernel::xboxkrnl::XInputdFFDeviceControl_entry)
REX_EXPORT(__imp__XInputdFFSetDeviceGain, rex::kernel::xboxkrnl::XInputdFFSetDeviceGain_entry)
REX_EXPORT(__imp__XInputdFFCancelIo, rex::kernel::xboxkrnl::XInputdFFCancelIo_entry)
REX_EXPORT(__imp__XInputdFFSetRumble, rex::kernel::xboxkrnl::XInputdFFSetRumble_entry)
REX_EXPORT_STUB(__imp__XInputdGetLastTextInputTime);
REX_EXPORT_STUB(__imp__XInputdSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XInputdSetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdGetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdControl);
REX_EXPORT_STUB(__imp__XInputdSetWifiChannel);
REX_EXPORT_STUB(__imp__XInputdGetDevicePid);
REX_EXPORT_STUB(__imp__XInputdGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XInputdSetFailedConnectionOrBindCallback);
REX_EXPORT_STUB(__imp__XInputdSetMinMaxAuthDelay);

// Drv stubs
REX_EXPORT_STUB(__imp__DrvSetSysReqCallback);
REX_EXPORT_STUB(__imp__DrvSetUserBindingCallback);
REX_EXPORT_STUB(__imp__DrvSetContentStorageCallback);
REX_EXPORT_STUB(__imp__DrvSetAutobind);
REX_EXPORT_STUB(__imp__DrvGetContentStorageNotification);
REX_EXPORT_STUB(__imp__DrvXenonButtonPressed);
REX_EXPORT_STUB(__imp__DrvBindToUser);
REX_EXPORT_STUB(__imp__DrvSetDeviceConfigChangeCallback);
REX_EXPORT_STUB(__imp__DrvDeviceConfigChange);
REX_EXPORT_STUB(__imp__DrvSetMicArrayStartCallback);
REX_EXPORT_STUB(__imp__DrvSetAudioLatencyCallback);
