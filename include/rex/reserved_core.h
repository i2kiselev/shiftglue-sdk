#pragma once
/**
 * @file        rex/reserved_core.h
 * @brief       Chooses one physical core for a single thread and the CPU sets left to the rest
 *              of the process.
 *
 * Platform-neutral so it can be tested on synthetic topologies; Windows feeds it from
 * GetSystemCpuSetInformation (see ReserveCoreForCurrentThread in rex/thread.h).
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace rex::thread {

struct CpuSetEntry {
  uint32_t id;         // CPU set id
  uint16_t group;      // processor group
  uint8_t logical;     // logical processor index in the group
  uint8_t core;        // CoreIndex
  uint8_t cache;       // LastLevelCacheIndex
  uint8_t efficiency;  // EfficiencyClass, higher is faster
  uint64_t l3_bytes;   // size of its last-level cache, 0 if unknown
};

struct CoreChoice {
  bool reserved = false;
  std::string skip_reason;       // set when !reserved
  uint32_t own = 0;              // the reserving thread's set
  std::vector<uint32_t> others;  // every allowed set outside the chosen core
  uint8_t core = 0, cache = 0, efficiency = 0;
  uint64_t l3_bytes = 0;
};

// allowed_mask: logical processors of process_group the process may use (bit = logical index).
// Sets of other groups are dropped: a process affinity mask confines the process to one group.
CoreChoice ChooseReservedCore(const std::vector<CpuSetEntry>& sets, uint16_t process_group,
                              uint64_t allowed_mask);

}  // namespace rex::thread
