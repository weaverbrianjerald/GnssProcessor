#!/bin/bash
# Gate extra arm of step PB3 (the SD-EWL row builder cut into stages): the one arm that reaches
# the SD-EWL row builder, which no dual-frequency golden arm does (gates/n4/make_extra_arms.sh,
# arm sdewl: "0 float row(s) (of 0 built)").
# Usage: make_extra_arms.sh <side>...   (tags pb3x_<side>_tri; side "cal" for the calibration run)
# Base: the three-band field arm runs_nom/_rap_n3b_base_tri (GPS L1/L2/L5, frequency_mode triple,
# 58-station DOY 2024-129 roster), cut to 0.25 h, with the SD pre-reduce EWL rows and their integer
# fix on.  The EWL pair of a GPS satellite is (L2, L5), so rows exist only where L5 is tracked.
# Calibrated once on gates/exe/x64-Release/gnss_processor_pb3base.exe (side "cal", 67 s, 31 epochs).
# Evidence the SD-EWL rows are built (the run's own report lines):
#   "Network (SD pre-reduce EWL): 3024 row(s) built over the run from 15067 (station,epoch,satellite)
#    visit(s) on an EWL-tier constellation" (sd_ewl_ctr.rows_built); skips counted by cause (1154 no-pivot,
#    38 no MW arc, 2451 immature);
#   "SD EWL integer fix, 2b fold: 1084 FIXED row(s) in 444 all-fixed whitened group(s) + 1940 float
#    row(s) (of 3024 built)": both group kinds of the group stage, so emitGroup runs fixed and float;
#   "entities as carried: station.sd_ewl_fix rows max 24" (the published integers the resolve stage reads).
# Baseline for the byte comparison of the cut: the same arm on gnss_processor_pb3.exe (side "new").
cd /c/GnssProcessor
export RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom
R=$RAPID_RUNS_ROOT; G=/d/GnssProcessorSupport/gates/g4
TRI=$R/_rap_n3b_base_tri/arm.json
mk() { local side=$1 x=$2 base=$3 h=$4; shift 4; local args=() s
  for s in "$@"; do
    if grep -qxF "${s%%=*}" $G/remove_keys.txt; then
      echo "pb3x_${side}_$x: --set ${s%%=*} is a retired key; refused" >&2; exit 1; fi
    args+=(--set "$s")
  done
  python scripts/rapid_arm.py make --base "$base" --tag pb3x_${side}_$x --hours $h "${args[@]}" | tail -1
  python $G/removed_keys_check.py $R/_rap_pb3x_${side}_$x/arm.json --strip | tail -1; }
for side in "$@"; do
  # tri: SD pre-reduce EWL rows (network.sd_ewl.enable) and the integer fix (fix_enable):
  # sd_ewl_ctr, the per-station sd_ewl_fix, the candidate, arc and group stages of the builder.
  mk $side tri $TRI 0.25 network.sd_ewl.enable=true network.sd_ewl.fix_enable=true
done
