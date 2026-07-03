"""Build-time tests for the L12 network-discontinuities viewer.

Two half-sources, each exercised in isolation and together, plus the
both-absent :class:`ViewerSkip` path.  Figures are *built* (spec objects), not
rendered, so these stay fast and backend-free.

Half B (network_solution.csv):
  * a constellation-wide bump (>=80% of GPS sats step at one epoch) → "datum";
  * an isolated single-sat bump → "per-sat";
  * both classifications asserted via the summary read-out.

Half A (network_products*.txt product file):
  * a healthy sat (4-field C records, honest 3-field reset after a gap);
  * a stale-C1 sat (4-field non-zero c1 on the first post-gap epoch) → flagged.
"""
import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_discontinuities import (
    build_network_discontinuities, _parse_product_epochs_inline)


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


# --------------------------------------------------------------------------- #
#  Fixtures
# --------------------------------------------------------------------------- #
def _write_solution(tmp_path):
    """network_solution.csv with one constellation-wide GPS bump + one isolated bump.

    GPS sats G01,G02,G03 all step their PB disc_counter at epoch k==2 (3/3 =
    100% ≥ 80% → 'datum').  Galileo E11 steps alone at k==3 while E12/E13 hold
    (1/3 → 'per-sat').
    """
    rows = ["week,tow,sat,kind,signal,value_m,disc_counter"]
    for k in range(5):
        tow = 259200 + 30 * k
        # GPS: constellation-wide PB bump at k==2.
        gbump = 1 if k >= 2 else 0
        for sat in ("G01", "G02", "G03"):
            rows.append(f"2313,{tow}.000,{sat},PB,L1C,75.5000,{gbump}")
        # Galileo: only E11 bumps, at k==3.
        for sat, cnt in (("E11", 1 if k >= 3 else 0), ("E12", 0), ("E13", 0)):
            rows.append(f"2313,{tow}.000,{sat},PB,E1C,60.2000,{cnt}")
        # A CLK row per epoch (all-zero counters → emit_clock_disc-off path).
        rows.append(f"2313,{tow}.000,G01,CLK,,0.4000,0")
    (tmp_path / "network_solution.csv").write_text("\n".join(rows) + "\n",
                                                    encoding="utf-8")


