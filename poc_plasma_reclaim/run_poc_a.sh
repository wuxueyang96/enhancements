#!/usr/bin/env bash
# Run the auditable PoC-A matrix. M and P below always mean client mappings and
# present client mappings; the raylet/store mapping is reported separately.
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "ERROR: PoC-A requires Linux MADV_REMOVE and futex support." >&2
  exit 1
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
BIN=${BIN:-"$SCRIPT_DIR/poc_a"}
SRC=${SRC:-"$SCRIPT_DIR/poc_a_hole_punch_latency.c"}
DIR=${DIR:-/dev/shm}
REGION_MB=${REGION_MB:-4096}
SAMPLES=${SAMPLES:-1024}
WARMUP=${WARMUP:-32}
RANGES=${RANGES:-1,4,16}
RUNS=${RUNS:-3}
SEED=${SEED:-20260820}
BARRIER_TIMEOUT_MS=${BARRIER_TIMEOUT_MS:-120000}
RAW_DIR=${RAW_DIR:-"$PWD/poc_a_raw_$(date +%Y%m%d_%H%M%S)"}

if [[ ! -x "$BIN" || "$SRC" -nt "$BIN" ]]; then
  echo "# building $BIN from $SRC" >&2
  gcc -std=gnu11 -O2 -Wall -Wextra -Werror -o "$BIN" "$SRC"
fi

mkdir -p "$RAW_DIR"
allowed_cpus=$(nproc)
dedicated_clients=$((allowed_cpus > 1 ? allowed_cpus - 1 : 0))
echo "# allowed_cpus=$allowed_cpus dedicated_active_clients=$dedicated_clients" >&2
echo "# raw_dir=$RAW_DIR runs=$RUNS seed=$SEED" >&2

# case format: client_mappers:present_clients:mode
cases=(
  "0:0:parked"
  "1:1:parked"
  "8:8:parked"
  "32:32:parked"
  "64:64:parked"
  "64:0:parked"
  "64:1:parked"
  "64:32:parked"
  "128:0:parked"
  "256:0:parked"
)

for n in 1 8 32 64; do
  if (( n <= dedicated_clients )); then
    cases+=("$n:$n:active")
  else
    echo "# skip active M=P=$n: only $dedicated_clients dedicated client CPUs" >&2
  fi
done

shuffle_cases() {
  local round=$1
  local -n values=$2
  local i
  RANDOM=$((SEED + round))
  for ((i=${#values[@]}-1; i>0; --i)); do
    local j=$((RANDOM % (i + 1)))
    local tmp=${values[i]}
    values[i]=${values[j]}
    values[j]=$tmp
  done
}

shuffle_ranges() {
  local -n values=$1
  local i
  for ((i=${#values[@]}-1; i>0; --i)); do
    local j=$((RANDOM % (i + 1)))
    local tmp=${values[i]}
    values[i]=${values[j]}
    values[j]=$tmp
  done
}

emitted_header=0
for ((round=1; round<=RUNS; ++round)); do
  round_cases=("${cases[@]}")
  shuffle_cases "$round" round_cases
  for case_spec in "${round_cases[@]}"; do
    IFS=: read -r clients touchers mode <<<"$case_spec"
    run_id="r${round}_m${clients}_p${touchers}_${mode}"
    raw_file="$RAW_DIR/${run_id}.csv"
    IFS=, read -r -a case_ranges <<<"$RANGES"
    shuffle_ranges case_ranges
    case_ranges_csv=$(IFS=,; printf '%s' "${case_ranges[*]}")
    mode_args=()
    if [[ $mode == active ]]; then mode_args+=(--active); fi
    echo "# run_id=$run_id M_clients=$clients P_clients=$touchers mode=$mode" >&2

    output=$(
      "$BIN" \
        --dir "$DIR" \
        --region-mb "$REGION_MB" \
        --clients "$clients" \
        --touchers "$touchers" \
        --samples "$SAMPLES" \
        --warmup "$WARMUP" \
        --ranges "$case_ranges_csv" \
        --barrier-timeout-ms "$BARRIER_TIMEOUT_MS" \
        --run-id "$run_id" \
        --raw "$raw_file" \
        "${mode_args[@]}"
    )
    if (( emitted_header == 0 )); then
      printf '%s\n' "$output"
      emitted_header=1
    else
      printf '%s\n' "$output" | tail -n +2
    fi
  done
done

echo "# complete: raw samples are in $RAW_DIR" >&2
