#!/usr/bin/env python3
"""Validate sif_propagate.py against the GnssProcessor C++ engine and unit tests.

Run from anywhere; it locates the bundled SIF fixtures the same way the tool does.

Layers of check:
  1. ORACLE  - full resolved ECEF + propagated 3x3 covariance for 12 (station,
               epoch) cases, captured from the C++ resolveStationCoordinate itself
               (tests/test_station_info.cpp temp oracle). Exercises periodic (CIT),
               post-seismic (ANTC, 29 to 69 mm), multi-occupation + zero-cross-cov
               (AB02 @ 2024.36), and an epoch before the first periodic window
               (AMC4 @ 2018.5: the first block's seasonal term continued).
  2. ANCHORS - the exact numbers asserted in tests/test_station_info.cpp
               (parsing / unit normalisation / PSD & periodic coefficients).
  3. SELFCHK - internal identities (find() keys, dt=0 -> Cxx, covariance symmetry,
               ENU<->ECEF round-trip, time conversions).
  4. MODES   - 'covering' vs 'latest' occupation selection (CORV00USA datasheet).
  5. LOOKUP  - synthetic SIF: exact ID / DOMES unconditional, 4-char prefix
               candidate vetted by distance (<= 10 km) or by a bare 4-char key;
               the candidate reference-position cascade.
  6. EPOCH   - synthetic SIF: per-occupation REFERENCE_EPOCH in the linear term
               and the covariance; METADATA fallback; periodic phase unchanged.
  7. EXTRAP  - synthetic SIF: extrapolation flags, gap tolerance (segment side
               only), window boundaries, equipment-change rules (record-only
               re-log, firmware-only, before-first-segment), 'latest' mode.
  8. CLI     - stderr warnings and the appended output columns.
  9. REAL    - the defects on the shipped SIFs (GAMB00PYF, STHL00GBR, AZAZ00USA,
               NIUM00NIU, CHUR00CAN) and the 8 stations of the 2024-129 field
               roster whose periodic block is extrapolated; each check skips when
               its station is absent.
 10. PERIODIC - synthetic SIF: the seasonal term outside its block windows (the
               NGS M-PAGES v1.2 rule): covering block, before the first, after the
               last, inside a gap, file order, overlap, continuity.
 11. PSD    - synthetic SIF: the post-seismic term is the change since the
               occupation's reference epoch (zero there, negative before it, the
               full relaxation for an event after it, the occupation's own epoch).
 12. SINEX  - the eight roster stations with an active post-seismic term against
               the IGS weekly SINEX of 2024-05-08 (horizontal tolerance stated at
               the table); ANTC00CHL and MIZU00JPN term sizes.

Sections 5 to 8, 10 and 11 need no fixtures.  Exit code 0 iff every check passes; 2 when no
SIF fixtures are found (the fixture-free sections still run first).
"""
from __future__ import annotations

import contextlib
import copy
import csv
import inspect
import io
import json
import logging
import math
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sif_propagate as sp  # noqa: E402

# ECEF agreement target [m]; libm rounding differences (MSVC vs CPython) keep the
# realised residual far below this.
TOL_ECEF = 1e-6
TOL_COV = 5e-12   # covariance entries are ~1e-6 m^2, printed to 12 decimals

# CSV column order before the extrapolation columns were appended.
HISTORICAL_CSV_COLUMNS = [
    "requested", "found", "valid", "id", "domes", "description",
    "reference_frame", "epoch_year",
    "x", "y", "z", "lat_deg", "lon_deg", "height_m",
    "sigma_x", "sigma_y", "sigma_z", "sigma_e", "sigma_n", "sigma_u",
    "in_window", "window_start", "window_end", "source",
    "occupation_mode",
]
EXTRAPOLATION_CSV_COLUMNS = [
    "extrapolated", "extrapolation_years", "segment_start_year", "segment_end_year",
    "antenna_change_in_gap", "receiver_change_in_gap",
]
PERIODIC_CSV_COLUMNS = [
    "periodic_reach", "periodic_extrapolation_years",
    "periodic_block_start_year", "periodic_block_end_year",
]

# --- oracle captured from the C++ engine (tests/test_station_info.cpp) --------- #
# (station, epoch, full_xyz, lin_xyz, cov3x3_or_None)
# The ANTC00CHL full_xyz rows were regenerated when the post-seismic term became the
# change since the reference epoch (they had carried the displacement accumulated by
# 2020.0 a second time, 0.5 m); tests/test_station_info_psd.cpp pins the same numbers on
# the C++ side.  Every other column is as captured.
ORACLE = [
    ("CIT100USA", 2022.0,
     (-2491490.660528890, -4660802.908887121, 3559129.153035940),
     (-2491490.660957403, -4660802.910144504, 3559129.154480755),
     [[0.000000713639, 0.000000955568, -0.000000690324],
      [0.000000955568, 0.000001996525, -0.000001297292],
      [-0.000000690324, -0.000001297292, 0.000001222217]]),
    ("CIT100USA", 2024.36,
     (-2491490.729150678, -4660802.853757640, 3559129.181971976),
     (-2491490.728872413, -4660802.854062885, 3559129.181114601),
     [[0.000001091979, 0.000001469314, -0.000001029245],
      [0.000001469314, 0.000003059361, -0.000001931155],
      [-0.000001029245, -0.000001931155, 0.000001751019]]),
    ("CIT100USA", 2018.5,
     (-2491490.565342857, -4660803.000792147, 3559129.110895291),
     (-2491490.564845042, -4660802.999378534, 3559129.110114238),
     [[0.000000535093, 0.000000706660, -0.000000494565],
      [0.000000706660, 0.000001478590, -0.000000934420],
      [-0.000000494565, -0.000000934420, 0.000000843825]]),
    ("ANTC00CHL", 2022.0,
     (1608538.527130818, -4816370.562133572, -3847798.263421474),
     (1608538.551865682, -4816370.539438507, -3847798.265112284),
     [[0.000002614535, -0.000000530337, 0.000000598358],
      [-0.000000530337, 0.000004042173, -0.000001791438],
      [0.000000598358, -0.000001791438, 0.000005151836]]),
    ("ANTC00CHL", 2024.36,
     (1608538.529997902, -4816370.603508504, -3847798.252403526),
     (1608538.577416177, -4816370.553907000, -3847798.251747113),
     [[0.000002852809, -0.000000818781, 0.000000386973],
      [-0.000000818781, 0.000005073534, -0.000001131949],
      [0.000000386973, -0.000001131949, 0.000005811371]]),
    ("ANTC00CHL", 2018.5,
     (1608538.535370068, -4816370.498579748, -3847798.285808865),
     (1608538.513972999, -4816370.517980994, -3847798.284933512),
     [[0.000002476099, -0.000000386366, 0.000000704476],
      [-0.000000386366, 0.000003517970, -0.000002109380],
      [0.000000704476, -0.000002109380, 0.000004821218]]),
    ("AB0200USA", 2022.0,
     (-3776808.406872013, -744083.772804013, 5068727.935502272),
     (-3776808.404542312, -744083.773264913, 5068727.933476972),
     [[0.000001610163, -0.000000755669, -0.000000861060],
      [-0.000000755669, 0.000003223955, 0.000001062201],
      [-0.000000861060, 0.000001062201, 0.000000991961]]),
    ("AB0200USA", 2024.36,
     (-3776808.470582560, -744083.783545945, 5068727.887543935),
     (-3776808.471497926, -744083.783228846, 5068727.887390122),
     [[0.000001720436, 0.0, 0.0],
      [0.0, 0.000003159459, 0.0],
      [0.0, 0.0, 0.000001046043]]),
    ("AB0200USA", 2018.5,
     (-3776808.336600869, -744083.787374819, 5068727.972330421),
     (-3776808.338627769, -744083.786731119, 5068727.973929721),
     [[0.000000454134, -0.000000116609, -0.000000198856],
      [-0.000000116609, 0.000000716674, 0.000000150390],
      [-0.000000198856, 0.000000150390, 0.000000236851]]),
    ("AMC400USA", 2022.0,
     (-1248596.444857900, -4819428.198639280, 3976505.904878852),
     (-1248596.445091199, -4819428.198338719, 3976505.905770926),
     [[0.000000224225, 0.000000265950, -0.000000196387],
      [0.000000265950, 0.000001245859, -0.000000800618],
      [-0.000000196387, -0.000000800618, 0.000000783439]]),
    ("AMC400USA", 2024.36,
     (-1248596.479924452, -4819428.195378378, 3976505.894068757),
     (-1248596.479915689, -4819428.195937997, 3976505.893316174),
     [[0.000000236918, 0.000000273158, -0.000000201682],
      [0.000000273158, 0.000001292601, -0.000000827206],
      [-0.000000201682, -0.000000827206, 0.000000813083]]),
    # 2.63 yr before the first periodic window (2021.13): the first block's seasonal
    # term is continued, so FULL differs from LIN by 0.68 mm.  Before the block
    # selection rule the engine applied no term here (FULL == LIN); the FULL value
    # is the one tests/test_station_info_seasonal.cpp pins on the C++ side.
    ("AMC400USA", 2018.5,
     (-1248596.393551557, -4819428.201226350, 3976505.924238630),
     (-1248596.393444710, -4819428.201899112, 3976505.924241956),
     [[0.000000247937, 0.000000313064, -0.000000229829],
      [0.000000313064, 0.000001442945, -0.000000932929],
      [-0.000000229829, -0.000000932929, 0.000000906179]]),
]

_fail = 0
_checks = 0


def check(cond, msg):
    global _fail, _checks
    _checks += 1
    if not cond:
        _fail += 1
        print(f"  FAIL: {msg}")
    return cond


def load_registry():
    """Registry over the bundled SIF fixtures, or None when none are found."""
    paths = sp.default_sif_paths()
    if not paths:
        print("ERROR: no SIF fixtures found (need data/static/sif/*.json). "
              "Set $SIF_PATHS to run the fixture sections.", file=sys.stderr)
        return None
    reg = sp.SifRegistry()
    for p in paths:
        reg.load(p)
    print(f"# loaded {len(reg)} stations from {len(paths)} SIF file(s)")
    return reg


def test_oracle(reg):
    print("[1] ORACLE cross-check vs C++ resolveStationCoordinate")
    max_full = max_lin = max_cov = 0.0
    for name, epoch, full, lin, cov in ORACLE:
        st = reg.find(name)
        if not check(st is not None, f"{name} not found"):
            continue
        rf = sp.resolve_station_coordinate(st, epoch, True, True)
        rl = sp.resolve_station_coordinate(st, epoch, False, False)
        check(rf.valid and rl.valid, f"{name}@{epoch} not valid")
        for k in range(3):
            df = abs(rf.ecef[k] - full[k]); max_full = max(max_full, df)
            dl = abs(rl.ecef[k] - lin[k]);  max_lin = max(max_lin, dl)
            check(df < TOL_ECEF, f"{name}@{epoch} FULL[{k}] d={df:.3e}")
            check(dl < TOL_ECEF, f"{name}@{epoch} LIN[{k}] d={dl:.3e}")
        if cov is not None:
            check(rf.covariance is not None, f"{name}@{epoch} missing covariance")
            if rf.covariance is not None:
                for i in range(3):
                    for j in range(3):
                        dc = abs(rf.covariance[i][j] - cov[i][j]); max_cov = max(max_cov, dc)
                        check(dc < TOL_COV, f"{name}@{epoch} COV[{i}][{j}] d={dc:.3e}")
    print(f"    max |dECEF_full|={max_full:.3e} m  |dECEF_lin|={max_lin:.3e} m  "
          f"|dCov|={max_cov:.3e} m^2")


