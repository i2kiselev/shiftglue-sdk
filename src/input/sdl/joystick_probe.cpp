/**
 * @file        input/sdl/joystick_probe.cpp
 * @brief       Streams the connected joysticks as JSON lines, for a launcher's wheel set-up.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/sdl/joystick_probe.h>

#include <algorithm>
#include <map>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include <rex/input/sdl/wheel_mapping.h>

namespace rex::input::sdl {

namespace {

std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

template <typename T>
std::string JsonNumbers(const std::vector<T>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); i++) {
    out += fmt::format("{}{}", i ? "," : "", static_cast<int>(values[i]));
  }
  return out + "]";
}

void WriteLine(std::FILE* out, const std::string& line) {
  std::fputs(line.c_str(), out);
  std::fputc('\n', out);
}

ProbeDevice DescribeJoystick(SDL_Joystick* joystick, SDL_JoystickID id) {
  ProbeDevice d;
  d.id = id;
  const char* name = SDL_GetJoystickName(joystick);
  d.name = name ? name : "";
  const char* path = SDL_GetJoystickPath(joystick);
  d.path = path ? path : "";
  d.vendor = SDL_GetJoystickVendor(joystick);
  d.product = SDL_GetJoystickProduct(joystick);
  d.wheel =
      SDL_GetJoystickType(joystick) == SDL_JOYSTICK_TYPE_WHEEL || IsKnownWheelVendor(d.vendor);
  const int axes = SDL_GetNumJoystickAxes(joystick);
  for (int i = 0; i < axes; i++) {
    int16_t rest = 0;
    if (!SDL_GetJoystickAxisInitialState(joystick, i, &rest)) {
      rest = 0;
    }
    d.rest.push_back(rest);
  }
  d.buttons = std::max(SDL_GetNumJoystickButtons(joystick), 0);
  d.hats = std::max(SDL_GetNumJoystickHats(joystick), 0);
  return d;
}

ProbeState ReadJoystick(SDL_Joystick* joystick, SDL_JoystickID id) {
  ProbeState s;
  s.id = id;
  for (int i = 0, n = SDL_GetNumJoystickAxes(joystick); i < n; i++) {
    s.axes.push_back(SDL_GetJoystickAxis(joystick, i));
  }
  for (int i = 0, n = SDL_GetNumJoystickButtons(joystick); i < n; i++) {
    s.buttons.push_back(SDL_GetJoystickButton(joystick, i));
  }
  for (int i = 0, n = SDL_GetNumJoystickHats(joystick); i < n; i++) {
    s.hats.push_back(SDL_GetJoystickHat(joystick, i));
  }
  return s;
}

}  // namespace

std::string ProbeDeviceLine(const ProbeDevice& d) {
  return fmt::format(
      "{{\"device\":{{\"id\":{},\"name\":{},\"path\":{},\"vendor\":\"{:04x}\",\"product\":"
      "\"{:04x}\",\"wheel\":{},\"rest\":{},\"buttons\":{},\"hats\":{}}}}}",
      d.id, JsonString(d.name), JsonString(d.path), d.vendor, d.product, d.wheel,
      JsonNumbers(d.rest), d.buttons, d.hats);
}

std::string ProbeStateLine(const ProbeState& s) {
  std::string buttons;
  for (const bool pressed : s.buttons) {
    buttons += pressed ? '1' : '0';
  }
  return fmt::format("{{\"state\":{{\"id\":{},\"axes\":{},\"buttons\":\"{}\",\"hats\":{}}}}}",
                     s.id, JsonNumbers(s.axes), buttons, JsonNumbers(s.hats));
}

std::string ProbeRemovedLine(uint32_t id) {
  return fmt::format("{{\"removed\":{}}}", id);
}

int RunJoystickProbe(std::FILE* out, const std::function<bool()>& keep_running) {
  if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
    return 1;
  }
  struct Opened {
    SDL_Joystick* sdl = nullptr;
    ProbeState last;
    bool sent = false;
  };
  std::map<SDL_JoystickID, Opened> opened;
  auto add = [&](SDL_JoystickID id) {
    if (opened.count(id)) {
      return;
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (!joystick) {
      return;
    }
    opened[id].sdl = joystick;
    WriteLine(out, ProbeDeviceLine(DescribeJoystick(joystick, id)));
  };

  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
    for (int i = 0; i < count; i++) {
      add(ids[i]);
    }
    SDL_free(ids);
  }
  WriteLine(out, "{\"ready\":true}");
  std::fflush(out);

  while (keep_running()) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_JOYSTICK_ADDED) {
        add(event.jdevice.which);
      } else if (event.type == SDL_EVENT_JOYSTICK_REMOVED) {
        auto it = opened.find(event.jdevice.which);
        if (it != opened.end()) {
          SDL_CloseJoystick(it->second.sdl);
          opened.erase(it);
          WriteLine(out, ProbeRemovedLine(event.jdevice.which));
        }
      }
    }
    for (auto& [id, joystick] : opened) {
      ProbeState state = ReadJoystick(joystick.sdl, id);
      if (!joystick.sent || state != joystick.last) {
        WriteLine(out, ProbeStateLine(state));
        joystick.last = std::move(state);
        joystick.sent = true;
      }
    }
    std::fflush(out);
    SDL_Delay(33);
  }

  for (auto& [id, joystick] : opened) {
    SDL_CloseJoystick(joystick.sdl);
  }
  SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
  return 0;
}

}  // namespace rex::input::sdl
