"""Build-time tests for the ssr_consumer viewer (synthetic trace CSVs).

Exercises ``build_ssr_consumer`` against stub ``ssr_consumer_trace.csv`` files --
enough to catch schema / contract regressions -- plus the optional closed-loop
overlay and the :class:`ViewerSkip` (header-only) path.  Figures are *built*
(spec objects), not rendered, so these stay fast and backend-free.
"""
import numpy as np
import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.ssr_consumer import build_ssr_consumer

_HEADER = ("week,tow,sat,ssr_age_s,clk_applied,c0_m,ura_m,r_add_var_m2,"
           "pb_age_s,n_pb,clk_disc,pb_disc_max,cb_disc_max,pb_fresh")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _write_consumer(tmp_path, rows):
    (tmp_path / "ssr_consumer_trace.csv").write_text(
        _HEADER + "\n" + "\n".join(rows) + "\n", encoding="utf-8")


def _base_rows():
    """8 epochs x 2 sats (G10, E15).

    Epoch scheduling (k = 0..7):
      * k in {3,4}  -> a product GAP window on G10 (clk_applied=0).
      * k == 5      -> G10 pb_fresh flips 1->0 (a stale-hold flip).
      * k == 6      -> G10 clk_disc increments 0->1 (a disc reset / bump).
    """
    rows = []
    prev_disc = {"G10": 0, "E15": 0}
    prev_fresh = {"G10": 1, "E15": 1}
    for k in range(8):
        tow = 259200 + 30 * k
        for sat in ("G10", "E15"):
            age = 5.0 + 2.0 * k
            clk_applied = 1
            fresh = prev_fresh[sat]
            disc = prev_disc[sat]
            r_add = 0.02 + 0.001 * k

            if sat == "G10":
                if k in (3, 4):
                    clk_applied = 0          # product gap
                if k == 5:
                    fresh = 0                # stale-hold flip (1 -> 0)
                if k == 6:
                    disc = 1                 # disc reset (0 -> 1)

            rows.append(
                f"2313,{tow}.000,{sat},{age:.1f},{clk_applied},"
                f"12.3,0.10,{r_add:.4f},4.0,2,{disc},{disc},0,{fresh}")
            prev_fresh[sat] = fresh
            prev_disc[sat] = disc
    return rows


def test_build_basic_figures(tmp_path):
    _write_consumer(tmp_path, _base_rows())
    res = build_ssr_consumer(_ctx(tmp_path))
    # per-epoch age line + >=1 age strip + URA tile fig + events fig.
    assert len(res.figures) >= 3
    names = [f.name for f in res.figures]
    assert "SSR age (per-epoch)" in names
    assert "Consumer SSR events" in names
    # No plotting library imported: purely declarative specs.
    assert "=== Consumer SSR Application ===" in res.summary


def test_event_counts_in_summary(tmp_path):
    _write_consumer(tmp_path, _base_rows())
    res = build_ssr_consumer(_ctx(tmp_path))
    s = res.summary
    # 2 gap epochs (k=3,4), 1 stale flip (k=5), 1 disc reset (k=6 -> clk_disc AND
    # pb_disc_max both increment on the same epoch => 2 counted increments).
    assert "Product gaps   : 2 epoch(s)" in s
    assert "Stale-hold flip: 1" in s
    assert "Disc resets    : 2" in s


def test_worst_sat_named_in_ura(tmp_path):
    # E15 gets a much larger R-inflation than G10 -> must be named worst-sat.
    rows = []
    for k in range(4):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,G10,5.0,1,12.3,0.1,0.0100,4.0,2,0,0,0,1")
        rows.append(f"2313,{tow}.000,E15,5.0,1,12.3,0.1,0.2500,4.0,2,0,0,0,1")
    _write_consumer(tmp_path, rows)
    res = build_ssr_consumer(_ctx(tmp_path))
    assert "worst-sat E15" in res.summary


