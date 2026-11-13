#!/usr/bin/env bash
# Build the GnssProcessor mathematical reference.
#
#   ./build.sh           # build combined volume + all standalone PDFs
#   ./build.sh --regen   # regenerate the code-derived tables first
#                        # (done automatically when generated/ is missing)
#   ./build.sh --clean   # clean intermediates and PDFs, then build
#
# Requires a TeX distribution with latexmk + biber on PATH.
set -euo pipefail
cd "$(dirname "$0")"

gen_tables() { echo "== generating code-derived tables =="; python ../../scripts/gen_report_tables.py; }

regen=0
for arg in "$@"; do
  [ "$arg" = "--regen" ] && regen=1
done
missing=0
for f in processing_modes config_toggles slip_detectors state_keys; do
  [ -f "generated/$f.tex" ] || missing=1
done
if [ "$regen" = 1 ] || [ "$missing" = 1 ]; then gen_tables; fi

for arg in "$@"; do
  case "$arg" in
    --regen) ;;     # handled above
    --clean) echo "== cleaning =="; latexmk -C ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

echo "== building (combined volume + standalone components) =="
latexmk            # @default_files in latexmkrc builds every target
echo "== done; PDFs:"
ls -1 *.pdf
