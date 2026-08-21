#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-all}
result_dir=${2:-"$script_dir/results/latest"}

case "$mode" in
  b|c|all) ;;
  *)
    echo "usage: $0 [b|c|all] [result-directory]" >&2
    exit 2
    ;;
esac

if [[ $(uname -s) != Linux ]]; then
  echo "error: PoC-B/C require Linux (tmpfs and MADV_REMOVE)" >&2
  exit 1
fi
if [[ ! -d /dev/shm ]]; then
  echo "error: /dev/shm is unavailable" >&2
  exit 1
fi
if [[ $mode == all || $mode == b ]]; then
  tmpfs_available=$(df -B1 --output=avail /dev/shm | tail -1 | tr -d ' ')
  if [[ ! $tmpfs_available =~ ^[0-9]+$ ]] || \
      (( tmpfs_available < 1073741824 )); then
    echo "error: PoC-B scale arms require at least 1 GiB free in /dev/shm" >&2
    exit 1
  fi
fi

# Force a native rebuild in case this directory was copied from another host.
make -C "$script_dir" clean
make -C "$script_dir" all
mkdir -p "$result_dir"
result_dir=$(CDPATH= cd -- "$result_dir" && pwd)
canonical_latest=$(CDPATH= cd -- "$script_dir/results/latest" && pwd)
if [[ $mode != all && $result_dir == "$canonical_latest" ]]; then
  echo "error: results/latest is authoritative and requires mode=all" >&2
  exit 2
fi
if [[ $result_dir != "$canonical_latest" ]]; then
  cp "$script_dir/results/latest/README.md" "$result_dir/README.md"
fi

# Remove every generated artifact so a partial custom run cannot authenticate
# stale outputs from a previous invocation. README.md is source documentation.
rm -f "$result_dir"/poc_b_*.csv "$result_dir"/poc_b_*.log \
  "$result_dir/poc_c.csv" "$result_dir/poc_c.log" \
  "$result_dir/commands.sh" "$result_dir/environment.txt" \
  "$result_dir/source_sha256.txt" "$result_dir/manifest.toml" \
  "$result_dir/build_commands.txt" "$result_dir/SHA256SUMS"

commands_file="$result_dir/commands.sh"
environment_file="$result_dir/environment.txt"
source_hashes_file="$result_dir/source_sha256.txt"
manifest_file="$result_dir/manifest.toml"
build_commands_file="$result_dir/build_commands.txt"
printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' 'export LC_ALL=C' \
  'SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)' \
  'RESULT_DIR=${1:-"$SCRIPT_DIR/results/replayed"}' \
  'mkdir -p "$RESULT_DIR"' \
  'make -C "$SCRIPT_DIR" clean' 'make -C "$SCRIPT_DIR" all' \
  >"$commands_file"

record_command() {
  local stem=$1
  local binary=$2
  shift 2
  printf '"$SCRIPT_DIR"/%q' "$binary" >>"$commands_file"
  printf ' %q' "$@" >>"$commands_file"
  printf ' >"$RESULT_DIR/%s.csv" 2>"$RESULT_DIR/%s.log"\n' \
    "$stem" "$stem" >>"$commands_file"
}

run_capture() {
  local stem=$1
  local binary=$2
  shift 2
  record_command "$stem" "$binary" "$@"
  "$script_dir/$binary" "$@" \
    >"$result_dir/$stem.csv" \
    2>"$result_dir/$stem.log"
}

capture_environment() {
  local build_cxx=${CXX:-g++}
  local git_revision=${SOURCE_GIT_REVISION:-}
  local git_dirty=${SOURCE_GIT_DIRTY:-}
  if [[ -z $git_revision ]]; then
    git_revision=$(git -C "$script_dir" rev-parse HEAD 2>/dev/null || echo unavailable)
  fi
  if [[ -z $git_dirty ]]; then
    if git -C "$script_dir" diff --quiet -- . 2>/dev/null && \
        git -C "$script_dir" diff --cached --quiet -- . 2>/dev/null; then
      git_dirty=false
    else
      git_dirty=true
    fi
  fi
  {
    echo "format_version=1"
    echo "generated_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel=$(uname -srmo)"
    if [[ -r /etc/os-release ]]; then
      sed -n 's/^\(ID\|VERSION_ID\|PRETTY_NAME\)=/os_\1=/p' /etc/os-release
    fi
    echo "page_size=$(getconf PAGESIZE)"
    echo "online_cpus=$(nproc)"
    awk '/^Cpus_allowed_list:|^Mems_allowed_list:/ {print "process_" $1 $2}' \
      /proc/self/status 2>/dev/null || true
    if command -v findmnt >/dev/null 2>&1; then
      echo "tmpfs=$(findmnt -n -o FSTYPE,OPTIONS /dev/shm 2>/dev/null || true)"
    fi
    echo "tmpfs_capacity_bytes=$(df -B1 --output=size /dev/shm | tail -1 | tr -d ' ')"
    if [[ -r /sys/kernel/mm/transparent_hugepage/shmem_enabled ]]; then
      echo "shmem_thp=$(tr '\n' ' ' \
        </sys/kernel/mm/transparent_hugepage/shmem_enabled | sed 's/[[:space:]]*$//')"
    fi
    echo "git_revision=$git_revision"
    echo "git_dirty=$git_dirty"
    echo "lc_all=$LC_ALL"
    echo "cxx_path=$(command -v "$build_cxx")"
    echo "cxx=$($build_cxx --version | head -1)"
    echo "cppflags=${CPPFLAGS:-}"
    echo "cxxflags=${CXXFLAGS:--O2 -g}"
    echo "make=$(make --version | head -1)"
    echo "suite_mode=$mode"
  } >"$environment_file"
  make -C "$script_dir" -B -n all >"$build_commands_file"

  (
    cd "$script_dir"
    sha256sum \
      Makefile run_pocs.sh poc_b_cursor_rebuild.cc poc_c_active_trim_churn.cc \
      standalone_allocator.cc standalone_reclaim.h standalone_trimmer.cc \
      third_party/dlmalloc.c
  ) >"$source_hashes_file"

  cat >"$manifest_file" <<EOF
schema_version = 1
generated_at_utc = "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
suite = "$mode"
complete = $([[ $mode == all ]] && echo true || echo false)
runner_command = "./run_pocs.sh $mode ./results/latest"
git_revision = "$git_revision"
git_dirty = "$git_dirty"
commands_file = "commands.sh"
environment_file = "environment.txt"
build_commands_file = "build_commands.txt"
source_hashes_file = "source_sha256.txt"
checksums_file = "SHA256SUMS"
EOF
}

