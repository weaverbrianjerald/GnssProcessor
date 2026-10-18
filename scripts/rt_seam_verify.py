#!/usr/bin/env python3
"""Retired: the relocation verifier of the RealTimeEngine seam decomposition.

    python scripts/rt_seam_verify.py        prints this note and exits 2

What it was.  The cut of 2026-08-01 moved 30 blocks of ``RealTimeEngine::processEpoch`` and
two other functions into seam functions.  For each seam this script took the relocated body out
of its new .cpp by brace matching, applied the inverse of the declared substitutions and
required every code line to equal the recorded line range of a frozen copy of the pre-cut
source (``runs/rtseam_goldens/pre/src/RealTimeEngine.cpp.frozen``, sha256
9fb3dc00883fbb0e37fbc35ecbca2e7e466ffe098bf212ef3e93e6c1c16944b7) character for character.
At the cut it passed 30 of 30, which with the four-arm byte-identity A/B
(``rt_seam_gate.ps1``, ``rt_seam_compare.py``) certified a pure move.
``docs/rt_seam_decomposition.md`` is the record.

Why it is retired (2026-10-17).  A relocation check ends when the relocation does.  Eight of
the 30 bodies have been extended since by later cards (``reactToGenerationChange``,
``resetOnPhaseBiasDisc``, ``reinflateOnCodeBiasDisc``, ``reinflateOnClockDisc``,
``detectAndRepairSlips``, ``resolveAmbiguities``, ``initStateManagerAndApriori``,
``initAmbiguityResolver``): lines were added for event recorders, new reactions and new
options, and in four of them existing lines were reshaped around the additions (no frozen line
was dropped outright).  Against the pre-cut source a check cannot tell such an edit from a
relocation error, so it reports 8 FAILED on every run and gates nothing.  Re-basing it on the
current bodies would turn it into a change detector that duplicates ``git diff`` and has to be
regenerated at every edit of the real-time service queue.  Its input was also kept only on the
D: drive, so it could not run from a clean clone.  An edit to a seam body is gated like any
other code: the unit suite, the same-build byte-identity comparison (``rt_seam_gate.ps1``,
``rt_seam_compare.py``) and, for a reaction the arms do not drive, a test of its own (see
"What gate 2 does NOT certify" in ``docs/rt_seam_decomposition.md``).

What is kept.  ``docs/rt_seam_manifest.json`` and ``rt_seamcut.py`` (the line map of the cut and
its generator) stay as the record; do not re-run the generator, it rewrites the seam files from
the frozen source and would undo every edit made since.  The C++ comment lexer this script
carried now lives in ``scripts/cpp_lex.py``; ``strip_comments`` and ``_CPP`` are re-exported
below only for the verifiers pinned to earlier commits, which import them from this name.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lex import _CPP, strip_comments  # noqa: E402,F401  (re-exported for the older verifiers)


def main():
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
