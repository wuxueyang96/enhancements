// PoC-A: Linux tmpfs MADV_REMOVE latency by mapping topology.
//
// M = client processes with one MAP_SHARED VMA for the arena.
// P = clients that read-fault the selected range before it is removed (P <= M).
// A = those P clients blocked in futex_wait or pinned and actively spinning.
// R = the MADV_REMOVE range size.
//
// The parent models the raylet. It owns one additional mapping, materializes
// the backing first, waits for selected clients to read-fault it, and times
// exactly one MADV_REMOVE call. The syscall is only a lower bound on Store-lock
// delay; this program does not model the Plasma event loop or allocator scan.

#define _GNU_SOURCE

#ifndef __linux__
#error "PoC-A requires Linux MADV_REMOVE, futex, and procfs"
#endif

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_REMOVE
#define MADV_REMOVE 9
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

struct control {
  int epoch;
  int completed_epoch;
  int mapped_ready;
  int touched_ready;
  int touch_errors;
  uint64_t offset;
  uint64_t length;
};

struct attempt_result {
  int rc;
  int error_number;
  int epoch;
  uint64_t latency_ns;
  long long backing_before;
  long long backing_after;
  long long backing_drop;
  size_t offset;
  int backing_ok;
  int zero_checked;
  int zero_ok;
};

static long g_page_size;
static volatile sig_atomic_t g_interrupted;

static void die(const char *fmt, ...) {
  int saved_errno = errno;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  if (saved_errno != 0)
    fprintf(stderr, ": %s", strerror(saved_errno));
  fputc('\n', stderr);
  exit(1);
}

static void on_signal(int signo) {
  (void)signo;
  g_interrupted = 1;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
    die("clock_gettime");
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int futex_wait_value(int *address, int expected, long timeout_ns) {
  struct timespec timeout;
  struct timespec *timeout_pointer = NULL;
  if (timeout_ns >= 0) {
    timeout.tv_sec = timeout_ns / 1000000000L;
    timeout.tv_nsec = timeout_ns % 1000000000L;
    timeout_pointer = &timeout;
  }
  return (int)syscall(SYS_futex, address, FUTEX_WAIT, expected, timeout_pointer,
                      NULL, 0);
}

static void futex_wake_all(int *address) {
  (void)syscall(SYS_futex, address, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static int pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof(set), &set);
}

static int collect_allowed_cpus(int **cpu_ids) {
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set) != 0)
    return -1;
  int count = CPU_COUNT(&set);
  int *ids = calloc((size_t)(count > 0 ? count : 1), sizeof(*ids));
  if (ids == NULL)
    return -1;
  int found = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set))
      ids[found++] = cpu;
  }
  *cpu_ids = ids;
  return found;
}

static void materialize_range(volatile unsigned char *base, size_t offset,
                              size_t length, unsigned int epoch) {
  for (size_t relative = 0; relative < length;
       relative += (size_t)g_page_size) {
    size_t page_index = (offset + relative) / (size_t)g_page_size;
    base[offset + relative] = (unsigned char)((page_index + epoch) % 255u + 1u);
  }
}

static int read_and_check_range(volatile const unsigned char *base,
                                size_t offset, size_t length) {
  int zeros = 0;
  unsigned int checksum = 0;
  for (size_t relative = 0; relative < length;
       relative += (size_t)g_page_size) {
    unsigned char value = base[offset + relative];
    checksum += value;
    if (value == 0)
      ++zeros;
  }
  if (checksum == UINT_MAX)
    __asm__ __volatile__("" ::: "memory");
  return zeros;
}

static int verify_zero_range(volatile const unsigned char *base, size_t offset,
                             size_t length) {
  for (size_t relative = 0; relative < length;
       relative += (size_t)g_page_size) {
    if (base[offset + relative] != 0)
      return 0;
  }
  return 1;
}

