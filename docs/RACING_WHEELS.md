# Racing wheels and wheel force feedback

Two pieces in the SDK let a title drive a modern racing wheel the way it drove an
Xbox 360 force feedback wheel:

- **The XInputdFF device** (`src/kernel/xboxkrnl/xboxkrnl_hid.cpp`,
  `rex/input/xinputd_ffb.h`). A title that sees a wheel with the force feedback
  flag (XInput subtype 2) talks to it through the kernel's `XInputdFF*` calls:
  device info, effect download, start and per-frame updates, device control and
  rumble. The exports complete every call and pass the decoded effects (USB HID PID
  types: constant force, spring, damper, friction, inertia) to whichever input
  driver registered as the device's sink. XAM ordinal 442
  (`XamInputGetUserVibrationLevel`) reports the profile default.
- **The raw wheel path** of the SDL input driver (`src/input/sdl/sdl_input_driver.cpp`,
  `rex/input/sdl/wheel_mapping.h`), on when `wheel_enabled = true`. It reads the
  wheel base, pedals and buttons as raw SDL joysticks, each from its own USB device,
  presents them to the title as one XInput wheel, and plays the title's effects as
  SDL haptic effects on the force feedback device. With `wheel_enabled` off, wheels
  go through the SDL gamepad path (`hid_wheel_*`) as before.

Pinyon Shift's launcher binds all of this in its Wheel screen, which writes the
settings below; they can also be set by hand in the title's config.

## Naming devices and inputs

A device is matched by any of:

- `vvvv:pppp`: USB vendor and product ID in hex, e.g. `0eb7:0004`;
- part of its name, e.g. `SimJack`;
- `path:<SDL device path>`, to tell identical devices apart.

An empty device means the wheel base (`wheel_device`). Inputs are written
`[device|]input`:

| Input | Meaning |
| --- | --- |
| `axisN`, `+axisN`, `-axisN` | Axis N. For a pedal, `+` or `-` forces the direction; without it the direction is detected from the axis's resting value. |
| `N` | Button N. |
| `hN.up`, `hN.down`, `hN.left`, `hN.right` | A direction of hat N. |
| `[+|-]axisN` in `wheel_buttons` | An axis used as a button (pressed past half travel), e.g. a handbrake lever. |

`wheel_buttons` lists `NAME=input` pairs, NAME as in `pad_remap` (A, B, X, Y, LB,
RB, LT, RT, LS, RS, BACK, START, UP, DOWN, LEFT, RIGHT). A NAME may appear more
than once, or list several inputs (`A=0,1`); any of them presses it, e.g.
`B=Handbrake|+axis0,B=9` for a handbrake lever and a rim button.

An H-pattern shifter's gates bind the same way, to the names `GEAR1` to `GEAR7` and
`REVERSE`: `GEAR1=0079:0006|0,GEAR2=0079:0006|1,...,REVERSE=0079:0006|7`. With no
gate held the wheel is in neutral.

## What the title sees

- Steering on the left stick's X axis, throttle on the right trigger, brake on the
  left trigger.
- The clutch pedal on the right stick's vertical axis, where Forza Horizon's wheel
  layouts read an analog clutch (`wheel_clutch_button` can also press a button).
- Capabilities of an XInput wheel with the force feedback flag; with
  `wheel_900_degree`, the 900-degree flag (left stick X of 900), so Forza Horizon
  uses its linear 900-degree steering instead of the pad's reduction at speed. The
  wheel base should be set to 900 degrees.
- With gears bound in `wheel_buttons`, an H-shifter: the capabilities give the right
  stick X a range, and Forza Horizon then reads the held gear from it (-1 reverse,
  0 neutral, 1 and up the gear). Without them, right stick X is 0 in the
  capabilities. The paddles shift either way.

## Settings

| Setting | Default | What it does |
| --- | --- | --- |
| `wheel_enabled` | `false` | Turns the raw wheel path on. |
| `wheel_device` | `""` | The wheel base; empty for the first device SDL reports as a wheel. |
| `wheel_steering_axis` | `0` | Steering axis on the wheel base. |
| `wheel_steering_invert` | `false` | Reverse the steering axis. |
| `wheel_steering_scale` | `100` | Steering sensitivity in percent; 200 reaches full lock at half the wheel's rotation. |
| `wheel_steering_deadzone` | `0` | Steering dead zone in percent of half travel. |
| `wheel_900_degree` | `true` | Report the 900-degree flag. |
| `wheel_throttle`, `wheel_brake`, `wheel_clutch` | `""` | Pedal axes, `[device|][+|-]axisN`. |
| `wheel_clutch_button` | `""` | Pad button the clutch pedal also presses past half travel. |
| `wheel_pedal_deadzone` | `3` | Pedal dead zone at rest, in percent, for pedals without their own. |
| `wheel_throttle_deadzone_bottom` / `_top` (also `brake`, `clutch`) | `-1` / `0` | Per-pedal dead zones: at rest (`-1` uses `wheel_pedal_deadzone`) and at full travel (past it reads fully pressed). |
| `wheel_buttons` | the hat as the D-pad | Button bindings, as above. |
| `wheel_hide_gamepads` | `true` | While a wheel is connected, hide other gamepads so none takes player one. |
| `wheel_ffb_device` | `""` | Device that receives force feedback; empty for the steering device. |
| `wheel_ffb_mode` | `"difference"` | `off`, `rumble` (vibration only), or a mode that plays forces; the title's effects play in every mode but `off` and `rumble`. |
| `wheel_ffb_effects` | `true` | Play the title's own XInputdFF effects. |
| `wheel_ffb_strength` | `25` | Overall strength in percent; start low on a direct-drive base. |
| `wheel_ffb_invert` | `false` | Reverse the force direction. |
| `wheel_ffb_min_force` | `0` | Lift any non-zero force to at least this percent, for geared or belted wheels. |
| `wheel_ffb_spring`, `_damper`, `_friction`, `_inertia` | `100` | Strength of each of the title's condition effects, in percent. |
| `wheel_ffb_rumble` | `50` | Strength of the title's rumble, played as a vibration. |
| `wheel_ffb_centering` | `0` | Extra host centring spring, in percent. |
| `wheel_ffb_magnitude_max` | `10000` | Full-scale value of the title's effect magnitudes (Forza Horizon uses byte values: 127). |
| `wheel_ffb_saturation_max` | `0` | Full-scale value of saturations and dead bands; 0 decides per effect. |
| `wheel_ffb_trace`, `wheel_ffb_trace_file` | `false`, `""` | Write every XInputdFF and XInput call (decoded effects, raw words, caller, steering, pedals, buttons) to a CSV beside the runtime log. |
| `wheel_log_inputs` | `false` | Log raw axis, button and hat changes, to find input numbers. |

Effect updates are rate-limited (constant force 10 ms, conditions 40 ms, keeping
the latest value) and effects a wheel stops are restarted. The force's sign follows
DirectInput's convention.

## Tests

`unit_tests` covers the descriptor decoding (`[xinputd_ffb]`) and the wheel
mapping (`[wheel_mapping]`): pedal and steering maths, dead zones, binding and
device parsing, effect scaling and forces.

Tested on a Fanatec CSL DD with SimJack Pro pedals on separate USB, Windows 11.
