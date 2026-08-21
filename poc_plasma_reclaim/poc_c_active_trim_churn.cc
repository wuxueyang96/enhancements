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

// PoC-C: controller safety and progress while allocator topology churns.
//
// This probe deliberately uses the standalone file-backed dlmalloc arena, its
// real free-page scanner, the standalone PhysicalPageTrimmer state machine, and
// MADV_REMOVE. It does NOT construct PlasmaStore and therefore does not test the
// Store mutex, event-loop callback ordering, CreateRequestQueue, OOM suspension,
// or shutdown gating. Those require separate Store integration/TSan tests. PoC-B
// remains the probe for deep-cursor rebuild cost at million-chunk scale; PoC-C
// focuses on controller behavior and live-data safety at a practical runtime
// size. The arena has no fallback allocation path.
//
// Linux only. Build with the standalone allocator/controller support, then run:
//   ./poc_c_active_trim_churn --dead-mb 256 --live-objs 64
//       --churn-ops 20000 --quantum-ms 10

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "standalone_reclaim.h"

namespace {

using reclaim_poc::Allocation;
using reclaim_poc::ArenaTrimBackend;
using reclaim_poc::FreePageRangeVisitor;
using reclaim_poc::FreePageScanCursor;
using reclaim_poc::FreePageScanResult;
using reclaim_poc::PhysicalPageTrimConfig;
using reclaim_poc::PhysicalPageTrimmer;
using reclaim_poc::PhysicalTrimState;
using reclaim_poc::PhysicalTrimStepResult;
using reclaim_poc::StandaloneArena;
using Clock = reclaim_poc::Clock;

constexpr size_t kMiB = 1024 * 1024;

uint64_t Checksum(const void *address, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(address);
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

void FillDeterministic(void *address, size_t size, uint64_t seed) {
  uint8_t *bytes = static_cast<uint8_t *>(address);
  uint64_t value = seed == 0 ? 0x9e3779b97f4a7c15ULL : seed;
  for (size_t index = 0; index < size; ++index) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    bytes[index] = static_cast<uint8_t>(value);
  }
}

const char *StateName(PhysicalTrimState state) {
  switch (state) {
  case PhysicalTrimState::kDisabled:
    return "Disabled";
  case PhysicalTrimState::kIdle:
    return "Idle";
  case PhysicalTrimState::kTrimming:
    return "Trimming";
  case PhysicalTrimState::kSuspendedOom:
    return "SuspendedOom";
  case PhysicalTrimState::kNoProgress:
    return "NoProgress";
  }
  return "Unknown";
}

struct LiveObject {
  Allocation allocation;
  uint64_t checksum;
};

struct ScanCounters {
  uint64_t visits = 0;
  uint64_t valid_results = 0;
  uint64_t invalid_results = 0;
  uint64_t visited_chunks = 0;
  uint64_t candidate_bytes = 0;
  uint64_t target_progress_bytes = 0;
  uint64_t validation_progress_bytes = 0;
  uint64_t target_stall_visits = 0;
  uint64_t zero_cursor_progress_visits = 0;
  uint64_t rebuild_only_visits = 0;
  uint64_t reached_end_visits = 0;
  uint64_t topology_changes_between_visits = 0;
  uint64_t validation_resets_after_topology_change = 0;
  uint64_t target_rewinds = 0;
  uint64_t input_generation_mismatches = 0;
  uint64_t generation_changes_inside_visit = 0;
  uint64_t free_generation_changes_inside_visit = 0;
  size_t max_target_offset = 0;
};

struct ScanObservation {
  bool present = false;
  FreePageScanCursor input;
  FreePageScanResult output;
  uint64_t topology_before = 0;
  uint64_t topology_after = 0;
  uint64_t free_generation_before = 0;
  uint64_t free_generation_after = 0;
  bool topology_changed_since_previous = false;
  bool validation_reset_since_previous = false;
  bool target_rewound_since_previous = false;
};

class InstrumentedBackend final : public ArenaTrimBackend {
 public:
  explicit InstrumentedBackend(StandaloneArena &arena) : arena_(arena) {}

  int64_t PrimaryLogicalBytes() const override {
    return arena_.PrimaryLogicalBytes();
  }

  std::optional<int64_t> PrimaryPhysicalBytes() const override {
    return arena_.PrimaryPhysicalBytes();
  }

  uint64_t TopologyGeneration() const override { return arena_.TopologyGeneration(); }

  uint64_t FreeGeneration() const override { return arena_.FreeGeneration(); }

