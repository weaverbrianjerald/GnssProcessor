#!/usr/bin/env python3
"""Validate sif_propagate.py against the GnssProcessor C++ engine and unit tests.

Run from anywhere; it locates the bundled SIF fixtures the same way the tool does.

Three layers of check:
  1. ORACLE  - full resolved ECEF + propagated 3x3 covariance for 12 (station,
               epoch) cases, captured from the C++ resolveStationCoordinate itself
               (tests/test_station_info.cpp temp oracle). Exercises periodic (CIT),
               heavy post-seismic (ANTC ~0.5 m), multi-occupation + zero-cross-cov
               (AB02 @ 2024.36), and a no-nonlinear-term epoch (AMC4 @ 2018.5).
  2. ANCHORS - the exact numbers asserted in tests/test_station_info.cpp
               (parsing / unit normalisation / PSD & periodic coefficients).
  3. SELFCHK - internal identities (find() keys, dt=0 -> Cxx, covariance symmetry,
               ENU<->ECEF round-trip, time conversions).

Exit code 0 iff every check passes.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sif_propagate as sp  # noqa: E402

# ECEF agreement target [m]; libm rounding differences (MSVC vs CPython) keep the
# realised residual far below this.
TOL_ECEF = 1e-6
TOL_COV = 5e-12   # covariance entries are ~1e-6 m^2, printed to 12 decimals

# --- oracle captured from the C++ engine (tests/test_station_info.cpp) --------- #
# (station, epoch, full_xyz, lin_xyz, cov3x3_or_None)
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
     (1608538.009691408, -4816370.970016915, -3847798.256277910),
     (1608538.551865682, -4816370.539438507, -3847798.265112284),
     [[0.000002614535, -0.000000530337, 0.000000598358],
      [-0.000000530337, 0.000004042173, -0.000001791438],
      [0.000000598358, -0.000001791438, 0.000005151836]]),
    ("ANTC00CHL", 2024.36,
     (1608538.012558494, -4816371.011391849, -3847798.245259961),
     (1608538.577416177, -4816370.553907000, -3847798.251747113),
     [[0.000002852809, -0.000000818781, 0.000000386973],
      [-0.000000818781, 0.000005073534, -0.000001131949],
      [0.000000386973, -0.000001131949, 0.000005811371]]),
    ("ANTC00CHL", 2018.5,
     (1608538.017930656, -4816370.906463088, -3847798.278665302),
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
    ("AMC400USA", 2018.5,
     (-1248596.393444710, -4819428.201899112, 3976505.924241956),
     (-1248596.393444710, -4819428.201899112, 3976505.924241956),
     [[0.000000247937, 0.000000313064, -0.000000229829],
      [0.000000313064, 0.000001442945, -0.000000932929],
      [-0.000000229829, -0.000000932929, 0.000000906179]]),
]

_fail = 0


def check(cond, msg):
    global _fail
    if not cond:
        _fail += 1
        print(f"  FAIL: {msg}")
    return cond


def load_registry():
    paths = sp.default_sif_paths()
    if not paths:
        print("ERROR: no SIF fixtures found (need data/static/sif/*.json). "
              "Set $SIF_PATHS to run this test.", file=sys.stderr)
        sys.exit(2)
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


def main():
    reg = load_registry()
    test_oracle(reg)
    test_anchors(reg)
    test_selfcheck(reg)
    print()
    if _fail:
        print(f"RESULT: FAIL ({_fail} check(s) failed)")
        return 1
    print("RESULT: PASS (all checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
