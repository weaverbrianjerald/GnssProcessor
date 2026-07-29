"""Build-time tests for the N-run comparators (synthetic runs).

Three synthetic run dirs under ``tmp_path`` are populated with schema-faithful
stub traces.  Every assertion keys off a *constructed* effect -- a leg that
withholds epochs the reference emitted, a tier whose RATE is 5x the reference's,
a station whose keyed pool jumps only at the onset epoch -- so the tests pin the
comparator's answer, not merely that a figure was produced.  Comparators are
exercised through their build functions directly (spec objects, not rendered), so
these stay fast and backend-free.
"""
import numpy as np
import pytest

from gnssviz.figures import Band, Line, Raster
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_run_compare import (
    RUN_COMPARATORS, RunSet, auto_label, compare_dispositions, compare_events,
    compare_keyed_world, compare_scorecard, compare_tracks, run_multi_comparators)

_WEEK = 2313
_TOW0 = 259200.0
_STEP = 30.0

# Two real IGS ECEFs so the roster join yields sane lat/lon (JPL + Caltech).
_ECEF = {
    "JPLM00USA": (-2493304.9886, -4655214.7866, 3565497.6745),
    "CIT100USA": (-2493296.0000, -4655200.0000, 3565500.0000),
    "MBAR00UGA": (5482951.0692, 3260442.9625, -66519.5412),
}


def _write(path, rows):
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")


def _epoch_status(dir_, *, n=16, withhold_from=None, disposition="sh_gate_withheld",
                  ceiling=6.0, sigma0=1.0, pf_scale=1.0, trailing_junk=False):
    """network_epoch_status.csv; rows from ``withhold_from`` carry ``disposition``."""
    hdr = ("week,tow,disposition,sigma0,n_adapt,n_present,n_clock_sats,"
           "clock_median_m,clock_std_m,worst_sat,worst_dev_m,n_refloat_sats,"
           "n_retro_applied,datum_changed,sigma0_trigger,withhold_ceiling,"
           "n_arc_refloat,sigma0_pf,sigma0_pf_code,sigma0_pf_phase,n_pf_rows")
    rows = [hdr]
    for k in range(n):
        tow = _TOW0 + _STEP * k
        withheld = withhold_from is not None and k >= withhold_from
        disp = disposition if withheld else "emitted"
        pf = pf_scale * (10.0 + k)
        rows.append(f"{_WEEK},{tow:.3f},{disp},{sigma0:.4f},0,3,18,-1.2,0.05,G10,"
                    f"0.20,0,0,0,3.0000,{ceiling:.4f},0,{pf:.4f},{pf:.4f},"
                    f"0.0003,240")
    text = "\n".join(rows) + "\n"
    if trailing_junk:
        text += "2"          # a run still writing: partial final line
    (dir_ / "network_epoch_status.csv").write_text(text, encoding="utf-8")


def _events(dir_, *, n=16, per_epoch=None):
    """network_events.csv; ``per_epoch`` maps tier -> events emitted every epoch."""
    per_epoch = per_epoch or {"T3_raim": 1}
    rows = ["week,tow,tier,event,sat,station,statistic,threshold,action,detail"]
    for k in range(n):
        tow = _TOW0 + _STEP * k
        for tier, count in per_epoch.items():
            for j in range(count):
                rows.append(f"{_WEEK},{tow:.3f},{tier},{tier.lower()}_evt,"
                            f"G{j + 1:02d},,1.5,3.0,refloat,detail")
    # A structural frame event the tracks figure draws a line for.
    rows.append(f"{_WEEK},{_TOW0 + _STEP * (n // 2):.3f},CB_EXC,cb_excursion_reinit,"
                f"G01,,172.3,50.0,reinit,max=1001")
    _write(dir_ / "network_events.csv", rows)


def _sh_health(dir_, *, n=16, a00=25.0, oob_from=None):
    hdr = ("week,tow,a00_tecu,a00_std_tecu,rms_all_tecu,slant_resid_mean_m,"
           "slant_resid_rms_m,slant_resid_max_m,n_los,n_los_gt3sigma,n_stations,"
           "n_stations_gt3sigma,a00_in_band,rms_deg0_tecu,kaula_ratio_deg0")
    rows = [hdr]
    for k in range(n):
        tow = _TOW0 + _STEP * k
        oob = oob_from is not None and k >= oob_from
        val = (a00 - 900.0) if oob else a00
        rows.append(f"{_WEEK},{tow:.3f},{val:.3f},0.5,54.2,210.0,520.5,12014.3,"
                    f"1874,1835,303,303,{0 if oob else 1},219.3,7.3")
    _write(dir_ / "network_vtec_sh_health.csv", rows)