def test_anchors(reg):
    print("[2] ANCHORS vs tests/test_station_info.cpp")
    cit = reg.find("CIT100USA")
    check(cit is not None and cit.id == "CIT100USA", "CIT100USA id")
    check(cit.reference_frame == "ITRF2020", "CIT frame")
    check(abs(cit.reference_epoch_year - 2020.0) < 1e-9, "CIT ref epoch")
    o = cit.occupations[0]
    check(abs(o.coordinates[0] - (-2491490.58952111)) < 1e-3, "CIT occ X")
    check(abs(o.coordinates[1] - (-4660802.91919166)) < 1e-3, "CIT occ Y")
    check(abs(o.coordinates[2] - 3559129.08584545) < 1e-3, "CIT occ Z")
    check(abs(o.velocity[0] - (-0.0286339210265503)) < 1e-9, "CIT vel X (mm/y->m/y)")
    check(o.covariance is not None, "CIT has covariance")
    check(abs(o.covariance[0][0] - 0.0047819 ** 2) < 1e-12, "CIT cov(0,0)=sigma_X^2")
    for i in range(6):
        for j in range(6):
            check(abs(o.covariance[i][j] - o.covariance[j][i]) < 1e-18, f"CIT cov sym {i},{j}")
    # periodic coeff mm->m
    pb = cit.periodic[0]
    check(len(pb.freqs) >= 2, "CIT annual+semiannual")
    check(abs(pb.freqs[0].period_yr - 1.0) < 1e-9, "CIT freq0 period")
    check(abs(pb.freqs[0].x_cos - 0.463155530400065e-3) < 1e-9, "CIT freq0 x_cos mm->m")
    # find() by prefix and domes -> same record
    check(reg.find("cit1") is cit, "find by 4-char prefix (case-insensitive)")
    check(reg.find("40400S201") is cit, "find by DOMES")
    check(reg.find("NOSUCHSTATION") is None, "unknown -> None")
    # ANTC post-seismic
    antc = reg.find("ANTC00CHL")
    check(antc is not None and antc.psd, "ANTC has PSD")
    pbk = antc.psd[0]
    check(abs(pbk.event_year - 2010.157) < 0.01, "ANTC event epoch 2010-02-27")
    check(len(pbk.east.alog) >= 2, "ANTC east alog count")
    check(abs(pbk.east.alog[0] - (-0.13792224996689)) < 1e-9, "ANTC east alog[0]")


def test_selfcheck(reg):
    print("[3] SELFCHK internal identities")
    # time conversions (cf. C++ decimalYearFromSif)
    check(abs(sp.decimal_year_from_sif("1994-09-01T00:00:00 GPS") - 1994.666) < 0.01,
          "1994-09-01 -> ~1994.666")
    check(sp.decimal_year_from_sif("9999-12-31T00:00:00 UTC") == 9999.0, "open-end sentinel")
    check(abs(sp.decimal_year_from_ymd(2010, 2, 27) - 2010.15616) < 1e-3, "2010-02-27 doy")
    check(sp.decimal_year_from_sif("") == 0.0, "empty timestamp -> 0")
    # dt = 0 -> propagated covariance equals Cxx exactly
    cit = reg.find("CIT100USA")
    r0 = sp.resolve_station_coordinate(cit, cit.reference_epoch_year, False, False)
    o = cit.occupations[0] if (cit.occupations[0].start_year <= 2020.0 < cit.occupations[0].end_year) \
        else sp._select_occupation(cit, 2020.0)
    o = sp._select_occupation(cit, cit.reference_epoch_year)
    for i in range(3):
        for j in range(3):
            check(abs(r0.covariance[i][j] - o.covariance[i][j]) < 1e-18,
                  f"dt=0 cov==Cxx [{i}][{j}]")
    # covariance symmetric + positive diagonal after propagation
    r = sp.resolve_station_coordinate(cit, 2024.36, True, True)
    for i in range(3):
        check(r.covariance[i][i] > 0.0, f"cov diag>0 [{i}]")
        for j in range(3):
            check(abs(r.covariance[i][j] - r.covariance[j][i]) < 1e-20, f"prop cov sym {i},{j}")
    # ENU <-> ECEF round-trip
    lat, lon = math.radians(34.0), math.radians(-118.0)
    R = sp.enu_to_ecef_matrix(lat, lon)
    enu_in = (1.234, -5.678, 9.012)
    ecef = sp._mat3_vec(R, enu_in)
    back = sp._mat3_vec(sp._mat3_transpose(R), ecef)
    for k in range(3):
        check(abs(back[k] - enu_in[k]) < 1e-12, f"ENU roundtrip [{k}]")
    # periodic on/off changes position but by < 5 cm (seasonal amplitude)
    rp = sp.resolve_station_coordinate(cit, 2024.36, True, False)
    rn = sp.resolve_station_coordinate(cit, 2024.36, False, False)
    d = math.dist(rp.ecef, rn.ecef)
    check(0.0 < d < 0.05, f"periodic delta {d:.4f} m in (0, 0.05)")


def test_occupation_mode(reg):
    """[4] 'latest' occupation mode (NGS datasheet convention).

    CORV00USA has a break at the 2019-07 antenna swap: the window covering
    2010.0 is 1998-04-26..2019-08-03, the 'latest' one is 2019-08-04..open.  The
    NGS datasheet AH2488 (NAD83(2011) epoch 2010.00) is built from the 'latest'
    occupation propagated linearly back, so only that route closes on the
    published value (0.4 mm, vs 32.8 mm for the covering window; the closure
    through the epoch-matched ITRF2020->NAD83(2011) Helmert is demonstrated in
    corv_worked_example.py, which is not run here to keep this test
    dependency-free).
    """
    print("[4] OCCUPATION selection modes")
    # (b) CORV 'latest' + no-periodic at 2010.0 equals the linear propagation of
    #     the current occupation: X20 + V*(2010.0 - 2020.0).
    expect = (-2498423.3133, -3802819.9155, 4454736.7162)
    corv = reg.find("CORV00USA")
    if check(corv is not None, "CORV00USA found"):
        rl = sp.resolve_station_coordinate(corv, 2010.0, with_periodic=False,
                                           with_psd=True, occupation="latest")
        check(rl.valid, "CORV latest valid")
        check(rl.occupation_mode == "latest", "CORV result reports mode 'latest'")
        check(rl.window[0] > 2019.0, f"latest window starts 2019+, got {rl.window[0]:.3f}")
        check(not rl.in_window, "2010.0 is OUTSIDE the latest window")
        check(not rl.extrapolated and rl.extrapolation_years == 0.0,
              "2010.0 is covered by the 1998 window, so the latest result is not extrapolated")
        for k in range(3):
            d = abs(rl.ecef[k] - expect[k])
            check(d <= 2e-4, f"CORV latest ECEF[{k}] d={d*1e3:.3f} mm > 0.2 mm")

        # (c) covering vs latest differ by 30-35 mm, dominated by Up.
        rc = sp.resolve_station_coordinate(corv, 2010.0, with_periodic=False,
                                           with_psd=True, occupation="covering")
        check(rc.occupation_mode == "covering", "covering result reports its mode")
        check(rc.window[0] < 2000.0, f"covering window is the 1998 one ({rc.window[0]:.3f})")
        d3 = math.dist(rc.ecef, rl.ecef)
        check(0.030 <= d3 <= 0.035, f"covering-vs-latest 3D {d3*1e3:.2f} mm not in [30,35]")
        lat, lon, _h = sp.ecef_to_llh(*rl.ecef)
        R = sp.enu_to_ecef_matrix(lat, lon)
        dv = tuple(rc.ecef[k] - rl.ecef[k] for k in range(3))
        de, dn, du = sp._mat3_vec(sp._mat3_transpose(R), dv)
        check(abs(du) > math.hypot(de, dn),
              f"jump not Up-dominated: E={de*1e3:.2f} N={dn*1e3:.2f} U={du*1e3:.2f} mm")
        check(abs(du) > 0.028, f"Up component {du*1e3:.2f} mm, expected ~32 mm")
        print(f"    CORV 2010.0 covering-vs-latest: 3D {d3*1e3:.2f} mm  "
              f"(E {de*1e3:+.2f}, N {dn*1e3:+.2f}, U {du*1e3:+.2f} mm)")

    # (d) a single-occupation station is mode-invariant.
    amc = reg.find("AMC400USA")
    if check(amc is not None, "AMC400USA found"):
        usable = [o for o in amc.occupations if not o.do_not_use]
        check(len(usable) == 1, f"AMC400USA expected 1 usable occupation, got {len(usable)}")
        for ep in (2010.0, 2018.5, 2024.36):
            a = sp.resolve_station_coordinate(amc, ep, True, True, occupation="covering")
            b = sp.resolve_station_coordinate(amc, ep, True, True, occupation="latest")
            check(a.valid and b.valid, f"AMC400USA@{ep} valid both modes")
            check(a.window == b.window, f"AMC400USA@{ep} same window both modes")
            for k in range(3):
                check(abs(a.ecef[k] - b.ecef[k]) == 0.0,
                      f"AMC400USA@{ep} ECEF[{k}] differs between modes")

    # default keyword == covering (so (a), the oracle block above, is untouched).
    cit = reg.find("CIT100USA")
    r_def = sp.resolve_station_coordinate(cit, 2024.36, True, True)
    r_cov = sp.resolve_station_coordinate(cit, 2024.36, True, True, occupation="covering")
    check(r_def.ecef == r_cov.ecef, "default occupation kwarg == 'covering'")
    check(r_def.occupation_mode == "covering", "default result reports 'covering'")
    # occupation_mode keeps its historical position (the last of the original 25
    # columns); the extrapolation columns are appended after it.
    check(sp._CSV_COLUMNS[:25] == HISTORICAL_CSV_COLUMNS,
          "historical CSV column order unchanged")
    check(sp._CSV_COLUMNS[24] == "occupation_mode", "occupation_mode at its historical index")
    check(sp._CSV_COLUMNS[25:31] == EXTRAPOLATION_CSV_COLUMNS,
          "extrapolation columns appended after occupation_mode")
    check(sp._CSV_COLUMNS[31:] == PERIODIC_CSV_COLUMNS,
          "periodic columns appended after the extrapolation columns")
    try:
        sp.resolve_station_coordinate(cit, 2024.36, occupation="bogus")
        check(False, "bogus occupation mode should raise")
    except ValueError:
        pass