static void child_main(int fd, size_t region_size, struct control *control,
                       int is_toucher, int active, int cpu, pid_t parent_pid) {
  (void)signal(SIGINT, SIG_DFL);
  (void)signal(SIGTERM, SIG_DFL);
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
    _exit(120);
  if (getppid() != parent_pid)
    _exit(121);
  if (active && is_toucher && pin_to_cpu(cpu) != 0)
    _exit(122);

  void *mapped =
      mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED)
    _exit(123);
  close(fd);
  volatile unsigned char *base = mapped;

  __atomic_fetch_add(&control->mapped_ready, 1, __ATOMIC_RELEASE);
  futex_wake_all(&control->mapped_ready);

  int last_epoch = 0;
  for (;;) {
    int epoch = __atomic_load_n(&control->epoch, __ATOMIC_ACQUIRE);
    if (epoch < 0)
      break;
    if (epoch == last_epoch) {
      (void)futex_wait_value(&control->epoch, epoch, -1);
      continue;
    }
    last_epoch = epoch;

    if (!is_toucher)
      continue;
    size_t offset = (size_t)__atomic_load_n(&control->offset, __ATOMIC_ACQUIRE);
    size_t length = (size_t)__atomic_load_n(&control->length, __ATOMIC_ACQUIRE);
    int zero_pages = read_and_check_range(base, offset, length);
    if (zero_pages != 0) {
      __atomic_fetch_add(&control->touch_errors, zero_pages, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&control->touched_ready, 1, __ATOMIC_RELEASE);
    futex_wake_all(&control->touched_ready);

    if (active) {
      while (__atomic_load_n(&control->completed_epoch, __ATOMIC_ACQUIRE) <
                 epoch &&
             __atomic_load_n(&control->epoch, __ATOMIC_RELAXED) >= 0) {
        __asm__ __volatile__("" ::: "memory");
      }
    } else {
      for (;;) {
        int completed =
            __atomic_load_n(&control->completed_epoch, __ATOMIC_ACQUIRE);
        if (completed >= epoch ||
            __atomic_load_n(&control->epoch, __ATOMIC_RELAXED) < 0) {
          break;
        }
        (void)futex_wait_value(&control->completed_epoch, completed, -1);
      }
    }
  }

  munmap(mapped, region_size);
  _exit(0);
}

static int check_children_alive(pid_t *children, int child_count) {
  for (int i = 0; i < child_count; ++i) {
    if (children[i] <= 0)
      continue;
    int status = 0;
    pid_t result = waitpid(children[i], &status, WNOHANG);
    if (result == children[i]) {
      fprintf(
          stderr,
          "ERROR: child pid=%ld exited during an experiment (status=0x%x)\n",
          (long)children[i], status);
      children[i] = 0;
      return -1;
    }
    if (result < 0 && errno != EINTR) {
      fprintf(stderr, "ERROR: waitpid(%ld): %s\n", (long)children[i],
              strerror(errno));
      return -1;
    }
  }
  return 0;
}

static int wait_for_counter(int *counter, int target, int timeout_ms,
                            pid_t *children, int child_count,
                            const char *description) {
  if (target == 0)
    return 0;
  uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
  while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target) {
    if (g_interrupted || check_children_alive(children, child_count) != 0)
      return -1;
    uint64_t current_time = now_ns();
    if (current_time >= deadline) {
      fprintf(stderr, "ERROR: timeout waiting for %s (%d/%d ready)\n",
              description, __atomic_load_n(counter, __ATOMIC_RELAXED), target);
      return -1;
    }
    uint64_t remaining = deadline - current_time;
    long sleep_ns = (long)(remaining < 10000000ull ? remaining : 10000000ull);
    int observed = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
    (void)futex_wait_value(counter, observed, sleep_ns);
  }
  return 0;
}

static void complete_epoch(struct control *control, int epoch) {
  __atomic_store_n(&control->completed_epoch, epoch, __ATOMIC_RELEASE);
  futex_wake_all(&control->completed_epoch);
}

