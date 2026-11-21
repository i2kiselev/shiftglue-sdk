/**
 * @file        tests/unit/core/reserved_core_test.cpp
 * @brief       Choice of the physical core reserved for one thread, on synthetic CPU topologies.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <rex/reserved_core.h>

using Catch::Matchers::ContainsSubstring;
using rex::thread::ChooseReservedCore;
using rex::thread::CpuSetEntry;

namespace {

constexpr uint64_t kMB = 1024ull * 1024ull;
constexpr uint64_t kAll = ~0ull;

struct Cores {
  int count = 1;
  int smt = 1;
  int first_logical = 0;
  uint8_t efficiency = 0;
  uint8_t cache = 0;
  uint64_t l3 = 32 * kMB;
  uint16_t group = 0;
};

void Add(std::vector<CpuSetEntry>& sets, const Cores& c) {
  for (int i = 0; i < c.count * c.smt; ++i) {
    auto logical = static_cast<uint8_t>(c.first_logical + i);
    auto core = static_cast<uint8_t>(c.first_logical + (i / c.smt) * c.smt);
    sets.push_back({static_cast<uint32_t>(256 + 64 * c.group + logical), c.group, logical, core,
                    c.cache, c.efficiency, c.l3});
  }
}

std::vector<CpuSetEntry> Uniform(int count, int smt) {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = count, .smt = smt});
  return sets;
}

bool Contains(const std::vector<uint32_t>& ids, uint32_t id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST_CASE("uniform 8 cores with SMT", "[reserved_core]") {
  auto choice = ChooseReservedCore(Uniform(8, 2), 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.core == 14);
  CHECK(choice.own == 256 + 14);
  CHECK(choice.others.size() == 14);
  CHECK_FALSE(Contains(choice.others, 256 + 14));
  CHECK_FALSE(Contains(choice.others, 256 + 15));
}

TEST_CASE("4 cores with SMT", "[reserved_core]") {
  auto choice = ChooseReservedCore(Uniform(4, 2), 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.core == 6);
  CHECK(choice.others.size() == 6);
}

TEST_CASE("4 cores without SMT", "[reserved_core]") {
  auto choice = ChooseReservedCore(Uniform(4, 1), 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.core == 3);
  CHECK(choice.others.size() == 3);
}

TEST_CASE("3 cores", "[reserved_core]") {
  auto choice = ChooseReservedCore(Uniform(3, 2), 0, kAll);
  CHECK_FALSE(choice.reserved);
  CHECK_THAT(choice.skip_reason, ContainsSubstring("3 physical cores"));
}

TEST_CASE("Intel 8P with SMT + 16E", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 8, .smt = 2, .efficiency = 1});
  Add(sets, {.count = 16, .smt = 1, .first_logical = 16, .efficiency = 0});
  auto choice = ChooseReservedCore(sets, 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.efficiency == 1);
  CHECK(choice.core == 14);
  CHECK(choice.others.size() == 30);
}

TEST_CASE("Core Ultra 8P without SMT + 16E", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 8, .smt = 1, .efficiency = 1});
  Add(sets, {.count = 16, .smt = 1, .first_logical = 8, .efficiency = 0});
  auto choice = ChooseReservedCore(sets, 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.efficiency == 1);
  CHECK(choice.core == 7);
  CHECK(choice.others.size() == 23);
}

TEST_CASE("2P with SMT + 4E", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 2, .smt = 2, .efficiency = 1});
  Add(sets, {.count = 4, .smt = 1, .first_logical = 4, .efficiency = 0});
  auto choice = ChooseReservedCore(sets, 0, kAll);
  CHECK_FALSE(choice.reserved);
  CHECK_THAT(choice.skip_reason, ContainsSubstring("2 cores in the fastest class"));
}

TEST_CASE("X3D", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 8, .smt = 2, .cache = 0, .l3 = 96 * kMB});
  Add(sets, {.count = 8, .smt = 2, .first_logical = 16, .cache = 1, .l3 = 32 * kMB});
  auto choice = ChooseReservedCore(sets, 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.cache == 0);
  CHECK(choice.core == 14);
  CHECK(choice.l3_bytes == 96 * kMB);
}

TEST_CASE("two equal dies", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 8, .smt = 2, .cache = 0});
  Add(sets, {.count = 8, .smt = 2, .first_logical = 16, .cache = 1});
  auto choice = ChooseReservedCore(sets, 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.cache == 0);
  CHECK(choice.core == 14);
}

TEST_CASE("equal L3, more cores wins", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 4, .smt = 1, .cache = 0});
  Add(sets, {.count = 6, .smt = 1, .first_logical = 4, .cache = 1});
  auto choice = ChooseReservedCore(sets, 0, kAll);
  REQUIRE(choice.reserved);
  CHECK(choice.cache == 1);
  CHECK(choice.core == 9);
}

TEST_CASE("allowed mask", "[reserved_core]") {
  auto choice = ChooseReservedCore(Uniform(8, 2), 0, 0xFF);
  REQUIRE(choice.reserved);
  CHECK(choice.core == 6);
  CHECK(choice.others.size() == 6);
  CHECK(choice.own < 256 + 8);
  for (auto id : choice.others) CHECK(id < 256 + 8);
}

TEST_CASE("allowed mask below floor", "[reserved_core]") {
  auto choice = ChooseReservedCore(Uniform(8, 2), 0, 0x3F);
  CHECK_FALSE(choice.reserved);
  CHECK_THAT(choice.skip_reason, ContainsSubstring("3 physical cores"));
}

TEST_CASE("second group", "[reserved_core]") {
  std::vector<CpuSetEntry> sets;
  Add(sets, {.count = 4, .smt = 2, .group = 0});
  Add(sets, {.count = 4, .smt = 2, .group = 1});
  auto choice = ChooseReservedCore(sets, 0, 0xFF);
  REQUIRE(choice.reserved);
  CHECK(choice.core == 6);
  CHECK(choice.own == 256 + 6);
  CHECK(choice.others.size() == 6);
  for (auto id : choice.others) CHECK(id < 256 + 64);
}

TEST_CASE("empty list", "[reserved_core]") {
  auto choice = ChooseReservedCore({}, 0, kAll);
  CHECK_FALSE(choice.reserved);
  CHECK_FALSE(choice.skip_reason.empty());
}