# --------------------------------------------------------------------------- #
# Synthetic-SIF helpers (sections 5 to 8)                                      #
# --------------------------------------------------------------------------- #
OPEN = "9999-12-31T00:00:00 GPS"
dy = sp.decimal_year_from_ymd


def _ts(y, m=1, d=1):
    return f"{y:04d}-{m:02d}-{d:02d}T00:00:00 GPS"


def _plus(p, d):
    return (p[0] + d[0], p[1] + d[1], p[2] + d[2])


def _occ(start, end, coords, vel=(0.0, 0.0, 0.0), ref=2020.0, dnu=False, cov=None):
    """One OCCUPATION record; ``ref=None`` omits REFERENCE_EPOCH."""
    o = {"START": start, "END": end, "COORDINATES": list(coords),
         "COORDINATE_SIGMAS": [0.002, 0.003, 0.004], "VELOCITY": list(vel),
         "VELOCITY_SIGMAS": [0.5, 0.6, 0.7], "DO_NOT_USE": dnu}
    if ref is not None:
        o["REFERENCE_EPOCH"] = ref
    if cov is not None:
        o["6X6_COVARIANCE_MATRIX"] = list(cov)
    return o


def _ant(start, end, typ, serial):
    return {"START": start, "END": end, "TYPE": typ, "SERIAL_NUMBER": serial}


def _rec(start, end, typ, firmware="1.0", serial="R1"):
    return {"START": start, "END": end, "TYPE": typ, "FIRMWARE": firmware,
            "SERIAL_NUMBER": serial}


def _station(sid, occupations, domes="", approx=None, antennas=None, receivers=None,
             periodic=None, psd=None):
    s = {"ID": sid, "DOMES": domes, "OCCUPATION": occupations}
    if approx is not None:
        s["APPROXIMATE_COORDINATES"] = list(approx)
    if antennas is not None:
        s["ANTENNA"] = antennas
    if receivers is not None:
        s["RECEIVER"] = receivers
    if periodic is not None or psd is not None:
        s["NONLINEAR_TERMS"] = {}
        if periodic is not None:
            s["NONLINEAR_TERMS"]["PERIODIC_TERMS"] = periodic
        if psd is not None:
            s["NONLINEAR_TERMS"]["POST_SEISMIC"] = psd
    return s


@contextlib.contextmanager
def _synthetic_registry(*files):
    """Yield (registry, paths) over synthetic SIF documents.

    Each file is ``(meta_epoch, stations)``; ``meta_epoch=None`` omits
    METADATA.REFERENCE_EPOCH.
    """
    with tempfile.TemporaryDirectory() as td:
        reg = sp.SifRegistry()
        paths = []
        for i, (meta_epoch, stations) in enumerate(files):
            meta = {"REFERENCE_FRAME": "ITRF2020"}
            if meta_epoch is not None:
                meta["REFERENCE_EPOCH"] = meta_epoch
            p = Path(td) / f"synthetic_{i}.json"
            p.write_text(json.dumps({"METADATA": meta, "STATIONS": stations}),
                         encoding="utf-8")
            reg.load(p)
            paths.append(str(p))
        yield reg, paths


class _LogCapture(logging.Handler):
    def __init__(self):
        super().__init__(logging.DEBUG)
        self.records = []

    def emit(self, record):
        self.records.append(record)

    def messages(self):
        return [r.getMessage() for r in self.records]


@contextlib.contextmanager
def _capture_sif_log():
    """Capture the tool's logger (and keep it off the console) for the block."""
    lg = logging.getLogger("sif_propagate")
    cap = _LogCapture()
    old_propagate = lg.propagate
    lg.addHandler(cap)
    lg.propagate = False
    try:
        yield cap
    finally:
        lg.removeHandler(cap)
        lg.propagate = old_propagate


def _expect_lookup(reg, cap, key, approx, expect_id, msg, warn_contains=None, **kw):
    """find() one key; check the match and the warning (exactly one, or none)."""
    cap.records.clear()
    if approx is None and not kw:
        st = reg.find(key)                       # the no-argument form
    else:
        st = reg.find(key, approx_ecef=approx, **kw)
    got = st.id if st is not None else None
    check(got == expect_id, f"{msg}: find({key!r}) -> {got}, want {expect_id}")
    if warn_contains is None:
        check(not cap.records, f"{msg}: unexpected warning {cap.messages()}")
        return
    check(len(cap.records) == 1 and cap.records[0].levelno == logging.WARNING,
          f"{msg}: want exactly one WARNING, got {cap.messages()}")
    text = cap.records[0].getMessage() if cap.records else ""
    for s in warn_contains:
        check(s in text, f"{msg}: warning lacks {s!r}: {text!r}")


def _lin(occ, t):
    """Linear term with the tool's own expression order: x_ref + v * (t - t_ref)."""
    dt = t - occ.reference_epoch_year
    return tuple(occ.coordinates[k] + occ.velocity[k] * dt for k in range(3))


def _prop_cov(occ, dt):
    C = occ.covariance
    return [[C[i][j] + dt * (C[i][3 + j] + C[3 + i][j]) + dt * dt * C[3 + i][3 + j]
             for j in range(3)] for i in range(3)]


def test_lookup_synthetic():
    print("[5] LOOKUP exact ID / DOMES / vetted 4-char prefix (synthetic SIF)")
    P_A = (-2491490.0, -4660803.0, 3559129.0)
    P_E1 = (1608538.0, -4816371.0, -3847798.0)
    P_E2 = _plus(P_E1, (20000.0, 0.0, 0.0))
    P_I1 = (-3857171.0, 3108693.0, 4004040.0)
    P_I2 = _plus(P_I1, (0.0, 20000.0, 0.0))
    P_Q = (6104817.0, -605828.0, -1740739.0)
    P_U1 = (4075580.0, 931854.0, 4801568.0)
    P_U2 = _plus(P_U1, (0.0, 0.0, 20000.0))
    P_Y0 = (5084625.0, 2670366.0, -2768494.0)
    P_Y2 = _plus(P_Y0, (20000.0, 0.0, 0.0))
    P_C = (-3950072.0, 2522415.0, -4311637.0)
    zero = (0.0, 0.0, 0.0)
    stations = [
        _station("ABCD00USA", [_occ(_ts(2000), OPEN, P_A)], domes="12345M001", approx=P_A),
        # APPROXIMATE_COORDINATES unusable -> first non-DO_NOT_USE occupation (P_E2).
        _station("EFGH00USA", [_occ(_ts(2000), _ts(2005), P_E1, dnu=True),
                               _occ(_ts(2005), OPEN, P_E2)], approx=zero),
        # No APPROXIMATE_COORDINATES and every occupation flagged -> first occupation.
        _station("IJKL00USA", [_occ(_ts(2000), _ts(2005), P_I1, dnu=True),
                               _occ(_ts(2005), OPEN, P_I2, dnu=True)]),
        # Nothing usable at all.
        _station("MNOP00USA", [], approx=zero),
        # First non-flagged occupation is a zero placeholder -> first occupation.
        _station("QRST00USA", [_occ(_ts(2000), _ts(2005), P_Q, dnu=True),
                               _occ(_ts(2005), OPEN, zero)], approx=zero),
        # Two usable non-flagged occupations 20 km apart -> the FIRST one (P_U1).
        _station("UVWX00USA", [_occ(_ts(2000), _ts(2005), P_U1),
                               _occ(_ts(2005), OPEN, P_U2)], approx=zero),
        # Only the first non-flagged occupation is consulted: its zero placeholder
        # falls through to the first occupation (P_Y0), not to the next
        # non-flagged occupation with usable coordinates (P_Y2).
        _station("YZAB00USA", [_occ(_ts(2000), _ts(2003), P_Y0, dnu=True),
                               _occ(_ts(2003), _ts(2006), zero),
                               _occ(_ts(2006), OPEN, P_Y2)], approx=zero),
        # Non-zero APPROXIMATE_COORDINATES with norm 1e5 m is not usable -> P_C.
        _station("CDEF00USA", [_occ(_ts(2000), OPEN, P_C)], approx=(1.0e5, 0.0, 0.0)),
    ]
    check(sp.MAX_PREFIX_DISTANCE_M == 10000.0, "MAX_PREFIX_DISTANCE_M is 10 km")
    check(inspect.signature(sp.SifRegistry.find).parameters["max_prefix_distance_m"].default
          == sp.MAX_PREFIX_DISTANCE_M, "find() defaults to MAX_PREFIX_DISTANCE_M")

    far = _plus(P_A, (500000.0, 0.0, 0.0))
    no_pos = "no approximate position"
    with _synthetic_registry((2020.0, stations)) as (reg, _paths), _capture_sif_log() as cap:
        L = lambda *a, **k: _expect_lookup(reg, cap, *a, **k)  # noqa: E731
        # Exact ID and DOMES: unconditional, silent.
        L("ABCD00USA", None, "ABCD00USA", "exact ID")
        L("abcd00usa ", None, "ABCD00USA", "exact ID, case-insensitive and trimmed")
        L("12345M001", None, "ABCD00USA", "exact DOMES")
        L("ABCD00USA", far, "ABCD00USA", "exact ID wins over a distant position")
        L("12345m001", far, "ABCD00USA", "exact DOMES wins over a distant position")
        # No position: a bare 4-char code is accepted, a longer key is not.
        L("abcd", None, "ABCD00USA", "bare 4-char code, no position")
        L(" ABCD ", None, "ABCD00USA", "bare 4-char code after trimming")
        L("ABC", None, None, "3-char key never matches")
        L("ABCD00XYZ", None, None, "9-char non-exact key rejected without position",
          warn_contains=["'ABCD00XYZ'", "ABCD00USA", no_pos])
        check("m from" not in (cap.messages() or [""])[0],
              "no distance quoted when no position was supplied")
        # With a position: the 3D distance decides, inclusive at the limit.
        L("ABCD00XYZ", _plus(P_A, (9999.0, 0.0, 0.0)), "ABCD00USA", "9,999 m accepted")
        L("ABCD00XYZ", _plus(P_A, (10000.0, 0.0, 0.0)), "ABCD00USA",
          "exactly 10,000 m accepted (limit inclusive)")
        L("ABCD00XYZ", _plus(P_A, (10001.0, 0.0, 0.0)), None, "10,001 m rejected",
          warn_contains=["'ABCD00XYZ'", "ABCD00USA", "10001.0 m", "limit 10000.0 m"])
        L("ABCD00XYZ", _plus(P_A, (5000.0, 5000.0, 5000.0)), "ABCD00USA",
          "3D distance 8,660 m accepted")
        L("ABCD00XYZ", _plus(P_A, (6000.0, 6000.0, 6000.0)), None,
          "3D distance 10,392 m rejected (per-axis offsets all < 10 km)",
          warn_contains=["10392.3 m"])
        L("abcd", _plus(P_A, (10001.0, 0.0, 0.0)), None,
          "a supplied position also governs a bare 4-char code",
          warn_contains=["'abcd'", "ABCD00USA", "10001.0 m"])
        L("ABCD00XYZ", _plus(P_A, (20000.0, 0.0, 0.0)), "ABCD00USA",
          "max_prefix_distance_m override", max_prefix_distance_m=25000.0)
        # Unusable positions behave as no position at all.
        for bad in (zero, (float("nan"), P_A[1], P_A[2]), (float("inf"), 0.0, 0.0),
                    (1.0e5, 0.0, 0.0), (P_A[0], P_A[1])):
            L("ABCD00XYZ", bad, None, f"unusable position {bad} = no position (long key)",
              warn_contains=["ABCD00USA", no_pos])
            L("ABCD", bad, "ABCD00USA", f"unusable position {bad} = no position (bare code)")
        # Candidate reference-position fallbacks.
        L("EFGH00ZZZ", P_E2, "EFGH00USA", "reference = first non-flagged occupation")
        L("EFGH00ZZZ", P_E1, None, "flagged occupation is not the reference",
          warn_contains=["EFGH00USA", "20000.0 m"])
        L("IJKL00ZZZ", P_I1, "IJKL00USA", "all flagged: reference = first occupation")
        L("IJKL00ZZZ", P_I2, None, "all flagged: second occupation is not the reference",
          warn_contains=["IJKL00USA", "20000.0 m"])
        L("QRST00ZZZ", P_Q, "QRST00USA",
          "zero placeholder on the first non-flagged occupation falls through")
        L("UVWX00ZZZ", P_U1, "UVWX00USA", "two non-flagged occupations: the first is the reference")
        L("UVWX00ZZZ", P_U2, None, "two non-flagged occupations: the last is not the reference",
          warn_contains=["UVWX00USA", "20000.0 m"])
        L("YZAB00ZZZ", P_Y0, "YZAB00USA",
          "zero placeholder on the first non-flagged occupation -> first occupation")
        L("YZAB00ZZZ", P_Y2, None,
          "a later non-flagged occupation with usable coordinates is not the reference",
          warn_contains=["YZAB00USA", "20000.0 m"])
        L("CDEF00ZZZ", P_C, "CDEF00USA",
          "small non-zero APPROXIMATE_COORDINATES falls through to the occupation")
        L("MNOP00ZZZ", P_A, None, "candidate with no usable reference rejected",
          warn_contains=["'MNOP00ZZZ'", "MNOP00USA", "no usable reference position"])
        L("MNOP", None, "MNOP00USA", "same candidate, bare code, no position")


