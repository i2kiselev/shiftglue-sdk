#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

using rex::X_RESULT;

using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_STATE;
using rex::input::mnk::MnkInputDriver;
using rex::ui::KeyEvent;
using rex::ui::VirtualKey;

namespace {

// The driver's one device id (mnk_input_driver.cpp).
constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

struct Driver {
  MnkInputDriver driver{nullptr, 0};
  bool active = true;

  Driver() {
    REQUIRE(rex::cvar::SetFlagByName("mnk_mode", "true"));
    driver.set_is_active_callback([this] { return active; });
  }
  ~Driver() { rex::cvar::SetFlagByName("mnk_mode", "false"); }

  void Down(VirtualKey key, bool repeat = false) {
    KeyEvent e(nullptr, key, 1, repeat, false, false, false, false);
    driver.OnKeyDown(e);
  }
  void Up(VirtualKey key) {
    KeyEvent e(nullptr, key, 1, true, false, false, false, false);
    driver.OnKeyUp(e);
  }
  uint16_t Buttons() {
    X_INPUT_STATE state = {};
    REQUIRE(driver.GetDeviceState(kMnkDevice, &state) == X_ERROR_SUCCESS);
    return uint16_t(state.gamepad.buttons);
  }
};

}  // namespace

TEST_CASE("A key tapped while the guest input is gated never reaches the guest",
          "[mnk_inactive_keys]") {
  Driver d;
  d.active = false;
  d.Down(VirtualKey::kSpace);
  d.Up(VirtualKey::kSpace);
  d.active = true;
  CHECK(d.Buttons() == 0);
}

TEST_CASE("A key pressed while gated and held past the gate counts only once pressed again",
          "[mnk_inactive_keys]") {
  Driver d;
  d.active = false;
  d.Down(VirtualKey::kSpace);
  d.active = true;
  CHECK(d.Buttons() == 0);
  d.Down(VirtualKey::kSpace, true);
  CHECK(d.Buttons() == 0);
  d.Up(VirtualKey::kSpace);
  d.Down(VirtualKey::kSpace);
  CHECK(d.Buttons() == X_INPUT_GAMEPAD_A);
}

TEST_CASE("A key pressed while the guest input is open still reaches the guest",
          "[mnk_inactive_keys]") {
  Driver d;
  d.Down(VirtualKey::kSpace);
  d.Up(VirtualKey::kSpace);
  CHECK(d.Buttons() == X_INPUT_GAMEPAD_A);
}