static void stop_children(struct control *control, pid_t *children,
                          int child_count, int force) {
  __atomic_store_n(&control->epoch, -1, __ATOMIC_RELEASE);
  __atomic_store_n(&control->completed_epoch, INT_MAX, __ATOMIC_RELEASE);
  futex_wake_all(&control->epoch);
  futex_wake_all(&control->completed_epoch);
  if (force) {
    for (int i = 0; i < child_count; ++i) {
      if (children[i] > 0)
        (void)kill(children[i], SIGTERM);
    }
  }
  for (int i = 0; i < child_count; ++i) {
    if (children[i] <= 0)
      continue;
    int status = 0;
    while (waitpid(children[i], &status, 0) < 0 && errno == EINTR) {
    }
    children[i] = 0;
  }
}

static long long backing_bytes(int fd) {
  struct stat status;
  if (fstat(fd, &status) != 0)
    return -1;
  return (long long)status.st_blocks * 512ll;
}

static int compare_u64(const void *left, const void *right) {
  uint64_t a = *(const uint64_t *)left;
  uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *sorted, size_t count,
                           unsigned int p) {
  if (count == 0)
    return 0;
  size_t rank = ((size_t)p * count + 99u) / 100u;
  if (rank == 0)
    rank = 1;
  if (rank > count)
    rank = count;
  return sorted[rank - 1];
}

static size_t greatest_common_divisor(size_t a, size_t b) {
  while (b != 0) {
    size_t next = a % b;
    a = b;
    b = next;
  }
  return a;
}

static size_t slot_for_attempt(size_t attempt, size_t slot_count, size_t salt) {
  if (slot_count <= 1)
    return 0;
  size_t step = slot_count / 2 + 1;
  while (greatest_common_divisor(step, slot_count) != 1)
    ++step;
  return (((attempt % slot_count) * step) + (salt % slot_count)) % slot_count;
}

static void print_first_line(const char *label, const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL)
    return;
  char line[1024];
  if (fgets(line, sizeof(line), file) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    fprintf(stderr, "# %s=%s\n", label, line);
  }
  fclose(file);
}

static void print_matching_line(const char *label, const char *path,
                                const char *prefix) {
  FILE *file = fopen(path, "r");
  if (file == NULL)
    return;
  char line[1024];
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, prefix, strlen(prefix)) != 0)
      continue;
    line[strcspn(line, "\n")] = '\0';
    fprintf(stderr, "# %s=%s\n", label, line);
    break;
  }
  fclose(file);
}

static void print_mountinfo(const char *directory) {
  char resolved[PATH_MAX];
  if (realpath(directory, resolved) == NULL)
    return;
  char needle[PATH_MAX + 2];
  if (snprintf(needle, sizeof(needle), " %s ", resolved) >=
      (int)sizeof(needle)) {
    return;
  }
  FILE *file = fopen("/proc/self/mountinfo", "r");
  if (file == NULL)
    return;
  char line[4096];
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strstr(line, needle) == NULL)
      continue;
    line[strcspn(line, "\n")] = '\0';
    fprintf(stderr, "# arena_mountinfo=%s\n", line);
    break;
  }
  fclose(file);
}

static void print_cgroup_environment(void) {
  FILE *file = fopen("/proc/self/cgroup", "r");
  if (file == NULL)
    return;
  char line[1024];
  char path[PATH_MAX] = {0};
  while (fgets(line, sizeof(line), file) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    fprintf(stderr, "# cgroup=%s\n", line);
    if (strncmp(line, "0::", 3) == 0) {
      if (snprintf(path, sizeof(path), "/sys/fs/cgroup%s", line + 3) >=
          (int)sizeof(path)) {
        path[0] = '\0';
      }
    }
  }
  fclose(file);
  if (path[0] == '\0')
    return;

  static const char *files[] = {
      "memory.current",  "memory.max",    "memory.swap.current",
      "memory.swap.max", "memory.events", "memory.pressure",
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    char full_path[1200];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i]) >=
        (int)sizeof(full_path)) {
      continue;
    }
    print_first_line(files[i], full_path);
  }
  char memory_stat_path[1200];
  if (snprintf(memory_stat_path, sizeof(memory_stat_path), "%s/memory.stat",
               path) < (int)sizeof(memory_stat_path)) {
    print_matching_line("memory.stat.shmem", memory_stat_path, "shmem ");
  }
}