def test_epoch_synthetic():
    print("[6] EPOCH per-occupation REFERENCE_EPOCH (synthetic SIF)")
    X0 = (-1981233.59243938, -4990304.17764443, 3431957.6684601)
    V = (-13.4, 1.2, -6.5)  # mm/yr
    # 15 off-diagonal terms [mm^2], nonzero position-velocity cross terms.
    cov15 = [0.8, -0.3, 0.05, -0.02, 0.01, 0.6, 0.03, 0.04, -0.01, 0.02, -0.03, 0.07,
             0.1, -0.05, 0.08]
    annual = [{"START": _ts(2000), "END": OPEN,
               "FREQUENCIES": [{"PERIOD": 1.0, "X_COS": 2.0, "X_SIN": -1.0,
                                "Y_COS": 0.5, "Y_SIN": 0.25, "Z_COS": -3.0, "Z_SIN": 1.5}]}]
    file_a = (2020.0, [
        _station("EPOA00USA", [_occ(_ts(2000), OPEN, X0, V, ref=2010.0, cov=cov15)]),
        _station("EPOB00USA", [_occ(_ts(2000), OPEN, X0, V, ref=2020.0, cov=cov15)]),
        _station("EPOC00USA", [_occ(_ts(2000), OPEN, X0, V, ref=None, cov=cov15)]),
        _station("EPOD00USA", [_occ(_ts(2000), OPEN, X0, V, ref="2010.0", cov=cov15)]),
        _station("EPOE00USA", [_occ(_ts(2000), OPEN, X0, V, ref=True, cov=cov15)]),
        _station("EPOM00USA", [_occ(_ts(2000), _ts(2010), X0, V, ref=2010.0, cov=cov15),
                               _occ(_ts(2010), OPEN, X0, V, ref=2020.0, cov=cov15)]),
    ])
    file_b = (2015.0, [
        _station("EPOF00USA", [_occ(_ts(2000), OPEN, X0, V, ref=None, cov=cov15)]),
        _station("EPOG00USA", [_occ(_ts(2000), OPEN, X0, V, ref=2010.0, cov=cov15)],
                 periodic=annual),
    ])
    file_c = (None, [_station("EPOH00USA", [_occ(_ts(2000), OPEN, X0, V, ref=None)])])

    t = 2024.35
    with _synthetic_registry(file_a, file_b, file_c) as (reg, _paths):
        a = reg.find("EPOA00USA")
        oa = a.occupations[0]
        check(oa.reference_epoch_year == 2010.0, "occupation REFERENCE_EPOCH 2010.0 parsed")
        check(a.reference_epoch_year == 2020.0, "station keeps the METADATA epoch")
        r = sp.resolve_station_coordinate(a, t, True, True)
        check(r.valid and r.ecef == _lin(oa, t), "2010.0: x = x_ref + v * (t - 2010.0), exact")
        check(r.covariance == _prop_cov(oa, t - 2010.0),
              "2010.0: covariance propagated with dt = t - 2010.0, exact")
        check(r.covariance != _prop_cov(oa, t - 2020.0),
              "2010.0: covariance differs from the dt = t - 2020.0 propagation")
        vnorm = math.sqrt(sum(v * v for v in oa.velocity))
        moved = math.dist(r.ecef, tuple(oa.coordinates[k] + oa.velocity[k] * (t - 2020.0)
                                        for k in range(3)))
        check(abs(moved - 10.0 * vnorm) < 1e-9,
              f"2010.0 vs 2020.0 linear term differs by |v| * 10 yr ({moved:.4f} m)")
        r0 = sp.resolve_station_coordinate(a, 2010.0, False, False)
        check(r0.ecef == oa.coordinates, "2010.0 record at t = 2010.0 returns x_ref exactly")
        check(r0.covariance == [row[:3] for row in oa.covariance[:3]],
              "2010.0 record at t = 2010.0 returns Cxx exactly")

        for sid, why in (("EPOB00USA", "explicit 2020.0"), ("EPOC00USA", "absent -> METADATA"),
                         ("EPOD00USA", "string -> METADATA"), ("EPOE00USA", "bool -> METADATA")):
            st = reg.find(sid)
            o = st.occupations[0]
            check(o.reference_epoch_year == 2020.0, f"{sid} ({why}) reference epoch 2020.0")
            rr = sp.resolve_station_coordinate(st, t, True, True)
            check(rr.ecef == tuple(o.coordinates[k] + o.velocity[k] * (t - 2020.0)
                                   for k in range(3)),
                  f"{sid}: linear term uses t - 2020.0, exact")
            check(rr.covariance == _prop_cov(o, t - 2020.0), f"{sid}: covariance dt = t - 2020.0")

        m = reg.find("EPOM00USA")
        rm1 = sp.resolve_station_coordinate(m, 2005.0, False, False)
        rm2 = sp.resolve_station_coordinate(m, 2015.0, False, False)
        check(rm1.ecef == _lin(m.occupations[0], 2005.0) and
              m.occupations[0].reference_epoch_year == 2010.0,
              "two occupations: the first uses its own 2010.0")
        check(rm2.ecef == _lin(m.occupations[1], 2015.0) and
              m.occupations[1].reference_epoch_year == 2020.0,
              "two occupations: the second uses its own 2020.0")

        f = reg.find("EPOF00USA")
        check(f.occupations[0].reference_epoch_year == 2015.0,
              "absent REFERENCE_EPOCH -> METADATA 2015.0 of its own file")
        rf = sp.resolve_station_coordinate(f, t, True, True)
        check(rf.ecef == tuple(f.occupations[0].coordinates[k]
                               + f.occupations[0].velocity[k] * (t - 2015.0) for k in range(3)),
              "METADATA 2015.0 fallback drives the linear term")

        g = reg.find("EPOG00USA")
        og = g.occupations[0]
        check(og.reference_epoch_year == 2010.0 and g.periodic[0].reference_epoch_year == 2015.0,
              "periodic block keeps the METADATA phase epoch (2015.0)")
        rg = sp.resolve_station_coordinate(g, t, True, True)
        fr = g.periodic[0].freqs[0]
        w = 2.0 * sp._PI / fr.period_yr
        c, s = math.cos(w * (t - 2015.0)), math.sin(w * (t - 2015.0))
        lin = _lin(og, t)
        expect = (lin[0] + (fr.x_cos * c + fr.x_sin * s),
                  lin[1] + (fr.y_cos * c + fr.y_sin * s),
                  lin[2] + (fr.z_cos * c + fr.z_sin * s))
        check(rg.ecef == expect,
              "linear term on the occupation epoch, periodic phase on the METADATA epoch")

        h = reg.find("EPOH00USA")
        check(h.occupations[0].reference_epoch_year == 2020.0 and h.reference_epoch_year == 2020.0,
              "no REFERENCE_EPOCH anywhere -> 2020.0")