  FreePageScanResult VisitFreePageRanges(const FreePageScanCursor &cursor,
                                         size_t page_size,
                                         size_t byte_budget,
                                         size_t range_cap,
                                         Clock::time_point deadline,
                                         const FreePageRangeVisitor &visitor) override {
    ScanObservation observation;
    observation.present = true;
    observation.input = cursor;
    observation.topology_before = arena_.TopologyGeneration();
    observation.free_generation_before = arena_.FreeGeneration();

    if (cursor.topology_generation != observation.topology_before) {
      ++counters_.input_generation_mismatches;
    }

    if (last_observation_.present) {
      if (cursor.topology_generation !=
          last_observation_.output.next_cursor.topology_generation) {
        observation.topology_changed_since_previous = true;
        ++counters_.topology_changes_between_visits;
        if (last_observation_.output.next_cursor.validation_chunk_offset != 0 &&
            cursor.validation_chunk_offset == 0) {
          observation.validation_reset_since_previous = true;
          ++counters_.validation_resets_after_topology_change;
        }
      }
      if (cursor.target_offset < last_observation_.output.next_cursor.target_offset) {
        observation.target_rewound_since_previous = true;
        ++counters_.target_rewinds;
      }
    }

    observation.output = arena_.VisitFreePageRanges(
        cursor, page_size, byte_budget, range_cap, deadline, visitor);
    observation.topology_after = arena_.TopologyGeneration();
    observation.free_generation_after = arena_.FreeGeneration();

    ++counters_.visits;
    if (observation.topology_before != observation.topology_after) {
      ++counters_.generation_changes_inside_visit;
    }
    if (observation.free_generation_before != observation.free_generation_after) {
      ++counters_.free_generation_changes_inside_visit;
    }
    if (!observation.output.valid) {
      ++counters_.invalid_results;
    } else {
      ++counters_.valid_results;
      counters_.visited_chunks += observation.output.visited_chunks;
      counters_.candidate_bytes += observation.output.candidate_bytes;
      if (observation.output.reached_end) {
        ++counters_.reached_end_visits;
      }

      const FreePageScanCursor &next = observation.output.next_cursor;
      if (next.target_offset > cursor.target_offset) {
        counters_.target_progress_bytes += next.target_offset - cursor.target_offset;
      } else if (next.target_offset == cursor.target_offset) {
        ++counters_.target_stall_visits;
      }
      if (next.validation_chunk_offset > cursor.validation_chunk_offset) {
        counters_.validation_progress_bytes +=
            next.validation_chunk_offset - cursor.validation_chunk_offset;
      }
      if (next.target_offset == cursor.target_offset &&
          next.validation_chunk_offset == cursor.validation_chunk_offset) {
        ++counters_.zero_cursor_progress_visits;
      }
      if (next.target_offset == cursor.target_offset &&
          next.validation_chunk_offset > cursor.validation_chunk_offset) {
        ++counters_.rebuild_only_visits;
      }
      counters_.max_target_offset =
          std::max(counters_.max_target_offset, next.target_offset);
    }

    last_observation_ = observation;
    return observation.output;
  }

  const ScanCounters &Counters() const { return counters_; }

  const ScanObservation &LastObservation() const { return last_observation_; }

