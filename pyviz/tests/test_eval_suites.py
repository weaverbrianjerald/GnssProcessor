"""Unit tests for gnssviz.eval.suites: declarative suite expansion."""
from __future__ import annotations

import itertools

import pytest

from gnssviz.eval.suites import (
    DESIGNS,
    expand_manifest_suites,
    expand_suite,
)


def _systems_dynamics_suite(design="full", **kw):
    return {
        "name": "system_ppp",
        "base": "ppp",
        "design": design,
        "stations": ["JPLM", "GCGO"],
        "pinned": {"processing.frequency_mode": "all"},
        "axes": {
            "systems": {
                "G":  {"processing.active_systems": ["GPS"]},
                "GE": {"processing.active_systems": ["GPS", "Galileo"]},
                "GREC": {"processing.active_systems": ["GPS", "Galileo", "GLONASS", "BeiDou"]},
            },
            "dynamics": {
                "static":    {"estimation.estimate_velocity": False},
                "kinematic": {"estimation.estimate_velocity": True,
                              "filter.process_noise.acceleration_psd_q": 1.0},
            },
        },
        **kw,
    }


# --------------------------------------------------------------------------- #
# full
# --------------------------------------------------------------------------- #

def test_full_is_cartesian_product():
    cells, matrix = expand_suite(_systems_dynamics_suite("full"))
    assert len(cells) == 3 * 2  # |systems| * |dynamics|
    labels = {c["label"] for c in cells}
    assert "system_ppp_G_static" in labels
    assert "system_ppp_GREC_kinematic" in labels
    # every combo present exactly once
    assert len(labels) == len(cells)


def test_full_override_merge_order_and_pinned():
    cells, _ = expand_suite(_systems_dynamics_suite("full"))
    by = {c["label"]: c for c in cells}
    gk = by["system_ppp_GREC_kinematic"]["overrides"]
    # pinned present
    assert gk["processing.frequency_mode"] == "all"
    # systems axis patch
    assert gk["processing.active_systems"] == ["GPS", "Galileo", "GLONASS", "BeiDou"]
    # dynamics axis patch (both keys)
    assert gk["estimation.estimate_velocity"] is True
    assert gk["filter.process_noise.acceleration_psd_q"] == 1.0
    # base + suite metadata
    assert by["system_ppp_G_static"]["base"] == "ppp"
    assert by["system_ppp_G_static"]["suite"] == "system_ppp"
    assert by["system_ppp_G_static"]["axes_levels"] == {"systems": "G", "dynamics": "static"}


def test_full_matrix_rows_one_per_station_with_all_labels():
    cells, matrix = expand_suite(_systems_dynamics_suite("full"))
    labels = [c["label"] for c in cells]
    assert [m["station"] for m in matrix] == ["JPLM", "GCGO"]
    for m in matrix:
        assert m["cells"] == labels


# --------------------------------------------------------------------------- #
# ofat
# --------------------------------------------------------------------------- #

def test_ofat_count_is_linear_and_baseline_first():
    cells, _ = expand_suite(_systems_dynamics_suite("ofat"))
    # 1 + (|systems|-1) + (|dynamics|-1) = 1 + 2 + 1 = 4
    assert len(cells) == 1 + (3 - 1) + (2 - 1)
    labels = [c["label"] for c in cells]
    # baseline = both axes at their first (reference) level
    assert labels[0] == "system_ppp_G_static"
    # only one axis ever differs from the reference
    for c in cells:
        diffs = sum(1 for ax, lvl in c["axes_levels"].items()
                    if lvl != {"systems": "G", "dynamics": "static"}[ax])
        assert diffs <= 1


def test_ofat_covers_each_nonref_level_once():
    cells, _ = expand_suite(_systems_dynamics_suite("ofat"))
    labels = {c["label"] for c in cells}
    assert {"system_ppp_GE_static", "system_ppp_GREC_static",
            "system_ppp_G_kinematic"} <= labels


# --------------------------------------------------------------------------- #
# pairwise
# --------------------------------------------------------------------------- #

def _all_pairs(axes):
    names = list(axes)
    pairs = set()
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            ai, aj = names[i], names[j]
            for li in axes[ai]:
                for lj in axes[aj]:
                    pairs.add(((ai, li), (aj, lj)))
    return pairs


def test_pairwise_covers_all_pairs_and_is_no_larger_than_full():
    suite = {
        "name": "px", "base": "ppp", "stations": ["JPLM"], "design": "pairwise",
        "axes": {
            "a": {"a0": {"x.a": 0}, "a1": {"x.a": 1}, "a2": {"x.a": 2}},
            "b": {"b0": {"x.b": 0}, "b1": {"x.b": 1}, "b2": {"x.b": 2}},
            "c": {"c0": {"x.c": 0}, "c1": {"x.c": 1}, "c2": {"x.c": 2}},
        },
    }
    cells, _ = expand_suite(suite)
    levels = {"a": ["a0", "a1", "a2"], "b": ["b0", "b1", "b2"], "c": ["c0", "c1", "c2"]}
    must_cover = _all_pairs(levels)
    covered = set()
    for c in cells:
        lv = c["axes_levels"]
        # canonicalize each pair by axis name so ordering matches _all_pairs
        for ai, aj in itertools.combinations(sorted(lv), 2):
            covered.add(((ai, lv[ai]), (aj, lv[aj])))
    assert must_cover <= covered                 # full 2-wise coverage
    assert len(cells) <= 3 * 3 * 3               # never worse than full
    assert len(cells) < 3 * 3 * 3                # and strictly better here


# --------------------------------------------------------------------------- #
# errors / edges
# --------------------------------------------------------------------------- #

def test_unknown_design_raises():
    with pytest.raises(ValueError, match="unknown design"):
        expand_suite(_systems_dynamics_suite("octree"))


def test_no_axes_raises():
    with pytest.raises(ValueError, match="no axes"):
        expand_suite({"name": "x", "base": "ppp", "axes": {}})


def test_duplicate_label_collision_raises():
    suite = _systems_dynamics_suite("full")
    with pytest.raises(ValueError, match="duplicate cell label"):
        expand_suite(suite, existing_labels={"system_ppp_G_static"})


def test_label_template_override():
    suite = _systems_dynamics_suite("full", label_template="{name}-{systems}-{dynamics}")
    cells, _ = expand_suite(suite)
    assert any(c["label"] == "system_ppp-GREC-kinematic" for c in cells)


# --------------------------------------------------------------------------- #
# manifest merge
# --------------------------------------------------------------------------- #

def test_expand_manifest_appends_and_preserves_explicit():
    man = {
        "cells": [{"label": "ppp_static", "base": "ppp", "overrides": {}}],
        "matrix": [{"station": "JPLM", "cells": ["ppp_static"]}],
        "suites": [_systems_dynamics_suite("ofat")],
    }
    expand_manifest_suites(man)
    labels = [c["label"] for c in man["cells"]]
    assert labels[0] == "ppp_static"            # explicit cell preserved, first
    assert "system_ppp_G_static" in labels      # suite cells appended
    # one explicit matrix row + two suite rows (JPLM, GCGO)
    assert len(man["matrix"]) == 1 + 2


def test_expand_manifest_noop_without_suites():
    man = {"cells": [{"label": "a", "base": "ppp", "overrides": {}}], "matrix": []}
    out = expand_manifest_suites(man)
    assert out is man
    assert len(man["cells"]) == 1


def test_designs_constant_exposed():
    assert set(DESIGNS) == {"full", "ofat", "pairwise"}
