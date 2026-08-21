// Copyright 2026 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Standalone support code shared by PoC-B and PoC-C.
//
// This is intentionally independent of Ray. The arena implementation uses the
// vendored public-domain dlmalloc 2.8.6 mspace API, and the scanner walks that
// allocator's real physical chunk chain.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace reclaim_poc {

using Clock = std::chrono::steady_clock;

struct Allocation {
  void *address = nullptr;
  size_t size = 0;
  ptrdiff_t offset = -1;

  Allocation() = default;
  Allocation(void *allocation_address, size_t allocation_size, ptrdiff_t arena_offset)
      : address(allocation_address), size(allocation_size), offset(arena_offset) {}

  Allocation(const Allocation &) = delete;
  Allocation &operator=(const Allocation &) = delete;
  // Moving into an already-populated Allocation is a caller error: Allocation
  // is only a token and cannot free without its owning arena.
  Allocation(Allocation &&other) noexcept;
  Allocation &operator=(Allocation &&other) noexcept;
};

enum class FreePageRangeVisitResult { kContinue, kStop };

// All positions are numeric offsets from the arena mapping. No allocator
// pointer survives a scan call. A validation offset is reusable only while the
// topology generation still matches; target_offset remains monotonic across a
// topology change.
struct FreePageScanCursor {
  uint64_t topology_generation = 0;
  size_t target_offset = 0;
  size_t validation_chunk_offset = 0;
};

struct FreePageScanResult {
  bool valid = false;
  bool reached_end = false;
  size_t visited_chunks = 0;
  size_t candidate_bytes = 0;
  FreePageScanCursor next_cursor;
};

using FreePageRangeVisitor =
    std::function<FreePageRangeVisitResult(void *address, size_t size)>;

class ArenaTrimBackend {
 public:
  virtual ~ArenaTrimBackend() = default;

  virtual int64_t PrimaryLogicalBytes() const = 0;
  virtual std::optional<int64_t> PrimaryPhysicalBytes() const = 0;
  virtual uint64_t TopologyGeneration() const = 0;
  virtual uint64_t FreeGeneration() const = 0;
  virtual FreePageScanResult VisitFreePageRanges(
      const FreePageScanCursor &cursor,
      size_t page_size,
      size_t byte_budget,
      size_t range_cap,
      Clock::time_point deadline,
      const FreePageRangeVisitor &visitor) = 0;
};

// A single fixed-size, file-backed, MAP_SHARED dlmalloc mspace. It deliberately
// has no fallback path or arena growth: every allocation and every scanner
// range belongs to the same inode and accounting domain. The class is not
// thread-safe; PoC-B/C serialize all calls exactly as PlasmaStore does.
class StandaloneArena final : public ArenaTrimBackend {
 public:
  StandaloneArena(const std::string &directory, size_t capacity);
  ~StandaloneArena() override;

  StandaloneArena(const StandaloneArena &) = delete;
  StandaloneArena &operator=(const StandaloneArena &) = delete;

  bool IsValid() const;
  const std::string &Status() const;
  size_t Capacity() const;

  std::optional<Allocation> Allocate(size_t size);
  bool Free(Allocation &&allocation);