def test_extrapolation_synthetic():
    print("[7] EXTRAP extrapolation flags and equipment changes in the gap (synthetic SIF)")
    P = (-5937160.9, -1054675.2, -2071386.0)
    V = (-21.3, 58.4, 33.1)
    T1, T2 = "TRM59800.00     NONE", "JAVRINGANT_DM   NONE"
    stations = [
        # Past the end; antenna TYPE change after the end (windows listed out of
        # START order); firmware-only receiver update.
        _station("EXA100USA", [_occ(_ts(2010), _ts(2018), P, V)],
                 antennas=[_ant(_ts(2019, 6, 1), OPEN, T2, "222"),
                           _ant(_ts(2005), _ts(2019, 6, 1), T1, "111")],
                 receivers=[_rec(_ts(2005), _ts(2019, 3, 1), "SEPT POLARX5", "5.3.2", "R1"),
                            _rec(_ts(2019, 3, 1), OPEN, "SEPT POLARX5", "5.5.0", "R1")]),
        # Record-only antenna re-log, then a serial swap; serial-only receiver swap.
        _station("EXA200USA", [_occ(_ts(2010), _ts(2018), P)],
                 antennas=[_ant(_ts(2005), _ts(2019), T1, "S1"),
                           _ant(_ts(2019), _ts(2019, 6, 1), T1, "S1"),
                           _ant(_ts(2019, 6, 1), OPEN, T1, "S9")],
                 receivers=[_rec(_ts(2005), _ts(2019), "TRIMBLE NETR9", "5.44", "A"),
                            _rec(_ts(2019), OPEN, "TRIMBLE NETR9", "5.44", "B")]),
        # Tolerance on the segment side of the end: radome-only change 1 day before
        # the end counts; receiver TYPE change 7 days before the end does not.
        _station("EXA300USA", [_occ(_ts(2010), _ts(2018), P)],
                 antennas=[_ant(_ts(2005), _ts(2017, 12, 31), "TRM59800.00     NONE", "9"),
                           _ant(_ts(2017, 12, 31), OPEN, "TRM59800.00     SCIS", "9")],
                 receivers=[_rec(_ts(2005), _ts(2017, 12, 25), "TPS NET-G3A"),
                            _rec(_ts(2017, 12, 25), OPEN, "SEPT POLARX5")]),
        # Before the first segment: antenna change before t (outside), antenna
        # change 1 day after the start (inside), receiver change 4 days after the
        # start (outside).
        _station("EXA400USA", [_occ(_ts(2015), OPEN, P)],
                 antennas=[_ant(_ts(2008), _ts(2011), "ANT0            NONE", "0"),
                           _ant(_ts(2011), _ts(2015, 1, 2), T1, "1"),
                           _ant(_ts(2015, 1, 2), OPEN, T2, "2")],
                 receivers=[_rec(_ts(2008), _ts(2015, 1, 5), "RX-A"),
                            _rec(_ts(2015, 1, 5), OPEN, "RX-B")]),
        # Two occupations: in 'latest' mode an epoch covered by the earlier one is
        # not extrapolated; one in the uncovered gap is measured from the latest.
        _station("EXA500USA", [_occ(_ts(2010), _ts(2015), P), _occ(_ts(2016), OPEN, P)],
                 antennas=[_ant(_ts(2005), _ts(2015, 7, 1), T1, "1"),
                           _ant(_ts(2015, 7, 1), OPEN, T2, "2")]),
        # The first window (by START) is never a change, even inside the gap.
        _station("EXA600USA", [_occ(_ts(2015), OPEN, P)],
                 antennas=[_ant(_ts(2014, 6, 1), OPEN, T1, "1")],
                 receivers=[_rec(_ts(2014, 6, 1), OPEN, "RX-A")]),
        # Receiver TYPE change after the end.
        _station("EXA700USA", [_occ(_ts(2010), _ts(2018), P)],
                 receivers=[_rec(_ts(2005), _ts(2018, 6, 1), "TPS NET-G3A", "4.7", "X"),
                            _rec(_ts(2018, 6, 1), OPEN, "SEPT POLARX5", "4.7", "X")]),
        # Every occupation flagged: no result, diagnostics at their defaults.
        _station("EXA800USA", [_occ(_ts(2010), OPEN, P, dnu=True)]),
        # Before the first segment, t side of the gap: antenna and receiver
        # changes on 2012-01-01, probed 1 day after (outside) and before (inside).
        _station("EXA900USA", [_occ(_ts(2015), OPEN, P)],
                 antennas=[_ant(_ts(2008), _ts(2012), T1, "1"),
                           _ant(_ts(2012), OPEN, T2, "2")],
                 receivers=[_rec(_ts(2008), _ts(2012), "RX-A"),
                            _rec(_ts(2012), OPEN, "RX-B")]),
        # A DO_NOT_USE window covering t does not make t covered.
        _station("EXC100USA", [_occ(_ts(2010), _ts(2018), P),
                               _occ(_ts(2018), OPEN, P, dnu=True)]),
    ]

    def flags(r):
        return (r.extrapolated, r.extrapolation_years, r.antenna_change_in_gap,
                r.receiver_change_in_gap)

    def R(reg, sid, t, mode="covering"):
        return sp.resolve_station_coordinate(reg.find(sid), t, True, True, occupation=mode)

    with _synthetic_registry((2020.0, stations)) as (reg, _paths):
        check(abs(sp.EQUIPMENT_GAP_TOL_YR - 2.0 / 365.25) == 0.0, "gap tolerance is 2 days")
        st1 = reg.find("EXA100USA")
        r = R(reg, "EXA100USA", 2020.0)
        check(flags(r) == (True, 2.0, True, False),
              f"past the end: antenna TYPE change in gap, firmware-only ignored {flags(r)}")
        check((r.segment_start_year, r.segment_end_year) == (2010.0, 2018.0),
              "segment years of the selected occupation")
        check(r.ecef == _lin(st1.occupations[0], 2020.0) and not r.in_window,
              "the coordinate itself is the unchanged extrapolation")
        r = R(reg, "EXA100USA", 2019.0)
        check(flags(r) == (True, 1.0, False, False), f"change after t is outside the gap {flags(r)}")
        r = R(reg, "EXA100USA", 2015.0)
        check(flags(r) == (False, 0.0, False, False) and r.in_window,
              "covered epoch: all diagnostics False / 0.0")
        check((r.segment_start_year, r.segment_end_year) == (2010.0, 2018.0),
              "covered epoch still reports its segment")
        # Window boundaries: start <= t < end, so t == end is extrapolated by 0.0 yr.
        r = R(reg, "EXA100USA", 2018.0)
        check(flags(r) == (True, 0.0, False, False) and not r.in_window
              and (r.segment_start_year, r.segment_end_year) == (2010.0, 2018.0),
              f"t == end: extrapolated, 0.0 yr {flags(r)}")
        r = R(reg, "EXA400USA", 2015.0)
        check(flags(r) == (False, 0.0, False, False) and r.in_window,
              f"t == start: covered {flags(r)}")
        # Past the end, the gap closes at t itself (no tolerance on the t side).
        t = dy(2019, 5, 31)
        r = R(reg, "EXA100USA", t)
        check(flags(r) == (True, t - 2018.0, False, False),
              f"past the end: antenna change 1 day after t is outside the gap {flags(r)}")
        r = R(reg, "EXA100USA", dy(2019, 6, 2))
        check(flags(r)[2:] == (True, False),
              f"past the end: antenna change 1 day before t is inside the gap {flags(r)}")
        r = R(reg, "EXA700USA", dy(2018, 5, 31))
        check(flags(r)[2:] == (False, False),
              f"past the end: receiver change 1 day after t is outside the gap {flags(r)}")
        r = R(reg, "EXA700USA", dy(2018, 6, 2))
        check(flags(r)[2:] == (False, True),
              f"past the end: receiver change 1 day before t is inside the gap {flags(r)}")
        # Before the start, the gap opens at t itself.
        t = dy(2012, 1, 2)
        r = R(reg, "EXA900USA", t)
        check(flags(r) == (True, t - 2015.0, False, False),
              f"before the start: changes 1 day before t are outside the gap {flags(r)}")
        t = dy(2011, 12, 31)
        r = R(reg, "EXA900USA", t)
        check(flags(r) == (True, t - 2015.0, True, True),
              f"before the start: changes 1 day after t are inside the gap {flags(r)}")

        r = R(reg, "EXA200USA", 2019.3)
        check(flags(r)[2:] == (False, False),
              f"record-only re-log and serial-only receiver swap are not changes {flags(r)}")
        r = R(reg, "EXA200USA", 2020.0)
        check(flags(r)[2:] == (True, False), f"antenna SERIAL_NUMBER change counts {flags(r)}")

        r = R(reg, "EXA300USA", 2018.5)
        check(flags(r)[2:] == (True, False),
              f"radome-only change 1 day before the end counts; 7 days does not {flags(r)}")

        r = R(reg, "EXA400USA", 2012.0)
        check(flags(r) == (True, -3.0, True, False),
              f"before the first segment: negative years, change 1 day after start {flags(r)}")
        check((r.segment_start_year, r.segment_end_year) == (2015.0, 9999.0),
              "open segment end reported as the 9999.0 sentinel")

        r = R(reg, "EXA500USA", 2012.0)
        check(flags(r) == (False, 0.0, False, False), "covering mode: 2012.0 is covered")
        r = R(reg, "EXA500USA", 2012.0, "latest")
        check(flags(r) == (False, 0.0, False, False) and r.segment_start_year == 2016.0
              and not r.in_window,
              f"latest mode: an epoch covered by the earlier occupation is not extrapolated "
              f"{flags(r)}")
        r = R(reg, "EXA500USA", 2015.5)
        check(r.extrapolated and r.antenna_change_in_gap and r.extrapolation_years == 2015.5 - 2015.0,
              "covering mode: between segments, nearest-midpoint pick past its end")
        # The antenna change on 2015-07-01 precedes t = 2015.5, so it lies in the
        # covering pick's gap [2015 - tol, t] but not in the latest's [t, 2016 + tol].
        r = R(reg, "EXA500USA", 2015.5, "latest")
        check(flags(r) == (True, -0.5, False, False) and r.segment_start_year == 2016.0,
              f"latest mode: uncovered epoch measured from the latest occupation {flags(r)}")
        r = R(reg, "EXA100USA", 2020.0, "latest")
        check(flags(r) == (True, 2.0, True, False),
              f"latest mode, single occupation: same diagnostics as covering {flags(r)}")

        r = R(reg, "EXA600USA", 2014.0)
        check(flags(r) == (True, 2014.0 - 2015.0, False, False),
              f"first window inside the gap is not a change {flags(r)}")

        r = R(reg, "EXA700USA", 2019.0)
        check(flags(r)[2:] == (False, True), f"receiver TYPE change counts {flags(r)}")

        r = R(reg, "EXA800USA", 2020.0)
        check(not r.valid and flags(r) == (False, 0.0, False, False),
              "no usable occupation: invalid, diagnostics at defaults")

        r = R(reg, "EXC100USA", 2020.0)
        check(flags(r) == (True, 2.0, False, False) and r.segment_end_year == 2018.0,
              f"a flagged window covering t does not count as covered {flags(r)}")


