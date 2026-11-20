#pragma once
/**
 * @file        rex/input/sdl/joystick_probe.h
 * @brief       Streams the connected joysticks as JSON lines, for a launcher's wheel set-up.
 *
 * One JSON object per line (UTF-8, "\n"):
 *   {"device":{"id":3,"name":"...","path":"...","vendor":"346e","product":"0004","wheel":true,
 *              "rest":[0,-32768],"buttons":64,"hats":1}}  when a joystick is found
 *   {"ready":true}                                         once, after the first enumeration
 *   {"state":{"id":3,"axes":[12,-32768],"buttons":"0100","hats":[0]}}  when its values change
 *   {"removed":3}                                          when it is unplugged
 * The ids are SDL's joystick instance ids; "rest" holds each axis's initial value, from which
 * the SDK decides a pedal's direction.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rex::input::sdl {

struct ProbeDevice {
  uint32_t id = 0;
  std::string name, path;
  uint16_t vendor = 0, product = 0;
  bool wheel = false;
  std::vector<int16_t> rest;
  int buttons = 0;
  int hats = 0;
};

struct ProbeState {
  uint32_t id = 0;
  std::vector<int16_t> axes;
  std::vector<bool> buttons;
  std::vector<uint8_t> hats;
  bool operator==(const ProbeState&) const = default;
};

std::string ProbeDeviceLine(const ProbeDevice& device);
std::string ProbeStateLine(const ProbeState& state);
std::string ProbeRemovedLine(uint32_t id);

// Initialises SDL's joystick subsystem, writes the lines above to `out` (flushed after each
// batch, every 33 ms) while `keep_running` returns true, then quits the subsystem. Returns 0,
// or 1 when SDL cannot start.
int RunJoystickProbe(std::FILE* out, const std::function<bool()>& keep_running);

}  // namespace rex::input::sdl