 private:
  StandaloneArena &arena_;
  ScanCounters counters_;
  ScanObservation last_observation_;
};

ScanCounters CounterDelta(const ScanCounters &after, const ScanCounters &before) {
  ScanCounters delta;
#define SUBTRACT_COUNTER(name) delta.name = after.name - before.name
  SUBTRACT_COUNTER(visits);
  SUBTRACT_COUNTER(valid_results);
  SUBTRACT_COUNTER(invalid_results);
  SUBTRACT_COUNTER(visited_chunks);
  SUBTRACT_COUNTER(candidate_bytes);
  SUBTRACT_COUNTER(target_progress_bytes);
  SUBTRACT_COUNTER(validation_progress_bytes);
  SUBTRACT_COUNTER(target_stall_visits);
  SUBTRACT_COUNTER(zero_cursor_progress_visits);
  SUBTRACT_COUNTER(rebuild_only_visits);
  SUBTRACT_COUNTER(reached_end_visits);
  SUBTRACT_COUNTER(topology_changes_between_visits);
  SUBTRACT_COUNTER(validation_resets_after_topology_change);
  SUBTRACT_COUNTER(target_rewinds);
  SUBTRACT_COUNTER(input_generation_mismatches);
  SUBTRACT_COUNTER(generation_changes_inside_visit);
  SUBTRACT_COUNTER(free_generation_changes_inside_visit);
#undef SUBTRACT_COUNTER
  delta.max_target_offset = after.max_target_offset;
  return delta;
}

bool ReadPhysicalBytes(const InstrumentedBackend &backend,
                       const char *where,
                       int64_t *physical_bytes) {
  const std::optional<int64_t> value = backend.PrimaryPhysicalBytes();
  if (!value.has_value() || *value < 0) {
    fprintf(stderr, "FAIL: primary physical accounting unavailable at %s\n", where);
    return false;
  }
  *physical_bytes = *value;
  return true;
}

bool ReadLogicalBytes(const InstrumentedBackend &backend,
                      const char *where,
                      int64_t *logical_bytes) {
  const int64_t value = backend.PrimaryLogicalBytes();
  if (value < 0) {
    fprintf(stderr, "FAIL: primary logical accounting is negative at %s\n", where);
    return false;
  }
  *logical_bytes = value;
  return true;
}

bool FreeLiveObjects(StandaloneArena &arena, std::vector<LiveObject> *objects) {
  bool ok = true;
  for (LiveObject &object : *objects) {
    if (object.allocation.address != nullptr) {
      ok = arena.Free(std::move(object.allocation)) && ok;
      object.allocation.address = nullptr;
    }
  }
  objects->clear();
  return ok;
}

int64_t PositiveDrop(int64_t before, int64_t after) {
  return before > after ? before - after : 0;
}

struct Config {
  std::string tmpfs = "/dev/shm";
  size_t dead_mb = 256;
  size_t live_objects = 64;
  size_t live_bytes = 64 * 1024;
  size_t churn_operations = 20000;
  size_t churn_bytes = 4096;
  size_t churn_per_step = 8;
  size_t cross_step_live = 2;
  size_t max_drain_steps = 100000;
  int quantum_ms = 10;
  size_t syscall_mb = 4;
  // One 4 MiB free chunk per turn by default, yielding enough real scans to
  // make cursor/reset behavior visible without pretending to be PoC-B's
  // million-chunk deep-cursor liveness test.
  size_t quantum_mb = 4;
  bool sleep_schedule = false;
};

bool ValidateConfig(const Config &config, size_t page_size) {
  if (config.dead_mb == 0 || config.live_objects == 0 || config.live_bytes == 0 ||
      config.churn_bytes == 0 || config.churn_per_step == 0 ||
      config.max_drain_steps == 0 || config.quantum_ms <= 0 || config.syscall_mb == 0 ||
      config.quantum_mb == 0 || config.cross_step_live > config.churn_per_step) {
    fprintf(stderr,
            "FAIL: sizes/counts must be positive and cross-step-live "
            "<= churn-per-step\n");
    return false;
  }
  if (config.dead_mb > std::numeric_limits<size_t>::max() / kMiB ||
      config.syscall_mb > std::numeric_limits<size_t>::max() / kMiB ||
      config.quantum_mb > std::numeric_limits<size_t>::max() / kMiB ||
      config.live_objects > std::numeric_limits<size_t>::max() / config.live_bytes ||
      config.cross_step_live > std::numeric_limits<size_t>::max() / config.churn_bytes) {
    fprintf(stderr, "FAIL: configured byte count overflows size_t\n");
    return false;
  }
  const size_t dead_bytes = config.dead_mb * kMiB;
  if (dead_bytes / config.live_objects < page_size) {
    fprintf(stderr, "FAIL: each dead chunk must be at least one system page\n");
    return false;
  }
  const size_t live_total = config.live_objects * config.live_bytes;
  const size_t cross_step_total = config.cross_step_live * config.churn_bytes;
  if (dead_bytes > std::numeric_limits<size_t>::max() - live_total ||
      dead_bytes + live_total > (std::numeric_limits<size_t>::max() - 64 * kMiB) / 2 ||
      live_total > std::numeric_limits<size_t>::max() - cross_step_total ||
      live_total + cross_step_total >
          static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    fprintf(stderr, "FAIL: arena footprint calculation overflows size_t\n");
    return false;
  }
  return true;
}

void PrintStep(const char *phase,
               size_t quantum,
               const PhysicalTrimStepResult &result,
               const InstrumentedBackend &backend,
               uint64_t visits_before) {
  const ScanCounters &counters = backend.Counters();
  const bool scanned = counters.visits != visits_before;
  const ScanObservation &scan = backend.LastObservation();
  printf("%s,%zu,%s,%llu,%llu,%d,%lld,%d",
         phase,
         quantum,
         StateName(result.state),
         static_cast<unsigned long long>(result.advised_bytes / 1024),
         static_cast<unsigned long long>(result.observed_physical_drop / 1024),
         result.quantum_ran ? 1 : 0,
         static_cast<long long>(result.next_delay.count()),
         scanned ? 1 : 0);
  if (scanned && scan.present) {
    printf(",%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%d,%zu,%zu,%zu,%zu,%zu,%d\n",
           static_cast<unsigned long long>(scan.input.topology_generation),
           static_cast<unsigned long long>(scan.output.next_cursor.topology_generation),
           static_cast<unsigned long long>(scan.topology_before),
           static_cast<unsigned long long>(scan.topology_after),
           static_cast<unsigned long long>(scan.free_generation_before),
           static_cast<unsigned long long>(scan.free_generation_after),
           scan.topology_changed_since_previous ? 1 : 0,
           scan.validation_reset_since_previous ? 1 : 0,
           scan.target_rewound_since_previous ? 1 : 0,
           scan.input.target_offset,
           scan.output.next_cursor.target_offset,
           scan.input.validation_chunk_offset,
           scan.output.next_cursor.validation_chunk_offset,
           scan.output.visited_chunks,
           scan.output.reached_end ? 1 : 0);
  } else {
    printf(",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n");
  }
}

int Run(const Config &config) {
  const size_t page_size = reclaim_poc::SystemPageSize();
  if (!ValidateConfig(config, page_size)) {
    return 2;
  }
  if (!reclaim_poc::IsTmpfs(config.tmpfs)) {
    fprintf(stderr, "SKIP: %s is not tmpfs\n", config.tmpfs.c_str());
    return 77;
  }

  const size_t dead_bytes = config.dead_mb * kMiB;
  const size_t live_total = config.live_objects * config.live_bytes;
  const size_t footprint = (dead_bytes + live_total) * 2 + 64 * kMiB;
  if (footprint > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    fprintf(stderr, "FAIL: arena footprint does not fit int64_t\n");
    return 2;
  }

  StandaloneArena arena(config.tmpfs, footprint);
  if (!arena.IsValid()) {
    fprintf(stderr, "FAIL: standalone arena initialization failed: %s\n",
            arena.Status().c_str());
    return 1;
  }
  InstrumentedBackend backend(arena);
  std::vector<LiveObject> stable_live;
  std::vector<LiveObject> cross_step_live;
  std::vector<Allocation> dead_allocations;

  auto cleanup = [&] {
    bool freed = FreeLiveObjects(arena, &cross_step_live);
    freed = FreeLiveObjects(arena, &stable_live) && freed;
    for (Allocation &allocation : dead_allocations) {
      if (allocation.address != nullptr) {
        freed = arena.Free(std::move(allocation)) && freed;
        allocation.address = nullptr;
      }
    }
    dead_allocations.clear();
    if (!freed) {
      fprintf(stderr, "FAIL: allocator rejected an allocation during cleanup\n");
    }
  };

  const size_t dead_chunk_bytes = dead_bytes / config.live_objects;
  for (size_t index = 0; index < config.live_objects; ++index) {
    auto live_allocation = arena.Allocate(config.live_bytes);
    if (!live_allocation.has_value()) {
      fprintf(stderr, "FAIL: live allocation failed at index %zu\n", index);
      cleanup();
      return 1;
    }
    FillDeterministic(live_allocation->address, config.live_bytes, 0x1000 + index);
    const uint64_t live_checksum = Checksum(live_allocation->address, config.live_bytes);
    stable_live.push_back({std::move(*live_allocation), live_checksum});

    auto dead_allocation = arena.Allocate(dead_chunk_bytes);
    if (!dead_allocation.has_value()) {
      fprintf(stderr, "FAIL: dead allocation failed at index %zu\n", index);
      cleanup();
      return 1;
    }
    for (size_t offset = 0; offset < dead_allocation->size;
         offset += page_size) {
      static_cast<volatile uint8_t *>(dead_allocation->address)[offset] =
          static_cast<uint8_t>(offset);
    }
    dead_allocations.push_back(std::move(*dead_allocation));
  }

  int64_t physical_touched = 0;
  if (!ReadPhysicalBytes(backend, "after initial touch", &physical_touched)) {
    cleanup();
    return 1;
  }
  for (Allocation &allocation : dead_allocations) {
    if (!arena.Free(std::move(allocation))) {
      fprintf(stderr, "FAIL: initial dead allocation free failed\n");
      cleanup();
      return 1;
    }
    allocation.address = nullptr;
  }
  dead_allocations.clear();

  int64_t physical_after_free = 0;
  if (!ReadPhysicalBytes(backend, "after initial free", &physical_after_free)) {
    cleanup();
    return 1;
  }
  int64_t logical_after_free = 0;
  if (!ReadLogicalBytes(backend, "after initial free", &logical_after_free) ||
      physical_after_free <= 0) {
    fprintf(stderr,
            "FAIL: invalid initial L/P accounting: L=%lld P=%lld\n",
            static_cast<long long>(logical_after_free),
            static_cast<long long>(physical_after_free));
    cleanup();
    return 1;
  }

  PhysicalPageTrimConfig trim_config;
  trim_config.enabled = true;
  trim_config.start_ratio = 0.5;
  trim_config.stop_ratio = 0.95;
  trim_config.low_ratio_grace_ms = 0;
  trim_config.quantum_bytes = config.quantum_mb * kMiB;
  trim_config.syscall_bytes = config.syscall_mb * kMiB;
  trim_config.quantum_time_ms = config.quantum_ms;
  const std::string config_error =
      reclaim_poc::ValidatePhysicalPageTrimConfig(trim_config, page_size);
  if (!config_error.empty()) {
    fprintf(stderr, "FAIL: invalid trimmer configuration: %s\n", config_error.c_str());
    cleanup();
    return 2;
  }

  const long double initial_ratio = static_cast<long double>(logical_after_free) /
                                    static_cast<long double>(physical_after_free);
  const int64_t dlfree_tolerance = 16 * static_cast<int64_t>(kMiB);
  if (initial_ratio >= trim_config.start_ratio ||
      PositiveDrop(physical_touched, physical_after_free) > dlfree_tolerance) {
    fprintf(stderr,
            "FAIL: experiment precondition not met: L/P=%.6Lf, touched=%lld, "
            "after_free=%lld\n",
            initial_ratio,
            static_cast<long long>(physical_touched),
            static_cast<long long>(physical_after_free));
    cleanup();
    return 1;
  }

  PhysicalPageTrimmer trimmer(
      backend, trim_config, page_size, reclaim_poc::RemovePagesWithMadvise);
  if (trimmer.GetState() != PhysicalTrimState::kIdle ||
      trimmer.GetStatus() != "enabled") {
    fprintf(stderr,
            "FAIL: trimmer disabled at construction: %s\n",
            trimmer.GetStatus().c_str());
    cleanup();
    return 1;
  }

  auto verify_objects = [&](const char *where) {
    const auto verify_vector = [&](const std::vector<LiveObject> &objects,
                                   const char *kind) {
      for (size_t index = 0; index < objects.size(); ++index) {
        const LiveObject &object = objects[index];
        if (object.allocation.address == nullptr || object.allocation.size == 0 ||
            Checksum(object.allocation.address, object.allocation.size) !=
                object.checksum) {
          fprintf(
              stderr, "FAIL: %s live object %zu corrupted at %s\n", kind, index, where);
          return false;
        }
      }
      return true;
    };
    return verify_vector(stable_live, "stable") &&
           verify_vector(cross_step_live, "cross-step");
  };

  fprintf(stderr,
          "# scope=standalone-dlmalloc+real-scanner+real-controller+MADV_REMOVE; "
          "no-PlasmaStore/no-Store-mutex/no-fallback; deprecated --fallback is "
          "ignored; deep-cursor verdict belongs to PoC-B\n");
  fprintf(stderr,
          "# schedule_mode=%s; returned schedule_next/next_delay are honored%s\n",
          config.sleep_schedule ? "wall-clock-sleep" : "virtual-no-sleep",
          config.sleep_schedule ? "" : " by advancing controller time without sleeping");
  fprintf(stderr,
          "# live=%zu logical=%lld physical: touched=%lld after_free=%lld "
          "L/P=%.6Lf\n",
          stable_live.size(),
          static_cast<long long>(logical_after_free),
          static_cast<long long>(physical_touched),
          static_cast<long long>(physical_after_free),
          initial_ratio);
  printf(
      "phase,quantum,state,advised_kb,obs_drop_kb,quantum_ran,next_delay_ms,"
      "scan,cursor_generation,next_generation,topology_before,topology_after,"
      "free_before,"
      "free_after,topology_changed,validation_reset,target_rewound,target_in,"
      "target_out,validation_in,validation_out,visited_chunks,reached_end\n");

  Clock::time_point controller_time = Clock::now();
  std::chrono::milliseconds pending_delay(0);
  bool successor_scheduled = true;
  auto run_scheduled_step = [&](PhysicalTrimStepResult *result) {
    if (!successor_scheduled) {
      fprintf(stderr, "FAIL: attempted Step without a scheduled successor\n");
      return false;
    }
    if (config.sleep_schedule) {
      if (pending_delay.count() > 0) {
        std::this_thread::sleep_for(pending_delay);
      }
      controller_time = Clock::now();
    } else {
      controller_time += pending_delay;
    }
    *result = trimmer.Step(/*create_queue_pending=*/false, controller_time);
    if (result->state == PhysicalTrimState::kDisabled) {
      fprintf(stderr,
              "FAIL: trimmer disabled during Step: status=%s errno=%d\n",
              trimmer.GetStatus().c_str(),
              result->error);
      return false;
    }
    if (result->state != trimmer.GetState() ||
        result->state == PhysicalTrimState::kSuspendedOom ||
        trimmer.GetStatus() != "enabled" || !result->schedule_next ||
        result->next_delay.count() < 0) {
      fprintf(stderr,
              "FAIL: invalid Step scheduling/state: result=%s actual=%s "
              "schedule_next=%d delay_ms=%lld status=%s\n",
              StateName(result->state),
              StateName(trimmer.GetState()),
              result->schedule_next ? 1 : 0,
              static_cast<long long>(result->next_delay.count()),
              trimmer.GetStatus().c_str());
      return false;
    }
    successor_scheduled = result->schedule_next;
    pending_delay = result->next_delay;
    return true;
  };

  const ScanCounters scan_before_active = backend.Counters();
  size_t churn_done = 0;
  size_t active_steps = 0;
  size_t cross_step_objects_verified = 0;
  uint64_t active_advised_bytes = 0;
  uint64_t active_observed_drop = 0;
  bool run_ok = true;

  while (churn_done < config.churn_operations) {
    if (!verify_objects("before active churn")) {
      run_ok = false;
      break;
    }
    cross_step_objects_verified += cross_step_live.size();
    if (!FreeLiveObjects(arena, &cross_step_live)) {
      fprintf(stderr, "FAIL: cross-step allocation free failed\n");
      run_ok = false;
      break;
    }

    for (size_t index = 0;
         index < config.churn_per_step && churn_done < config.churn_operations;
         ++index, ++churn_done) {
      auto allocation = arena.Allocate(config.churn_bytes);
      if (!allocation.has_value()) {
        fprintf(stderr, "FAIL: churn allocation failed at operation %zu\n", churn_done);
        run_ok = false;
        break;
      }
      FillDeterministic(
          allocation->address, config.churn_bytes, 0xc0000000ULL + churn_done);
      if (index < config.cross_step_live) {
        const uint64_t checksum = Checksum(allocation->address, config.churn_bytes);
        cross_step_live.push_back({std::move(*allocation), checksum});
      } else {
        if (!arena.Free(std::move(*allocation))) {
          fprintf(stderr, "FAIL: churn allocation free failed at operation %zu\n",
                  churn_done);
          run_ok = false;
          break;
        }
      }
    }
    if (!run_ok) {
      break;
    }

    const uint64_t visits_before = backend.Counters().visits;
    PhysicalTrimStepResult result;
    if (!run_scheduled_step(&result)) {
      run_ok = false;
      break;
    }
    active_advised_bytes += result.advised_bytes;
    active_observed_drop += result.observed_physical_drop;
    if (backend.Counters().visits != visits_before || (active_steps % 200) == 0 ||
        result.state == PhysicalTrimState::kNoProgress) {
      PrintStep("churn", active_steps, result, backend, visits_before);
    }
    if (!verify_objects("after active-trim Step")) {
      run_ok = false;
      break;
    }
    cross_step_objects_verified += cross_step_live.size();
    ++active_steps;
  }

  int64_t physical_after_churn = 0;
  int64_t logical_after_churn = 0;
  if (run_ok &&
      !ReadPhysicalBytes(backend, "after active churn", &physical_after_churn)) {
    run_ok = false;
  }
  if (run_ok && !ReadLogicalBytes(backend, "after active churn", &logical_after_churn)) {
    run_ok = false;
  }
  const int64_t expected_active_logical =
      static_cast<int64_t>(config.live_objects * config.live_bytes +
                           cross_step_live.size() * config.churn_bytes);
  if (run_ok && logical_after_churn != expected_active_logical) {
    fprintf(stderr,
            "FAIL: active logical accounting mismatch: expected=%lld actual=%lld\n",
            static_cast<long long>(expected_active_logical),
            static_cast<long long>(logical_after_churn));
    run_ok = false;
  }
  const ScanCounters scan_after_active = backend.Counters();
  const ScanCounters active_scan = CounterDelta(scan_after_active, scan_before_active);

  if (run_ok && !verify_objects("active phase boundary")) {
    run_ok = false;
  }
  cross_step_objects_verified += cross_step_live.size();
  if (!FreeLiveObjects(arena, &cross_step_live)) {
    fprintf(stderr, "FAIL: active-phase cross-step free failed\n");
    run_ok = false;
  }

  int64_t logical_before_drain = 0;
  if (run_ok && !ReadLogicalBytes(backend, "before drain", &logical_before_drain)) {
    run_ok = false;
  }
  if (run_ok && logical_before_drain !=
                    static_cast<int64_t>(config.live_objects * config.live_bytes)) {
    fprintf(stderr, "FAIL: logical accounting did not drain cross-step objects\n");
    run_ok = false;
  }

  const ScanCounters scan_before_drain = backend.Counters();
  uint64_t drain_advised_bytes = 0;
  uint64_t drain_observed_drop = 0;
  size_t drain_steps = 0;
  PhysicalTrimState final_state = trimmer.GetState();
  bool terminal_reached = false;
  if (run_ok) {
    for (; drain_steps < config.max_drain_steps;) {
      const uint64_t visits_before = backend.Counters().visits;
      PhysicalTrimStepResult result;
      if (!run_scheduled_step(&result)) {
        run_ok = false;
        break;
      }
      ++drain_steps;
      drain_advised_bytes += result.advised_bytes;
      drain_observed_drop += result.observed_physical_drop;
      final_state = result.state;
      if (backend.Counters().visits != visits_before || (drain_steps % 200) == 0) {
        PrintStep("drain", drain_steps - 1, result, backend, visits_before);
      }
      if (!verify_objects("drain Step")) {
        run_ok = false;
        break;
      }
      if ((final_state == PhysicalTrimState::kNoProgress ||
           final_state == PhysicalTrimState::kIdle) &&
          result.advised_bytes == 0) {
        terminal_reached = true;
        break;
      }
    }
  }

  const ScanCounters scan_after_drain = backend.Counters();
  const ScanCounters drain_scan = CounterDelta(scan_after_drain, scan_before_drain);
  int64_t physical_final = 0;
  if (run_ok && !ReadPhysicalBytes(backend, "final", &physical_final)) {
    run_ok = false;
  }
  int64_t logical_final = 0;
  if (run_ok && !ReadLogicalBytes(backend, "final", &logical_final)) {
    run_ok = false;
  }
  const int64_t expected_final_logical =
      static_cast<int64_t>(config.live_objects * config.live_bytes);
  if (run_ok && logical_final != expected_final_logical) {
    fprintf(stderr,
            "FAIL: final logical accounting mismatch: expected=%lld actual=%lld\n",
            static_cast<long long>(expected_final_logical),
            static_cast<long long>(logical_final));
    run_ok = false;
  }
  if (run_ok && !terminal_reached) {
    fprintf(stderr,
            "FAIL: drain did not reach Idle/NoProgress within %zu scheduled "
            "Steps\n",
            config.max_drain_steps);
    run_ok = false;
  }
  if (backend.Counters().invalid_results != 0 ||
      backend.Counters().input_generation_mismatches != 0 ||
      backend.Counters().generation_changes_inside_visit != 0 ||
      backend.Counters().free_generation_changes_inside_visit != 0) {
    fprintf(stderr,
            "FAIL: invalid scan result or allocator generation changed "
            "inside a scan\n");
    run_ok = false;
  }

  const int64_t active_physical_drop =
      PositiveDrop(physical_after_free, physical_after_churn);
  const int64_t post_stop_physical_drop =
      PositiveDrop(physical_after_churn, physical_final);
  const int64_t total_physical_drop = PositiveDrop(physical_after_free, physical_final);
  const int64_t initial_excess =
      std::max<int64_t>(0, physical_after_free - logical_after_free);
  const int64_t meaningful_drop = std::min<int64_t>(
      8 * static_cast<int64_t>(kMiB),
      std::max<int64_t>(static_cast<int64_t>(page_size), initial_excess / 16));
  const bool reclaimed = total_physical_drop >= meaningful_drop;
  if (!reclaimed) {
    fprintf(stderr,
            "FAIL: physical backing did not fall meaningfully: drop=%lld "
            "required=%lld\n",
            static_cast<long long>(total_physical_drop),
            static_cast<long long>(meaningful_drop));
    run_ok = false;
  }
  if (backend.Counters().visits == 0) {
    fprintf(stderr, "FAIL: controller never invoked the real free-page scanner\n");
    run_ok = false;
  }
  if (config.cross_step_live != 0 && config.churn_operations != 0 &&
      cross_step_objects_verified == 0) {
    fprintf(stderr, "FAIL: no reallocated object survived across a trim Step\n");
    run_ok = false;
  }
  const bool live_ok = verify_objects("final");
  run_ok = run_ok && live_ok;

  const bool topology_pressure =
      active_scan.visits >= 8 &&
      static_cast<long double>(active_scan.topology_changes_between_visits) /
              static_cast<long double>(active_scan.visits) >=
          0.5L;
  const bool cursor_stalled = active_scan.reached_end_visits == 0 &&
                              active_scan.visits >= 8 &&
                              static_cast<long double>(active_scan.target_stall_visits) /
                                      static_cast<long double>(active_scan.visits) >=
                                  0.75L;
  const bool post_stop_cursor_progress =
      drain_scan.reached_end_visits != 0 || drain_scan.target_progress_bytes != 0;
  const bool reclaim_shifted_after_stop =
      post_stop_physical_drop >= meaningful_drop &&
      active_physical_drop < post_stop_physical_drop / 4;
  const bool starvation_signal = topology_pressure && cursor_stalled &&
                                 post_stop_cursor_progress && reclaim_shifted_after_stop;

  fprintf(stderr, "\n# ===== VERDICT =====\n");
  fprintf(stderr, "# live_data_intact           : %s\n", live_ok ? "PASS" : "FAIL");
  fprintf(stderr,
          "# cross_step_live_verified   : %zu object-checks\n",
          cross_step_objects_verified);
  fprintf(stderr,
          "# terminal_state             : %s after %zu drain Steps (%s)\n",
          StateName(final_state),
          drain_steps,
          terminal_reached ? "PASS" : "FAIL");
  fprintf(stderr,
          "# physical_active_drop       : %.1f MiB (%lld -> %lld)\n",
          static_cast<double>(active_physical_drop) / static_cast<double>(kMiB),
          static_cast<long long>(physical_after_free),
          static_cast<long long>(physical_after_churn));
  fprintf(stderr,
          "# physical_post_stop_drop    : %.1f MiB (%lld -> %lld)\n",
          static_cast<double>(post_stop_physical_drop) / static_cast<double>(kMiB),
          static_cast<long long>(physical_after_churn),
          static_cast<long long>(physical_final));
  fprintf(stderr,
          "# physical_total_drop        : %.1f MiB (%s)\n",
          static_cast<double>(total_physical_drop) / static_cast<double>(kMiB),
          reclaimed ? "PASS" : "FAIL");
  fprintf(stderr,
          "# active_scan                : visits=%llu topology_changes=%llu "
          "validation_resets=%llu target_progress=%.1fMiB "
          "validation_progress=%.1fMiB "
          "target_stalls=%llu zero_progress=%llu reached_end=%llu "
          "generation_mismatches=%llu "
          "visited_chunks=%llu\n",
          static_cast<unsigned long long>(active_scan.visits),
          static_cast<unsigned long long>(active_scan.topology_changes_between_visits),
          static_cast<unsigned long long>(
              active_scan.validation_resets_after_topology_change),
          static_cast<double>(active_scan.target_progress_bytes) /
              static_cast<double>(kMiB),
          static_cast<double>(active_scan.validation_progress_bytes) /
              static_cast<double>(kMiB),
          static_cast<unsigned long long>(active_scan.target_stall_visits),
          static_cast<unsigned long long>(active_scan.zero_cursor_progress_visits),
          static_cast<unsigned long long>(active_scan.reached_end_visits),
          static_cast<unsigned long long>(active_scan.input_generation_mismatches),
          static_cast<unsigned long long>(active_scan.visited_chunks));
  fprintf(stderr,
          "# drain_scan                 : visits=%llu target_progress=%.1fMiB "
          "validation_progress=%.1fMiB reached_end=%llu visited_chunks=%llu\n",
          static_cast<unsigned long long>(drain_scan.visits),
          static_cast<double>(drain_scan.target_progress_bytes) /
              static_cast<double>(kMiB),
          static_cast<double>(drain_scan.validation_progress_bytes) /
              static_cast<double>(kMiB),
          static_cast<unsigned long long>(drain_scan.reached_end_visits),
          static_cast<unsigned long long>(drain_scan.visited_chunks));
  fprintf(stderr,
          "# advised_bytes_supplemental : active=%.1fMiB drain=%.1fMiB\n",
          static_cast<double>(active_advised_bytes) / static_cast<double>(kMiB),
          static_cast<double>(drain_advised_bytes) / static_cast<double>(kMiB));
  fprintf(stderr,
          "# observed_drop_supplemental : active=%.1fMiB drain=%.1fMiB\n",
          static_cast<double>(active_observed_drop) / static_cast<double>(kMiB),
          static_cast<double>(drain_observed_drop) / static_cast<double>(kMiB));
  fprintf(stderr,
          "# starvation_signal          : %s\n",
          starvation_signal ? "LIKELY: physical reclaim and cursor progress "
                              "shifted after churn stopped"
                            : "not observed at this scale; use PoC-B for the "
                              "deep-cursor verdict");
  fprintf(stderr, "# overall                    : %s\n", run_ok ? "PASS" : "FAIL");

  cleanup();
  return run_ok ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string key = argv[index];
    const auto next = [&]() -> const char * {
      return index + 1 < argc ? argv[++index] : "";
    };
    if (key == "--tmpfs") {
      config.tmpfs = next();
    } else if (key == "--fallback") {
      // Deprecated compatibility option. The standalone arena deliberately has
      // no fallback path, but accepting the old spelling keeps prior commands
      // runnable while ignoring the supplied value.
      static_cast<void>(next());
    } else if (key == "--dead-mb") {
      config.dead_mb = strtoull(next(), nullptr, 10);
    } else if (key == "--live-objs") {
      config.live_objects = strtoull(next(), nullptr, 10);
    } else if (key == "--live-bytes") {
      config.live_bytes = strtoull(next(), nullptr, 10);
    } else if (key == "--churn-ops") {
      config.churn_operations = strtoull(next(), nullptr, 10);
    } else if (key == "--churn-bytes") {
      config.churn_bytes = strtoull(next(), nullptr, 10);
    } else if (key == "--churn-per-step") {
      config.churn_per_step = strtoull(next(), nullptr, 10);
    } else if (key == "--cross-step-live") {
      config.cross_step_live = strtoull(next(), nullptr, 10);
    } else if (key == "--max-drain-steps") {
      config.max_drain_steps = strtoull(next(), nullptr, 10);
    } else if (key == "--quantum-ms") {
      config.quantum_ms = atoi(next());
    } else if (key == "--syscall-mb") {
      config.syscall_mb = strtoull(next(), nullptr, 10);
    } else if (key == "--quantum-mb") {
      config.quantum_mb = strtoull(next(), nullptr, 10);
    } else if (key == "--sleep-schedule") {
      config.sleep_schedule = true;
    } else {
      fprintf(stderr, "unknown argument: %s\n", key.c_str());
      return 2;
    }
  }
  return Run(config);
}
