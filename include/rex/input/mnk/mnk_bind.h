/**
 * @file        rex/input/mnk/mnk_bind.h
 * @brief       Matching keyboard state against keybind_* values.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rex::input::mnk {

// Binds an app supplies at run time in place of keybind_* options, by option
// name. The driver takes the table once per poll; the options themselves are
// plain strings, unsafe to write while the input and UI threads read them.
struct KeybindNameHash {
  using is_transparent = void;
  size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
};
using KeybindMap = std::unordered_map<std::string, std::string, KeybindNameHash, std::equal_to<>>;
// nullptr goes back to the options.
void SetKeybindOverrides(std::shared_ptr<const KeybindMap> binds);
std::shared_ptr<const KeybindMap> KeybindOverrides();
// Keys held when the overrides change stay silent until released, so a key
// that opens a menu does not also press the button the menu's binds give it.
class HeldKeyMask {
 public:
  // The keys to match binds against: key_down without the held-back keys.
  void Apply(const std::shared_ptr<const KeybindMap>& binds, const bool (&key_down)[256],
             bool (&keys)[256]);

 private:
  // Kept alive so a new table at the same address still counts as a change.
  std::shared_ptr<const KeybindMap> binds_;
  bool held_back_[256] = {};
};

// The override for the option, else its value.
const std::string& ResolveKeybind(const KeybindMap* overrides, std::string_view option,
                                  const std::string& value);

// A bind is a comma-separated list of alternatives, each optionally carrying
// modifier prefixes: "Q,I" or "Shift+W". With exact_modifiers the held
// modifiers must equal the token's, so "Up" stays silent while Shift is held
// and a bare modifier name never fires (Pinyon's D-pad on Shift+arrows). Without
// it a token without prefixes matches whatever modifiers are held, a bare
// "Shift" matches while Shift is down, and a prefixed token still needs its
// modifiers (mnk_exact_modifiers).
bool BindPressed(const bool (&key_down)[256], std::string_view bind, bool exact_modifiers);

// Whether a token of the bind names key vk, with the modifiers matched as in
// BindPressed: the binds a newly pressed key latches.
bool BindNamesKey(const bool (&key_down)[256], std::string_view bind, uint16_t vk,
                  bool exact_modifiers);

}  // namespace rex::input::mnk
