#pragma once
/**
 * @file        rex/input/ffb_trace.h
 * @brief       CSV trace of the title's force feedback traffic (FB-2.1).
 *
 * With `wheel_ffb_trace` on, every XamInputSetState the title makes is
 * written as one CSV row with the raw motor values, the guest caller, the
 * wheel's steering and pedals at that moment and the force the host sent to
 * the wheel. Capability queries (whether the title sees the wheel) and
 * XamInputGetUserVibrationLevel calls are written when they change. The rows
 * are what the title really produced, so the meaning of its two motor values
 * on a wheel can be read off a drive instead of guessed.
 *
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <string>

namespace rex::input::ffb_trace {

bool Enabled();

// Inputs and output of the wheel driver, kept so XAM rows can carry them.
void NoteWheelInput(int16_t steering, uint8_t throttle, uint8_t brake, uint16_t buttons);
void NoteWheelOutput(int16_t force, const char* mode);

// A title call to XamInputSetState. `extra` holds the guest bytes after the
// two motor words, in case a wheel call passes more than XINPUT_VIBRATION.
void RecordSetState(uint32_t user, uint32_t arg, uint16_t left, uint16_t right, uint32_t extra,
                    uint32_t caller, uint32_t result);
// A capability answer, written when it differs from the last one per caller.
void RecordCapabilities(uint32_t user, uint32_t flags, uint32_t result, uint8_t type,
                        uint8_t sub_type, uint16_t caps_flags, uint32_t caller);
void RecordVibrationLevel(uint32_t user, uint32_t level, uint32_t caller);
// A call to the wheel's XInputdFF* device (set_effect, update_effect, effect_op, device_control,
// set_gain, set_rumble, get_info, cancel_io), with the decoded effect or arguments in `detail`.
void RecordWheelCall(const char* event, uint32_t caller, uint32_t a, uint32_t b, uint32_t c,
                     uint32_t result, const std::string& detail);

}  // namespace rex::input::ffb_trace