static void print_environment(const char *directory, int fd,
                              const int *allowed_cpus, int allowed_count) {
  struct utsname name;
  if (uname(&name) == 0) {
    fprintf(stderr, "# kernel=%s %s %s machine=%s\n", name.sysname,
            name.release, name.version, name.machine);
  }
  fprintf(stderr,
          "# page_size=%ld allowed_cpu_count=%d allowed_cpus=", g_page_size,
          allowed_count);
  for (int i = 0; i < allowed_count; ++i) {
    fprintf(stderr, "%s%d", i == 0 ? "" : ",", allowed_cpus[i]);
  }
  fputc('\n', stderr);

  struct statfs file_system;
  if (fstatfs(fd, &file_system) == 0) {
    fprintf(stderr, "# arena_f_type=0x%lx\n",
            (unsigned long)file_system.f_type);
  }
  struct statvfs capacity;
  if (statvfs(directory, &capacity) == 0) {
    unsigned long long available = (unsigned long long)capacity.f_bavail *
                                   (unsigned long long)capacity.f_frsize;
    fprintf(stderr, "# arena_available_bytes=%llu\n", available);
  }
  fprintf(stderr, "# arena_dir=%s\n", directory);
  print_mountinfo(directory);

  print_first_line("thp_shmem_enabled",
                   "/sys/kernel/mm/transparent_hugepage/shmem_enabled");
  glob_t matches;
  memset(&matches, 0, sizeof(matches));
  if (glob("/sys/kernel/mm/transparent_hugepage/hugepages-*kB/shmem_enabled", 0,
           NULL, &matches) == 0) {
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
      print_first_line(matches.gl_pathv[i], matches.gl_pathv[i]);
    }
  }
  globfree(&matches);
  print_cgroup_environment();
  print_first_line("memory_psi", "/proc/pressure/memory");
}

static int start_epoch(struct control *control, int touchers,
                       volatile unsigned char *base, size_t offset,
                       size_t length, pid_t *children, int child_count,
                       int timeout_ms, int *epoch_out) {
  if (g_interrupted || check_children_alive(children, child_count) != 0)
    return -1;
  int epoch = __atomic_load_n(&control->epoch, __ATOMIC_RELAXED) + 1;
  materialize_range(base, offset, length, (unsigned int)epoch);
  __atomic_store_n(&control->offset, (uint64_t)offset, __ATOMIC_RELAXED);
  __atomic_store_n(&control->length, (uint64_t)length, __ATOMIC_RELAXED);
  __atomic_store_n(&control->touched_ready, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&control->touch_errors, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&control->epoch, epoch, __ATOMIC_RELEASE);
  futex_wake_all(&control->epoch);

  if (wait_for_counter(&control->touched_ready, touchers, timeout_ms, children,
                       child_count, "client read faults") != 0) {
    complete_epoch(control, epoch);
    return -1;
  }
  int touch_errors = __atomic_load_n(&control->touch_errors, __ATOMIC_ACQUIRE);
  if (touch_errors != 0) {
    fprintf(stderr,
            "ERROR: toucher clients observed %d unexpected zero pages\n",
            touch_errors);
    complete_epoch(control, epoch);
    return -1;
  }
  *epoch_out = epoch;
  return 0;
}

