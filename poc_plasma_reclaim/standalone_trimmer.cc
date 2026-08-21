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

#include "standalone_reclaim.h"

#include <cerrno>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace reclaim_poc {

std::string ValidatePhysicalPageTrimConfig(const PhysicalPageTrimConfig &config,
                                           size_t page_size) {
  if (!config.enabled) {
    return "";
  }
  if (!std::isfinite(config.start_ratio) || !std::isfinite(config.stop_ratio) ||
      config.start_ratio <= 0 || config.start_ratio >= config.stop_ratio ||
      config.stop_ratio > 1) {
    return "ratios must satisfy 0 < start_ratio < stop_ratio <= 1";
  }
  if (page_size == 0 || (page_size & (page_size - 1)) != 0) {
    return "page_size must be a nonzero power of two";
  }
  if (config.syscall_bytes < page_size ||
      config.syscall_bytes > config.quantum_bytes) {
    return "byte limits must satisfy page_size <= syscall_bytes <= quantum_bytes";
  }
  if (config.syscall_bytes % page_size != 0 ||
      config.quantum_bytes % page_size != 0) {
    return "syscall_bytes and quantum_bytes must be page aligned";
  }
  if (config.syscall_bytes > std::numeric_limits<size_t>::max() ||
      config.quantum_bytes > std::numeric_limits<size_t>::max()) {
    return "syscall_bytes and quantum_bytes must fit in size_t";
  }
  if (config.low_ratio_grace_ms < 0 || config.check_interval_ms <= 0 ||
      config.quantum_time_ms <= 0 || config.min_yield_ms < 0 ||
      config.oom_resume_cooldown_ms < 0) {
    return "timings must be nonnegative and check_interval_ms and "
           "quantum_time_ms must be positive";
  }
  return "";
}

namespace {

std::string ErrnoStatus(const char *operation, int error) {
  std::ostringstream stream;
  stream << operation << " failed with errno " << error;
  return stream.str();
}

}  // namespace

PhysicalPageTrimmer::PhysicalPageTrimmer(ArenaTrimBackend &backend,
                                         PhysicalPageTrimConfig config,
                                         size_t page_size,
                                         RemovePages remove_pages)
    : PhysicalPageTrimmer(backend,
                          std::move(config),
                          page_size,
                          std::move(remove_pages),
                          [] { return Clock::now(); }) {}

PhysicalPageTrimmer::PhysicalPageTrimmer(ArenaTrimBackend &backend,
                                         PhysicalPageTrimConfig config,
                                         size_t page_size,
                                         RemovePages remove_pages,
                                         NowProvider now_provider)
    : backend_(backend),
      config_(std::move(config)),
      page_size_(page_size),
      remove_pages_(std::move(remove_pages)),
      now_provider_(std::move(now_provider)) {
  if (!config_.enabled) {
    status_ = "disabled by configuration";
    return;
  }
  if (const std::string error =
          ValidatePhysicalPageTrimConfig(config_, page_size_);
      !error.empty()) {
    status_ = error;
    return;
  }
  if (!remove_pages_) {
    status_ = "page removal callback must be set";
    return;
  }
  if (!now_provider_) {
    status_ = "steady clock callback must be set";
    return;
  }
  state_ = PhysicalTrimState::kIdle;
  status_ = "enabled";
}

PhysicalTrimStepResult PhysicalPageTrimmer::Step(bool create_queue_pending,
                                                 Clock::time_point now) {
  PhysicalTrimStepResult result;
  result.state = state_;
  if (state_ == PhysicalTrimState::kDisabled) {
    return result;
  }
  if (HandleOomState(create_queue_pending, now, &result)) {
    return result;
  }

  ArenaUsage before;
  if (!ReadArenaUsage(&before, &result)) {
    return result;
  }
  if (UsageMeetsStop(before)) {
    EnterIdle();
    result.state = state_;
    ScheduleCheck(&result);
    return result;
  }
  if (PrepareTrim(before, now, &result)) {
    return result;
  }
  return RunTrimQuantum(before, std::move(result));
}