run_b_arm() {
  local scale=$1
  local live_objects=$2
  local cadence=$3
  local max_quanta=$4
  local recovery_quanta=$5
  local stem="poc_b_${scale}_churn_${cadence}"
  echo "running PoC-B scale=$scale live_objects=$live_objects cadence=$cadence" >&2
  run_capture "$stem" poc_b_cursor_rebuild \
    --live-objs "$live_objects" \
    --obj-bytes 512 \
    --churn-every "$cadence" \
    --quantum-ms 10 \
    --max-quanta "$max_quanta" \
    --recovery-max-quanta "$recovery_quanta"
}

run_b() {
  local cadence
  for cadence in 0 8 4 2 1; do
    run_b_arm 200k 200000 "$cadence" 4000 4000
  done
  for cadence in 0 1; do
    run_b_arm 500k 500000 "$cadence" 400 400
    run_b_arm 1m 1000000 "$cadence" 400 400
  done
}

run_c() {
  echo "running PoC-C active-trim churn" >&2
  run_capture poc_c poc_c_active_trim_churn \
    --dead-mb 256 \
    --live-objs 64 \
    --churn-ops 20000 \
    --quantum-ms 10
}

write_b_summary() {
  local output="$result_dir/poc_b_summary.csv"
  local file scale summary values
  echo "scale,live_objects,churn_every,mutation_iterations,arrived_during_mutation,scheduled_mutations,generation_resets,recovery_iterations,recovery_required,recovered_same_cursor,deep_reached,deep_target" >"$output"
  for file in \
    "$result_dir"/poc_b_200k_churn_{0,8,4,2,1}.csv \
    "$result_dir"/poc_b_500k_churn_{0,1}.csv \
    "$result_dir"/poc_b_1m_churn_{0,1}.csv; do
    [[ -f $file ]] || continue
    summary=$(tail -n 1 "$file")
    [[ $summary == summary,* ]] || {
      echo "error: missing PoC-B summary in $file" >&2
      exit 1
    }
    case $(basename "$file") in
      poc_b_200k_*) scale=200k; live_objects=200000 ;;
      poc_b_500k_*) scale=500k; live_objects=500000 ;;
      poc_b_1m_*) scale=1m; live_objects=1000000 ;;
      *) echo "error: unknown PoC-B result name: $file" >&2; exit 1 ;;
    esac
    values=$(printf '%s\n' "${summary#summary,}" | \
      sed -E 's/(^|,)[a-z_]+=([^,]*)/\1\2/g')
    printf '%s,%s,%s\n' "$scale" "$live_objects" "$values" >>"$output"
  done
}

write_checksums() {
  (
    cd "$result_dir"
    find . -maxdepth 1 -type f ! -name SHA256SUMS -print0 | \
      sort -z | xargs -0 sha256sum
  ) >"$result_dir/SHA256SUMS"
}

validate_results() {
  local expected=() file
  if [[ $mode == b || $mode == all ]]; then
    expected+=(
      poc_b_200k_churn_0 poc_b_200k_churn_8 poc_b_200k_churn_4
      poc_b_200k_churn_2 poc_b_200k_churn_1
      poc_b_500k_churn_0 poc_b_500k_churn_1
      poc_b_1m_churn_0 poc_b_1m_churn_1
    )
    for file in "${expected[@]}"; do
      [[ -s $result_dir/$file.csv && -s $result_dir/$file.log ]] || {
        echo "error: incomplete result pair for $file" >&2
        exit 1
      }
      tail -n 1 "$result_dir/$file.csv" | \
        grep -q '^summary,.*deep_reached=1' || {
        echo "error: PoC-B did not reach the deep target for $file" >&2
        exit 1
      }
    done
  fi
  if [[ $mode == c || $mode == all ]]; then
    [[ -s $result_dir/poc_c.csv && -s $result_dir/poc_c.log ]] || {
      echo "error: incomplete PoC-C result pair" >&2
      exit 1
    }
    grep -q '^# overall                    : PASS$' \
      "$result_dir/poc_c.log" || {
      echo "error: PoC-C did not report PASS" >&2
      exit 1
    }
  fi
}

capture_environment
if [[ $mode == b || $mode == all ]]; then
  run_b
fi
if [[ $mode == c || $mode == all ]]; then
  run_c
fi
write_b_summary
chmod +x "$commands_file"
validate_results
write_checksums

echo "results written to $result_dir" >&2
