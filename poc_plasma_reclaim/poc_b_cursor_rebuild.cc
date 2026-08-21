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

// PoC-B: cursor rebuild cost and topology-churn starvation using the standalone
// dlmalloc scanner.
//
// The benchmark constructs this primary-arena layout:
//
//   live guard | reusable small chunk | live guard | long live prefix
//              | deep free chunk | live guard | top
//
// It discovers and verifies the scanner token for a complete page range in the
// known deep free allocation. The measured loop starts at that target with
// validation reset to the arena base. A real primary Allocate+Free mutation is
// injected every K quanta. After the bounded mutation phase, mutations stop and
// the same cursor continues until the deep range is reached. The visitor never
// calls madvise, so this benchmark isolates chunk-chain validation and cursor
// progress from hole-punch latency.
//
// Linux only. Run one churn cadence per process so allocator and cache state
// are not shared across benchmark arms.
//
// Example:
//   ./poc_b_cursor_rebuild --live-objs 200000 --obj-bytes 512
//       --churn-every 1 --quantum-ms 10 --max-quanta 4000

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "standalone_reclaim.h"

namespace {

using reclaim_poc::Allocation;
using reclaim_poc::FreePageRangeVisitResult;
using reclaim_poc::FreePageScanCursor;
using reclaim_poc::FreePageScanResult;
using reclaim_poc::StandaloneArena;
using Clock = reclaim_poc::Clock;

constexpr size_t kMiB = 1024 * 1024;

struct Config {
  std::string tmpfs = "/dev/shm";
  size_t live_objs = 200000;
  size_t obj_bytes = 512;
  size_t churn_bytes = 64;
  size_t deep_free_mb = 16;
  int churn_every = 1;  // 0 means no mutation after initial validation reset.
  int quantum_ms = 10;
  size_t range_mb = 4;
  size_t quantum_mb = 128;
  size_t max_quanta = 4000;
  size_t recovery_max_quanta = 4000;
};

struct KnownFreeRange {
  uintptr_t address = 0;
  size_t size = 0;
  size_t offset = 0;
  FreePageScanCursor token;
};

struct LoopOutcome {
  size_t iterations = 0;
  bool arrived = false;
  bool failed = false;
  size_t scheduled_mutations = 0;
  size_t generation_resets = 0;
};

bool CheckedMultiply(size_t lhs, size_t rhs, size_t *result) {
  if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
    return false;
  }
  *result = lhs * rhs;
  return true;
}

bool CheckedAdd(size_t lhs, size_t rhs, size_t *result) {
  if (rhs > std::numeric_limits<size_t>::max() - lhs) {
    return false;
  }
  *result = lhs + rhs;
  return true;
}

bool ParseSize(const char *text, size_t *result) {
  if (text == nullptr || text[0] == '\0' || text[0] == '-') {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' ||
      value > std::numeric_limits<size_t>::max()) {
    return false;
  }
  *result = static_cast<size_t>(value);
  return true;
}

