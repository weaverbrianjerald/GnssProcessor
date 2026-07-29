#!/usr/bin/env python3
"""Independent reference oracle for HigherOrderIonosphere (I2 + I3).

Locks the May-2026 second/third-order ionosphere fixes:

  1. I2 (second-order, group/code) coefficient is 7527*c, NOT 7527*c/2.
     The /2 form is the *phase* coefficient; using it for the code delay
     made both code and phase I2 a factor of two too small.

  2. I3 (third-order, group/code) scales with the integral of N_e^2 along
     the ray, which the shape factor eta=0.66 relates to N_max*STEC -- it
     is NOT proportional to STEC^2.  The old STEC^2 form was ~2.3e5x too
     large (hundreds of metres instead of ~mm).

Methodology note (matches scripts/solid_tide_reference.py, nmf_reference.py):
the dipole magnetic-field constants are parsed straight out of
MagneticField.cpp so the B-field geometry matches the engine bit-for-bit
(MagneticField was audited clean -- it is not the thing under test).  The
I2/I3 coefficients here are derived INDEPENDENTLY from first principles and
the POINT reference (higher_order_iono/Class_Iono.cpp Iono_Second_Order /
Iono_Third_Order), so reverting either engine coefficient breaks the gtest.

First principles (quasi-longitudinal Appleton-Hartree):
    n_ph = 1 - X/2 - (1/2) X Y_L - (1/8) X^2 - ...
    X = A * N_e / f^2          (A = e^2/(4 pi^2 eps0 m_e) = 80.6 m^3/s^2)
    Y_L = (e B cos(theta)) / (2 pi m_e f)
  group delay term k -> (1-k) * phase term:
    I2_code = +(A e / (2 pi m_e)) * B.k_hat * STEC / f^3   = 7527*c * (...)
            (k=3 => group = -2*phase; phase coeff = A e/(4 pi m_e) = 7527*c/2)
    I3_code = +(3 A^2 / 8) * eta * N_max * STEC / f^4       (3 A^2/8 = 2437)
            (k=4 => group = -3*phase)
  phase = -1/2 * I2_code,  -1/3 * I3_code.

Run from the repo root:  python scripts/higher_order_iono_reference.py
Prints the golden i2_m / i3_m values pinned in tests/test_atmosphere_models.cpp.
"""

import math
import re
from pathlib import Path

# --- physics constants (canonical; independent of the engine) ---------------
C = 299792458.0                     # speed of light (m/s), matches Constants.hpp
TECU_TO_ELECTRONS = 1.0e16          # 1 TECU = 1e16 el/m^2

# Second-order GROUP (code) coefficient: 7527*c  (== A*e/(2 pi m_e), 2.2564e12).
KI2_CODE = 7527.0 * C
# Third-order GROUP (code): (3 A^2 / 8) * eta * N_max * STEC / f^4.
KI3_BASE = 2437.0                   # = 3 * 80.6^2 / 8
KI3_ETA = 0.66
# Hoque & Jakowski (2008) linear N_max(STEC) fit (also POINT Iono_Third_Order):
# anchors TEC=1.38e18<->N_max=6e12 and TEC=4.55e18<->N_max=20e12.
KNMAX_PER_STEC = (20.0 - 6.0) * 1.0e12 / ((4.55 - 1.38) * 1.0e18)


def parse_magnetic_field_constants():
    """Pull the dipole constants out of MagneticField.cpp so B matches exactly."""
    src = (Path(__file__).resolve().parent.parent /
           "src" / "core" / "atmosphere" / "MagneticField.cpp").read_text()

    def grab(name):
        m = re.search(r"constexpr\s+double\s+" + name + r"\s*=\s*([-0-9.eE+]+)\s*;", src)
        if not m:
            raise RuntimeError(f"could not parse {name} from MagneticField.cpp")
        return float(m.group(1))

    return {
        "pole_lat_deg": grab("kPoleLatDeg"),
        "pole_lon_deg": grab("kPoleLonDeg"),
        "b0_tesla": grab("kB0Tesla"),
        "ref_radius_m": grab("kRefEarthRadiusM"),
    }


def dipole_axis_ecef(mf):
    lat = math.radians(mf["pole_lat_deg"])
    lon = math.radians(mf["pole_lon_deg"])
    return (math.cos(lat) * math.cos(lon),
            math.cos(lat) * math.sin(lon),
            math.sin(lat))


def field_at_ecef(pos, mf):
    r = math.sqrt(sum(x * x for x in pos))
    rhat = tuple(x / r for x in pos)
    mhat = dipole_axis_ecef(mf)
    dot = sum(a * b for a, b in zip(mhat, rhat))
    scale = mf["b0_tesla"] * (mf["ref_radius_m"] / r) ** 3
    return tuple(scale * (mhat[i] - 3.0 * dot * rhat[i]) for i in range(3))


def higher_order_components(rx, sat, ipp_lat, ipp_lon, ipp_radius, stec_tecu, freq, meas, mf):
    cos_lat = math.cos(ipp_lat)
    ipp = (ipp_radius * cos_lat * math.cos(ipp_lon),
           ipp_radius * cos_lat * math.sin(ipp_lon),
           ipp_radius * math.sin(ipp_lat))
    B = field_at_ecef(ipp, mf)

    los = tuple(sat[i] - rx[i] for i in range(3))           # receiver -> satellite
    n = math.sqrt(sum(x * x for x in los))
    khat = tuple(x / n for x in los)
    b_dot_k = sum(B[i] * khat[i] for i in range(3))

    stec_el = stec_tecu * TECU_TO_ELECTRONS
    f3 = freq ** 3
    f4 = freq ** 4

    ne_max = KNMAX_PER_STEC * stec_el
    i2_code = KI2_CODE * stec_el * b_dot_k / f3
    i3_code = KI3_BASE * KI3_ETA * ne_max * stec_el / f4

    if meas == 'C':
        return i2_code, i3_code
    elif meas == 'L':
        return -0.5 * i2_code, -(1.0 / 3.0) * i3_code
    return 0.0, 0.0


def main():
    mf = parse_magnetic_field_constants()
    print("# MagneticField constants parsed from MagneticField.cpp:")
    for k, v in mf.items():
        print(f"#   {k} = {v!r}")

    # Fixed geometry (explicit ECEF literals -- the gtest uses the same ones).
    rx = (4.6e6, 0.8e6, 4.3e6)          # ~lat 40N lon 10E, on the surface
    sat = (1.5e7, 1.0e7, 2.0e7)         # an above-horizon line of sight
    ipp_lat = 0.70                      # rad (~40.1 N)
    ipp_lon = 0.18                      # rad (~10.3 E)
    ipp_radius = (6371.0 + 450.0) * 1.0e3
    freqs = {"L1": 1575.42e6, "L2": 1227.60e6}

    cases = [
        ("L1", "C", 50.0),
        ("L1", "L", 50.0),
        ("L2", "C", 50.0),
        ("L1", "C", 120.0),   # high TEC -> I2 ~ cm-scale
    ]
    print("\n# golden values for tests/test_atmosphere_models.cpp (i2_m, i3_m):")
    for band, meas, stec in cases:
        i2, i3 = higher_order_components(
            rx, sat, ipp_lat, ipp_lon, ipp_radius, stec, freqs[band], meas, mf)
        print(f"{{ \"{band}\", '{meas}', {stec:.1f}, {i2!r}, {i3!r} }},")


if __name__ == "__main__":
    main()
