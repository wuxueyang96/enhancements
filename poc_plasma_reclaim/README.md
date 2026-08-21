# Standalone Plasma physical-page reclaim probes

This directory contains the focused Linux PoC-A, PoC-B, and PoC-C probes used by the
Plasma physical-page-reclamation REP. They are self-contained: no Ray checkout,
Ray headers, Bazel workspace, source overlay, patch, or Ray fork is required.

The probes vendor Doug Lea's public-domain dlmalloc 2.8.6 and create one
fixed-size, file-backed `mspace`. The scanner walks dlmalloc's real physical
chunk chain; it is not a synthetic container model. The controller is a
standalone reproduction of the state machine under study.

This evidence boundary matters: the probes exercise the same allocator family
and scanner/controller algorithms, but they do **not** compile or execute the
production Ray implementation. They do not test `PlasmaStore`, Store mutex
ownership, event-loop ordering, CreateRequestQueue, shutdown gating, or real
`Create` latency.

## Contents

- `poc_a_hole_punch_latency.c` and `run_poc_a.sh`: shared-mapping
  `MADV_REMOVE` latency matrix across mapper count, present PTE count, range,
  and parked/active clients.
- `poc_b_cursor_rebuild.cc`: deep-cursor liveness under topology churn. It
  performs real dlmalloc Allocate+Free mutations but never removes pages.
- `poc_c_active_trim_churn.cc`: active allocation/free churn while the
  standalone controller issues real `MADV_REMOVE`, with live-data checksums.
- `standalone_allocator.cc`: tmpfs-backed fixed dlmalloc arena and validated
  resumable chunk-chain scanner.
- `standalone_trimmer.cc`: ratio controller and bounded scan quantum.
- `standalone_reclaim.h`: shared API.
- `third_party/dlmalloc.c`: unmodified dlmalloc 2.8.6 source with its original
  public-domain/CC0 provenance.
- `run_pocs.sh`: rebuilds and regenerates the complete result suite.
- `results/latest/`: the single authoritative, reproducible result set.

## Requirements

- Linux;
- a C++17 compiler (`g++` or `clang++`);
- GNU Make and coreutils (`sha256sum`);
- writable tmpfs at `/dev/shm`;
- at least 1 GiB free in `/dev/shm` for the 1M-live-object PoC-B arms;
- kernel/filesystem support for `MADV_REMOVE` on shared tmpfs mappings.

## Reproduce the complete result set

From this directory:

```bash
./run_pocs.sh all ./results/latest
```

The script always performs a clean native rebuild, captures the environment and
source hashes, records exact replay commands, and generates checksums. It runs:

- PoC-B with 200K live prefix objects and churn cadences `0,8,4,2,1`;
- PoC-B with 500K and 1M live prefix objects for stable cadence `0` and
  adversarial cadence `1`;
- the default PoC-C active-trim workload.

PoC-A is more expensive and host-topology-sensitive, so its exact matrix is
regenerated separately with the committed command in `results/latest/`:

```bash
./results/latest/poc_a_command.sh ./results/replayed/poc_a
```

This produces the aggregate CSV plus per-case raw samples. The committed
`poc_a_summary.csv` is the complete aggregate from the cited run; raw per-sample
files are intentionally regenerated rather than committed.

See [`results/latest/README.md`](results/latest/README.md) for the artifact
layout and interpretation. No result cited by the REP needs to be recovered
from an older commit.

To build without running:

```bash
make
```

`make compile` is available for compiler-only checking on a non-Linux host, but
running the probes there is unsupported.

## Individual probes

PoC-B example:

```bash
./poc_b_cursor_rebuild \
  --live-objs 200000 \
  --obj-bytes 512 \
  --churn-every 1 \
  --quantum-ms 10 \
  --max-quanta 4000 \
  --recovery-max-quanta 4000
```

The strong starvation signature is: the stable arm reaches the deep range, the
cadence-1 arm cannot reach it while mutations continue, and the same cursor
reaches it after mutation stops. Slow rebuilding alone is not proof of
starvation.

PoC-C example:

```bash
./poc_c_active_trim_churn \
  --dead-mb 256 \
  --live-objs 64 \
  --churn-ops 20000 \
  --quantum-ms 10
```

PoC-C succeeds only if scanner/generation invariants remain valid, live data
remains intact, physical backing falls meaningfully, and the controller reaches
`Idle` or `NoProgress` after churn stops. PoC-B, not PoC-C, is the focused
deep-cursor verdict.

The standalone arena deliberately has no fallback path. All logical bytes,
physical bytes, allocations, and scanner ranges belong to one held-open tmpfs
inode. The legacy `--fallback DIR` spelling is accepted only as an ignored CLI
compatibility option.

Do not present these results as evidence about production `PlasmaStore::Create`
p99, Store-lock hold time, OOM/SIGBUS behavior under pressure, refault cost, or
production canary behavior.
