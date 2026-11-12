/**
 * @file        include/rex/filesystem/content_override.h
 * @brief       Host-supplied bytes for a guest file, served to read-only opens
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rex::filesystem {

class File;

/// Registers bytes that replace the guest file at `guest_path` (any case,
/// either slash direction); empty `content` removes the override. Only an
/// override of the real file's exact size is served, so the file's size,
/// attributes and queries stay those of the real file.
void SetContentOverride(std::string_view guest_path, std::vector<uint8_t> content);

/// Returns `file`, or a file serving the registered bytes when `guest_path`
/// has an override of the same size and `desired_access` cannot write. Takes
/// ownership of `file` either way.
File* ApplyContentOverride(std::string_view guest_path, uint32_t desired_access, File* file);

bool GuestPathsEqual(std::string_view a, std::string_view b);

}  // namespace rex::filesystem