static int run_attempt(struct control *control, int fd, int touchers,
                       volatile unsigned char *base, size_t offset,
                       size_t length, pid_t *children, int child_count,
                       int timeout_ms, int check_zero,
                       struct attempt_result *result) {
  memset(result, 0, sizeof(*result));
  result->offset = offset;
  if (start_epoch(control, touchers, base, offset, length, children,
                  child_count, timeout_ms, &result->epoch) != 0) {
    return -1;
  }

  result->backing_before = backing_bytes(fd);
  if (result->backing_before < 0) {
    complete_epoch(control, result->epoch);
    fprintf(stderr, "ERROR: fstat backing before MADV_REMOVE: %s\n",
            strerror(errno));
    return -1;
  }
  errno = 0;
  uint64_t start = now_ns();
  result->rc = madvise((void *)(base + offset), length, MADV_REMOVE);
  result->error_number = errno;
  uint64_t end = now_ns();
  result->latency_ns = end - start;
  complete_epoch(control, result->epoch);

  result->backing_after = backing_bytes(fd);
  if (result->backing_after < 0) {
    fprintf(stderr, "ERROR: fstat backing after MADV_REMOVE: %s\n",
            strerror(errno));
    return -1;
  }
  result->backing_drop = result->backing_before - result->backing_after;
  result->backing_ok =
      result->rc == 0 && result->backing_drop >= (long long)length;
  if (result->rc == 0 && check_zero) {
    result->zero_checked = 1;
    result->zero_ok = verify_zero_range(base, offset, length);
  }
  return 0;
}

static void write_raw_header(FILE *raw) {
  fprintf(raw,
          "run_id,phase,client_mappers,present_clients,mode,oversubscribed,"
          "range_bytes,attempt,epoch,offset,rc,errno,latency_ns,"
          "backing_before,backing_after,backing_drop,backing_ok,zero_checked,"
          "zero_ok\n");
}

static void write_raw_result(FILE *raw, const char *run_id, const char *phase,
                             int clients, int touchers, const char *mode,
                             int oversubscribed, size_t range, size_t attempt,
                             const struct attempt_result *result) {
  if (raw == NULL)
    return;
  fprintf(
      raw,
      "%s,%s,%d,%d,%s,%d,%zu,%zu,%d,%zu,%d,%d,%llu,%lld,%lld,%lld,%d,%d,%d\n",
      run_id, phase, clients, touchers, mode, oversubscribed, range, attempt,
      result->epoch, result->offset, result->rc, result->error_number,
      (unsigned long long)result->latency_ns, result->backing_before,
      result->backing_after, result->backing_drop, result->backing_ok,
      result->zero_checked, result->zero_ok);
}