bool ParseNonnegativeInt(const char *text, int *result) {
  size_t value = 0;
  if (!ParseSize(text, &value) ||
      value > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *result = static_cast<int>(value);
  return true;
}

bool IsPrimary(const Allocation &allocation) {
  return allocation.address != nullptr && allocation.size > 0 &&
         allocation.offset >= 0;
}

// Reuse is deliberately strict. If dlmalloc does not return the reserved
// near-base chunk, the mutation no longer has the controlled topology and
// locality this benchmark claims to measure.
bool ChurnOnce(StandaloneArena &allocator,
               size_t bytes,
               ptrdiff_t expected_offset,
               uint64_t *generation_after) {
  const uint64_t before = allocator.TopologyGeneration();
  auto allocation = allocator.Allocate(bytes);
  if (!allocation.has_value()) {
    fprintf(stderr, "ERROR: scheduled churn allocation failed\n");
    return false;
  }
  const uint64_t after_allocate = allocator.TopologyGeneration();
  const ptrdiff_t actual_offset = allocation->offset;
  const bool expected_allocation = IsPrimary(*allocation) &&
                                   actual_offset == expected_offset &&
                                   after_allocate == before + 1;
  const bool freed = allocator.Free(std::move(*allocation));
  const uint64_t after_free = allocator.TopologyGeneration();
  if (!expected_allocation || !freed || after_free != after_allocate + 1) {
    fprintf(stderr,
            "ERROR: churn must reuse the reserved primary chunk and advance "
            "generation twice: expected_offset=%td actual_offset=%td fallback=0 "
            "generation=%llu->%llu->%llu\n",
            expected_offset,
            actual_offset,
            static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(after_allocate),
            static_cast<unsigned long long>(after_free));
    return false;
  }
  *generation_after = after_free;
  return true;
}

bool RangeEnd(uintptr_t begin, size_t size, uintptr_t *end) {
  if (size > std::numeric_limits<uintptr_t>::max() - begin) {
    return false;
  }
  *end = begin + size;
  return true;
}

// Finds the first scanner-offered range wholly inside the known deep
// allocation. Returning kStop preserves an exact numeric token for that range.
bool DiscoverDeepRange(StandaloneArena &allocator,
                       size_t page_size,
                       size_t byte_budget,
                       size_t range_cap,
                       uintptr_t deep_begin,
                       size_t deep_size,
                       size_t deep_allocation_offset,
                       KnownFreeRange *known) {
  uintptr_t deep_end = 0;
  if (!RangeEnd(deep_begin, deep_size, &deep_end)) {
    return false;
  }

  bool found = false;
  bool malformed_overlap = false;
  uintptr_t offered_address = 0;
  size_t offered_size = 0;
  auto visitor = [&](void *address, size_t size) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    uintptr_t end = 0;
    if (!RangeEnd(begin, size, &end)) {
      malformed_overlap = true;
      return FreePageRangeVisitResult::kStop;
    }
    if (begin < deep_end && end > deep_begin) {
      if (begin < deep_begin || end > deep_end) {
        malformed_overlap = true;
      } else {
        found = true;
        offered_address = begin;
        offered_size = size;
      }
      return FreePageRangeVisitResult::kStop;
    }
    return FreePageRangeVisitResult::kContinue;
  };

  const FreePageScanCursor start{allocator.TopologyGeneration(), 0, 0};
  const FreePageScanResult result =
      allocator.VisitFreePageRanges(start,
                                    page_size,
                                    byte_budget,
                                    range_cap,
                                    Clock::now() + std::chrono::hours(1),
                                    visitor);
  if (!result.valid || malformed_overlap || !found || result.reached_end) {
    fprintf(stderr,
            "ERROR: failed to discover deep range: valid=%d found=%d malformed=%d "
            "reached_end=%d visited=%zu\n",
            result.valid ? 1 : 0,
            found ? 1 : 0,
            malformed_overlap ? 1 : 0,
            result.reached_end ? 1 : 0,
            result.visited_chunks);
    return false;
  }

  const size_t bytes_into_allocation = static_cast<size_t>(offered_address - deep_begin);
  size_t expected_range_offset = 0;
  if (!CheckedAdd(
          deep_allocation_offset, bytes_into_allocation, &expected_range_offset) ||
      result.next_cursor.target_offset != expected_range_offset) {
    fprintf(stderr,
            "ERROR: scanner token does not name the known deep range: token=%zu "
            "expected=%zu\n",
            result.next_cursor.target_offset,
            expected_range_offset);
    return false;
  }

  known->address = offered_address;
  known->size = offered_size;
  known->offset = result.next_cursor.target_offset;
  known->token = result.next_cursor;
  if (known->size == 0 || known->address % page_size != 0 ||
      known->size % page_size != 0) {
    fprintf(stderr, "ERROR: discovered deep range is not page aligned\n");
    return false;
  }
  fprintf(stderr,
          "# discovered deep range: allocation_offset=%zu range_offset=%zu "
          "range_bytes=%zu validation_offset=%zu visited=%zu\n",
          deep_allocation_offset,
          known->offset,
          known->size,
          known->token.validation_chunk_offset,
          result.visited_chunks);
  return true;
}

