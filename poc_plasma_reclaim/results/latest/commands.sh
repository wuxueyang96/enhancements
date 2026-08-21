#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RESULT_DIR=${1:-"$SCRIPT_DIR/results/replayed"}
mkdir -p "$RESULT_DIR"
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR" all
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 200000 --obj-bytes 512 --churn-every 0 --quantum-ms 10 --max-quanta 4000 --recovery-max-quanta 4000 >"$RESULT_DIR/poc_b_200k_churn_0.csv" 2>"$RESULT_DIR/poc_b_200k_churn_0.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 200000 --obj-bytes 512 --churn-every 8 --quantum-ms 10 --max-quanta 4000 --recovery-max-quanta 4000 >"$RESULT_DIR/poc_b_200k_churn_8.csv" 2>"$RESULT_DIR/poc_b_200k_churn_8.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 200000 --obj-bytes 512 --churn-every 4 --quantum-ms 10 --max-quanta 4000 --recovery-max-quanta 4000 >"$RESULT_DIR/poc_b_200k_churn_4.csv" 2>"$RESULT_DIR/poc_b_200k_churn_4.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 200000 --obj-bytes 512 --churn-every 2 --quantum-ms 10 --max-quanta 4000 --recovery-max-quanta 4000 >"$RESULT_DIR/poc_b_200k_churn_2.csv" 2>"$RESULT_DIR/poc_b_200k_churn_2.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 200000 --obj-bytes 512 --churn-every 1 --quantum-ms 10 --max-quanta 4000 --recovery-max-quanta 4000 >"$RESULT_DIR/poc_b_200k_churn_1.csv" 2>"$RESULT_DIR/poc_b_200k_churn_1.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 500000 --obj-bytes 512 --churn-every 0 --quantum-ms 10 --max-quanta 400 --recovery-max-quanta 400 >"$RESULT_DIR/poc_b_500k_churn_0.csv" 2>"$RESULT_DIR/poc_b_500k_churn_0.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 1000000 --obj-bytes 512 --churn-every 0 --quantum-ms 10 --max-quanta 400 --recovery-max-quanta 400 >"$RESULT_DIR/poc_b_1m_churn_0.csv" 2>"$RESULT_DIR/poc_b_1m_churn_0.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 500000 --obj-bytes 512 --churn-every 1 --quantum-ms 10 --max-quanta 400 --recovery-max-quanta 400 >"$RESULT_DIR/poc_b_500k_churn_1.csv" 2>"$RESULT_DIR/poc_b_500k_churn_1.log"
"$SCRIPT_DIR"/poc_b_cursor_rebuild --live-objs 1000000 --obj-bytes 512 --churn-every 1 --quantum-ms 10 --max-quanta 400 --recovery-max-quanta 400 >"$RESULT_DIR/poc_b_1m_churn_1.csv" 2>"$RESULT_DIR/poc_b_1m_churn_1.log"
"$SCRIPT_DIR"/poc_c_active_trim_churn --dead-mb 256 --live-objs 64 --churn-ops 20000 --quantum-ms 10 >"$RESULT_DIR/poc_c.csv" 2>"$RESULT_DIR/poc_c.log"
