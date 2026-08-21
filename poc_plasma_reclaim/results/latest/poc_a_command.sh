#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output_dir=${1:-"$script_dir/results/replayed/poc_a"}
mkdir -p "$output_dir/raw"

RAW_DIR="$output_dir/raw" \
  RUNS=3 \
  REGION_MB=4096 \
  SAMPLES=1024 \
  WARMUP=32 \
  RANGES=1,4,16 \
  SEED=20260820 \
  "$script_dir/run_poc_a.sh" \
  >"$output_dir/poc_a_summary.csv" \
  2>"$output_dir/poc_a_run.log"