bool VerifyDeepRangeToken(StandaloneArena &allocator,
                          size_t page_size,
                          size_t byte_budget,
                          size_t range_cap,
                          const KnownFreeRange &known) {
  bool saw_exact_range = false;
  bool saw_other_range = false;
  auto visitor = [&](void *address, size_t size) {
    if (reinterpret_cast<uintptr_t>(address) == known.address && size == known.size) {
      saw_exact_range = true;
    } else {
      saw_other_range = true;
    }
    return FreePageRangeVisitResult::kStop;
  };
  const FreePageScanResult result =
      allocator.VisitFreePageRanges(known.token,
                                    page_size,
                                    byte_budget,
                                    range_cap,
                                    Clock::now() + std::chrono::hours(1),
                                    visitor);
  const bool valid =
      result.valid && !result.reached_end && saw_exact_range && !saw_other_range &&
      result.next_cursor.target_offset == known.offset &&
      result.next_cursor.validation_chunk_offset == known.token.validation_chunk_offset;
  if (!valid) {
    fprintf(stderr,
            "ERROR: deep-range token verification failed: valid=%d exact=%d "
            "other=%d "
            "target=%zu validation=%zu\n",
            result.valid ? 1 : 0,
            saw_exact_range ? 1 : 0,
            saw_other_range ? 1 : 0,
            result.next_cursor.target_offset,
            result.next_cursor.validation_chunk_offset);
  }
  return valid;
}

void PrintIteration(const char *phase,
                    size_t iteration,
                    bool churned,
                    uint64_t generation,
                    const FreePageScanCursor &input,
                    const FreePageScanResult &result,
                    uint64_t elapsed_ns,
                    bool deep_arrival) {
  printf("%s,%zu,%d,%llu,%zu,%zu,%zu,%zu,%zu,%.3f,%zu,%d,%d,%d\n",
         phase,
         iteration,
         churned ? 1 : 0,
         static_cast<unsigned long long>(generation),
         input.target_offset,
         input.validation_chunk_offset,
         result.next_cursor.target_offset,
         result.next_cursor.validation_chunk_offset,
         result.visited_chunks,
         static_cast<double>(elapsed_ns) / 1000.0,
         result.candidate_bytes,
         deep_arrival ? 1 : 0,
         result.reached_end ? 1 : 0,
         result.valid ? 1 : 0);
}

LoopOutcome RunLoop(StandaloneArena &allocator,
                    const Config &config,
                    size_t page_size,
                    size_t byte_budget,
                    size_t range_cap,
                    ptrdiff_t churn_offset,
                    const KnownFreeRange &deep_range,
                    const char *phase,
                    bool allow_mutations,
                    size_t max_iterations,
                    FreePageScanCursor *cursor) {
  LoopOutcome outcome;
  for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
    bool churned = false;
    if (allow_mutations && config.churn_every > 0 &&
        iteration % static_cast<size_t>(config.churn_every) == 0) {
      uint64_t generation_after = 0;
      if (!ChurnOnce(allocator, config.churn_bytes, churn_offset, &generation_after)) {
        outcome.failed = true;
        return outcome;
      }
      churned = true;
      ++outcome.scheduled_mutations;
    }

    const uint64_t generation = allocator.TopologyGeneration();
    if (cursor->topology_generation != generation) {
      cursor->topology_generation = generation;
      cursor->validation_chunk_offset = 0;
      ++outcome.generation_resets;
    }
    const FreePageScanCursor input = *cursor;
    bool deep_arrival = false;
    bool unexpected_range = false;
    auto visitor = [&](void *address, size_t size) {
      if (reinterpret_cast<uintptr_t>(address) == deep_range.address &&
          size == deep_range.size) {
        deep_arrival = true;
      } else {
        unexpected_range = true;
      }
      // No pages are modified. Stop at the known range so arrival is
      // unambiguous and its exact target token is preserved.
      return FreePageRangeVisitResult::kStop;
    };

    const auto start = Clock::now();
    const FreePageScanResult result = allocator.VisitFreePageRanges(
        input,
        page_size,
        byte_budget,
        range_cap,
        start + std::chrono::milliseconds(config.quantum_ms),
        visitor);
    const auto elapsed_count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
            .count();
    const uint64_t elapsed_ns =
        elapsed_count < 0 ? 0 : static_cast<uint64_t>(elapsed_count);
    PrintIteration(
        phase, iteration, churned, generation, input, result, elapsed_ns, deep_arrival);
    ++outcome.iterations;

    const bool cursor_mismatch = result.next_cursor.topology_generation != generation ||
                                 result.next_cursor.target_offset != deep_range.offset ||
                                 (result.reached_end && !deep_arrival);
    if (!result.valid || unexpected_range || cursor_mismatch) {
      fprintf(stderr,
              "ERROR: %s iteration %zu produced valid=%d unexpected_range=%d "
              "cursor_mismatch=%d\n",
              phase,
              iteration,
              result.valid ? 1 : 0,
              unexpected_range ? 1 : 0,
              cursor_mismatch ? 1 : 0);
      outcome.failed = true;
      return outcome;
    }
    *cursor = result.next_cursor;
    if (deep_arrival) {
      outcome.arrived = true;
      return outcome;
    }
  }
  return outcome;
}