def test_blank_optional_columns_are_tolerated(tmp_path):
    # c0_m / ura_m / clk_disc / pb_disc_max / cb_disc_max blank -> NaN, no crash.
    rows = []
    for k in range(3):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,G10,5.0,1,,,0.0100,4.0,2,,,,1")
    _write_consumer(tmp_path, rows)
    res = build_ssr_consumer(_ctx(tmp_path))
    assert res.summary  # built without error
    # blank disc columns never count as increments.
    assert "Disc resets    : 0" in res.summary


def test_closed_loop_overlay_causality(tmp_path):
    """With a closed-loop trace, 3D error INSIDE gap epochs vs OUTSIDE is reported.

    Construct the closed loop so the error is high exactly on the gap epochs
    (k=3,4) and low elsewhere -> median-inside must exceed median-outside.
    """
    _write_consumer(tmp_path, _base_rows())
    cl_rows = ["week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,"
               "pos_err_3d_m,east_m,north_m,up_m"]
    for k in range(8):
        tow = 259200 + 30 * k
        err = 3.0 if k in (3, 4) else 0.05
        cl_rows.append(f"2313,{tow}.000,10,1,1,8,10,3.5,{err:.3f},0.01,0.01,0.02")
    (tmp_path / "closed_loop_trace.csv").write_text(
        "\n".join(cl_rows) + "\n", encoding="utf-8")

    res = build_ssr_consumer(_ctx(tmp_path))
    s = res.summary
    assert "Closed-loop causality" in s
    assert "INSIDE  gap epochs (n=2)" in s
    assert "OUTSIDE gap epochs (n=6)" in s
    # The events figure right-axis went log for the overlay.
    ev = next(f for f in res.figures if f.name == "Consumer SSR events")
    assert ev.axes[0].yscale == "log"
    # A gap-shading Band is present on the right axis.
    from gnssviz.figures import Band
    assert any(isinstance(t, Band) for t in ev.axes[0].traces)
    # Causality direction: inside error >> outside error.
    inside = _num_after(s, "INSIDE  gap epochs (n=2) : ")
    outside = _num_after(s, "OUTSIDE gap epochs (n=6) : ")
    assert inside > outside
    assert np.isclose(inside, 3.0, atol=1e-6)
    assert np.isclose(outside, 0.05, atol=1e-6)


def test_epoch_set_mismatch_tolerated(tmp_path):
    # Closed-loop has an extra epoch (k=8) not in the consumer trace -> counted
    # OUTSIDE (only consumer-known gaps define shaded windows); must not crash.
    _write_consumer(tmp_path, _base_rows())
    cl_rows = ["week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,"
               "pos_err_3d_m,east_m,north_m,up_m"]
    for k in range(9):                      # one epoch beyond the consumer's 8
        tow = 259200 + 30 * k
        err = 3.0 if k in (3, 4) else 0.05
        cl_rows.append(f"2313,{tow}.000,10,1,1,8,10,3.5,{err:.3f},0.01,0.01,0.02")
    (tmp_path / "closed_loop_trace.csv").write_text(
        "\n".join(cl_rows) + "\n", encoding="utf-8")

    res = build_ssr_consumer(_ctx(tmp_path))
    # 2 inside (k=3,4) still; the extra k=8 lands OUTSIDE -> 7 outside.
    assert "INSIDE  gap epochs (n=2)" in res.summary
    assert "OUTSIDE gap epochs (n=7)" in res.summary


def test_header_only_skips(tmp_path):
    (tmp_path / "ssr_consumer_trace.csv").write_text(_HEADER + "\n",
                                                     encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_ssr_consumer(_ctx(tmp_path))


def test_viewer_registration_metadata():
    from gnssviz.viewers.ssr_consumer import viewer
    assert viewer.name == "ssr_consumer"
    assert viewer.required_csvs == ("ssr_consumer_trace.csv",)


def _num_after(text, marker):
    """Parse the float immediately following ``marker`` in ``text``."""
    i = text.index(marker) + len(marker)
    frag = text[i:].splitlines()[0].strip()
    return float(frag)