def test_periodic_synthetic():
    print("[10] PERIODIC seasonal term outside its block windows (synthetic SIF)")
    P = (4000000.0, 1000000.0, 4800000.0)

    def blk(start, end, k):
        """A periodic block whose amplitudes are k mm, so the block is identifiable."""
        return {"START": _ts(*start), "END": OPEN if end is None else _ts(*end),
                "FREQUENCIES": [{"PERIOD": 1.0, "X_COS": 1.0 * k, "X_SIN": -0.75 * k,
                                 "Y_COS": -0.5 * k, "Y_SIN": 1.5 * k,
                                 "Z_COS": 0.25 * k, "Z_SIN": 2.0 * k}]}

    b1, b2, b3 = (blk((2010,), (2014,), 1.0), blk((2015,), (2019,), 2.0),
                  blk((2019,), (2022,), 3.0))
    mk = lambda sid, blocks: _station(sid, [_occ(_ts(1990), OPEN, P)], periodic=blocks)
    stations = [
        mk("PER100USA", [b1, b2, b3]),
        mk("PER200USA", [b2, b3, b1]),                       # file order: not chronological
        mk("PER300USA", [b3, b1, b2]),
        mk("PER400USA", [blk((2010,), (2014,), 1.0), blk((2012,), (2018,), 2.0)]),  # overlap
        mk("PER500USA", [blk((2010,), None, 1.0)]),          # open-ended
        mk("PER600USA", []),                                 # no block
    ]
    expect = lambda block, t: _block_term(block, t)
    with _synthetic_registry((2020.0, stations)) as (reg, _paths):
        for sid in ("PER100USA", "PER200USA", "PER300USA"):
            st = reg.find(sid)
            for t, want_blk, want_reach in (
                    (2008.5, b1, "before_first"), (2012.0, b1, "covered"),
                    (2014.0, b1, "in_gap"), (2014.6, b1, "in_gap"), (2015.0, b2, "covered"),
                    (2019.0, b3, "covered"), (2021.5, b3, "covered"),
                    (2022.0, b3, "after_last"), (2023.25, b3, "after_last")):
                r = sp.resolve_station_coordinate(st, t, True, False)
                nop = sp.resolve_station_coordinate(st, t, False, False)
                got = tuple(r.ecef[k] - nop.ecef[k] for k in range(3))
                check(r.periodic_reach == want_reach,
                      f"{sid}@{t}: reach {r.periodic_reach} (want {want_reach})")
                check(math.dist(got, expect(want_blk, t)) < 5e-9,
                      f"{sid}@{t}: the {want_reach} case evaluates the right block")
        st = reg.find("PER100USA")
        r = sp.resolve_station_coordinate(st, 2008.5, True, False)
        check(abs(r.periodic_extrapolation_years - (2008.5 - 2010.0)) < 1e-12
              and r.periodic_block_start_year == 2010.0, "before_first: signed years, first block")
        r = sp.resolve_station_coordinate(st, 2023.25, True, False)
        check(abs(r.periodic_extrapolation_years - 1.25) < 1e-12
              and r.periodic_block_end_year == 2022.0, "after_last: years past the end, last block")
        r = sp.resolve_station_coordinate(st, 2014.6, True, False)
        check(abs(r.periodic_extrapolation_years - 0.6) < 1e-12
              and r.periodic_block_start_year == 2010.0, "in_gap: years past the earlier block's end")
        r = sp.resolve_station_coordinate(st, 2014.0, True, False)
        check(r.periodic_reach == "in_gap" and r.periodic_extrapolation_years == 0.0,
              "the window end is exclusive: in the gap, 0.0 yr")
        r = sp.resolve_station_coordinate(st, 2012.0, True, False)
        check(r.periodic_extrapolation_years == 0.0 and r.periodic_block_end_year == 2014.0,
              "covered: 0.0 yr, bounds of the covering block")

        # The term is continuous where the last window ends (the old rule dropped it).
        eps = 1e-6
        a = sp.resolve_station_coordinate(st, 2022.0 - eps, True, False).ecef
        z = sp.resolve_station_coordinate(st, 2022.0 + eps, True, False).ecef
        n = sp.resolve_station_coordinate(st, 2022.0, False, False).ecef
        step = math.dist(a, z)
        size = math.dist(a, n)
        check(size > 1e-3 and step < 5e-7,
              f"no step where the last window ends: term {size * 1e3:.2f} mm, step {step * 1e3:.5f} mm")

        # Overlapping windows are not summed: the first in start order only.
        r = sp.resolve_station_coordinate(reg.find("PER400USA"), 2013.0, True, False)
        nop = sp.resolve_station_coordinate(reg.find("PER400USA"), 2013.0, False, False)
        got = tuple(r.ecef[k] - nop.ecef[k] for k in range(3))
        check(r.periodic_reach == "covered" and r.periodic_block_start_year == 2010.0
              and math.dist(got, expect(blk((2010,), (2014,), 1.0), 2013.0)) < 5e-9,
              "overlap: only the first block, not the sum")

        r = sp.resolve_station_coordinate(reg.find("PER500USA"), 2060.0, True, False)
        check(r.periodic_reach == "covered" and r.periodic_block_end_year == 9999.0,
              "an open-ended last block covers every later epoch")

        for sid, flag in (("PER100USA", False), ("PER600USA", True)):
            r = sp.resolve_station_coordinate(reg.find(sid), 2023.0, flag, False)
            check(r.periodic_reach == "not_evaluated" and r.periodic_extrapolation_years == 0.0,
                  f"{sid}: switched off or without blocks -> not_evaluated")
        check(sp.format_periodic_warning("PER100USA",
                                         sp.resolve_station_coordinate(reg.find("PER100USA"), 2012.0),
                                         2012.0) == "",
              "a covered epoch has no periodic warning text")
        w = sp.format_periodic_warning("PER100USA",
                                       sp.resolve_station_coordinate(reg.find("PER100USA"), 2023.25),
                                       2023.25)
        check("1.250 yr after the last periodic block [2019.0000, 2022.0000)" in w
              and w.startswith("PER100USA: periodic term at 2023.250000"), f"after_last warning: {w!r}")
        w = sp.format_periodic_warning("PER100USA",
                                       sp.resolve_station_coordinate(reg.find("PER100USA"), 2008.5),
                                       2008.5)
        check("1.500 yr before the first periodic block [2010.0000, 2014.0000)" in w,
              f"before_first warning: {w!r}")
        w = sp.format_periodic_warning("PER100USA",
                                       sp.resolve_station_coordinate(reg.find("PER100USA"), 2014.6),
                                       2014.6)
        check("0.600 yr after the periodic block [2010.0000, 2014.0000) and before the next one" in w,
              f"in_gap warning: {w!r}")


def _block_term(block, t, ref=2020.0):
    """Seasonal ECEF displacement of one raw SIF periodic block (mm -> m) at ``t``."""
    out = [0.0, 0.0, 0.0]
    for f in block["FREQUENCIES"]:
        w = 2.0 * sp._PI / f["PERIOD"]
        c, s = math.cos(w * (t - ref)), math.sin(w * (t - ref))
        out[0] += 1e-3 * (f["X_COS"] * c + f["X_SIN"] * s)
        out[1] += 1e-3 * (f["Y_COS"] * c + f["Y_SIN"] * s)
        out[2] += 1e-3 * (f["Z_COS"] * c + f["Z_SIN"] * s)
    return tuple(out)


def test_cli_synthetic():
    print("[8] CLI warnings and appended columns (synthetic SIF)")
    P_A = (-2491490.0, -4660803.0, 3559129.0)
    stations = [
        _station("ABCD00USA", [_occ(_ts(2000), OPEN, P_A)], approx=P_A),
        _station("EXA100USA", [_occ(_ts(2010), _ts(2018), P_A)],
                 antennas=[_ant(_ts(2005), _ts(2019), "T1              NONE", "1"),
                           _ant(_ts(2019), OPEN, "T2              NONE", "2")],
                 receivers=[_rec(_ts(2005), OPEN, "RX")]),
        _station("EXB100USA", [_occ(_ts(2005), _ts(2015), P_A), _occ(_ts(2016), OPEN, P_A)]),
        # Covered occupation, closed periodic block: only the seasonal term is extrapolated.
        _station("PER100USA", [_occ(_ts(2000), OPEN, P_A)],
                 periodic=[{"START": _ts(2005), "END": _ts(2018),
                            "FREQUENCIES": [{"PERIOD": 1.0, "X_COS": 2.0}]}]),
    ]
    with _synthetic_registry((2020.0, stations)) as (_reg, paths):
        def run(*extra, epoch="2020.0", keys=("EXA100USA", "ABCD00XYZ", "abcd")):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = sp.main(["--sif", *paths, "--epoch", epoch, *extra, *keys])
            return rc, out.getvalue(), err.getvalue()

        rc, out, err = run()
        check(rc == 0, "CLI exit code 0")
        check("# WARNING: SIF lookup: 'ABCD00XYZ'" in err and "ABCD00USA" in err,
              f"rejected prefix candidate warned on stderr: {err!r}")
        check("# WARNING: EXA100USA: epoch 2020.000000 is 2.000 yr past the end" in err,
              f"extrapolated station warned on stderr: {err!r}")
        check("antenna change in gap: yes; receiver change in gap: no" in err,
              "extrapolation warning states the equipment changes")
        check("ABCD00USA: epoch" not in err, "covered station is not warned")
        check(not logging.getLogger("sif_propagate").handlers,
              "CLI removes its stderr handler on return")

        rows = list(csv.reader(io.StringIO(out)))
        check(rows[0] == HISTORICAL_CSV_COLUMNS + EXTRAPOLATION_CSV_COLUMNS + PERIODIC_CSV_COLUMNS,
              "CSV header: historical columns, then the extrapolation and periodic columns")
        byreq = {row[0]: dict(zip(rows[0], row)) for row in rows[1:]}
        e = byreq.get("EXA100USA", {})
        check((e.get("extrapolated"), e.get("extrapolation_years"), e.get("segment_start_year"),
               e.get("segment_end_year"), e.get("antenna_change_in_gap"),
               e.get("receiver_change_in_gap"))
              == ("True", "2.000000", "2010.000000", "2018.000000", "True", "False"),
              f"CSV extrapolation cells: {e}")
        check(byreq.get("ABCD00XYZ", {}).get("found") == "False", "rejected key -> found=False")
        a = byreq.get("abcd", {})
        check(a.get("found") == "True" and a.get("extrapolated") == "False"
              and a.get("segment_end_year") == "", "bare code row: covered, open segment blank")

        # Periodic block diagnostics: the warning, the appended cells, the switch.
        rc, out, err = run(keys=("PER100USA", "ABCD00USA"))
        check("# WARNING: PER100USA: periodic term at 2020.000000: epoch is 2.000 yr after the "
              "last periodic block [2005.0000, 2018.0000)" in err,
              f"extrapolated periodic block warned on stderr: {err!r}")
        check("PER100USA: epoch" not in err, "the occupation is covered: no occupation warning")
        check("ABCD00USA: periodic" not in err, "a station without blocks is not warned")
        rows = list(csv.reader(io.StringIO(out)))
        byreq = {row[0]: dict(zip(rows[0], row)) for row in rows[1:]}
        p = byreq.get("PER100USA", {})
        check((p.get("periodic_reach"), p.get("periodic_extrapolation_years"),
               p.get("periodic_block_start_year"), p.get("periodic_block_end_year"),
               p.get("extrapolated"))
              == ("after_last", "2.000000", "2005.000000", "2018.000000", "False"),
              f"CSV periodic cells: {p}")
        a = byreq.get("ABCD00USA", {})
        check((a.get("periodic_reach"), a.get("periodic_block_start_year"),
               a.get("periodic_block_end_year")) == ("not_evaluated", "", ""),
              f"no-block row: not_evaluated, blank bounds: {a}")
        rc, out, err = run("--no-periodic", keys=("PER100USA",))
        check("periodic term" not in err, f"--no-periodic draws no periodic warning: {err!r}")
        rc, out, err = run("--format", "json", keys=("PER100USA",))
        pj = {r["requested"]: r for r in json.loads(out)["stations"]}.get("PER100USA", {})
        check(pj.get("periodic_reach") == "after_last" and pj.get("periodic_extrapolation_years") == 2.0
              and pj.get("periodic_block_end_year") == 2018.0, f"JSON periodic fields: {pj}")

        rc, out, err = run("--format", "json")
        recs = {r["requested"]: r for r in json.loads(out)["stations"]}
        e = recs.get("EXA100USA", {})
        check(e.get("extrapolated") is True and e.get("extrapolation_years") == 2.0
              and e.get("segment_end_year") == 2018.0 and e.get("antenna_change_in_gap") is True
              and e.get("receiver_change_in_gap") is False, f"JSON extrapolation fields: {e}")
        check(recs.get("abcd", {}).get("segment_end_year", "missing") is None,
              "JSON open segment end is null")

        # --datasheet (latest mode): an epoch covered by an earlier occupation is
        # not warned; an epoch no window covers is, measured from the latest.
        rc, out, err = run("--datasheet", epoch="2010.0", keys=("EXB100USA",))
        rows = list(csv.reader(io.StringIO(out)))
        b = dict(zip(rows[0], rows[1])) if len(rows) > 1 else {}
        check(rc == 0 and "EXB100USA: epoch" not in err,
              f"datasheet run on a covered epoch is not warned: {err!r}")
        check((b.get("occupation_mode"), b.get("in_window"), b.get("extrapolated"))
              == ("latest", "False", "False"),
              f"datasheet row: latest window does not contain t, not extrapolated: {b}")
        rc, out, err = run("--datasheet", epoch="2015.5", keys=("EXB100USA",))
        check("# WARNING: EXB100USA: epoch 2015.500000 is 0.500 yr before the start" in err
              and "(latest)" in err,
              f"datasheet run on an uncovered epoch is warned: {err!r}")


