/**
 * @file        tests/unit/input/mnk_hold_look_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_bind.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

using rex::X_RESULT;

using rex::input::X_INPUT_STATE;
using rex::input::mnk::HoldLookSettings;
using rex::input::mnk::IsHoldLookEngaged;
using rex::input::mnk::MnkInputDriver;
using rex::input::mnk::SetHoldLook;
using rex::input::mnk::SetMouseLookActive;
using rex::ui::KeyEvent;
using rex::ui::MouseEvent;
using rex::ui::UISetupEvent;
using rex::ui::VirtualKey;

namespace {

constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

struct Stick {
  int rx;
  int ry;
};

bool Near(Stick actual, Stick expected) {
  return std::abs(actual.rx - expected.rx) <= 2 && std::abs(actual.ry - expected.ry) <= 2;
}

struct Driver {
  MnkInputDriver driver{nullptr, 0};
  int32_t x = 0;
  int32_t y = 0;

  Driver() {
    REQUIRE(rex::cvar::SetFlagByName("mnk_mode", "true"));
    SetHoldLook({.enabled = true, .degrees_per_count = 0.15});
    SetMouseLookActive(true);
    driver.set_is_active_callback([] { return true; });
  }
  ~Driver() {
    rex::input::mnk::SetHostDialogOpen(false);
    SetHoldLook({});
    SetMouseLookActive(true);
    rex::cvar::SetFlagByName("mnk_mode", "false");
  }

  void PressRight() {
    MouseEvent e(nullptr, MouseEvent::Button::kRight, x, y);
    driver.OnMouseDown(e);
  }
  void ReleaseRight() {
    MouseEvent e(nullptr, MouseEvent::Button::kRight, x, y);
    driver.OnMouseUp(e);
  }
  void Move(int32_t dx, int32_t dy = 0) {
    x += dx;
    y += dy;
    MouseEvent e(nullptr, MouseEvent::Button::kNone, x, y);
    driver.OnMouseMove(e);
  }
  Stick Poll() {
    X_INPUT_STATE state = {};
    REQUIRE(driver.GetDeviceState(kMnkDevice, &state) == X_ERROR_SUCCESS);
    return {state.gamepad.thumb_rx, state.gamepad.thumb_ry};
  }
};

}  // namespace

TEST_CASE("Holding the right button sends a full stick straight behind until the mouse moves",
          "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  CHECK(Near(d.Poll(), {0, 32767}));
  CHECK(IsHoldLookEngaged());
}

TEST_CASE("Mouse travel turns the stick by the set degrees per count", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Poll();
  d.Move(600);
  CHECK(Near(d.Poll(), {32767, 0}));
  d.Move(-1200);
  CHECK(Near(d.Poll(), {-32767, 0}));
}

TEST_CASE("The angle wraps past the front", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Poll();
  d.Move(1300);
  const Stick stick = d.Poll();
  CHECK(Near(stick, {-8481, -31651}));
  CHECK(std::abs(std::hypot(stick.rx, stick.ry) - 32767.0) <= 2.0);
  d.Move(1200);
  CHECK(Near(d.Poll(), {8481, 31651}));
}

TEST_CASE("Releasing the button gives the keys back", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Move(600);
  d.Poll();
  d.ReleaseRight();
  CHECK(Near(d.Poll(), {0, 0}));
  CHECK(!IsHoldLookEngaged());
  d.PressRight();
  CHECK(Near(d.Poll(), {0, 32767}));
}

TEST_CASE("Mouse Y does nothing", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Poll();
  d.Move(0, 500);
  CHECK(Near(d.Poll(), {0, 32767}));
}

TEST_CASE("A press held from where look was not allowed waits for a new press",
          "[mnk_hold_look]") {
  Driver d;
  SetMouseLookActive(false);
  d.PressRight();
  d.Poll();
  SetMouseLookActive(true);
  CHECK(Near(d.Poll(), {0, 0}));
  CHECK(!IsHoldLookEngaged());
  d.ReleaseRight();
  d.Poll();
  d.PressRight();
  CHECK(Near(d.Poll(), {0, 32767}));
}

TEST_CASE("Look disallowed while held resets the angle", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Poll();
  d.Move(600);
  d.Poll();
  SetMouseLookActive(false);
  CHECK(Near(d.Poll(), {0, 0}));
  CHECK(!IsHoldLookEngaged());
}

TEST_CASE("Mouse travel before the press is not added", "[mnk_hold_look]") {
  Driver d;
  d.Poll();
  d.Move(600);
  d.PressRight();
  CHECK(Near(d.Poll(), {0, 32767}));
}

TEST_CASE("Focus loss while held ends the look", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Poll();
  UISetupEvent lost;
  d.driver.OnLostFocus(lost);
  UISetupEvent got;
  d.driver.OnGotFocus(got);
  CHECK(Near(d.Poll(), {0, 0}));
  CHECK(!IsHoldLookEngaged());
}

TEST_CASE("The other mouse modes switch hold look off", "[mnk_hold_look]") {
  Driver d;
  REQUIRE(rex::cvar::SetFlagByName("mnk_mouse_steering", "true"));
  d.PressRight();
  d.Poll();
  d.Move(600);
  d.Poll();
  CHECK(!IsHoldLookEngaged());
  rex::cvar::SetFlagByName("mnk_mouse_steering", "false");
}

TEST_CASE("An open host dialog switches hold look off", "[mnk_hold_look]") {
  Driver d;
  d.PressRight();
  d.Poll();
  rex::input::mnk::SetHostDialogOpen(true);
  d.Move(600);
  CHECK(Near(d.Poll(), {0, 0}));
  CHECK(!IsHoldLookEngaged());
  rex::input::mnk::SetHostDialogOpen(false);
  CHECK(Near(d.Poll(), {0, 0}));
  d.ReleaseRight();
  d.Poll();
  d.PressRight();
  CHECK(Near(d.Poll(), {0, 32767}));
}

TEST_CASE("Hold look off in the settings leaves the right button inert", "[mnk_hold_look]") {
  Driver d;
  SetHoldLook({});
  d.PressRight();
  d.Poll();
  d.Move(600);
  CHECK(Near(d.Poll(), {0, 0}));
  CHECK(!IsHoldLookEngaged());
}

TEST_CASE("The right-stick keys still work when the button is not held", "[mnk_hold_look]") {
  Driver d;
  auto binds = std::make_shared<rex::input::mnk::KeybindMap>();
  binds->emplace("keybind_rstick_right", "Right");
  rex::input::mnk::SetKeybindOverrides(binds);
  d.Poll();
  KeyEvent e(nullptr, VirtualKey::kRight, 1, false, false, false, false, false);
  d.driver.OnKeyDown(e);
  CHECK(Near(d.Poll(), {32767, 0}));
  rex::input::mnk::SetKeybindOverrides(nullptr);
}
