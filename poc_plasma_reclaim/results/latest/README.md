# Latest reproducible standalone results

This directory is the complete result set cited by the REP. It is generated
from the sources in the same repository tree and has no dependency on an older
commit or a separate Ray checkout.

Regenerate the complete PoC-B/C snapshot on Linux with at least 1 GiB free in
writable `/dev/shm`:

```bash
cd poc_plasma_reclaim
./run_pocs.sh all ./results/latest
```

Regenerate PoC-A's aggregate and per-case raw samples on a host with at least
4 GiB free in `/dev/shm`:

```bash
./results/latest/poc_a_command.sh ./results/replayed/poc_a
```

The invocation rebuilds the native binaries and runs:

- PoC-B with 200K live prefix objects for churn cadences `0,8,4,2,1`, with up
  to 4,000 mutation and recovery quanta;
- PoC-B with 500K and 1M live prefix objects for the stable control (`0`) and
  adversarial per-quantum mutation arm (`1`), with up to 400 mutation and
  recovery quanta;
- the default PoC-C active-trim workload with 256 MiB of dead payload and
  20,000 churn operations.

Files:

- `poc_a_summary.csv`: complete aggregate rows for the three-round PoC-A
  matrix;
- `poc_a_command.sh`: exact PoC-A replay wrapper, which regenerates the
  aggregate and per-case raw samples;
- `poc_a_environment.txt`: recorded environment for the committed aggregate;
- `poc_a_source_sha256.txt`: hashes of the PoC-A source and runner;
- `commands.sh`: exact commands used for each captured output;
- `environment.txt`: kernel, userspace, page size, CPU/cpuset, tmpfs, THP, and
  compiler metadata;
- `manifest.toml`: suite completeness and provenance pointers;
- `build_commands.txt`: effective native compile/link commands;
- `source_sha256.txt`: hashes of every source/build file affecting PoC-B/C;
- `poc_b_<scale>_churn_<cadence>.csv/.log`: complete per-arm output;
- `poc_b_summary.csv`: rectangular summary of every PoC-B arm;
- `poc_c.csv/.log`: complete PoC-C telemetry and verdict;
- `SHA256SUMS`: hashes for every other file in this directory.

PoC-B's strong starvation signature is: a stable arm reaches the known deep
range; a cadence-1 arm does not reach it during the bounded mutation phase;
and the same cursor reaches it after mutation stops. PoC-C is the focused
live-data/reclaim/controller test and is not used as the deep-cursor verdict.