def _pf_keyed(dir_, *, n=16, onset=12, spike_station="JPLM00USA", spike=50.0,
              extra_key=None):
    """network_pf_keyed.csv: flat pools except a spike on one station at ``onset``.

    ``extra_key`` adds a STA key with no roster counterpart (the unjoined-key path).
    """
    rows = ["week,tow,kind,key,n_code,s0_code,rms_code,max_code,"
            "n_phase,s0_phase,rms_phase,max_phase"]
    stations = ["JPLM00USA", "CIT100USA"] + ([extra_key] if extra_key else [])
    for k in range(n):
        tow = _TOW0 + _STEP * k
        for i, sta in enumerate(stations):
            base = 10.0 + i          # per-station level, stable over the run
            # A tiny alternating wobble gives every pool a non-zero MAD, so the
            # robust-z denominator is real rather than the sigma<=0 fallback.
            val = base + (0.5 if k % 2 else -0.5)
            if sta == spike_station and k == onset:
                val = base + spike
            rows.append(f"{_WEEK},{tow:.3f},STA,{sta},40,{val:.4f},{val:.4f},"
                        f"{val:.4f},38,1.0,1.0,1.0")
        rows.append(f"{_WEEK},{tow:.3f},SAT,G01,20,{5.0 + (0.2 if k % 2 else -0.2):.4f},"
                    f"5.0,9.0,18,1.0,1.0,1.0")
    _write(dir_ / "network_pf_keyed.csv", rows)


def _roster(dir_, *, datum="MBAR00UGA"):
    rows = ["station,role,ecef_x,ecef_y,ecef_z"]
    for name, (x, y, z) in _ECEF.items():
        role = "datum" if name == datum else "network"
        rows.append(f"{name},{role},{x:.4f},{y:.4f},{z:.4f}")
    _write(dir_ / "network_stations.csv", rows)


def _leg(tmp_path, name):
    d = tmp_path / name
    d.mkdir()
    return d


def _runs(tmp_path, *names):
    return RunSet.from_dirs([str(tmp_path / n) for n in names], list(names))


# --------------------------------------------------------------------------- #
#  RunSet plumbing
# --------------------------------------------------------------------------- #
def test_auto_label_strips_logs_subdir():
    assert auto_label(r"C:\runs\day129_guards\leg01\logs") == "leg01"
    assert auto_label(r"C:\runs\day129_guards\leg01") == "leg01"


def test_runset_disambiguates_duplicate_auto_labels(tmp_path):
    for parent in ("a", "b"):
        (tmp_path / parent).mkdir()
        (tmp_path / parent / "logs").mkdir()
    runs = RunSet.from_dirs([str(tmp_path / "a" / "logs"), str(tmp_path / "a" / "logs")])
    assert runs.labels == ["a", "a#2"]           # never two legs with one name


def test_runset_reference_is_leg_zero_and_colors_differ(tmp_path):
    for n in ("A", "B"):
        _leg(tmp_path, n)
    runs = _runs(tmp_path, "A", "B")
    assert runs.reference.label == "A"
    assert runs.legs[0].color != runs.legs[1].color


def test_require_names_the_legs_that_lack_the_trace(tmp_path):
    a, b = _leg(tmp_path, "A"), _leg(tmp_path, "B")
    _epoch_status(a)
    runs = _runs(tmp_path, "A", "B")
    with pytest.raises(ViewerSkip) as exc:
        runs.require("network_epoch_status.csv", minimum=2)
    assert "B" in str(exc.value)


