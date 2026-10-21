#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

#include <rex/input/mnk/mnk_bind.h>
#include <rex/ui/virtual_key.h>

using rex::input::mnk::BindNamesKey;
using rex::input::mnk::BindPressed;
using rex::ui::VirtualKey;

namespace {

struct Keys {
  bool down[256] = {};
  Keys& Hold(VirtualKey key) {
    down[static_cast<uint16_t>(key)] = true;
    return *this;
  }
};

}  // namespace

TEST_CASE("The exact modifier rule is unchanged", "[fm4_mnk_bind]") {
  Keys shift_w;
  shift_w.Hold(VirtualKey::kShift).Hold(VirtualKey::kW);
  CHECK_FALSE(BindPressed(shift_w.down, "W", true));
  CHECK_FALSE(BindPressed(shift_w.down, "Shift", true));
  Keys shift_up;
  shift_up.Hold(VirtualKey::kShift).Hold(VirtualKey::kUp);
  CHECK(BindPressed(shift_up.down, "Shift+Up", true));
  CHECK(BindPressed(shift_up.down, "Q,Shift+Up", true));
  Keys w;
  w.Hold(VirtualKey::kW);
  CHECK(BindPressed(w.down, "W", true));
}

TEST_CASE("The loose modifier rule lets Shift be the clutch while driving keys are held",
          "[fm4_mnk_bind]") {
  Keys shift_w;
  shift_w.Hold(VirtualKey::kShift).Hold(VirtualKey::kW);
  CHECK(BindPressed(shift_w.down, "W", false));
  CHECK(BindPressed(shift_w.down, "Shift", false));
  CHECK(BindPressed(shift_w.down, "A,Shift", false));
  Keys up;
  up.Hold(VirtualKey::kUp);
  CHECK_FALSE(BindPressed(up.down, "Shift+Up", false));
  CHECK_FALSE(BindPressed(up.down, "Shift", false));
  CHECK(BindPressed(up.down, "Up", false));
}

TEST_CASE("A newly pressed key latches the binds that name it under the same rule",
          "[fm4_mnk_bind]") {
  Keys shift_w;
  shift_w.Hold(VirtualKey::kShift).Hold(VirtualKey::kW);
  const auto w = static_cast<uint16_t>(VirtualKey::kW);
  const auto shift = static_cast<uint16_t>(VirtualKey::kShift);
  CHECK_FALSE(BindNamesKey(shift_w.down, "W", w, true));
  CHECK(BindNamesKey(shift_w.down, "W", w, false));
  CHECK(BindNamesKey(shift_w.down, "Shift", shift, false));
  CHECK_FALSE(BindNamesKey(shift_w.down, "Shift", shift, true));
  CHECK(BindNamesKey(shift_w.down, "Shift+W", w, true));
}

TEST_CASE("Keybind overrides replace the options until cleared", "[fm4_mnk_bind]") {
  using rex::input::mnk::KeybindOverrides;
  using rex::input::mnk::SetKeybindOverrides;
  CHECK(KeybindOverrides() == nullptr);
  auto binds = std::make_shared<rex::input::mnk::KeybindMap>();
  (*binds)["keybind_a"] = "Return";
  SetKeybindOverrides(binds);
  const auto taken = KeybindOverrides();
  REQUIRE(taken != nullptr);
  CHECK(rex::input::mnk::ResolveKeybind(taken.get(), "keybind_a", "Space") == "Return");
  CHECK(rex::input::mnk::ResolveKeybind(taken.get(), "keybind_b", "Escape") == "Escape");
  CHECK(rex::input::mnk::ResolveKeybind(nullptr, "keybind_a", "Space") == "Space");
  SetKeybindOverrides(nullptr);
  CHECK(KeybindOverrides() == nullptr);
}

TEST_CASE("Keys held when the binds change stay silent until released", "[fm4_mnk_bind]") {
  using rex::input::mnk::HeldKeyMask;
  using rex::input::mnk::KeybindMap;
  const auto driving = std::make_shared<const KeybindMap>(KeybindMap{{"keybind_start", "Escape"}});
  const auto menu = std::make_shared<const KeybindMap>(KeybindMap{{"keybind_b", "Escape"}});
  HeldKeyMask mask;
  Keys escape;
  escape.Hold(VirtualKey::kEscape);
  bool keys[256];
  Keys none;
  mask.Apply(driving, none.down, keys);
  mask.Apply(driving, escape.down, keys);
  CHECK(keys[static_cast<uint16_t>(VirtualKey::kEscape)]);
  // Esc opened the pause menu, whose binds read Esc as B: still held, it is not B.
  mask.Apply(menu, escape.down, keys);
  CHECK_FALSE(keys[static_cast<uint16_t>(VirtualKey::kEscape)]);
  // Released, then pressed again: B.
  mask.Apply(menu, none.down, keys);
  mask.Apply(menu, escape.down, keys);
  CHECK(keys[static_cast<uint16_t>(VirtualKey::kEscape)]);
  // A key pressed after the change is not held back.
  Keys w;
  w.Hold(VirtualKey::kW);
  mask.Apply(driving, none.down, keys);
  mask.Apply(driving, w.down, keys);
  CHECK(keys[static_cast<uint16_t>(VirtualKey::kW)]);
}