int Run(const Config &config) {
  const size_t page_size = reclaim_poc::SystemPageSize();
  size_t deep_free_bytes = 0;
  size_t byte_budget = 0;
  size_t range_cap = 0;
  if (!CheckedMultiply(config.deep_free_mb, kMiB, &deep_free_bytes) ||
      !CheckedMultiply(config.quantum_mb, kMiB, &byte_budget) ||
      !CheckedMultiply(config.range_mb, kMiB, &range_cap) ||
      deep_free_bytes < 2 * page_size || byte_budget < page_size ||
      range_cap < page_size) {
    fprintf(stderr, "ERROR: byte-size arguments overflow or are smaller than a page\n");
    return 2;
  }
  if (!reclaim_poc::IsTmpfs(config.tmpfs)) {
    fprintf(stderr, "SKIP: %s is not tmpfs\n", config.tmpfs.c_str());
    return 77;
  }

  size_t per_prefix = 0;
  size_t prefix_footprint = 0;
  size_t guard_footprint = 0;
  size_t requested_footprint = 0;
  if (!CheckedAdd(config.obj_bytes, 128, &per_prefix) ||
      !CheckedMultiply(config.live_objs, per_prefix, &prefix_footprint) ||
      !CheckedAdd(prefix_footprint, deep_free_bytes, &requested_footprint) ||
      !CheckedMultiply(config.churn_bytes, 4, &guard_footprint) ||
      !CheckedAdd(requested_footprint, guard_footprint, &requested_footprint) ||
      !CheckedAdd(requested_footprint, 64 * kMiB, &requested_footprint) ||
      requested_footprint > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    fprintf(stderr, "ERROR: requested arena footprint overflows\n");
    return 2;
  }
  const size_t footprint = std::max<size_t>(64 * kMiB, requested_footprint);
  fprintf(stderr,
          "# arena=%.1f MiB live_objs=%zu obj_bytes=%zu churn_bytes=%zu "
          "deep_free=%.1f MiB churn_every=%d quantum=%d ms\n",
          static_cast<double>(footprint) / static_cast<double>(kMiB),
          config.live_objs,
          config.obj_bytes,
          config.churn_bytes,
          static_cast<double>(deep_free_bytes) / static_cast<double>(kMiB),
          config.churn_every,
          config.quantum_ms);

  StandaloneArena allocator(config.tmpfs, footprint);
  if (!allocator.IsValid()) {
    fprintf(stderr,
            "ERROR: could not create standalone arena: %s\n",
            allocator.Status().c_str());
    return 1;
  }
  std::vector<Allocation> live;
  live.reserve(config.live_objs + 4);
  size_t actual_allocations = 0;
  const size_t expected_allocations = config.live_objs + 5;

  auto allocate_primary = [&](size_t bytes, const char *label) {
    auto allocation = allocator.Allocate(bytes);
    if (!allocation.has_value() || !IsPrimary(*allocation)) {
      fprintf(stderr,
              "ERROR: %s allocation %zu/%zu failed or was not primary\n",
              label,
              actual_allocations + 1,
              expected_allocations);
      return allocation;
    }
    ++actual_allocations;
    static_cast<volatile uint8_t *>(allocation->address)[0] =
        static_cast<uint8_t>(actual_allocations);
    return allocation;
  };

  auto left_guard = allocate_primary(config.churn_bytes, "left churn guard");
  if (!left_guard.has_value() || !IsPrimary(*left_guard)) return 1;
  const ptrdiff_t left_guard_offset = left_guard->offset;
  live.push_back(std::move(*left_guard));

  auto churn_slot = allocate_primary(config.churn_bytes, "reusable churn slot");
  if (!churn_slot.has_value() || !IsPrimary(*churn_slot)) return 1;
  const ptrdiff_t churn_offset = churn_slot->offset;

  auto right_guard = allocate_primary(config.churn_bytes, "right churn guard");
  if (!right_guard.has_value() || !IsPrimary(*right_guard)) return 1;
  const ptrdiff_t right_guard_offset = right_guard->offset;
  live.push_back(std::move(*right_guard));

  ptrdiff_t first_prefix_offset = -1;
  ptrdiff_t last_prefix_offset = -1;
  for (size_t i = 0; i < config.live_objs; ++i) {
    auto allocation = allocate_primary(config.obj_bytes, "live prefix");
    if (!allocation.has_value() || !IsPrimary(*allocation)) {
      fprintf(stderr,
              "ERROR: allocated only %zu/%zu requested live prefix objects\n",
              i,
              config.live_objs);
      return 1;
    }
    if (i == 0) first_prefix_offset = allocation->offset;
    last_prefix_offset = allocation->offset;
    live.push_back(std::move(*allocation));
  }

  auto deep = allocate_primary(deep_free_bytes, "deep free target");
  if (!deep.has_value() || !IsPrimary(*deep)) return 1;
  const uintptr_t deep_begin = reinterpret_cast<uintptr_t>(deep->address);
  const ptrdiff_t deep_offset = deep->offset;
  const size_t deep_size = deep->size;

  auto deep_guard = allocate_primary(config.churn_bytes, "deep live guard");
  if (!deep_guard.has_value() || !IsPrimary(*deep_guard)) return 1;
  const ptrdiff_t deep_guard_offset = deep_guard->offset;
  live.push_back(std::move(*deep_guard));

  if (actual_allocations != expected_allocations ||
      !(left_guard_offset < churn_offset && churn_offset < right_guard_offset &&
        right_guard_offset < first_prefix_offset &&
        first_prefix_offset <= last_prefix_offset && last_prefix_offset < deep_offset &&
        deep_offset < deep_guard_offset)) {
    fprintf(stderr,
            "ERROR: allocator layout mismatch: actual=%zu expected=%zu offsets="
            "%td,%td,%td,%td,%td,%td,%td\n",
            actual_allocations,
            expected_allocations,
            left_guard_offset,
            churn_offset,
            right_guard_offset,
            first_prefix_offset,
            last_prefix_offset,
            deep_offset,
            deep_guard_offset);
    return 1;
  }

  if (!allocator.Free(std::move(*churn_slot)) ||
      !allocator.Free(std::move(*deep))) {
    fprintf(stderr, "ERROR: failed to free setup allocations\n");
    return 1;
  }
  fprintf(stderr,
          "# setup complete: actual_allocations=%zu primary_logical=%lld "
          "topology_generation=%llu\n",
          actual_allocations,
          static_cast<long long>(allocator.PrimaryLogicalBytes()),
          static_cast<unsigned long long>(allocator.TopologyGeneration()));

  KnownFreeRange deep_range;
  if (!DiscoverDeepRange(allocator,
                         page_size,
                         byte_budget,
                         range_cap,
                         deep_begin,
                         deep_size,
                         static_cast<size_t>(deep_offset),
                         &deep_range) ||
      !VerifyDeepRangeToken(allocator, page_size, byte_budget, range_cap, deep_range)) {
    return 1;
  }

  printf(
      "phase,iteration,churned,generation,input_target,input_validation,"
      "output_target,output_validation,visited_chunks,elapsed_us,candidate_"
      "bytes,"
      "deep_arrival,reached_end,valid\n");

  // The exact target came from the scanner. Clearing only validation models the
  // controller state immediately after it observes a topology change.
  FreePageScanCursor cursor = deep_range.token;
  cursor.validation_chunk_offset = 0;
  const LoopOutcome mutation = RunLoop(allocator,
                                       config,
                                       page_size,
                                       byte_budget,
                                       range_cap,
                                       churn_offset,
                                       deep_range,
                                       "mutation",
                                       /*allow_mutations=*/true,
                                       config.max_quanta,
                                       &cursor);

  LoopOutcome recovery;
  if (!mutation.failed && !mutation.arrived) {
    recovery = RunLoop(allocator,
                       config,
                       page_size,
                       byte_budget,
                       range_cap,
                       churn_offset,
                       deep_range,
                       "recovery",
                       /*allow_mutations=*/false,
                       config.recovery_max_quanta,
                       &cursor);
  }

  printf(
      "summary,churn_every=%d,mutation_iterations=%zu,arrived_during_mutation=%"
      "d,"
      "scheduled_mutations=%zu,generation_resets=%zu,recovery_iterations=%zu,"
      "recovery_required=%d,recovered_same_cursor=%d,deep_reached=%d,"
      "deep_target=%zu\n",
      config.churn_every,
      mutation.iterations,
      mutation.arrived ? 1 : 0,
      mutation.scheduled_mutations,
      mutation.generation_resets,
      recovery.iterations,
      mutation.arrived ? 0 : 1,
      (!mutation.arrived && recovery.arrived) ? 1 : 0,
      (mutation.arrived || recovery.arrived) ? 1 : 0,
      deep_range.offset);

  for (auto &allocation : live) {
    if (!allocator.Free(std::move(allocation))) {
      fprintf(stderr, "ERROR: failed to free live allocation\n");
      return 1;
    }
  }
  if (mutation.failed || recovery.failed || (!mutation.arrived && !recovery.arrived)) {
    fprintf(stderr,
            "ERROR: benchmark failed or same cursor did not recover "
            "after churn stopped\n");
    return 1;
  }
  return 0;
}