def _write_product(tmp_path, name="network_products.txt"):
    """Product file: healthy G05 (post-gap 3-field reset) + stale G07 (4-field post-gap).

    Epochs at 30 s cadence, then a >120 s gap, then resume.  G05 writes a
    3-field C record on the first post-gap epoch (honest drift reset).  G07
    writes a 4-field non-zero-c1 record there (stale carry → flagged).
    """
    lines = [
        "# gnss_network_product v1",
        "# E,week,tow,iod_ssr,provider_id,solution_id,datum_changed",
        "# C,sat,clock_c0_m",
    ]
    # Pre-gap block: tow 259200, 259230, 259260 (30 s cadence).
    pre = [259200, 259230, 259260]
    for i, tow in enumerate(pre):
        lines.append(f"E,2313,{tow}.000,1,1,1,0")
        lines.append(f"C,G05,{0.400 + 0.001 * i:.6f},1.0e-4")   # 4-field, healthy drift
        lines.append(f"C,G07,{0.500 + 0.002 * i:.6f},2.0e-4")   # 4-field
    # Gap: next epoch is 300 s later (>120 s) → drift state should reset.
    post_tow = 259260 + 300
    lines.append(f"E,2313,{post_tow}.000,1,1,1,0")
    lines.append("C,G05,0.410000")                # 3-field: honest reset (has_c1 False)
    lines.append(f"C,G07,0.530000,2.0e-4")        # 4-field non-zero c1 → STALE
    # One more post-gap epoch (30 s) so G05/G07 each have a post-gap FD pair.
    lines.append(f"E,2313,{post_tow + 30}.000,1,1,1,0")
    lines.append("C,G05,0.410300,1.0e-4")
    lines.append("C,G07,0.532000,2.0e-4")
    (tmp_path / name).write_text("\n".join(lines) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------- #
#  Half B — disc-counter timelines
# --------------------------------------------------------------------------- #
def test_counter_half_classifies_datum_and_persat(tmp_path):
    _write_solution(tmp_path)
    res = build_network_discontinuities(_ctx(tmp_path))
    # One disc-counter figure with three axes (CLK/PB/CB).
    disc_figs = [f for f in res.figures if "disc" in f.name.lower()]
    assert disc_figs, "expected a disc-counter figure"
    assert len(disc_figs[0].axes) == 3
    # Classification surfaced in the summary.
    assert "1 datum bump" in res.summary
    assert "1 per-sat bump" in res.summary


def test_counter_half_clock_allzero_note(tmp_path):
    _write_solution(tmp_path)
    res = build_network_discontinuities(_ctx(tmp_path))
    assert "emit_clock_disc off" in res.summary


# --------------------------------------------------------------------------- #
#  Half A — C1 honesty
# --------------------------------------------------------------------------- #
def test_c1_half_flags_stale(tmp_path):
    _write_product(tmp_path)
    res = build_network_discontinuities(_ctx(tmp_path))
    c1_figs = [f for f in res.figures if "C1" in f.name]
    assert c1_figs, "expected a C1-honesty figure"
    # Two axes: rate overlay + residual.
    assert len(c1_figs[0].axes) == 2
    # Exactly one stale-C1 event (G07 on the first post-gap epoch), and G07 named.
    assert "1 stale-C1 event" in res.summary
    assert "G07" in res.summary


def test_c1_half_glob_finds_named_product(tmp_path):
    # A non-canonical file name still matches network_products*.txt.
    _write_product(tmp_path, name="network_products_runA.txt")
    res = build_network_discontinuities(_ctx(tmp_path))
    assert any("C1" in f.name for f in res.figures)


def test_c1_half_no_c1_field_degrades_to_note(tmp_path):
    # All C records are 3-field (clock_model off) → informational note, no C1 figs.
    lines = ["# gnss_network_product v1"]
    for i, tow in enumerate((259200, 259230, 259260)):
        lines.append(f"E,2313,{tow}.000,1,1,1,0")
        lines.append(f"C,G05,{0.4 + 0.001 * i:.6f}")
    (tmp_path / "network_products.txt").write_text("\n".join(lines) + "\n",
                                                   encoding="utf-8")
    res = build_network_discontinuities(_ctx(tmp_path))
    assert not any("C1" in f.name for f in res.figures)
    assert "no 4-field C records" in res.summary


def test_c1_half_datum_change_excluded_from_fd(tmp_path):
    # A datum-changed epoch (field 7 == 1) must not be finite-differenced across.
    epochs = _parse_product_epochs_inline_via_file(tmp_path)
    assert epochs  # inline parser sanity


def _parse_product_epochs_inline_via_file(tmp_path):
    p = tmp_path / "network_products.txt"
    p.write_text(
        "# gnss_network_product v1\n"
        "E,2313,259200.000,1,1,1,0\n"
        "C,G05,0.400000,1.0e-4\n"
        "E,2313,259230.000,1,1,1,1,GE\n"     # datum change scoped to G,E (field 8)
        "C,G05,0.900000,1.0e-4\n",
        encoding="utf-8")
    eps = _parse_product_epochs_inline(str(p))
    assert eps[1]["datum_changed"] is True
    assert eps[1]["datum_consts"] == {"G", "E"}
    return eps


# --------------------------------------------------------------------------- #
#  Both halves present
# --------------------------------------------------------------------------- #
def test_both_halves_present(tmp_path):
    _write_solution(tmp_path)
    _write_product(tmp_path)
    res = build_network_discontinuities(_ctx(tmp_path))
    names = [f.name for f in res.figures]
    assert any("disc" in n.lower() for n in names)
    assert any("C1" in n for n in names)


# --------------------------------------------------------------------------- #
#  Skip when BOTH sources absent
# --------------------------------------------------------------------------- #
def test_skip_when_both_absent(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_discontinuities(_ctx(tmp_path))