# --------------------------------------------------------------------------- #
#  (a) dispositions
# --------------------------------------------------------------------------- #
def test_dispositions_detect_constructed_divergence(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "guarded")
    _epoch_status(a, n=16)                      # all emitted
    _epoch_status(b, n=16, withhold_from=10)    # 6 withheld
    res = compare_dispositions(_runs(tmp_path, "ref", "guarded"))

    raster = next(t for t in res.figures[0].axes[0].traces if isinstance(t, Raster))
    assert raster.grid.shape[0] == 2            # one row per leg
    assert list(raster.row_labels) == ["ref", "guarded"]
    assert "16 shared, 10 agree" in res.summary
    assert "6 differ" in res.summary
    assert "ref=emitted  ->  guarded=sh_gate_withheld" in res.summary
    # The cumulative-emitted curve stops climbing where the withholds start.
    cum = [t for t in res.figures[0].axes[1].traces if isinstance(t, Line)][1]
    assert np.asarray(cum.y, float)[-1] == 10.0


def test_dispositions_column_axis_is_a_regular_time_grid(tmp_path):
    """Disjoint legs must not be painted as one continuous block.

    A Raster spreads columns uniformly across its time span, so the columns have
    to be a regular time grid; with the union-of-distinct-epochs axis a seed and
    its later resume leg render adjacent and every tick label is wrong.
    """
    a, b = _leg(tmp_path, "early"), _leg(tmp_path, "late")
    _epoch_status(a, n=10)
    # 'late' starts 100 epochs after 'early' ends -> a real gap in the middle.
    hdr_rows = (tmp_path / "early" / "network_epoch_status.csv").read_text().splitlines()
    shifted = [hdr_rows[0]]
    for r in hdr_rows[1:]:
        f = r.split(",")
        f[1] = f"{float(f[1]) + 110 * _STEP:.3f}"
        shifted.append(",".join(f))
    _write(b / "network_epoch_status.csv", shifted)

    res = compare_dispositions(_runs(tmp_path, "early", "late"))
    raster = next(t for t in res.figures[0].axes[0].traces if isinstance(t, Raster))
    assert raster.grid.shape[1] == 120          # 110-epoch offset + 10 epochs
    # The gap columns are painted on NEITHER row.
    assert np.isnan(raster.grid[:, 50]).all()
    assert "regular 30 s grid" in res.summary
    assert "only-early 10, only-late 10" in res.summary


def test_dispositions_report_dropped_partial_rows(tmp_path):
    a = _leg(tmp_path, "live")
    _epoch_status(a, n=8, trailing_junk=True)   # a run still writing its trace
    res = compare_dispositions(_runs(tmp_path, "live"))
    # 8 usable epochs, and the malformed row is counted, not silently swallowed.
    assert "live" in res.summary
    badrows = [ln for ln in res.summary.splitlines() if ln.startswith("live")]
    assert badrows and badrows[0].split()[-1] == "1"


# --------------------------------------------------------------------------- #
#  (b) tracks
# --------------------------------------------------------------------------- #
def test_tracks_shade_each_legs_withheld_epochs(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "guarded")
    _epoch_status(a, n=16)
    _epoch_status(b, n=16, withhold_from=10)
    res = compare_tracks(_runs(tmp_path, "ref", "guarded"))

    ax0 = res.figures[0].axes[0]
    bands = [t for t in ax0.traces if isinstance(t, Band)]
    assert len(bands) == 1                       # only the leg that withheld
    assert "guarded withheld (6)" in bands[0].label
    # The shade spans exactly the withheld epochs.
    assert int(np.isfinite(np.asarray(bands[0].lo, float)).sum()) == 6


def test_tracks_call_out_config_echo_divergence(tmp_path):
    """Two legs graded against different sigma0 ceilings is a grading defect."""
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "arm")
    _epoch_status(a, n=12, ceiling=6.0)
    _epoch_status(b, n=12, ceiling=0.0)          # ceiling disabled on this leg
    res = compare_tracks(_runs(tmp_path, "ref", "arm"))
    assert "CONFIG ECHO DIVERGENCE" in res.summary
    assert "ceil 6.000" in res.summary and "ceil 0.000" in res.summary


def test_tracks_a00_axis_shades_out_of_band(tmp_path):
    a = _leg(tmp_path, "leg")
    _epoch_status(a, n=16)
    _sh_health(a, n=16, oob_from=8)
    res = compare_tracks(_runs(tmp_path, "leg"))
    a00_ax = res.figures[0].axes[-1]
    assert "a₀₀" in a00_ax.title
    band = next(t for t in a00_ax.traces if isinstance(t, Band))
    assert "out-of-band (8)" in band.label
    assert "out-of-band" in res.summary


