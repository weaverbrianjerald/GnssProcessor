#!/bin/bash
# Timed 2 h golden arms, in the order given.
#
#   scripts/cost_attrib/abba.sh <Release exe> <label> <entry>...
#
# entry = <bed>:<variant>[:s]            one run, alone
#       | <entry>+<entry>                two runs at the same time (a concurrent pair)
#   bed      f = field golden, s = sim golden from 00:30:15 (network.start_offset_hours 0.504167)
#   variant  ge = the golden as it is (GPS + Galileo)
#            g  = the golden with processing.active_systems ["GPS"] and nothing else changed
#            on = the golden with network.sparse_station_normals true and nothing else changed
#   :s       the serial measurement configuration (one thread everywhere: parallel_stations, parallel_carry
#            and parallel_seam off, eigen_threads 1, parallel_max_threads 1, OMP_NUM_THREADS 1; normal priority)
#
# Runs one after the other (ABBA ordering is the caller's choice) are exposed to whatever else the machine does
# in between.  When the load drifts, a concurrent pair (f:ge+f:on, run twice with the order swapped) sees the
# same load for both members, so the ratio of their loop times is the load-robust reading.
#
# An arm is made with scripts/rapid_arm.py (golden copy plus the one knob), run with run_timed.ps1 (wall,
# process CPU, system load) and parsed by nc_parse.py into <arm>/nc.json.  Output:
# $RAPID_RUNS_ROOT/_rap_nc_<label>_<bed>_<variant>_<n> and one line per run in $RAPID_RUNS_ROOT/nc_runs_<label>.txt.
# Tables: nc_table.py.
#
# Environment: RAPID_RUNS_ROOT (default D:/GnssProcessorSupport/runs_nom), NC_PRIORITY_PAR (priority class of the
# parallel runs, default AboveNormal).  The exe must be a copy under a path that contains x64-Release.
HERE=$(cd "$(dirname "$0")" && pwd)
WT=${WT:-$(cd "$HERE/../.." && pwd)}
export RAPID_RUNS_ROOT=${RAPID_RUNS_ROOT:-D:/GnssProcessorSupport/runs_nom}
R=$RAPID_RUNS_ROOT
exe=$1; label=$2; shift 2
cd "$WT"

one_run() {                                   # one_run <entry> <n>
  local e=$1 n=$2 bed var mode tag base sets
  IFS=: read bed var mode <<< "$e"
  tag=nc_${label}_${bed}_${var}_${n}
  if [ "$bed" = f ]; then base=config/golden/golden_field_refound.json; sets=(); else base=config/golden/golden_sim_refound.json; sets=(--set network.start_offset_hours=0.504167); fi
  [ "$var" = g ] && sets+=(--set 'processing.active_systems=["GPS"]')
  [ "$var" = on ] && sets+=(--set network.sparse_station_normals=true)
  if [ "$mode" = s ]; then
    sets+=(--set network.parallel_stations=false --set network.parallel_carry=false --set network.parallel_seam=false
           --set processing.eigen_threads=1 --set network.parallel_max_threads=1)
    export OMP_NUM_THREADS=1; export NC_PRIORITY=Normal
  else
    unset OMP_NUM_THREADS; export NC_PRIORITY=${NC_PRIORITY_PAR:-AboveNormal}
  fi
  python scripts/rapid_arm.py make --base "$base" --tag "$tag" --hours 2.0 "${sets[@]}" | tail -1
  pwsh -NoProfile -File "$HERE/run_timed.ps1" -Exe "$exe" -Config "$R/_rap_$tag/arm.json" -RunDir "$R/_rap_$tag"
  python "$HERE/nc_parse.py" "$R/_rap_$tag" > /dev/null
  echo "$tag $(cat "$R/_rap_$tag/timing.txt")" | tee -a "$R/nc_runs_${label}.txt"
}

n=0
for e in "$@"; do
  if [[ "$e" == *+* ]]; then
    a=${e%%+*}; b=${e##*+}
    n=$((n+1)); one_run "$a" $n &
    n=$((n+1)); one_run "$b" $n &
    wait
  else
    n=$((n+1)); one_run "$e" $n
  fi
done
