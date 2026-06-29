"""gnssviz.eval.suites -- declarative test-suite expansion.

A *suite* is a small, purpose-built slice of config space anchored to one shared
base config. Instead of hand-listing every ``cell`` in the manifest, you declare:

* ``base``     -- which base config the cells extend (e.g. ``"ppp"``),
* ``axes``     -- named factors, each a map ``level-name -> override patch``,
* ``pinned``   -- overrides held constant across the whole suite,
* ``design``   -- which *combinations* of levels actually run,
* ``stations`` -- which stations the suite runs on.

The generator expands that into ordinary harness cells
(``{"label", "base", "overrides"}``) plus ``matrix`` rows, so the rest of
``run_test_suite.py`` is untouched. A cell's override patch is the ordered merge

    overrides = pinned  +  axis[0] level patch  +  axis[1] level patch + ...

(later patches win on key collision; axis order follows the JSON ``axes`` order).

Designs -- the lever that tames the combinatorial explosion:

* ``full``     -- the Cartesian product of all axis levels. Use for the two (or
  few) axes whose *interaction* you actually want to read. n = product(|axis|).
* ``ofat``     -- one factor at a time: a baseline (every axis at its **first**
  level = its reference) plus, for each axis, one cell per non-reference level
  with all other axes at reference. n = 1 + sum(|axis|-1) -- *linear*, not
  exponential. Use for main-effect / sensitivity sweeps over many knobs.
* ``pairwise`` -- a 2-wise covering array: every (level_i, level_j) pair across
  every axis pair appears in at least one cell, in ~tens of cells rather than
  the full product. Use when you suspect 2-way coupling across many axes.

See ``docs/test_suite.md``.
"""

from __future__ import annotations

import itertools
from typing import Any, Dict, Iterable, Iterator, List, Tuple

DESIGNS = ("full", "ofat", "pairwise")

# An axis is (name, [level_names]); levels preserve the JSON declaration order so
# the *first* level is the OFAT reference and labels read predictably.
AxisLevels = List[Tuple[str, List[str]]]


def _merge(*patches: Dict[str, Any]) -> Dict[str, Any]:
    """Ordered shallow merge of flat dot-path override dicts (later wins)."""
    out: Dict[str, Any] = {}
    for p in patches:
        if p:
            out.update(p)
    return out


def _combos_full(axis_levels: AxisLevels) -> Iterator[Dict[str, str]]:
    names = [a for a, _ in axis_levels]
    for combo in itertools.product(*[lv for _, lv in axis_levels]):
        yield dict(zip(names, combo))


def _combos_ofat(axis_levels: AxisLevels) -> Iterator[Dict[str, str]]:
    names = [a for a, _ in axis_levels]
    ref = {a: lv[0] for a, lv in axis_levels}
    seen: set = set()

    def _key(c: Dict[str, str]) -> Tuple[str, ...]:
        return tuple(c[a] for a in names)

    seen.add(_key(ref))
    yield dict(ref)
    for a, lv in axis_levels:
        for level in lv[1:]:
            c = dict(ref)
            c[a] = level
            k = _key(c)
            if k not in seen:
                seen.add(k)
                yield c


def _combos_pairwise(axis_levels: AxisLevels) -> Iterator[Dict[str, str]]:
    names = [a for a, _ in axis_levels]
    levels = {a: list(lv) for a, lv in axis_levels}
    if len(names) <= 1:
        yield from _combos_full(axis_levels)
        return

    def all_pairs_of(combo: Dict[str, str]) -> set:
        s = set()
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                ai, aj = names[i], names[j]
                if ai in combo and aj in combo:
                    s.add(((ai, combo[ai]), (aj, combo[aj])))
        return s

    uncovered: set = set()
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            ai, aj = names[i], names[j]
            for li in levels[ai]:
                for lj in levels[aj]:
                    uncovered.add(((ai, li), (aj, lj)))

    # Deterministic greedy: seed each new test from the lexicographically first
    # still-uncovered pair, then fill remaining axes with the level that covers
    # the most additional uncovered pairs. Valid (not necessarily minimal).
    while uncovered:
        (a1, l1), (a2, l2) = sorted(uncovered)[0]
        combo = {a1: l1, a2: l2}
        for a in names:
            if a in combo:
                continue
            best_level, best_gain = levels[a][0], -1
            for level in levels[a]:
                trial = dict(combo)
                trial[a] = level
                gain = len(all_pairs_of(trial) & uncovered)
                if gain > best_gain:
                    best_gain, best_level = gain, level
            combo[a] = best_level
        uncovered -= all_pairs_of(combo)
        yield combo


_DESIGN_FN = {
    "full": _combos_full,
    "ofat": _combos_ofat,
    "pairwise": _combos_pairwise,
}


def expand_suite(suite: Dict[str, Any], existing_labels: Iterable[str] = ()) -> Tuple[list, list]:
    """Expand one suite into ``(cells, matrix_rows)``.

    Raises ``ValueError`` on an unknown design, an empty/level-less axis, or a
    cell label that collides with an existing label (so silent overwrites are
    impossible).
    """
    try:
        name = suite["name"]
        base = suite["base"]
    except KeyError as exc:  # pragma: no cover - config error
        raise ValueError(f"suite missing required key {exc}") from exc

    design = suite.get("design", "full")
    if design not in DESIGNS:
        raise ValueError(f"suite '{name}': unknown design '{design}' (use one of {DESIGNS})")

    axes: Dict[str, Dict[str, dict]] = suite.get("axes", {})
    if not axes:
        raise ValueError(f"suite '{name}': no axes declared")
    axis_levels: AxisLevels = [(a, list(axes[a].keys())) for a in axes]
    for a, lv in axis_levels:
        if not lv:
            raise ValueError(f"suite '{name}': axis '{a}' has no levels")

    pinned = suite.get("pinned", {})
    tmpl = suite.get("label_template")
    seen = set(existing_labels)
    cells: list = []
    labels: list = []

    for combo in _DESIGN_FN[design](axis_levels):
        patches = [axes[a][combo[a]] for a, _ in axis_levels]
        overrides = _merge(pinned, *patches)
        if tmpl:
            label = tmpl.format(name=name, **combo)
        else:
            label = "_".join([name] + [combo[a] for a, _ in axis_levels])
        if label in seen:
            raise ValueError(
                f"suite '{name}': duplicate cell label '{label}' "
                f"(collides with another cell/suite; set a unique label_template)")
        seen.add(label)
        cells.append({
            "label": label, "base": base, "overrides": overrides,
            # metadata (ignored by the runner; enables suite-aware reporting later)
            "suite": name, "axes_levels": dict(combo),
        })
        labels.append(label)

    matrix = [{"station": s, "cells": list(labels)} for s in suite.get("stations", [])]
    return cells, matrix


def expand_manifest_suites(man: Dict[str, Any]) -> Dict[str, Any]:
    """In place: expand ``man['suites']`` into ``man['cells']`` + ``man['matrix']``.

    Explicit ``cells`` / ``matrix`` in the manifest are preserved; suite-generated
    ones are appended. Returns the same manifest for convenience. No-op if the
    manifest has no ``suites`` key.
    """
    suites = man.get("suites")
    if not suites:
        return man
    cells = man.setdefault("cells", [])
    matrix = man.setdefault("matrix", [])
    existing = {c["label"] for c in cells}
    for suite in suites:
        new_cells, new_matrix = expand_suite(suite, existing)
        cells.extend(new_cells)
        existing.update(c["label"] for c in new_cells)
        matrix.extend(new_matrix)
    return man
