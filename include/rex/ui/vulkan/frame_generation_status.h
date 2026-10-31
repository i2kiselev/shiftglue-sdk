#pragma once

#include <string>

namespace rex::ui::vulkan {

// Runtime state for settings UI. These return actual provider state, not the
// requested CVar value.
std::string Fm4FrameGenerationStatus();
std::string Fm4ReflexStatus();

}  // namespace rex::ui::vulkan
