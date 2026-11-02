#!/usr/bin/env bash
# Sweep CAGRA build parameters on real AMD hardware and report (time, recall).
#
# Run this on the contest AMD instance, inside the contest repo root, AFTER:
#   HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/roc tools/hipvs_bridge/build.sh "$PWD/out"
#
#   bash tools/hipvs_bridge/sweep_cagra.sh 250000 128 200 100
#
# Each line is one (build seconds, Recall@10, Recall@100) triple. Compare build_s
# against the baseline row (no env vars set) to get the speedup ratio.
set -u

N=${1:-250000}
DIM=${2:-128}
NQ=${3:-200}
TOPK=${4:-100}

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/out"
BRIDGE="$OUT/libseekdb_cuvs_bridge.so"
BENCH="$OUT/bench_cagra"

if [ ! -f "$BRIDGE" ]; then
  echo "missing $BRIDGE -- build the bridge first:" >&2
  echo "  HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/roc tools/hipvs_bridge/build.sh $OUT" >&2
  exit 1
fi

if [ ! -x "$BENCH" ]; then
  echo "compiling bench_cagra ..." >&2
  HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
  ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
  HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
  # Single-threaded ground truth: the bench is I/O and probe bound, and a -fopenmp
  # dependency on libomp is one more thing that can fail to link on a fresh box.
  "$HIPCC" -O3 -std=gnu17 \
    -D__HIP_PLATFORM_AMD__ \
    -I"$ROOT/tools/hipvs_bridge" \
    -I"$HIPVS_ROOT/include" \
    -I"$ROCM_ROOT/include" \
    "$ROOT/tools/hipvs_bridge/bench_cagra.c" \
    -L"$OUT" -L"$HIPVS_ROOT/lib" -L"$ROCM_ROOT/lib" \
    -l:libseekdb_cuvs_bridge.so -lcuvs_c \
    -Wl,-rpath,"$OUT:$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
    -o "$BENCH" || { echo "bench compile failed" >&2; exit 1; }
fi

run() {
  echo "--- $1"
  shift
  env "$@" "$BENCH" "$N" "$DIM" "$NQ" "$TOPK" 2>/dev/null
}

# Shipped default: 50 iterations (upstream 100). Every row below is compared
# against the value this branch actually submits, so a row only wins if it beats
# what we already ship.
run "shipped default (gd=128 igd=256 niter=50)"

# The old upstream value, kept so the speedup we claim can be reproduced against
# the real baseline rather than against our own default.
run "upstream baseline (gd=128 igd=256 niter=100)" OB_CAGRA_NN_ITER=100

# nn-descent iterations: the dominant build cost. Recall must not drop at all
# versus the upstream baseline row, so step down and watch recall@10 @100.
for it in 40 30 20; do
  run "niter=$it" OB_CAGRA_NN_ITER=$it
done

# intermediate degree: halves the candidate list width the descent works on.
for igd in 192 160 128; do
  run "igd=$igd" OB_CAGRA_INTERMEDIATE_DEGREE=$igd
done

# Combined: the two levers together usually beat either alone.
for pair in "40 160" "30 160" "30 128"; do
  set -- $pair
  run "niter=$1 igd=$2" OB_CAGRA_NN_ITER=$1 OB_CAGRA_INTERMEDIATE_DEGREE=$2
done

echo
echo "Speedup is build_s(upstream baseline) / build_s(row)."
echo "Only accept a row whose recall@10 and recall@100 are >= the upstream baseline row."