PhysicalTrimState PhysicalPageTrimmer::GetState() const { return state_; }

int64_t PhysicalPageTrimmer::PrimaryLogicalBytes() const {
  return backend_.PrimaryLogicalBytes();
}

std::optional<int64_t> PhysicalPageTrimmer::PrimaryPhysicalBytes() const {
  return backend_.PrimaryPhysicalBytes();
}

const std::string &PhysicalPageTrimmer::GetStatus() const { return status_; }

bool PhysicalPageTrimmer::HandleOomState(bool create_queue_pending,
                                         Clock::time_point now,
                                         PhysicalTrimStepResult *result) {
  if (create_queue_pending) {
    state_ = PhysicalTrimState::kSuspendedOom;
    low_ratio_since_ = std::nullopt;
    oom_cleared_at_ = std::nullopt;
    ResetScanCursor();
    result->state = state_;
    ScheduleCheck(result);
    return true;
  }
  if (state_ != PhysicalTrimState::kSuspendedOom) {
    return false;
  }
  if (!oom_cleared_at_.has_value() || now < *oom_cleared_at_) {
    oom_cleared_at_ = now;
    if (config_.oom_resume_cooldown_ms > 0) {
      result->state = state_;
      result->schedule_next = true;
      result->next_delay =
          std::chrono::milliseconds(config_.oom_resume_cooldown_ms);
      return true;
    }
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - *oom_cleared_at_);
  if (elapsed.count() < config_.oom_resume_cooldown_ms) {
    result->state = state_;
    result->schedule_next = true;
    result->next_delay = std::chrono::milliseconds(
        config_.oom_resume_cooldown_ms - elapsed.count());
    return true;
  }
  EnterIdle();
  result->state = state_;
  ScheduleCheck(result);
  return true;
}

bool PhysicalPageTrimmer::ReadArenaUsage(ArenaUsage *usage,
                                         PhysicalTrimStepResult *result) {
  const int64_t logical = backend_.PrimaryLogicalBytes();
  const std::optional<int64_t> physical = backend_.PrimaryPhysicalBytes();
  if (logical < 0) {
    Disable("primary logical byte accounting is negative", EINVAL, result);
    return false;
  }
  if (!physical.has_value()) {
    Disable("primary physical byte accounting is unavailable", EIO, result);
    return false;
  }
  if (*physical < 0) {
    Disable("primary physical byte accounting is negative", EINVAL, result);
    return false;
  }
  usage->logical_bytes = static_cast<uint64_t>(logical);
  usage->physical_bytes = static_cast<uint64_t>(*physical);
  return true;
}

bool PhysicalPageTrimmer::PrepareTrim(const ArenaUsage &usage,
                                      Clock::time_point now,
                                      PhysicalTrimStepResult *result) {
  if (state_ == PhysicalTrimState::kNoProgress) {
    const uint64_t free_generation = backend_.FreeGeneration();
    if (free_generation == pass_start_free_generation_) {
      result->state = state_;
      ScheduleCheck(result);
      return true;
    }
    EnterIdle();
    low_ratio_since_ = now;
    pass_start_free_generation_ = free_generation;
    result->state = state_;
    ScheduleCheck(result);
    return true;
  }
  if (state_ != PhysicalTrimState::kIdle) {
    return false;
  }
  if (RatioAtLeast(usage, config_.start_ratio)) {
    low_ratio_since_ = std::nullopt;
    result->state = state_;
    ScheduleCheck(result);
    return true;
  }
  if (!low_ratio_since_.has_value() || now < *low_ratio_since_) {
    low_ratio_since_ = now;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - *low_ratio_since_);
  if (elapsed.count() < config_.low_ratio_grace_ms) {
    result->state = state_;
    ScheduleCheck(result);
    return true;
  }
  state_ = PhysicalTrimState::kTrimming;
  ResetScanCursor();
  pass_start_free_generation_ = backend_.FreeGeneration();
  return false;
}

