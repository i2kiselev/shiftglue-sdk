/**
 * @file        src/filesystem/content_override.cpp
 * @brief       Host-supplied bytes for a guest file, served to read-only opens
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/content_override.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>

namespace rex::filesystem {
namespace {

using Content = std::shared_ptr<const std::vector<uint8_t>>;

struct Override {
  std::string guest_path;
  Content content;
};

std::mutex overrides_mutex;
std::vector<Override> overrides;

char Fold(char c) {
  if (c == '/') return '\\';
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

class ContentOverrideFile final : public File {
 public:
  ContentOverrideFile(File* inner, Content content)
      : File(inner->file_access(), inner->entry()), inner_(inner), content_(std::move(content)) {}

  void Destroy() override {
    inner_->Destroy();
    delete this;
  }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    if (byte_offset >= content_->size()) return X_STATUS_END_OF_FILE;
    const size_t count = std::min(buffer.size(), content_->size() - byte_offset);
    std::memcpy(buffer.data(), content_->data() + byte_offset, count);
    *out_bytes_read = count;
    return X_STATUS_SUCCESS;
  }

  X_STATUS ReadAsync(std::span<uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_read) override {
    return ReadSync(buffer, byte_offset, out_bytes_read);
  }

  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return X_STATUS_ACCESS_DENIED;
  }

  X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }

 private:
  File* inner_;
  Content content_;
};

}  // namespace

bool GuestPathsEqual(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](char x, char y) { return Fold(x) == Fold(y); });
}

void SetContentOverride(std::string_view guest_path, std::vector<uint8_t> content) {
  std::lock_guard lock(overrides_mutex);
  std::erase_if(overrides, [&](const Override& o) { return GuestPathsEqual(o.guest_path, guest_path); });
  if (!content.empty()) {
    overrides.push_back({std::string(guest_path),
                         std::make_shared<const std::vector<uint8_t>>(std::move(content))});
  }
}

File* ApplyContentOverride(std::string_view guest_path, uint32_t desired_access, File* file) {
  constexpr uint32_t kWriteAccess = FileAccess::kGenericWrite | FileAccess::kGenericAll |
                                    FileAccess::kFileWriteData | FileAccess::kFileAppendData;
  if (!file || (desired_access & kWriteAccess) ||
      (file->entry()->attributes() & kFileAttributeDirectory)) {
    return file;
  }
  Content content;
  {
    std::lock_guard lock(overrides_mutex);
    for (const auto& o : overrides) {
      if (GuestPathsEqual(o.guest_path, guest_path)) content = o.content;
    }
  }
  if (!content) return file;
  if (content->size() != file->entry()->size()) {
    REXFS_WARN("Content override for {} not used: {} bytes, the file has {}", guest_path,
               content->size(), file->entry()->size());
    return file;
  }
  REXFS_INFO("Content override served for {}", guest_path);
  return new ContentOverrideFile(file, std::move(content));
}

}  // namespace rex::filesystem
