#!/usr/bin/env bash
# RTB-7(d) RS-card harness: run ONE warm-resume arm under the HB-2 protocol.
#
#   run_warm_resume_arm.sh <config.json> <logfile> <expect: none|warm|warm+carry>
#
# Enforces the §0.4 protocol hazard fix:
#   1. re-seed the working snapshot from the IMMUTABLE converge artifact before
#      every resume launch (a resume run overwrites the file it read, so a second
#      launch against the un-re-seeded file sees a snapshot from its own future,
#      warmResumeAllowed rejects the non-positive age, and the run SILENTLY
#      full-cold-starts with only a warn);
#   2. assert the expected resume verdict in the run log afterwards, so an arm can
#      never be graded as warm when it actually cold-started.
# Release-only, renamed exe, name-scoped zombie pre-kill (CLAUDE.md standing rules).
set -u
CFG="$1"; LOG="$2"; EXPECT="${3:-none}"
REL="C:/GnssProcessor/build/x64-Release"
EXE="$REL/gnss_processor_rs_run.exe"
VO="C:/GnssProcessor/val_out"
# Override per matrix via RS_CONV / RS_WORK (e.g. the 305-station arms).
CONV="${RS_CONV:-$VO/snap_wconv.txt}"   # immutable converge artifact
WORK="${RS_WORK:-$VO/snap_wwork.txt}"   # throwaway the resume reads AND overwrites

# name-scoped zombie pre-kill
tasklist 2>/dev/null | grep -qi gnss_processor_rs_run && taskkill //F //IM gnss_processor_rs_run.exe 2>/dev/null

# HB-2 step 1: re-seed (resume arms only)
if [ "$EXPECT" != "none" ]; then
  [ -f "$CONV" ] || { echo "FATAL: converge artifact $CONV missing"; exit 2; }
  cp -f "$CONV" "$WORK" || { echo "FATAL: re-seed failed"; exit 2; }
fi

( cd "$REL" && "$EXE" "$CFG" > "$LOG" 2>&1 ); RC=$?

# HB-2 step 2: assert the verdict
# NB: `grep -c` already prints 0 and exits 1 on no-match — a `|| echo 0` fallback
# would emit a SECOND 0 and break the integer tests below.
WARM=$(grep -c 'WARM RESUME from' "$LOG" 2>/dev/null); WARM=${WARM:-0}
BF=$(grep -c 'BRANCH-FAITHFUL — restored invariant DD carry' "$LOG" 2>/dev/null); BF=${BF:-0}
COLD=$(grep -c 'COLD START' "$LOG" 2>/dev/null); COLD=${COLD:-0}
case "$EXPECT" in
  none)       V="n/a";        OK=$([ "$RC" -eq 0 ] && echo 1 || echo 0) ;;
  warm)       V="warm";       OK=$([ "$WARM" -ge 1 ] && [ "$COLD" -eq 0 ] && echo 1 || echo 0) ;;
  warm+carry) V="warm+carry"; OK=$([ "$WARM" -ge 1 ] && [ "$BF" -ge 1 ] && [ "$COLD" -eq 0 ] && echo 1 || echo 0) ;;
esac
printf "%-42s rc=%s warm=%s bf=%s cold=%s expect=%-10s %s\n" \
  "$(basename "$CFG")" "$RC" "$WARM" "$BF" "$COLD" "$V" \
  "$([ "$OK" -eq 1 ] && echo PASS || echo '*** ASSERT FAILED ***')"
[ "$RC" -eq 0 ] && [ "$OK" -eq 1 ]
