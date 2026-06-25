#!/usr/bin/env bash
# Build the GnssProcessor mathematical reference.
#
#   ./build.sh           # build combined volume + all standalone PDFs
#   ./build.sh --regen   # regenerate the code-derived tables first
#   ./build.sh --clean   # clean intermediates and PDFs, then build
#
# Requires a TeX distribution with latexmk + biber on PATH.
set -euo pipefail
cd "$(dirname "$0")"

for arg in "$@"; do
  case "$arg" in
    --regen) echo "== regenerating code-derived tables =="; python ../../scripts/gen_report_tables.py ;;
    --clean) echo "== cleaning =="; latexmk -C ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

echo "== building (combined volume + standalone components) =="
latexmk            # @default_files in latexmkrc builds every target
echo "== done; PDFs:"
ls -1 *.pdf