def test_tracks_gate_levels_are_inside_the_y_limit(tmp_path):
    """A reference line parked outside the limits is invisible AND collapses layout."""
    a = _leg(tmp_path, "leg")
    _epoch_status(a, n=12, sigma0=1.0, ceiling=6.0)    # sigma0 far below the gate
    ax0 = compare_tracks(_runs(tmp_path, "leg")).figures[0].axes[0]
    assert ax0.ylim is not None and ax0.ylim[1] >= 6.0


def test_tracks_skip_without_any_input(tmp_path):
    _leg(tmp_path, "empty")
    with pytest.raises(ViewerSkip):
        compare_tracks(_runs(tmp_path, "empty"))


# --------------------------------------------------------------------------- #
#  (c) events -- RATES, not raw counts
# --------------------------------------------------------------------------- #
def test_events_ratio_is_rate_normalized_not_count_based(tmp_path):
    """Twice the epochs at the same rate must read 1.00x, not 2x."""
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "long")
    _epoch_status(a, n=10)
    _epoch_status(b, n=20)
    _events(a, n=10, per_epoch={"T3_raim": 1})
    _events(b, n=20, per_epoch={"T3_raim": 1})   # 2x the COUNT, same rate
    res = compare_events(_runs(tmp_path, "ref", "long"))
    raim = next(ln for ln in res.summary.splitlines() if ln.startswith("T3_raim"))
    assert "1.00x" in raim
    assert "20" in raim and "10" in raim          # both counts still shown
    assert "every tier within 3x" in res.summary


def test_events_call_out_a_constructed_5x_rate(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "arm")
    _epoch_status(a, n=10)
    _epoch_status(b, n=10)
    _events(a, n=10, per_epoch={"T3_raim": 1})
    _events(b, n=10, per_epoch={"T3_raim": 5})
    res = compare_events(_runs(tmp_path, "ref", "arm"))
    assert "5.00x <<" in res.summary
    assert "Called out" in res.summary and "T3_raim/arm" in res.summary
    # CB_EXC exists only via the frame event on both legs -> not "new".
    assert "CB_EXC/arm (new)" not in res.summary


def test_events_flag_a_tier_new_to_a_leg(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "arm")
    _epoch_status(a, n=10)
    _epoch_status(b, n=10)
    _events(a, n=10, per_epoch={"T3_raim": 1})
    _events(b, n=10, per_epoch={"T3_raim": 1, "SH_GATE": 2})
    res = compare_events(_runs(tmp_path, "ref", "arm"))
    assert "new <<" in res.summary
    assert "SH_GATE/arm (new)" in res.summary


# --------------------------------------------------------------------------- #
#  (d) keyed worldmap
# --------------------------------------------------------------------------- #
def test_keyed_ranks_the_constructed_excursion_first(tmp_path):
    a = _leg(tmp_path, "leg")
    _pf_keyed(a, n=16, onset=12, spike_station="JPLM00USA", spike=50.0)
    _roster(a)
    res = compare_keyed_world(_runs(tmp_path, "leg"), onset_tow=_TOW0 + 12 * _STEP)
    top = [ln for ln in res.summary.splitlines() if "STA" in ln and "z=" in ln]
    assert top, res.summary
    assert "JPLM00USA" in top[0]                  # the spiked station leads
    assert "+" in top[0].split("z=")[1][:10]      # and its z is positive
    assert "joined 2; unjoined 0" in res.summary


def test_keyed_auto_onset_picks_the_worst_epoch(tmp_path):
    a = _leg(tmp_path, "leg")
    _pf_keyed(a, n=16, onset=13, spike=80.0)
    _roster(a)
    res = compare_keyed_world(_runs(tmp_path, "leg"))
    assert "auto: worst cross-STA median s0_code" in res.summary
    assert f"tow {_TOW0 + 13 * _STEP:.0f}" in res.summary


def test_keyed_counts_unjoined_keys_rather_than_dropping_them(tmp_path):
    """A pool with no roster row must be reported -- a silent drop fakes a clean map."""
    a = _leg(tmp_path, "leg")
    _pf_keyed(a, n=16, onset=12, extra_key="ZZZZ00XXX")
    _roster(a)
    res = compare_keyed_world(_runs(tmp_path, "leg"), onset_tow=_TOW0 + 12 * _STEP)
    assert "unjoined 1" in res.summary and "ZZZZ00XXX" in res.summary