def test_real_files(reg):
    """[9] The three defects on the shipped SIFs; each check skips if its station is absent."""
    print("[9] REAL the defects on the shipped SIF files")

    def have(*ids):
        missing = [i for i in ids if reg.find(i) is None or reg.find(i).id != i]
        if missing:
            print(f"    SKIP: {', '.join(missing)} not in the loaded SIF(s)")
        return not missing

    with _capture_sif_log() as cap:
        # D1: GAMB00PYF (Gambier, absent) must not resolve to GAMB00USA (7,688 km).
        if have("GAMB00USA"):
            gusa = reg.find("GAMB00USA")
            _expect_lookup(reg, cap, "GAMB00PYF", None, None, "GAMB00PYF without position",
                           warn_contains=["GAMB00PYF", "GAMB00USA"])
            _expect_lookup(reg, cap, "GAMB00PYF", gusa.approximate_coordinates, "GAMB00USA",
                           "GAMB00PYF at GAMB00USA's own position")
            gambier = (-4147127.4937, -4152221.6682, -2490032.7642)
            d = math.dist(gambier, gusa.approximate_coordinates)
            check(7.6e6 < d < 7.8e6, f"Gambier to GAMB00USA {d / 1e3:.0f} km")
            _expect_lookup(reg, cap, "GAMB00PYF", gambier, None, "GAMB00PYF at Gambier",
                           warn_contains=["GAMB00USA", f"{d:.1f} m"])
        # D1: STHL00GBR -> STHL00SHN is a rename on the same monument.
        if have("STHL00SHN"):
            shn = reg.find("STHL00SHN")
            _expect_lookup(reg, cap, "STHL00GBR", None, None, "STHL00GBR without position",
                           warn_contains=["STHL00GBR", "STHL00SHN"])
            _expect_lookup(reg, cap, "STHL00GBR", shn.approximate_coordinates, "STHL00SHN",
                           "STHL00GBR with STHL00SHN's position")
        if have("P38900USA"):
            _expect_lookup(reg, cap, "P389", None, "P38900USA", "bare P389")
        if have("CIT100USA"):
            _expect_lookup(reg, cap, "cit1", None, "CIT100USA", "bare cit1")
            _expect_lookup(reg, cap, "CIT100USA", (0.0, 0.0, 6.4e6), "CIT100USA",
                           "exact CIT100USA ignores a distant position")

    # D3: exactly four occupations sit on 2010.0, all on single-occupation NCN records.
    off = sorted(s.id for s in reg.stations for o in s.occupations
                 if o.reference_epoch_year != 2020.0)
    if have("AZAZ00USA", "AZOR00USA", "AZPL00USA", "FLSI00USA"):
        check(off == ["AZAZ00USA", "AZOR00USA", "AZPL00USA", "FLSI00USA"],
              f"occupations off 2020.0: {off}")
        az = reg.find("AZAZ00USA")
        o = az.occupations[0]
        check(o.reference_epoch_year == 2010.0, "AZAZ00USA occupation REFERENCE_EPOCH 2010.0")
        # The shipped record is DO_NOT_USE, so the unchanged selection rule
        # yields no coordinate; D3 shows only once the flag is cleared.
        check(o.do_not_use and not sp.resolve_station_coordinate(az, 2024.35).valid,
              "AZAZ00USA as shipped: DO_NOT_USE, no resolved coordinate")
        usable = copy.deepcopy(az)
        usable.occupations[0].do_not_use = False
        r = sp.resolve_station_coordinate(usable, 2024.35, True, True)
        expect = tuple(o.coordinates[k] + o.velocity[k] * (2024.35 - 2010.0) for k in range(3))
        check(r.valid and r.ecef == expect,
              "AZAZ00USA (flag cleared) @2024.35 = coords + v * (2024.35 - 2010.0), exact")
        old = tuple(o.coordinates[k] + o.velocity[k] * (2024.35 - 2020.0) for k in range(3))
        d = math.dist(r.ecef, old)
        check(0.11 < d < 0.16, f"AZAZ00USA: the 2020.0 assumption was off by {d * 100:.1f} cm")

    # D2: closed last segments.
    if have("NIUM00NIU"):
        r = sp.resolve_station_coordinate(reg.find("NIUM00NIU"), 2024.35, True, True)
        check(r.valid and r.extrapolated and 0.3 < r.extrapolation_years < 0.35,
              f"NIUM00NIU @2024.35 extrapolated {r.extrapolation_years:.3f} yr")
        check(r.antenna_change_in_gap and not r.receiver_change_in_gap,
              "NIUM00NIU: TRM59800.00 -> JAVRINGANT_DM in the gap, no receiver TYPE change")
        r = sp.resolve_station_coordinate(reg.find("NIUM00NIU"), 2010.0, True, True)
        check(r.valid and not r.extrapolated and not r.antenna_change_in_gap,
              "NIUM00NIU @2010.0 covered")
    if have("CHUR00CAN"):
        r = sp.resolve_station_coordinate(reg.find("CHUR00CAN"), 2024.35, True, True)
        check(r.valid and r.extrapolated and r.receiver_change_in_gap
              and not r.antenna_change_in_gap,
              "CHUR00CAN @2024.35: TPS NET-G3A -> SEPT POLARX5 in the gap (2-day tolerance)")
    if have("CIT100USA"):
        r = sp.resolve_station_coordinate(reg.find("CIT100USA"), 2024.35, True, True)
        check(r.valid and not r.extrapolated and r.segment_end_year == 9999.0,
              "CIT100USA @2024.35 covered by its open segment")

    # Seasonal term past the last periodic window: the 8 stations of the 2024-129
    # field golden roster (epoch 2024 + 128.5 / 366).  Offsets are the term now
    # applied; the summing rule applied none of it (every epoch is past all windows).
    golden = 2024.0 + (129.0 - 0.5) / 366.0
    roster = (  # id, seasonal XYZ [m], years past the last window end, block window
        ("BSHM00ISR", (0.000390350, -0.000331744, 0.000834791), 0.359312, 2023.101370, 2023.991781),
        ("PALM00ATA", (-0.000266316, -0.001007491, -0.000626162), 0.331968, 2020.568306, 2024.019125),
        ("NIUM00NIU", (-0.000479490, -0.000263441, 0.000028151), 0.331968, 2022.950685, 2024.019125),
        ("CHUR00CAN", (-0.001482611, 0.004894193, -0.003925403), 0.230875, 2010.671233, 2024.120218),
        ("MAJU00MHL", (-0.001004803, -0.000469438, 0.000000759), 3.353826, 2015.441096, 2020.997267),
        ("CPVG00CPV", (0.000651877, -0.001013427, -0.000729831), 0.214481, 2023.635616, 2024.136612),
        ("URUM00CHN", (0.000981005, 0.000920497, 0.002439989), 0.211749, 2018.767123, 2024.139344),
        ("WROC00POL", (0.000022002, -0.000243837, 0.000364945), 0.225411, 2012.808743, 2024.125682),
    )
    if have(*[row[0] for row in roster]):
        for sid, xyz, yrs, b0, b1 in roster:
            st = reg.find(sid)
            r = sp.resolve_station_coordinate(st, golden, True, False)
            nop = sp.resolve_station_coordinate(st, golden, False, False)
            got = tuple(r.ecef[k] - nop.ecef[k] for k in range(3))
            check(all(golden < b.start_year or golden >= b.end_year for b in st.periodic),
                  f"{sid}: no periodic window covers the golden epoch (the old rule applied nothing)")
            check(r.periodic_reach == "after_last" and abs(r.periodic_extrapolation_years - yrs) < 5e-7
                  and abs(r.periodic_block_start_year - b0) < 5e-7
                  and abs(r.periodic_block_end_year - b1) < 5e-7,
                  f"{sid}: after_last, {r.periodic_extrapolation_years:.6f} yr past the last block")
            check(math.dist(got, xyz) < 5e-9, f"{sid}: seasonal term {got} vs {xyz}")
        chur = reg.find("CHUR00CAN")
        mm = lambda sid: 1e3 * math.dist(
            sp.resolve_station_coordinate(reg.find(sid), golden, True, False).ecef,
            sp.resolve_station_coordinate(reg.find(sid), golden, False, False).ecef)
        check(abs(mm("CHUR00CAN") - 6.447) < 0.005 and abs(mm("URUM00CHN") - 2.786) < 0.005,
              f"CHUR00CAN {mm('CHUR00CAN'):.3f} mm, URUM00CHN {mm('URUM00CHN'):.3f} mm")


def _psd_block(event, east=None, north=None, up=None):
    """One raw POST_SEISMIC record; each component is a dict of AEXP/TEXP/ALOG/TLOG lists."""
    pj = {"EVENT_EPOCH": event}
    for key, comp in (("EAST", east), ("NORTH", north), ("UP", up)):
        if comp is not None:
            pj[key] = comp
    return pj


def _psd_ref(comp, dT):
    """One component's relaxation since an event, from the definition (metres)."""
    if comp is None or dT <= 0.0:
        return 0.0
    d = 0.0
    for a, t in zip(comp.get("ALOG", []), comp.get("TLOG", [])):
        d += a * math.log(1.0 + dT / t)
    for a, t in zip(comp.get("AEXP", []), comp.get("TEXP", [])):
        d += a * (1.0 - math.exp(-dT / t))
    return d