int main(int argc, char **argv) {
  const char *directory = "/dev/shm";
  const char *raw_path = NULL;
  const char *run_id = "manual";
  size_t region_size = (size_t)4096 << 20;
  int clients = 8;
  int touchers = -1;
  size_t ranges[16];
  int range_count = 0;
  size_t target_samples = 1024;
  size_t warmup_samples = 32;
  int active = 0;
  int pin_parent = 1;
  int allow_oversubscription = 0;
  int barrier_timeout_ms = 120000;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--dir") && i + 1 < argc) {
      directory = argv[++i];
    } else if (!strcmp(argv[i], "--region-mb") && i + 1 < argc) {
      region_size = (size_t)strtoull(argv[++i], NULL, 10) << 20;
    } else if ((!strcmp(argv[i], "--clients") ||
                !strcmp(argv[i], "--nprocs")) &&
               i + 1 < argc) {
      clients = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--touchers") && i + 1 < argc) {
      touchers = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
      target_samples = (size_t)strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) {
      warmup_samples = (size_t)strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(argv[i], "--barrier-timeout-ms") && i + 1 < argc) {
      barrier_timeout_ms = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--run-id") && i + 1 < argc) {
      run_id = argv[++i];
    } else if (!strcmp(argv[i], "--raw") && i + 1 < argc) {
      raw_path = argv[++i];
    } else if (!strcmp(argv[i], "--active") || !strcmp(argv[i], "--spin")) {
      active = 1;
    } else if (!strcmp(argv[i], "--no-pin-parent")) {
      pin_parent = 0;
    } else if (!strcmp(argv[i], "--allow-oversubscription")) {
      allow_oversubscription = 1;
    } else if (!strcmp(argv[i], "--ranges") && i + 1 < argc) {
      char *token = strtok(argv[++i], ",");
      while (token != NULL && range_count < 16) {
        ranges[range_count++] = (size_t)strtoull(token, NULL, 10) << 20;
        token = strtok(NULL, ",");
      }
    } else {
      errno = 0;
      die("unknown or invalid argument: %s", argv[i]);
    }
  }
  if (range_count == 0) {
    ranges[range_count++] = (size_t)1 << 20;
    ranges[range_count++] = (size_t)4 << 20;
    ranges[range_count++] = (size_t)16 << 20;
  }
  if (clients < 0 || clients > 4096 || target_samples == 0 ||
      region_size == 0 || barrier_timeout_ms <= 0) {
    errno = 0;
    die("invalid clients/samples/region/timeout configuration");
  }
  if (touchers < 0)
    touchers = clients;
  if (touchers < 0 || touchers > clients) {
    errno = 0;
    die("touchers must be between 0 and clients");
  }

  g_page_size = sysconf(_SC_PAGESIZE);
  if (g_page_size <= 0)
    die("sysconf(_SC_PAGESIZE)");
  size_t maximum_range = 0;
  for (int i = 0; i < range_count; ++i) {
    if (ranges[i] < (size_t)g_page_size || ranges[i] > region_size ||
        ranges[i] % (size_t)g_page_size != 0) {
      errno = 0;
      die("range %zu is invalid for region %zu and page size %ld", ranges[i],
          region_size, g_page_size);
    }
    if (ranges[i] > maximum_range)
      maximum_range = ranges[i];
  }

  int *allowed_cpus = NULL;
  int allowed_count = collect_allowed_cpus(&allowed_cpus);
  if (allowed_count <= 0)
    die("sched_getaffinity");
  int parent_cpu = pin_parent ? allowed_cpus[allowed_count - 1] : -1;
  int child_cpu_count = allowed_count - (pin_parent ? 1 : 0);
  int oversubscribed = active && touchers > child_cpu_count;
  if (active && touchers > 0 && child_cpu_count <= 0) {
    errno = 0;
    die("no CPU remains for active clients after reserving the parent CPU");
  }
  if (oversubscribed && !allow_oversubscription) {
    errno = 0;
    die("active touchers=%d exceed dedicated child CPUs=%d; use "
        "--allow-oversubscription only for a labelled control run",
        touchers, child_cpu_count);
  }

  struct statvfs capacity;
  if (statvfs(directory, &capacity) != 0)
    die("statvfs %s", directory);
  unsigned long long available = (unsigned long long)capacity.f_bavail *
                                 (unsigned long long)capacity.f_frsize;
  if (available < (unsigned long long)maximum_range * 2ull) {
    errno = 0;
    die("insufficient tmpfs headroom: available=%llu max_range=%zu", available,
        maximum_range);
  }

  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/poc_a_plasmaXXXXXX", directory) >=
      (int)sizeof(path)) {
    errno = 0;
    die("arena path is too long");
  }
  int fd = mkstemp(path);
  if (fd < 0)
    die("mkstemp %s", path);
  if (unlink(path) != 0)
    die("unlink %s", path);
  if (ftruncate(fd, (off_t)region_size) != 0)
    die("ftruncate %zu", region_size);
  struct statfs arena_fs;
  if (fstatfs(fd, &arena_fs) != 0)
    die("fstatfs arena");
  if ((unsigned long)arena_fs.f_type != TMPFS_MAGIC) {
    errno = 0;
    die("arena is not tmpfs (f_type=0x%lx)", (unsigned long)arena_fs.f_type);
  }

  FILE *raw = NULL;
  if (raw_path != NULL) {
    raw = fopen(raw_path, "w");
    if (raw == NULL)
      die("open raw output %s", raw_path);
    write_raw_header(raw);
  }

  print_environment(directory, fd, allowed_cpus, allowed_count);
  fprintf(stderr,
          "# run_id=%s client_mappers=%d present_clients=%d store_mappers=1 "
          "total_mappers=%d total_present_mappers=%d mode=%s oversubscribed=%d "
          "arena_bytes=%zu\n",
          run_id, clients, touchers, clients + 1, touchers + 1,
          active ? "pinned-active" : "futex-parked", oversubscribed,
          region_size);

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGINT, &action, NULL);
  (void)sigaction(SIGTERM, &action, NULL);

  struct control *control = mmap(NULL, sizeof(*control), PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (control == MAP_FAILED)
    die("mmap control");
  memset(control, 0, sizeof(*control));

  pid_t *children =
      calloc((size_t)(clients > 0 ? clients : 1), sizeof(*children));
  if (children == NULL)
    die("calloc children");
  pid_t parent_pid = getpid();
  int started_children = 0;
  int exit_code = 0;
  for (int i = 0; i < clients; ++i) {
    pid_t child = fork();
    if (child < 0) {
      fprintf(stderr, "ERROR: fork child %d: %s\n", i, strerror(errno));
      exit_code = 1;
      goto cleanup_without_parent_map;
    }
    if (child == 0) {
      int cpu = -1;
      if (active && i < touchers)
        cpu = allowed_cpus[i % child_cpu_count];
      free(children);
      free(allowed_cpus);
      child_main(fd, region_size, control, i < touchers, active, cpu,
                 parent_pid);
    }
    children[i] = child;
    ++started_children;
  }

  void *parent_mapping =
      mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (parent_mapping == MAP_FAILED) {
    fprintf(stderr, "ERROR: parent mmap: %s\n", strerror(errno));
    exit_code = 1;
    goto cleanup_without_parent_map;
  }
  volatile unsigned char *base = parent_mapping;

  if (pin_parent && pin_to_cpu(parent_cpu) != 0) {
    fprintf(stderr, "ERROR: pin parent to CPU %d: %s\n", parent_cpu,
            strerror(errno));
    exit_code = 1;
    goto cleanup;
  }
  if (wait_for_counter(&control->mapped_ready, clients, barrier_timeout_ms,
                       children, started_children,
                       "all client mappings") != 0) {
    exit_code = 1;
    goto cleanup;
  }

  printf("run_id,client_mappers,present_clients,store_mappers,total_mappers,"
         "total_present_mappers,mode,oversubscribed,range_bytes,samples,p50_us,"
         "p95_us,p99_us,max_us,mean_us,MiBps_at_p50,syscall_failures,"
         "backing_checks,backing_failures,zero_checks,zero_failures\n");

  for (int range_index = 0; range_index < range_count; ++range_index) {
    size_t range = ranges[range_index];
    size_t slot_count = region_size / range;
    uint64_t *latencies = malloc(target_samples * sizeof(*latencies));
    if (latencies == NULL) {
      fprintf(stderr, "ERROR: allocate latency samples\n");
      exit_code = 1;
      goto cleanup;
    }

    size_t warm_successes = 0;
    size_t warm_attempts = 0;
    size_t warm_limit = warmup_samples + 16;
    while (warm_successes < warmup_samples && warm_attempts < warm_limit) {
      size_t slot = slot_for_attempt(warm_attempts, slot_count,
                                     (size_t)range_index * 17u);
      struct attempt_result result;
      if (run_attempt(control, fd, touchers, base, slot * range, range,
                      children, started_children, barrier_timeout_ms, 0,
                      &result) != 0) {
        free(latencies);
        exit_code = 1;
        goto cleanup;
      }
      write_raw_result(raw, run_id, "warmup", clients, touchers,
                       active ? "active" : "parked", oversubscribed, range,
                       warm_attempts, &result);
      ++warm_attempts;
      if (result.rc == 0 && result.backing_ok)
        ++warm_successes;
    }
    if (warm_successes < warmup_samples) {
      fprintf(stderr, "ERROR: too many failed warmups for range=%zu\n", range);
      free(latencies);
      exit_code = 1;
      goto cleanup;
    }

    size_t successes = 0;
    size_t attempts = 0;
    size_t attempt_limit = target_samples + target_samples / 10 + 16;
    size_t syscall_failures = 0;
    size_t backing_checks = 0;
    size_t backing_failures = 0;
    size_t zero_checks = 0;
    size_t zero_failures = 0;
    while (successes < target_samples && attempts < attempt_limit) {
      size_t global_attempt = warm_attempts + attempts;
      size_t slot = slot_for_attempt(global_attempt, slot_count,
                                     (size_t)range_index * 17u);
      struct attempt_result result;
      int check_zero = successes == 0;
      if (run_attempt(control, fd, touchers, base, slot * range, range,
                      children, started_children, barrier_timeout_ms,
                      check_zero, &result) != 0) {
        free(latencies);
        exit_code = 1;
        goto cleanup;
      }
      write_raw_result(raw, run_id, "sample", clients, touchers,
                       active ? "active" : "parked", oversubscribed, range,
                       attempts, &result);
      ++attempts;
      if (result.rc != 0) {
        ++syscall_failures;
        continue;
      }
      ++backing_checks;
      if (!result.backing_ok)
        ++backing_failures;
      if (result.zero_checked) {
        ++zero_checks;
        if (!result.zero_ok)
          ++zero_failures;
      }
      latencies[successes++] = result.latency_ns;
    }
    if (successes < target_samples) {
      fprintf(stderr, "ERROR: too many syscall failures for range=%zu\n",
              range);
      free(latencies);
      exit_code = 1;
      goto cleanup;
    }

    qsort(latencies, successes, sizeof(*latencies), compare_u64);
    uint64_t p50 = percentile(latencies, successes, 50);
    uint64_t p95 = percentile(latencies, successes, 95);
    uint64_t p99 = percentile(latencies, successes, 99);
    uint64_t maximum = latencies[successes - 1];
    long double sum = 0;
    for (size_t i = 0; i < successes; ++i)
      sum += latencies[i];
    double mean_us = (double)(sum / successes) / 1000.0;
    double throughput =
        p50 == 0 ? 0.0
                 : ((double)range / (double)(1u << 20)) / ((double)p50 / 1e9);

    printf("%s,%d,%d,1,%d,%d,%s,%d,%zu,%zu,%.1f,%.1f,%.1f,%.1f,%.2f,"
           "%.1f,%zu,%zu,%zu,%zu,%zu\n",
           run_id, clients, touchers, clients + 1, touchers + 1,
           active ? "active" : "parked", oversubscribed, range, successes,
           p50 / 1000.0, p95 / 1000.0, p99 / 1000.0, maximum / 1000.0, mean_us,
           throughput, syscall_failures, backing_checks, backing_failures,
           zero_checks, zero_failures);
    fflush(stdout);
    if (raw != NULL)
      fflush(raw);
    if (backing_failures != 0 || zero_failures != 0)
      exit_code = 2;
    free(latencies);
  }

cleanup:
  stop_children(control, children, started_children,
                exit_code != 0 || g_interrupted);
  munmap(parent_mapping, region_size);
  goto finish;

cleanup_without_parent_map:
  stop_children(control, children, started_children, 1);

finish:
  if (g_interrupted && exit_code == 0)
    exit_code = 130;
  if (raw != NULL)
    fclose(raw);
  munmap(control, sizeof(*control));
  free(children);
  free(allowed_cpus);
  close(fd);
  return exit_code;
}