def test_keyed_map_marks_roster_stations_with_no_pool(tmp_path):
    a = _leg(tmp_path, "leg")
    _pf_keyed(a, n=16, onset=12)                 # pools for 2 of the 3 roster rows
    _roster(a)
    res = compare_keyed_world(_runs(tmp_path, "leg"), onset_tow=_TOW0 + 12 * _STEP)
    ax = res.figures[0].axes[0]
    assert ax.projection == "geo"
    labels = [getattr(t, "label", "") or "" for t in ax.traces]
    assert any("no keyed pool (1)" in ln for ln in labels)   # the datum has no pool
    assert any("keyed z (2)" in ln for ln in labels)


def test_keyed_skips_without_a_station_roster(tmp_path):
    a = _leg(tmp_path, "leg")
    _pf_keyed(a, n=16, onset=12)                 # keyed pools but no roster/config
    with pytest.raises(ViewerSkip):
        compare_keyed_world(_runs(tmp_path, "leg"))


def test_keyed_reports_a_leg_with_no_epoch_near_the_onset(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "shifted")
    _pf_keyed(a, n=16, onset=12)
    _roster(a)
    rows = (tmp_path / "ref" / "network_pf_keyed.csv").read_text().splitlines()
    out = [rows[0]]
    for r in rows[1:]:
        f = r.split(",")
        f[1] = f"{float(f[1]) + 500 * _STEP:.3f}"     # a different window entirely
        out.append(",".join(f))
    _write(b / "network_pf_keyed.csv", out)
    _roster(b)
    res = compare_keyed_world(_runs(tmp_path, "ref", "shifted"),
                              onset_tow=_TOW0 + 12 * _STEP)
    assert "not mapped" in res.summary
    assert len(res.figures[0].axes) == 1              # only the leg that had data


# --------------------------------------------------------------------------- #
#  (e) scorecard + orchestration
# --------------------------------------------------------------------------- #
def test_scorecard_rolls_up_and_records_skips(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "arm")
    _epoch_status(a, n=12)
    _epoch_status(b, n=12, withhold_from=8)
    res = compare_scorecard(_runs(tmp_path, "ref", "arm"))
    assert "N-Run Scorecard: ref vs arm" in res.summary
    assert "4 differ" in res.summary                  # the disposition roll-up
    assert "skipped" in res.summary                   # events/keyed have no input
    assert "epoch_status" in res.summary              # the per-leg trace inventory


def test_run_multi_comparators_mixed(tmp_path):
    a, b = _leg(tmp_path, "ref"), _leg(tmp_path, "arm")
    _epoch_status(a, n=12)
    _epoch_status(b, n=12, withhold_from=8)
    _events(a, n=12)                                  # events only on the reference
    out = run_multi_comparators(_runs(tmp_path, "ref", "arm"))
    ran = {c.name for c, _ in out.ran}
    skipped = {n for n, _ in out.skipped}
    assert {"disposition", "tracks", "scorecard"} <= ran
    assert "keyed" in skipped                         # no pf_keyed anywhere
    assert not out.failed


def test_run_multi_comparators_only_subset(tmp_path):
    a = _leg(tmp_path, "ref")
    _epoch_status(a, n=12)
    out = run_multi_comparators(_runs(tmp_path, "ref"), only=["disposition"])
    assert {c.name for c, _ in out.ran} == {"disposition"}


def test_run_comparators_are_not_in_the_single_ctx_registry():
    # These take a RunSet; the single-ctx driver would call them with a ctx.
    from gnssviz.viewers import REGISTRY
    reg_names = {v.name for v in REGISTRY}
    for c in RUN_COMPARATORS:
        assert c.name not in reg_names


def test_module_is_not_wired_into_the_viewer_registry_module():
    """It stays out of ``viewers/__init__`` on purpose (wrong call signature).

    Asserted against the file's SOURCE: importing this test module already binds
    the submodule as an attribute of the package, so ``hasattr`` would pass
    whether or not ``__init__`` ever mentions it.
    """
    import gnssviz.viewers as pkg
    src = open(pkg.__file__, encoding="utf-8").read()
    assert "network_run_compare" not in src