def _psd_enu_ref(blocks, year):
    """Accumulated post-seismic (E, N, U) at ``year`` for raw blocks, from the definition."""
    out = [0.0, 0.0, 0.0]
    for pj in blocks:
        dT = year - sp.decimal_year_from_sif(pj["EVENT_EPOCH"])
        for k, key in enumerate(("EAST", "NORTH", "UP")):
            out[k] += _psd_ref(pj.get(key), dT)
    return out


def _enu_of(ecef, base, ref):
    """Local (E, N, U) at ``ref`` of the displacement ``ecef - base``."""
    lat, lon, _h = sp.ecef_to_llh(*ref)
    R = sp.enu_to_ecef_matrix(lat, lon)
    return sp._mat3_vec(sp._mat3_transpose(R), tuple(ecef[k] - base[k] for k in range(3)))


def test_psd_synthetic():
    print("[11] PSD post-seismic term relative to the reference epoch (synthetic SIF)")
    P = (4000000.0, 1000000.0, 4800000.0)
    quake_a = _psd_block(
        "2010-01-01T00:00:00 UTC",
        east={"AEXP": [0.020, 0.005], "TEXP": [4.0, 0.2], "ALOG": [0.010], "TLOG": [0.5]},
        north={"ALOG": [-0.030], "TLOG": [1.0]},
        up={"AEXP": [0.004], "TEXP": [2.0]})
    quake_b = _psd_block("2021-01-01T00:00:00 UTC", east={"AEXP": [0.050], "TEXP": [1.5]})
    quake_c = _psd_block("2005-01-01T00:00:00 UTC", north={"AEXP": [0.040], "TEXP": [3.0]})
    stations = [
        _station("PSD100USA", [_occ(_ts(1990), OPEN, P)], psd=[quake_a]),
        _station("PSD200USA", [_occ(_ts(1990), OPEN, P)], psd=[quake_a, quake_b]),
        _station("PSD300USA", [_occ(_ts(1990), OPEN, P, ref=2010.0)], psd=[quake_c]),
    ]
    tol = 1e-7   # ECEF round trip of a 6e6 m position
    with _synthetic_registry((2020.0, stations)) as (reg, _paths):
        s1 = reg.find("PSD100USA")
        lin = sp.resolve_station_coordinate(s1, 2024.0, False, False).ecef

        def term(st, t):
            full = sp.resolve_station_coordinate(st, t, False, True).ecef
            base = sp.resolve_station_coordinate(st, t, False, False).ecef
            return _enu_of(full, base, base)

        # At the reference epoch the term vanishes: the reference coordinate holds
        # everything accumulated by then.
        at_ref = sp.resolve_station_coordinate(s1, 2020.0, False, True).ecef
        check(at_ref == sp.resolve_station_coordinate(s1, 2020.0, False, False).ecef,
              "the post-seismic term is exactly zero at the reference epoch")
        check(sp.resolve_station_coordinate(s1, 2024.0, False, False).ecef == lin,
              "with_psd=False leaves the linear coordinate")

        # After the reference epoch: the change since the reference epoch, not the
        # relaxation since the event.
        for t in (2022.0, 2024.0, 2030.0):
            got = term(s1, t)
            want = [a - b for a, b in zip(_psd_enu_ref([quake_a], t), _psd_enu_ref([quake_a], 2020.0))]
            check(max(abs(got[k] - want[k]) for k in range(3)) < tol,
                  f"PSD100USA@{t}: change since t_ref {tuple(round(v, 6) for v in got)} "
                  f"vs {tuple(round(v, 6) for v in want)}")
        full_now = _psd_enu_ref([quake_a], 2024.0)
        got = term(s1, 2024.0)
        check(abs(got[0] - full_now[0]) > 1e-3,
              "the old rule (full relaxation since the event) differs by more than a millimetre here")

        # Before the reference epoch the change is negative.
        got = term(s1, 2015.0)
        want = [a - b for a, b in zip(_psd_enu_ref([quake_a], 2015.0), _psd_enu_ref([quake_a], 2020.0))]
        check(max(abs(got[k] - want[k]) for k in range(3)) < tol and got[1] > 0.0,
              f"PSD100USA@2015: negative change (north {got[1] * 1e3:.2f} mm)")

        # Before the event: the term is minus the displacement accumulated by t_ref
        # (the position is the pre-event linear one), a reference coordinate that
        # also precedes the event is handled by the same rule.
        got = term(s1, 2005.0)
        want = [-v for v in _psd_enu_ref([quake_a], 2020.0)]
        check(max(abs(got[k] - want[k]) for k in range(3)) < tol,
              f"PSD100USA@2005 (before the event): {tuple(round(v, 6) for v in got)}")

        # An earthquake after the reference epoch has nothing to subtract: the term
        # is its full relaxation, zero until it happens.
        s2 = reg.find("PSD200USA")
        got = term(s2, 2020.5)
        want = [a - b for a, b in zip(_psd_enu_ref([quake_a, quake_b], 2020.5),
                                      _psd_enu_ref([quake_a, quake_b], 2020.0))]
        check(max(abs(got[k] - want[k]) for k in range(3)) < tol,
              "PSD200USA@2020.5: the later event has not happened, the earlier one relaxes on")
        got = term(s2, 2023.0)
        want = [a - b for a, b in zip(_psd_enu_ref([quake_a, quake_b], 2023.0),
                                      _psd_enu_ref([quake_a, quake_b], 2020.0))]
        later = _psd_enu_ref([quake_b], 2023.0)
        check(max(abs(got[k] - want[k]) for k in range(3)) < tol and later[0] > 0.03,
              f"PSD200USA@2023: the later event enters in full ({later[0] * 1e3:.2f} mm east)")

        # The reference epoch is the occupation's own.
        s3 = reg.find("PSD300USA")
        got = term(s3, 2024.0)
        want = [a - b for a, b in zip(_psd_enu_ref([quake_c], 2024.0), _psd_enu_ref([quake_c], 2010.0))]
        check(max(abs(got[k] - want[k]) for k in range(3)) < tol,
              f"PSD300USA@2024 (REFERENCE_EPOCH 2010.0): {tuple(round(v, 6) for v in got)}")
        check(sp.resolve_station_coordinate(s3, 2010.0, False, True).ecef
              == sp.resolve_station_coordinate(s3, 2010.0, False, False).ecef,
              "zero at the occupation's own reference epoch, not at the file epoch")


# The IGS weekly combined SINEX of GPS week 2313 (IGS0OPSSNX_20241260000_07D_07D_SOL.SNX): a
# mean position at 2024-05-08 12:00 (the epoch below), with formal 3D sigmas of 0.3 to 1.9 mm.
# The eight stations are those of the 2024-129 field golden roster that carry a post-seismic
# term at that epoch; the last column is the per-component horizontal tolerance in mm.  The
# tolerance is twice the 95th percentile (4.1 mm) of the same horizontal difference over 264
# stations without a post-seismic block, so it stands for weekly noise and unmodelled
# motion, not for the post-seismic term; NIUM00NIU is extrapolated 0.33 yr past its occupation
# across an antenna change and gets 12 mm.
SINEX_PSD_STATIONS = [
    ("GCGO00USA", (-2281621.77483, -1453585.12678, 5756964.86859), 8.0),
    ("SOLO00SLB", (-5911340.08837, 2156887.93540, -1038663.46655), 8.0),
    ("CUSV00THA", (-1132915.09704, 6092528.52228, 1504633.12884), 8.0),
    ("MAC100AUS", (-3464038.98262, 1334173.29737, -5169223.81332), 8.0),
    ("NIUM00NIU", (-5937161.02270, -1054674.91356, -2071385.86297), 12.0),
    ("COCO00AUS", (-741951.26822, 6190961.73679, -1337767.15664), 8.0),
    ("REYK00ISL", (2587383.90543, -1043033.59986, 5716564.19577), 8.0),
    ("MIZU00JPN", (-3857171.22964, 3108692.78119, 4004040.11358), 8.0),
]


def test_psd_sinex(reg):
    print("[12] PSD eight stations against the IGS weekly SINEX of 2024-05-08")
    t = dy(2024, 5, 8, 12, 0, 0)
    worst = 0.0
    for sid, snx, tol_mm in SINEX_PSD_STATIONS:
        st = reg.find(sid)
        if not check(st is not None and st.psd, f"{sid} found with a post-seismic block"):
            continue
        rc = sp.resolve_station_coordinate(st, t, True, True)
        nop = sp.resolve_station_coordinate(st, t, True, False)
        e, n, _u = (v * 1e3 for v in _enu_of(rc.ecef, snx, snx))
        worst = max(worst, abs(e), abs(n))
        check(abs(e) <= tol_mm and abs(n) <= tol_mm,
              f"{sid}: east {e:+.1f} mm, north {n:+.1f} mm against the SINEX (tolerance {tol_mm:.0f} mm)")
        # The term is the change since the reference epoch, millimetres to decimetres here.
        shift = math.dist(rc.ecef, nop.ecef)
        check(shift < 0.2, f"{sid}: post-seismic term {shift * 1e3:.1f} mm")
    print(f"    worst horizontal component {worst:.1f} mm")
    miz = reg.find("MIZU00JPN")
    if miz is not None:
        full = sp.resolve_station_coordinate(miz, t, True, True).ecef
        nop = sp.resolve_station_coordinate(miz, t, True, False).ecef
        check(0.10 < math.dist(full, nop) < 0.20,
              f"MIZU00JPN term {math.dist(full, nop) * 1e3:.0f} mm (the full relaxation is 1.64 m)")
    antc = reg.find("ANTC00CHL")
    if antc is not None:
        a = sp.resolve_station_coordinate(antc, 2024.36, True, True).ecef
        b = sp.resolve_station_coordinate(antc, 2024.36, True, False).ecef
        check(0.05 < math.dist(a, b) < 0.09,
              f"ANTC00CHL term {math.dist(a, b) * 1e3:.0f} mm at 2024.36 (0.66 m accumulated by 2020.0)")


def main():
    reg = load_registry()
    if reg is not None:
        test_oracle(reg)
        test_anchors(reg)
        test_selfcheck(reg)
        test_occupation_mode(reg)
    test_lookup_synthetic()
    test_epoch_synthetic()
    test_extrapolation_synthetic()
    test_cli_synthetic()
    if reg is not None:
        test_real_files(reg)
    test_periodic_synthetic()
    test_psd_synthetic()
    if reg is not None:
        test_psd_sinex(reg)
    print()
    if _fail:
        print(f"RESULT: FAIL ({_fail} of {_checks} check(s) failed)")
        return 1
    if reg is None:
        print(f"RESULT: INCOMPLETE ({_checks} fixture-free check(s) passed; "
              "sections 1-4, 9 and 12 need the SIF fixtures)")
        return 2
    print(f"RESULT: PASS (all {_checks} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