  int64_t PrimaryLogicalBytes() const override;
  std::optional<int64_t> PrimaryPhysicalBytes() const override;
  uint64_t TopologyGeneration() const override;
  uint64_t FreeGeneration() const override;
  FreePageScanResult VisitFreePageRanges(
      const FreePageScanCursor &cursor,
      size_t page_size,
      size_t byte_budget,
      size_t range_cap,
      Clock::time_point deadline,
      const FreePageRangeVisitor &visitor) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class PhysicalTrimState : int {
  kDisabled = 0,
  kIdle = 1,
  kTrimming = 2,
  kSuspendedOom = 3,
  kNoProgress = 4,
};

struct PhysicalPageTrimConfig {
  bool enabled = false;
  double start_ratio = 0.50;
  double stop_ratio = 0.60;
  int64_t low_ratio_grace_ms = 30000;
  int64_t check_interval_ms = 1000;
  uint64_t quantum_bytes = 128ULL * 1024 * 1024;
  uint64_t syscall_bytes = 4ULL * 1024 * 1024;
  int64_t quantum_time_ms = 10;
  int64_t min_yield_ms = 0;
  int64_t oom_resume_cooldown_ms = 1000;
};

std::string ValidatePhysicalPageTrimConfig(const PhysicalPageTrimConfig &config,
                                           size_t page_size);

struct PhysicalTrimStepResult {
  PhysicalTrimState state = PhysicalTrimState::kDisabled;
  bool schedule_next = false;
  bool quantum_ran = false;
  std::chrono::milliseconds next_delay{0};
  uint64_t advised_bytes = 0;
  uint64_t observed_physical_drop = 0;
  uint64_t syscall_count = 0;
  double syscall_duration_ms = 0;
  int error = 0;
};

class PhysicalPageTrimmer {
 public:
  using RemovePages = std::function<int(void *, size_t)>;
  using NowProvider = std::function<Clock::time_point()>;

  PhysicalPageTrimmer(ArenaTrimBackend &backend,
                      PhysicalPageTrimConfig config,
                      size_t page_size,
                      RemovePages remove_pages);
  PhysicalPageTrimmer(ArenaTrimBackend &backend,
                      PhysicalPageTrimConfig config,
                      size_t page_size,
                      RemovePages remove_pages,
                      NowProvider now_provider);

  PhysicalTrimStepResult Step(bool create_queue_pending, Clock::time_point now);
  PhysicalTrimState GetState() const;
  int64_t PrimaryLogicalBytes() const;
  std::optional<int64_t> PrimaryPhysicalBytes() const;
  const std::string &GetStatus() const;

 private:
  struct ArenaUsage {
    uint64_t logical_bytes = 0;
    uint64_t physical_bytes = 0;
  };

  bool HandleOomState(bool create_queue_pending,
                      Clock::time_point now,
                      PhysicalTrimStepResult *result);
  bool ReadArenaUsage(ArenaUsage *usage, PhysicalTrimStepResult *result);
  bool PrepareTrim(const ArenaUsage &usage,
                   Clock::time_point now,
                   PhysicalTrimStepResult *result);
  PhysicalTrimStepResult RunTrimQuantum(const ArenaUsage &before,
                                        PhysicalTrimStepResult result);
  FreePageRangeVisitResult RemoveRange(void *address,
                                       size_t size,
                                       uint64_t byte_budget,
                                       Clock::time_point quantum_deadline,
                                       int *visit_error,
                                       PhysicalTrimStepResult *result);
  void Disable(const std::string &reason, int error, PhysicalTrimStepResult *result);
  void EnterIdle();
  void ResetScanCursor();
  void ScheduleCheck(PhysicalTrimStepResult *result) const;
  void ScheduleTrim(PhysicalTrimStepResult *result) const;
  bool RatioAtLeast(const ArenaUsage &usage, double threshold) const;
  bool UsageMeetsStop(const ArenaUsage &usage) const;
  uint64_t ComputeByteBudget() const;
  Clock::time_point DeadlineFrom(Clock::time_point now) const;

  ArenaTrimBackend &backend_;
  const PhysicalPageTrimConfig config_;
  const size_t page_size_;
  const RemovePages remove_pages_;
  const NowProvider now_provider_;

  PhysicalTrimState state_ = PhysicalTrimState::kDisabled;
  std::string status_;
  std::optional<Clock::time_point> low_ratio_since_;
  std::optional<Clock::time_point> oom_cleared_at_;
  FreePageScanCursor scan_cursor_;
  uint64_t pass_start_free_generation_ = 0;
};

size_t SystemPageSize();
bool IsTmpfs(const std::string &path);
int RemovePagesWithMadvise(void *address, size_t size);

}  // namespace reclaim_poc
