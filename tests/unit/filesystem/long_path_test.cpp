/**
 * @file        tests/unit/filesystem/long_path_test.cpp
 * @brief       Host path mounts reach files whose host path exceeds MAX_PATH
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

using rex::X_STATUS;

namespace {

constexpr std::string_view kPayload = "rexglue";
constexpr auto kMount = "\\Device\\LongPathTest";

// Written without ExtendedLengthPath so the setup does not depend on the code under test.
std::filesystem::path RawLongPath(const std::filesystem::path& path) {
#ifdef _WIN32
  return std::filesystem::path(L"\\\\?\\" + path.native());
#else
  return path;
#endif
}

class ScopedLongTree {
 public:
  ScopedLongTree() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() / ("rexglue-long-path-" + std::to_string(nonce));
    guest_ = "long:";
    auto directory = root_;
    for (char letter = 'a'; letter < 'g'; ++letter) {
      const std::string part(40, letter);
      directory /= part;
      guest_ += "\\" + part;
    }
    directory_ = directory;
    std::filesystem::create_directories(RawLongPath(directory_));
  }

  ~ScopedLongTree() {
    std::error_code error;
    std::filesystem::remove_all(RawLongPath(root_), error);
  }

  const std::filesystem::path& root() const { return root_; }
  const std::filesystem::path& directory() const { return directory_; }
  const std::string& guest_directory() const { return guest_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path directory_;
  std::string guest_;
};

void Mount(rex::filesystem::VirtualFileSystem& vfs, const std::filesystem::path& host_root,
           bool read_only) {
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMount, host_root, read_only);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("long:", kMount));
}

}  // namespace

TEST_CASE("Host path mount opens a file past MAX_PATH", "[filesystem][long_path]") {
  ScopedLongTree tree;
  const auto host_file = tree.directory() / "payload.bin";
  REQUIRE(host_file.native().size() > 260);
  {
    FILE* file = rex::filesystem::OpenFile(RawLongPath(host_file), "wb");
    REQUIRE(file != nullptr);
    fwrite(kPayload.data(), 1, kPayload.size(), file);
    fclose(file);
  }

  rex::filesystem::VirtualFileSystem vfs;
  Mount(vfs, tree.root(), true);

  const auto guest_file = tree.guest_directory() + "\\payload.bin";
  REQUIRE(vfs.ResolvePath(guest_file) != nullptr);

  rex::filesystem::File* file = nullptr;
  rex::filesystem::FileAction action;
  REQUIRE(vfs.OpenFile(nullptr, guest_file, rex::filesystem::FileDisposition::kOpen,
                       rex::filesystem::FileAccess::kGenericRead, false, true, &file,
                       &action) == X_STATUS_SUCCESS);
  REQUIRE(file != nullptr);
  std::array<uint8_t, 16> buffer{};
  size_t bytes_read = 0;
  CHECK(file->ReadSync(std::span(buffer.data(), kPayload.size()), 0, &bytes_read) ==
        X_STATUS_SUCCESS);
  CHECK(std::string_view(reinterpret_cast<char*>(buffer.data()), bytes_read) == kPayload);
  file->Destroy();
}

TEST_CASE("Host path mount creates a file past MAX_PATH", "[filesystem][long_path]") {
  ScopedLongTree tree;
  rex::filesystem::VirtualFileSystem vfs;
  Mount(vfs, tree.root(), false);

  const auto guest_file = tree.guest_directory() + "\\created.bin";
  REQUIRE(vfs.CreatePath(guest_file, rex::filesystem::kFileAttributeNormal) != nullptr);
  CHECK(std::filesystem::is_regular_file(RawLongPath(tree.directory() / "created.bin")));
}

#ifdef _WIN32
TEST_CASE("ExtendedLengthPath makes host paths absolute and prefixed", "[filesystem][long_path]") {
  using rex::filesystem::ExtendedLengthPath;
  CHECK(ExtendedLengthPath(L"C:\\a\\..\\b\\c.bin").native() == L"\\\\?\\C:\\b\\c.bin");
  CHECK(ExtendedLengthPath(L"C:/a/b").native() == L"\\\\?\\C:\\a\\b");
  CHECK(ExtendedLengthPath(L"\\\\server\\share\\a").native() == L"\\\\?\\UNC\\server\\share\\a");
  CHECK(ExtendedLengthPath(L"\\\\?\\C:\\a").native() == L"\\\\?\\C:\\a");
  const auto relative = ExtendedLengthPath(L"relative.bin").native();
  CHECK(relative.starts_with(L"\\\\?\\"));
  CHECK(relative.ends_with(L"\\relative.bin"));
}
#endif
