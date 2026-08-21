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

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/vfs.h>
#endif

#define MSPACES 1
#define ONLY_MSPACES 1
#define USE_LOCKS 0
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#include "third_party/dlmalloc.c"  // NOLINT(build/include)

namespace reclaim_poc {
namespace {

constexpr size_t kAllocationAlignment = 64;

std::string ErrnoStatus(const char *operation, int error) {
  std::ostringstream stream;
  stream << operation << " failed: " << std::strerror(error) << " (errno "
         << error << ")";
  return stream.str();
}

FreePageScanResult InvalidScanResult(const FreePageScanCursor &cursor) {
  FreePageScanResult result;
  result.next_cursor = cursor;
  return result;
}

bool CheckedAddressEnd(uintptr_t begin, size_t size, uintptr_t *end) {
  if (size > std::numeric_limits<uintptr_t>::max() - begin) {
    return false;
  }
  *end = begin + size;
  return true;
}

bool AlignAddressUp(uintptr_t address, size_t alignment, uintptr_t *aligned) {
  const size_t remainder = address % alignment;
  if (remainder == 0) {
    *aligned = address;
    return true;
  }
  return CheckedAddressEnd(address, alignment - remainder, aligned);
}

uintptr_t AlignAddressDown(uintptr_t address, size_t alignment) {
  return address - address % alignment;
}

struct FreePageScanContext {
  const FreePageScanCursor &original_cursor;
  uintptr_t mmap_begin;
  uintptr_t mmap_end;
  size_t page_size;
  size_t byte_budget;
  size_t capped_range_size;
  Clock::time_point deadline;
  const FreePageRangeVisitor &visitor;
};

struct ArenaView {
  msegmentptr segment = nullptr;
  uintptr_t segment_begin = 0;
  uintptr_t segment_end = 0;
  mchunkptr first_chunk = nullptr;
  uintptr_t first_chunk_address = 0;
  size_t first_chunk_offset = 0;
};

struct ValidatedChunk {
  mchunkptr chunk = nullptr;
  mchunkptr next = nullptr;
  uintptr_t begin = 0;
  uintptr_t end = 0;
  size_t size = 0;
  size_t offset = 0;
  size_t next_offset = 0;
  bool is_top = false;
};

enum class ChunkValidationResult { kValid, kFencepost, kInvalid };
enum class FreeChunkVisitResult { kAdvance, kStop, kInvalid };

bool BuildValidatedArenaView(mstate state,
                             void *mapping,
                             size_t mapping_size,
                             const FreePageScanContext &context,
                             ArenaView *arena) {
  if (state == nullptr || !is_initialized(state) ||
      state->seg.base != static_cast<char *>(mapping) ||
      state->seg.size != mapping_size || state->seg.next != nullptr ||
      !is_extern_segment(&state->seg)) {
    return false;
  }

  arena->segment = &state->seg;
  arena->segment_begin = reinterpret_cast<uintptr_t>(state->seg.base);
  if (arena->segment_begin != context.mmap_begin ||
      !CheckedAddressEnd(arena->segment_begin, state->seg.size, &arena->segment_end) ||
      arena->segment_end != context.mmap_end) {
    return false;
  }

  const uintptr_t top_address = reinterpret_cast<uintptr_t>(state->top);
  if (top_address < arena->segment_begin || top_address >= arena->segment_end) {
    return false;
  }
  arena->first_chunk = align_as_chunk(state->seg.base);
  arena->first_chunk_address = reinterpret_cast<uintptr_t>(arena->first_chunk);
  if (arena->first_chunk_address < arena->segment_begin ||
      arena->first_chunk_address >= arena->segment_end) {
    return false;
  }
  arena->first_chunk_offset =
      static_cast<size_t>(arena->first_chunk_address - context.mmap_begin);
  return true;
}

bool InitializeScanState(const FreePageScanContext &context,
                         const ArenaView &arena,
                         FreePageScanResult *result,
                         mchunkptr *chunk) {
  result->valid = true;
  result->next_cursor = context.original_cursor;
  if (result->next_cursor.target_offset < arena.first_chunk_offset) {
    result->next_cursor.target_offset = arena.first_chunk_offset;
  }

  *chunk = arena.first_chunk;
  if (context.original_cursor.validation_chunk_offset == 0) {
    return true;
  }
  if (context.original_cursor.validation_chunk_offset < arena.first_chunk_offset ||
      context.original_cursor.validation_chunk_offset >
          result->next_cursor.target_offset) {
    return false;
  }
  uintptr_t validation_address = 0;
  if (!CheckedAddressEnd(context.mmap_begin,
                         context.original_cursor.validation_chunk_offset,
                         &validation_address) ||
      validation_address >= arena.segment_end) {
    return false;
  }
  *chunk = reinterpret_cast<mchunkptr>(validation_address);
  return true;
}

ChunkValidationResult ValidateChunk(mstate state,
                                    const FreePageScanContext &context,
                                    const ArenaView &arena,
                                    mchunkptr chunk,
                                    FreePageScanResult *result,
                                    ValidatedChunk *validated) {
  validated->chunk = chunk;
  validated->begin = reinterpret_cast<uintptr_t>(chunk);
  uintptr_t chunk_head_end = 0;
  if (validated->begin < arena.first_chunk_address ||
      !CheckedAddressEnd(validated->begin,
                         offsetof(struct malloc_chunk, head) + sizeof(chunk->head),
                         &chunk_head_end) ||
      chunk_head_end > arena.segment_end || !is_aligned(chunk2mem(chunk))) {
    return ChunkValidationResult::kInvalid;
  }
  if (chunk->head == FENCEPOST_HEAD) {
    return ChunkValidationResult::kFencepost;
  }

  ++result->visited_chunks;
  validated->size = chunksize(chunk);
  if (validated->begin < arena.segment_begin ||
      validated->size < MIN_CHUNK_SIZE ||
      (validated->size & CHUNK_ALIGN_MASK) != 0 ||
      !CheckedAddressEnd(validated->begin, validated->size, &validated->end) ||
      validated->end <= validated->begin || validated->end > arena.segment_end) {
    return ChunkValidationResult::kInvalid;
  }
  validated->is_top = chunk == state->top;
  if (validated->is_top &&
      (validated->size != state->topsize || is_inuse(validated->chunk))) {
    return ChunkValidationResult::kInvalid;
  }
  validated->next = reinterpret_cast<mchunkptr>(validated->end);
  validated->offset = static_cast<size_t>(validated->begin - context.mmap_begin);
  validated->next_offset = static_cast<size_t>(validated->end - context.mmap_begin);
  result->next_cursor.validation_chunk_offset = validated->offset;
  return ChunkValidationResult::kValid;
}

bool AdvancePastChunk(const ValidatedChunk &chunk,
                      FreePageScanResult *result,
                      mchunkptr *next_chunk) {
  result->next_cursor.target_offset = chunk.next_offset;
  result->next_cursor.validation_chunk_offset = chunk.next_offset;
  if (chunk.is_top) {
    result->reached_end = true;
    return false;
  }
  *next_chunk = chunk.next;
  return true;
}

FreeChunkVisitResult VisitFreeChunkPages(const FreePageScanContext &context,
                                         const ValidatedChunk &chunk,
                                         uintptr_t requested_cursor,
                                         FreePageScanResult *result) {
  const size_t bookkeeping_size =
      is_small(chunk.size) ? sizeof(struct malloc_chunk)
                           : sizeof(struct malloc_tree_chunk);
  uintptr_t safe_begin = 0;
  if (bookkeeping_size > chunk.size ||
      !CheckedAddressEnd(chunk.begin, bookkeeping_size, &safe_begin)) {
    return FreeChunkVisitResult::kInvalid;
  }
  uintptr_t page_begin = 0;
  if (!AlignAddressUp(safe_begin, context.page_size, &page_begin)) {
    return FreeChunkVisitResult::kInvalid;
  }
  const uintptr_t page_end = AlignAddressDown(chunk.end, context.page_size);
  if (page_begin >= page_end) {
    return FreeChunkVisitResult::kAdvance;
  }

  uintptr_t range_begin = std::max(page_begin, requested_cursor);
  if (!AlignAddressUp(range_begin, context.page_size, &range_begin)) {
    return FreeChunkVisitResult::kInvalid;
  }
  while (range_begin < page_end) {
    result->next_cursor.target_offset =
        static_cast<size_t>(range_begin - context.mmap_begin);
    result->next_cursor.validation_chunk_offset = chunk.offset;
    if (Clock::now() >= context.deadline) {
      return FreeChunkVisitResult::kStop;
    }

    const size_t budget_remaining = context.byte_budget - result->candidate_bytes;
    size_t range_size = std::min({static_cast<size_t>(page_end - range_begin),
                                  budget_remaining,
                                  context.capped_range_size});
    range_size -= range_size % context.page_size;
    if (range_size == 0) {
      return FreeChunkVisitResult::kStop;
    }
    if (context.visitor(reinterpret_cast<void *>(range_begin), range_size) ==
        FreePageRangeVisitResult::kStop) {
      return FreeChunkVisitResult::kStop;
    }
    result->candidate_bytes += range_size;
    range_begin += range_size;
    result->next_cursor.target_offset =
        static_cast<size_t>(range_begin - context.mmap_begin);
    if (result->candidate_bytes == context.byte_budget ||
        context.byte_budget - result->candidate_bytes < context.page_size) {
      return FreeChunkVisitResult::kStop;
    }
  }
  return FreeChunkVisitResult::kAdvance;
}

FreePageScanResult ScanFreePageRanges(mstate state,
                                      const FreePageScanContext &context,
                                      const ArenaView &arena,
                                      mchunkptr chunk,
                                      FreePageScanResult result) {
  while (segment_holds(arena.segment, chunk)) {
    if (Clock::now() >= context.deadline) {
      return result;
    }

    ValidatedChunk validated;
    const ChunkValidationResult validation =
        ValidateChunk(state, context, arena, chunk, &result, &validated);
    if (validation != ChunkValidationResult::kValid) {
      break;
    }

    uintptr_t requested_cursor = 0;
    if (!CheckedAddressEnd(context.mmap_begin,
                           result.next_cursor.target_offset,
                           &requested_cursor)) {
      return InvalidScanResult(context.original_cursor);
    }
    if (validated.end <= requested_cursor) {
      result.next_cursor.validation_chunk_offset = validated.next_offset;
      if (validated.is_top) {
        result.reached_end = true;
        return result;
      }
      chunk = validated.next;
      continue;
    }

    if (Clock::now() >= context.deadline) {
      return result;
    }
    if (is_inuse(validated.chunk)) {
      if (!AdvancePastChunk(validated, &result, &chunk)) {
        return result;
      }
      continue;
    }

    const FreeChunkVisitResult visit =
        VisitFreeChunkPages(context, validated, requested_cursor, &result);
    if (visit == FreeChunkVisitResult::kInvalid) {
      return InvalidScanResult(context.original_cursor);
    }
    if (visit == FreeChunkVisitResult::kStop ||
        !AdvancePastChunk(validated, &result, &chunk)) {
      return result;
    }
  }
  return InvalidScanResult(context.original_cursor);
}

FreePageScanResult VisitFreePageRanges(mstate state,
                                       void *mapping,
                                       size_t mapping_size,
                                       const FreePageScanCursor &cursor,
                                       size_t page_size,
                                       size_t byte_budget,
                                       size_t range_cap,
                                       Clock::time_point deadline,
                                       const FreePageRangeVisitor &visitor) {
  if (state == nullptr || mapping == nullptr || mapping_size == 0 ||
      page_size == 0 || (page_size & (page_size - 1)) != 0 ||
      byte_budget < page_size || range_cap < page_size || !visitor ||
      cursor.target_offset > mapping_size ||
      cursor.validation_chunk_offset > mapping_size) {
    return InvalidScanResult(cursor);
  }

  const size_t capped_range_size = range_cap - range_cap % page_size;
  const uintptr_t mmap_begin = reinterpret_cast<uintptr_t>(mapping);
  uintptr_t mmap_end = 0;
  if (capped_range_size == 0 ||
      !CheckedAddressEnd(mmap_begin, mapping_size, &mmap_end)) {
    return InvalidScanResult(cursor);
  }
  if (cursor.target_offset == mapping_size) {
    FreePageScanResult result;
    result.valid = true;
    result.reached_end = true;
    result.next_cursor = cursor;
    result.next_cursor.validation_chunk_offset = mapping_size;
    return result;
  }

  const FreePageScanContext context{cursor,
                                    mmap_begin,
                                    mmap_end,
                                    page_size,
                                    byte_budget,
                                    capped_range_size,
                                    deadline,
                                    visitor};
  ArenaView arena;
  if (!BuildValidatedArenaView(state, mapping, mapping_size, context, &arena)) {
    return InvalidScanResult(cursor);
  }
  FreePageScanResult result;
  mchunkptr chunk = nullptr;
  if (!InitializeScanState(context, arena, &result, &chunk)) {
    return InvalidScanResult(cursor);
  }
  return ScanFreePageRanges(state, context, arena, chunk, result);
}

}  // namespace

struct StandaloneArena::Impl {
  int fd = -1;
  void *mapping = nullptr;
  size_t capacity = 0;
  mspace space = nullptr;
  int64_t logical_bytes = 0;
  uint64_t topology_generation = 0;
  uint64_t free_generation = 0;
  std::string status;
};

Allocation::Allocation(Allocation &&other) noexcept
    : address(other.address), size(other.size), offset(other.offset) {
  other.address = nullptr;
  other.size = 0;
  other.offset = -1;
}

Allocation &Allocation::operator=(Allocation &&other) noexcept {
  if (this != &other) {
    if (address != nullptr) {
      std::abort();
    }
    address = other.address;
    size = other.size;
    offset = other.offset;
    other.address = nullptr;
    other.size = 0;
    other.offset = -1;
  }
  return *this;
}

StandaloneArena::StandaloneArena(const std::string &directory, size_t capacity)
    : impl_(new (std::nothrow) Impl) {
  if (!impl_) {
    return;
  }
  if (capacity == 0 ||
      capacity > static_cast<size_t>(std::numeric_limits<off_t>::max()) ||
      capacity > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
    impl_->status = "arena capacity is invalid";
    return;
  }

  std::string path = directory;
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path += "plasma-reclaim-XXXXXX";
  std::vector<char> path_buffer(path.begin(), path.end());
  path_buffer.push_back('\0');
  impl_->fd = ::mkstemp(path_buffer.data());
  if (impl_->fd < 0) {
    impl_->status = ErrnoStatus("mkstemp", errno);
    return;
  }
  if (::unlink(path_buffer.data()) != 0) {
    impl_->status = ErrnoStatus("unlink", errno);
    return;
  }
  const int flags = ::fcntl(impl_->fd, F_GETFD);
  if (flags < 0 || ::fcntl(impl_->fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
    impl_->status = ErrnoStatus("fcntl(FD_CLOEXEC)", errno);
    return;
  }
  if (::ftruncate(impl_->fd, static_cast<off_t>(capacity)) != 0) {
    impl_->status = ErrnoStatus("ftruncate", errno);
    return;
  }
  impl_->mapping = ::mmap(nullptr,
                          capacity,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          impl_->fd,
                          0);
  if (impl_->mapping == MAP_FAILED) {
    impl_->mapping = nullptr;
    impl_->status = ErrnoStatus("mmap", errno);
    return;
  }
  impl_->capacity = capacity;
  impl_->space = create_mspace_with_base(impl_->mapping, capacity, 0);
  if (impl_->space == nullptr) {
    impl_->status = "create_mspace_with_base failed";
    return;
  }
  impl_->status = "ok";
}

StandaloneArena::~StandaloneArena() {
  if (!impl_) {
    return;
  }
  if (impl_->space != nullptr) {
    static_cast<void>(destroy_mspace(impl_->space));
    impl_->space = nullptr;
  }
  if (impl_->mapping != nullptr) {
    static_cast<void>(::munmap(impl_->mapping, impl_->capacity));
    impl_->mapping = nullptr;
  }
  if (impl_->fd >= 0) {
    static_cast<void>(::close(impl_->fd));
    impl_->fd = -1;
  }
}

bool StandaloneArena::IsValid() const {
  return impl_ != nullptr && impl_->space != nullptr && impl_->mapping != nullptr &&
         impl_->fd >= 0;
}

const std::string &StandaloneArena::Status() const {
  static const std::string kAllocationFailure = "allocator state allocation failed";
  return impl_ ? impl_->status : kAllocationFailure;
}

size_t StandaloneArena::Capacity() const { return impl_ ? impl_->capacity : 0; }

std::optional<Allocation> StandaloneArena::Allocate(size_t size) {
  if (!IsValid() || size == 0 ||
      size > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
      impl_->logical_bytes >
          std::numeric_limits<int64_t>::max() - static_cast<int64_t>(size)) {
    return std::nullopt;
  }
  void *address = mspace_memalign(impl_->space, kAllocationAlignment, size);
  if (address == nullptr) {
    return std::nullopt;
  }
  const uintptr_t mapping_begin = reinterpret_cast<uintptr_t>(impl_->mapping);
  const uintptr_t allocation_begin = reinterpret_cast<uintptr_t>(address);
  uintptr_t mapping_end = 0;
  uintptr_t allocation_end = 0;
  if (!CheckedAddressEnd(mapping_begin, impl_->capacity, &mapping_end) ||
      allocation_begin < mapping_begin || allocation_begin >= mapping_end ||
      !CheckedAddressEnd(allocation_begin, size, &allocation_end) ||
      allocation_end > mapping_end) {
    mspace_free(impl_->space, address);
    ++impl_->topology_generation;
    ++impl_->free_generation;
    return std::nullopt;
  }

  impl_->logical_bytes += static_cast<int64_t>(size);
  ++impl_->topology_generation;
  return Allocation(address,
                    size,
                    static_cast<ptrdiff_t>(allocation_begin - mapping_begin));
}

bool StandaloneArena::Free(Allocation &&allocation) {
  if (!IsValid() || allocation.address == nullptr || allocation.offset < 0 ||
      allocation.size > static_cast<size_t>(impl_->logical_bytes)) {
    return false;
  }
  const uintptr_t mapping_begin = reinterpret_cast<uintptr_t>(impl_->mapping);
  uintptr_t mapping_end = 0;
  uintptr_t allocation_end = 0;
  const uintptr_t allocation_begin = reinterpret_cast<uintptr_t>(allocation.address);
  if (!CheckedAddressEnd(mapping_begin, impl_->capacity, &mapping_end) ||
      allocation_begin < mapping_begin || allocation_begin >= mapping_end ||
      !CheckedAddressEnd(allocation_begin, allocation.size, &allocation_end) ||
      allocation_end > mapping_end ||
      allocation_begin - mapping_begin != static_cast<size_t>(allocation.offset)) {
    return false;
  }

  mspace_free(impl_->space, allocation.address);
  impl_->logical_bytes -= static_cast<int64_t>(allocation.size);
  ++impl_->topology_generation;
  ++impl_->free_generation;
  allocation.address = nullptr;
  allocation.size = 0;
  allocation.offset = -1;
  return true;
}

int64_t StandaloneArena::PrimaryLogicalBytes() const {
  return impl_ ? impl_->logical_bytes : 0;
}

std::optional<int64_t> StandaloneArena::PrimaryPhysicalBytes() const {
  if (!IsValid()) {
    return std::nullopt;
  }
  struct stat stats = {};
  if (::fstat(impl_->fd, &stats) != 0 || stats.st_blocks < 0 ||
      static_cast<uint64_t>(stats.st_blocks) >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 512) {
    return std::nullopt;
  }
  return static_cast<int64_t>(stats.st_blocks) * 512;
}

uint64_t StandaloneArena::TopologyGeneration() const {
  return impl_ ? impl_->topology_generation : 0;
}

uint64_t StandaloneArena::FreeGeneration() const {
  return impl_ ? impl_->free_generation : 0;
}

FreePageScanResult StandaloneArena::VisitFreePageRanges(
    const FreePageScanCursor &cursor,
    size_t page_size,
    size_t byte_budget,
    size_t range_cap,
    Clock::time_point deadline,
    const FreePageRangeVisitor &visitor) {
  if (!IsValid()) {
    return InvalidScanResult(cursor);
  }
  FreePageScanCursor effective_cursor = cursor;
  if (effective_cursor.topology_generation != impl_->topology_generation) {
    effective_cursor.topology_generation = impl_->topology_generation;
    effective_cursor.validation_chunk_offset = 0;
  }
  return reclaim_poc::VisitFreePageRanges(static_cast<mstate>(impl_->space),
                                          impl_->mapping,
                                          impl_->capacity,
                                          effective_cursor,
                                          page_size,
                                          byte_budget,
                                          range_cap,
                                          deadline,
                                          visitor);
}

size_t SystemPageSize() {
  const long value = ::sysconf(_SC_PAGESIZE);
  return value > 0 ? static_cast<size_t>(value) : 0;
}

bool IsTmpfs(const std::string &path) {
#if defined(__linux__)
  struct statfs stats = {};
  return ::statfs(path.c_str(), &stats) == 0 &&
         static_cast<unsigned long>(stats.f_type) == 0x01021994UL;
#else
  static_cast<void>(path);
  return false;
#endif
}

int RemovePagesWithMadvise(void *address, size_t size) {
#if defined(MADV_REMOVE)
  if (::madvise(address, size, MADV_REMOVE) == 0) {
    return 0;
  }
  return errno;
#else
  static_cast<void>(address);
  static_cast<void>(size);
  return ENOTSUP;
#endif
}

}  // namespace reclaim_poc
