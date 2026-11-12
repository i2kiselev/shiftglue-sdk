/**
 * @file        tests/unit/filesystem/content_override_test.cpp
 * @brief       The host replaces a guest file's bytes for read-only opens of the same size
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/content_override.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

namespace {

using rex::filesystem::File;
using rex::filesystem::FileAccess;
using rex::filesystem::FileAction;
using rex::filesystem::FileDisposition;
using rex::X_STATUS;

constexpr std::string_view kGuestPath = "game:\\media\\audio\\mixer.xml";

std::vector<uint8_t> Bytes(std::string_view text) {
  return {text.begin(), text.end()};
}

struct GameMount {
  std::filesystem::path host;
  rex::filesystem::VirtualFileSystem vfs;

  GameMount() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    host = std::filesystem::temp_directory_path() /
           ("rexglue-content-override-" + std::to_string(nonce));
    std::filesystem::create_directories(host / "media" / "audio");
    FILE* file = rex::filesystem::OpenFile(host / "media" / "audio" / "mixer.xml", "wb");
    REQUIRE(file != nullptr);
    fwrite("0123456789", 1, 10, file);
    fclose(file);
    auto device =
        std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Game", host, true);
    REQUIRE(device->Initialize());
    REQUIRE(vfs.RegisterDevice(std::move(device)));
    REQUIRE(vfs.RegisterSymbolicLink("game:", "\\Device\\Game"));
  }

  ~GameMount() {
    rex::filesystem::SetContentOverride(kGuestPath, {});
    std::error_code error;
    std::filesystem::remove_all(host, error);
  }

  File* Open(uint32_t access) {
    File* file = nullptr;
    FileAction action;
    REQUIRE(vfs.OpenFile(nullptr, kGuestPath, FileDisposition::kOpen, access, false, true, &file,
                         &action) == X_STATUS_SUCCESS);
    return rex::filesystem::ApplyContentOverride(kGuestPath, access, file);
  }
};

std::string Read(File* file, size_t offset, size_t length, X_STATUS* status = nullptr) {
  std::array<uint8_t, 32> buffer{};
  size_t read = 0;
  const X_STATUS result = file->ReadSync(std::span<uint8_t>(buffer.data(), length), offset, &read);
  if (status) *status = result;
  return std::string(reinterpret_cast<const char*>(buffer.data()), read);
}

}  // namespace

TEST_CASE("A read-only open of an overridden file reads the replacement",
          "[filesystem][override]") {
  GameMount mount;
  rex::filesystem::SetContentOverride(kGuestPath, Bytes("abcdefghij"));
  File* file = mount.Open(FileAccess::kGenericRead);
  CHECK(Read(file, 3, 4) == "defg");
  CHECK(file->entry()->size() == 10);
  file->Destroy();
}

TEST_CASE("An override matches the guest path in any case and slash direction",
          "[filesystem][override]") {
  GameMount mount;
  rex::filesystem::SetContentOverride("GAME:/Media/AUDIO\\Mixer.XML", Bytes("abcdefghij"));
  File* file = mount.Open(FileAccess::kGenericRead);
  CHECK(Read(file, 0, 10) == "abcdefghij");
  file->Destroy();
  rex::filesystem::SetContentOverride("GAME:/Media/AUDIO\\Mixer.XML", {});
  CHECK_FALSE(rex::filesystem::GuestPathsEqual(kGuestPath, "game:\\media\\audio\\mixer.xml.bak"));
}

TEST_CASE("A replacement of another size leaves the real file", "[filesystem][override]") {
  GameMount mount;
  rex::filesystem::SetContentOverride(kGuestPath, Bytes("abcdefghijk"));
  File* file = mount.Open(FileAccess::kGenericRead);
  CHECK(Read(file, 0, 10) == "0123456789");
  file->Destroy();
}

TEST_CASE("An open with write access leaves the real file", "[filesystem][override]") {
  GameMount mount;
  rex::filesystem::SetContentOverride(kGuestPath, Bytes("abcdefghij"));
  File* file = mount.Open(FileAccess::kGenericRead | FileAccess::kFileWriteData);
  CHECK(Read(file, 0, 10) == "0123456789");
  file->Destroy();
}

TEST_CASE("An empty replacement removes the override", "[filesystem][override]") {
  GameMount mount;
  rex::filesystem::SetContentOverride(kGuestPath, Bytes("abcdefghij"));
  rex::filesystem::SetContentOverride(kGuestPath, {});
  File* file = mount.Open(FileAccess::kGenericRead);
  CHECK(Read(file, 0, 10) == "0123456789");
  file->Destroy();
}

TEST_CASE("An overridden file reads short at its end and refuses writes",
          "[filesystem][override]") {
  GameMount mount;
  rex::filesystem::SetContentOverride(kGuestPath, Bytes("abcdefghij"));
  File* file = mount.Open(FileAccess::kGenericRead);
  X_STATUS status = 0;
  CHECK(Read(file, 6, 8, &status) == "ghij");
  CHECK(status == X_STATUS_SUCCESS);
  CHECK(Read(file, 10, 4, &status).empty());
  CHECK(status == X_STATUS_END_OF_FILE);
  const std::array<uint8_t, 1> one{};
  size_t written = 0;
  CHECK(file->WriteSync(one, 0, &written) == X_STATUS_ACCESS_DENIED);
  CHECK(file->SetLength(4) == X_STATUS_ACCESS_DENIED);
  file->Destroy();
}