PhysicalTrimStepResult PhysicalPageTrimmer::RunTrimQuantum(
    const ArenaUsage &before, PhysicalTrimStepResult result) {
  const uint64_t byte_budget = ComputeByteBudget();
  if (byte_budget == 0) {
    EnterIdle();
    result.state = state_;
    ScheduleCheck(&result);
    return result;
  }
  result.quantum_ran = true;
  const Clock::time_point deadline = DeadlineFrom(now_provider_());
  int visit_error = 0;
  const auto visitor = [this, &result, &visit_error, byte_budget, deadline](
                           void *address, size_t size) {
    return RemoveRange(
        address, size, byte_budget, deadline, &visit_error, &result);
  };

  const uint64_t topology_generation = backend_.TopologyGeneration();
  if (scan_cursor_.topology_generation != topology_generation) {
    scan_cursor_.topology_generation = topology_generation;
    scan_cursor_.validation_chunk_offset = 0;
  }
  const FreePageScanCursor scan_start = scan_cursor_;
  const FreePageScanResult scan = backend_.VisitFreePageRanges(
      scan_cursor_,
      page_size_,
      static_cast<size_t>(byte_budget),
      static_cast<size_t>(config_.syscall_bytes),
      deadline,
      visitor);
  if (!scan.valid || scan.candidate_bytes != result.advised_bytes ||
      scan.candidate_bytes > byte_budget ||
      scan.next_cursor.topology_generation != topology_generation ||
      scan.next_cursor.target_offset < scan_start.target_offset ||
      scan.next_cursor.validation_chunk_offset <
          scan_start.validation_chunk_offset ||
      (scan.next_cursor.validation_chunk_offset != 0 &&
       scan.next_cursor.validation_chunk_offset >
           scan.next_cursor.target_offset) ||
      scan.candidate_bytes >
          scan.next_cursor.target_offset - scan_start.target_offset) {
    Disable("free-page scan validation failed", EINVAL, &result);
    return result;
  }
  scan_cursor_ = scan.next_cursor;
  const bool cursor_progressed =
      scan_cursor_.target_offset > scan_start.target_offset ||
      scan_cursor_.validation_chunk_offset > scan_start.validation_chunk_offset;

  ArenaUsage after;
  if (!ReadArenaUsage(&after, &result)) {
    return result;
  }
  if (before.physical_bytes > after.physical_bytes) {
    result.observed_physical_drop = before.physical_bytes - after.physical_bytes;
  }
  if (visit_error != 0 && visit_error != EAGAIN) {
    Disable(ErrnoStatus("page removal", visit_error), visit_error, &result);
    return result;
  }
  if (UsageMeetsStop(after)) {
    EnterIdle();
    result.state = state_;
    result.error = visit_error;
    ScheduleCheck(&result);
    return result;
  }
  if (visit_error == EAGAIN) {
    state_ = PhysicalTrimState::kTrimming;
    result.state = state_;
    result.error = visit_error;
    ScheduleCheck(&result);
    return result;
  }
  if (!scan.reached_end && scan.candidate_bytes == 0 && !cursor_progressed) {
    state_ = PhysicalTrimState::kTrimming;
    result.state = state_;
    ScheduleCheck(&result);
    return result;
  }
  if (scan.reached_end) {
    const uint64_t free_generation = backend_.FreeGeneration();
    if (free_generation != pass_start_free_generation_) {
      ResetScanCursor();
      pass_start_free_generation_ = free_generation;
      state_ = PhysicalTrimState::kTrimming;
      result.state = state_;
      ScheduleTrim(&result);
      return result;
    }
    state_ = PhysicalTrimState::kNoProgress;
    result.state = state_;
    ScheduleCheck(&result);
    return result;
  }
  state_ = PhysicalTrimState::kTrimming;
  result.state = state_;
  ScheduleTrim(&result);
  return result;
}