void PrintUsage(const char *program) {
  fprintf(stderr,
          "usage: %s [--tmpfs DIR] [--fallback DIR] [--live-objs N] "
          "[--obj-bytes N] [--churn-bytes N] [--deep-free-mb N] "
          "[--churn-every N] [--quantum-ms N] [--range-mb N] "
          "[--quantum-mb N] [--max-quanta N] [--recovery-max-quanta N]\n",
          program);
  fprintf(stderr, "       --fallback is accepted for CLI compatibility and ignored\n");
}

}  // namespace

int main(int argc, char **argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto next = [&]() -> const char * {
      if (i + 1 >= argc) return nullptr;
      return argv[++i];
    };
    bool parsed = true;
    if (key == "--help") {
      PrintUsage(argv[0]);
      return 0;
    } else if (key == "--tmpfs") {
      const char *value = next();
      if (value == nullptr)
        parsed = false;
      else
        config.tmpfs = value;
    } else if (key == "--fallback") {
      const char *value = next();
      parsed = value != nullptr && value[0] != '\0';
    } else if (key == "--live-objs") {
      parsed = ParseSize(next(), &config.live_objs);
    } else if (key == "--obj-bytes") {
      parsed = ParseSize(next(), &config.obj_bytes);
    } else if (key == "--churn-bytes") {
      parsed = ParseSize(next(), &config.churn_bytes);
    } else if (key == "--deep-free-mb") {
      parsed = ParseSize(next(), &config.deep_free_mb);
    } else if (key == "--churn-every") {
      parsed = ParseNonnegativeInt(next(), &config.churn_every);
    } else if (key == "--quantum-ms") {
      parsed = ParseNonnegativeInt(next(), &config.quantum_ms);
    } else if (key == "--range-mb") {
      parsed = ParseSize(next(), &config.range_mb);
    } else if (key == "--quantum-mb") {
      parsed = ParseSize(next(), &config.quantum_mb);
    } else if (key == "--max-quanta") {
      parsed = ParseSize(next(), &config.max_quanta);
    } else if (key == "--recovery-max-quanta") {
      parsed = ParseSize(next(), &config.recovery_max_quanta);
    } else {
      fprintf(stderr, "ERROR: unknown argument: %s\n", key.c_str());
      PrintUsage(argv[0]);
      return 2;
    }
    if (!parsed) {
      fprintf(stderr, "ERROR: missing or invalid value for %s\n", key.c_str());
      PrintUsage(argv[0]);
      return 2;
    }
  }

  if (config.tmpfs.empty() || config.live_objs == 0 || config.obj_bytes == 0 ||
      config.churn_bytes == 0 || config.deep_free_mb == 0 ||
      config.quantum_ms <= 0 || config.range_mb == 0 || config.quantum_mb == 0 ||
      config.max_quanta == 0 || config.recovery_max_quanta == 0 ||
      config.live_objs > std::numeric_limits<size_t>::max() - 5) {
    fprintf(stderr, "ERROR: all sizes/counts must be positive and in range\n");
    PrintUsage(argv[0]);
    return 2;
  }
  return Run(config);
}
