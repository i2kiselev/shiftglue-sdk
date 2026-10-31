/**
 * @file        input/mnk/mnk_bind.cpp
 * @brief       Matching keyboard state against keybind_* values.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_bind.h>

#include <mutex>

#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

namespace rex::input::mnk {

namespace {

constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

uint8_t LiveModifiers(const bool (&key_down)[256]) {
  uint8_t mods = 0;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Strips leading modifier prefixes off 'token', advancing it to the bare key
// name and returning the mask they require.
uint8_t TakeModifiers(std::string_view& token) {
  uint8_t mods = 0;
  for (;;) {
    size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    std::string_view head = token.substr(0, plus);
    uint8_t bit = 0;
    if (head == "Shift") {
      bit = kModShift;
    } else if (head == "Ctrl" || head == "Control") {
      bit = kModCtrl;
    } else if (head == "Alt") {
      bit = kModAlt;
    } else {
      break;
    }
    mods |= bit;
    token.remove_prefix(plus + 1);
  }
  return mods;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

// The key of each token whose modifiers match, until 'match' returns true.
template <typename Match>
bool AnyMatchingKey(const bool (&key_down)[256], std::string_view bind, bool exact_modifiers,
                    Match match) {
  const uint8_t live_mods = LiveModifiers(key_down);
  while (!bind.empty()) {
    const size_t comma = bind.find(',');
    std::string_view token = TrimSpaces(bind.substr(0, comma));
    bind = comma == std::string_view::npos ? std::string_view() : bind.substr(comma + 1);
    const uint8_t want = TakeModifiers(token);
    const bool modifiers_match = exact_modifiers ? want == live_mods : (want & live_mods) == want;
    if (token.empty() || !modifiers_match) {
      continue;
    }
    const uint16_t vk = static_cast<uint16_t>(rex::ui::ParseVirtualKey(token));
    if (vk != static_cast<uint16_t>(rex::ui::VirtualKey::kNone) && match(vk)) {
      return true;
    }
  }
  return false;
}

std::mutex g_overrides_mutex;
std::shared_ptr<const KeybindMap> g_overrides;

}  // namespace

void SetKeybindOverrides(std::shared_ptr<const KeybindMap> binds) {
  std::lock_guard lock(g_overrides_mutex);
  g_overrides = std::move(binds);
}

std::shared_ptr<const KeybindMap> KeybindOverrides() {
  std::lock_guard lock(g_overrides_mutex);
  return g_overrides;
}

void HeldKeyMask::Apply(const std::shared_ptr<const KeybindMap>& binds, const bool (&key_down)[256],
                        bool (&keys)[256]) {
  const bool changed = binds != binds_;
  binds_ = binds;
  for (size_t key = 0; key < 256; ++key) {
    held_back_[key] = key_down[key] && (held_back_[key] || changed);
    keys[key] = key_down[key] && !held_back_[key];
  }
}

const std::string& ResolveKeybind(const KeybindMap* overrides, std::string_view option,
                                  const std::string& value) {
  if (overrides) {
    if (const auto found = overrides->find(option); found != overrides->end()) {
      return found->second;
    }
  }
  return value;
}

bool BindPressed(const bool (&key_down)[256], std::string_view bind, bool exact_modifiers) {
  return AnyMatchingKey(key_down, bind, exact_modifiers,
                        [&](uint16_t vk) { return vk < 256 && key_down[vk]; });
}

bool BindNamesKey(const bool (&key_down)[256], std::string_view bind, uint16_t vk,
                  bool exact_modifiers) {
  return AnyMatchingKey(key_down, bind, exact_modifiers, [&](uint16_t key) { return key == vk; });
}

}  // namespace rex::input::mnk