FreePageRangeVisitResult PhysicalPageTrimmer::RemoveRange(
    void *address,
    size_t size,
    uint64_t byte_budget,
    Clock::time_point quantum_deadline,
    int *visit_error,
    PhysicalTrimStepResult *result) {
  const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
  if (address == nullptr || begin % page_size_ != 0 || size == 0 ||
      size % page_size_ != 0 || size > config_.syscall_bytes ||
      result->advised_bytes > byte_budget ||
      size > byte_budget - result->advised_bytes) {
    *visit_error = EINVAL;
    return FreePageRangeVisitResult::kStop;
  }
  int error = 0;
  do {
    const Clock::time_point started = now_provider_();
    ++result->syscall_count;
    error = remove_pages_(address, size);
    const Clock::time_point finished = now_provider_();
    if (finished >= started) {
      result->syscall_duration_ms +=
          std::chrono::duration<double, std::milli>(finished - started).count();
    }
    if (error == EINTR && finished >= quantum_deadline) {
      error = EAGAIN;
    }
  } while (error == EINTR);
  if (error != 0) {
    *visit_error = error;
    return FreePageRangeVisitResult::kStop;
  }
  result->advised_bytes += size;
  return FreePageRangeVisitResult::kContinue;
}

void PhysicalPageTrimmer::Disable(const std::string &reason,
                                  int error,
                                  PhysicalTrimStepResult *result) {
  state_ = PhysicalTrimState::kDisabled;
  status_ = reason;
  low_ratio_since_ = std::nullopt;
  oom_cleared_at_ = std::nullopt;
  ResetScanCursor();
  result->state = state_;
  result->schedule_next = false;
  result->next_delay = std::chrono::milliseconds(0);
  result->error = error;
}

void PhysicalPageTrimmer::EnterIdle() {
  state_ = PhysicalTrimState::kIdle;
  low_ratio_since_ = std::nullopt;
  ResetScanCursor();
}

void PhysicalPageTrimmer::ResetScanCursor() { scan_cursor_ = FreePageScanCursor{}; }

void PhysicalPageTrimmer::ScheduleCheck(PhysicalTrimStepResult *result) const {
  result->schedule_next = true;
  result->next_delay = std::chrono::milliseconds(config_.check_interval_ms);
}

void PhysicalPageTrimmer::ScheduleTrim(PhysicalTrimStepResult *result) const {
  result->schedule_next = true;
  result->next_delay = std::chrono::milliseconds(config_.min_yield_ms);
}

bool PhysicalPageTrimmer::RatioAtLeast(const ArenaUsage &usage,
                                       double threshold) const {
  if (usage.physical_bytes == 0) {
    return true;
  }
  return static_cast<long double>(usage.logical_bytes) /
             static_cast<long double>(usage.physical_bytes) >=
         static_cast<long double>(threshold);
}

bool PhysicalPageTrimmer::UsageMeetsStop(const ArenaUsage &usage) const {
  return usage.physical_bytes == 0 || usage.logical_bytes > usage.physical_bytes ||
         RatioAtLeast(usage, config_.stop_ratio);
}

uint64_t PhysicalPageTrimmer::ComputeByteBudget() const {
  // MADV_REMOVE on an already sparse tmpfs range can succeed without reducing
  // physical backing. A budget derived only from the P reduction needed to hit
  // stop_ratio can therefore make negligible virtual-address progress. Step
  // calls this only while the fresh ratio is below stop_ratio, so use the full
  // configured scan/advice quantum and re-read L/P after the quantum.
  return config_.quantum_bytes;
}

Clock::time_point PhysicalPageTrimmer::DeadlineFrom(Clock::time_point now) const {
  const auto duration = std::chrono::milliseconds(config_.quantum_time_ms);
  if (Clock::time_point::max() - now < duration) {
    return Clock::time_point::max();
  }
  return now + duration;
}

}  // namespace reclaim_poc
