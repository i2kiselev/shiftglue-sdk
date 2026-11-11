/**
 * @file        tests/unit/filesystem/delete_on_close_test.cpp
 * @brief       A guest file or directory marked for deletion goes when its last handle closes
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>

namespace {

using rex::filesystem::File;
using rex::filesystem::FileAccess;
using rex::filesystem::FileAction;
using rex::filesystem::FileDisposition;
using rex::X_STATUS;

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rexglue-delete-on-close-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

struct SaveMount {
  ScopedTempDirectory host;
  rex::filesystem::VirtualFileSystem vfs;

  SaveMount() {
    // Save devices share delete access, as the content manager mounts them.
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Save", host.path(),
                                                                    false, true);
    REQUIRE(device->Initialize());
    REQUIRE(vfs.RegisterDevice(std::move(device)));
    REQUIRE(vfs.RegisterSymbolicLink("save:", "\\Device\\Save"));
  }

  File* Open(const char* path, FileDisposition disposition, bool directory = false) {
    File* file = nullptr;
    FileAction action;
    auto status =
        vfs.OpenFile(nullptr, path, disposition, FileAccess::kGenericRead | FileAccess::kGenericWrite,
                     directory, !directory, &file, &action);
    REQUIRE(status == X_STATUS_SUCCESS);
    REQUIRE(file != nullptr);
    return file;
  }
};

}  // namespace

TEST_CASE("A file marked for deletion is removed when its handle closes",
          "[filesystem][delete]") {
  SaveMount mount;
  auto* file = mount.Open("save:\\0003_layers.lvg", FileDisposition::kCreate);
  REQUIRE(std::filesystem::is_regular_file(mount.host.path() / "0003_layers.lvg"));

  file->entry()->SetForDeletion(true);
  CHECK(std::filesystem::exists(mount.host.path() / "0003_layers.lvg"));
  file->Destroy();

  CHECK_FALSE(std::filesystem::exists(mount.host.path() / "0003_layers.lvg"));
  CHECK(mount.vfs.ResolvePath("save:\\0003_layers.lvg") == nullptr);
}

TEST_CASE("A file marked for deletion stays while another handle is open",
          "[filesystem][delete]") {
  SaveMount mount;
  auto* first = mount.Open("save:\\thumb.xdc", FileDisposition::kCreate);
  auto* second = mount.Open("save:\\thumb.xdc", FileDisposition::kOpen);

  second->entry()->SetForDeletion(true);
  second->Destroy();
  CHECK(std::filesystem::exists(mount.host.path() / "thumb.xdc"));

  first->Destroy();
  CHECK_FALSE(std::filesystem::exists(mount.host.path() / "thumb.xdc"));
}

TEST_CASE("A file whose deletion mark was cleared stays", "[filesystem][delete]") {
  SaveMount mount;
  auto* file = mount.Open("save:\\keep.lvg", FileDisposition::kCreate);
  file->entry()->SetForDeletion(true);
  file->entry()->SetForDeletion(false);
  file->Destroy();

  CHECK(std::filesystem::exists(mount.host.path() / "keep.lvg"));
}

TEST_CASE("A directory marked for deletion is removed when its handle closes",
          "[filesystem][delete]") {
  SaveMount mount;
  REQUIRE(mount.vfs.CreatePath("save:\\LivGroups\\Test 1",
                               rex::filesystem::kFileAttributeDirectory) != nullptr);
  auto* directory = mount.Open("save:\\LivGroups\\Test 1", FileDisposition::kOpen, true);

  directory->entry()->SetForDeletion(true);
  directory->Destroy();

  CHECK_FALSE(std::filesystem::exists(mount.host.path() / "LivGroups" / "Test 1"));
  CHECK(std::filesystem::is_directory(mount.host.path() / "LivGroups"));
}

TEST_CASE("A file replaced while open keeps its handle and spares the new file",
          "[filesystem][delete]") {
  SaveMount mount;
  auto* old_handle = mount.Open("save:\\thumb.xdc", FileDisposition::kCreate);
  old_handle->entry()->SetForDeletion(true);

  auto* new_handle = mount.Open("save:\\thumb.xdc", FileDisposition::kOverwriteIf);
  CHECK(new_handle->entry() != old_handle->entry());
  CHECK(old_handle->entry()->name() == "thumb.xdc");

  old_handle->Destroy();
  new_handle->Destroy();
  CHECK(std::filesystem::exists(mount.host.path() / "thumb.xdc"));
  CHECK(mount.vfs.ResolvePath("save:\\thumb.xdc") != nullptr);
}

TEST_CASE("A re-saved file replaces the one deleted before it", "[filesystem][delete]") {
  SaveMount mount;
  auto* old_layers = mount.Open("save:\\0003_layers.lvg", FileDisposition::kCreate);
  old_layers->Destroy();

  auto* doomed = mount.Open("save:\\0003_layers.lvg", FileDisposition::kOpen);
  doomed->entry()->SetForDeletion(true);
  doomed->Destroy();
  mount.Open("save:\\0005_layers.lvg", FileDisposition::kCreate)->Destroy();

  CHECK_FALSE(std::filesystem::exists(mount.host.path() / "0003_layers.lvg"));
  CHECK(std::filesystem::exists(mount.host.path() / "0005_layers.lvg"));
}
