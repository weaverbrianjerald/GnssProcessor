#!/usr/bin/env python3
"""First-principles check of the SIGN of the second-order ionospheric term (I2), card I2S.

Card: docs/refound/I2S_second_order_iono_sign_2026-10-01.md.  The engine
(HigherOrderIonosphere::slantHigherOrderComponents) and its oracle
(scripts/higher_order_iono_reference.py) model the code term as
+7527 c STEC (B.k)/f^3 with k the PROPAGATION direction (satellite to
receiver); until card I2S both used the receiver-to-satellite line of sight,
which inverts the term.  This script derives the sign without assuming the
Appleton-Hartree formula or any handedness naming from the literature:

  1. electron gyration sense about B (RK4 on m dv/dt = q v x B, q = -e);
  2. a driven electron in the real time domain (no complex convention):
     the susceptibility seen by a circular drive co-rotating with the
     electrons is -X/(1-Y), counter-rotating -X/(1+Y);
  3. the cold magnetized electron-plasma dielectric tensor, its plane-wave
     eigenmodes, and the time-domain rotation sense of each mode ABOUT k:
     the mode right-handed about k (IEEE RHCP) has
     n = 1 - X/2 - X Y cos(theta_k)/2 - X^2/8, theta_k = angle(k, B), under
     both time conventions exp(-iwt) and exp(+iwt);
  4. the group index of that mode by numerical differentiation: the
     second-order code excess is +7527 c STEC B cos(theta_k)/f^3, the phase
     excess -1/2 of it;
  5. the engine's pinned geometry (tests/test_atmosphere_models.cpp,
     HigherOrderIonosphereTest.MatchesCanonicalReference): the eigenmode
     solver's code I2 against the pinned golden;
  6. a handedness anchor independent of the IEEE-versus-optics naming: the
     repo's field-validated phase wind-up model
     (GnssCorrections::computePhaseWindUp; docs/cards/network-vehicle/
     WINDUP-FIELD-LADDER.md section 9.3, corrected sign best in post-fit
     phase RMS in 7 of 7 configurations) responds to an antenna rotation
     about k the way a physical crossed-dipole pair does only when the
     received field rotates right-handed about k.

Direction vocabulary: k_hat = propagation, satellite -> receiver;
u_hat = line of sight, receiver -> satellite = -k_hat.

Run from anywhere:  python scripts/higher_order_iono_sign_check.py
Exit status 0 when every check passes.  Needs numpy.
"""

import math
import re
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------- constants
E_CH = 1.602176634e-19       # elementary charge (C)
M_E = 9.1093837015e-31       # electron mass (kg)
EPS0 = 8.8541878128e-12      # vacuum permittivity (F/m)
C = 299792458.0              # speed of light (m/s)

F_L1 = 1575.42e6
F_L2 = 1227.60e6
NE = 1.0e12                  # el/m^3
BMAG = 3.0e-5                # T

Q = -E_CH                    # electron charge (signed)

# Pinned engine golden (L1 code I2, 50 TECU) after card I2S.
PINNED_L1_CODE_I2 = 0.007714137670442714

CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok)))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))


# ------------------------------------------------------- dielectric tensor
def eps_tensor(f, ne, B0vec, conv="A"):
    """Cold, collisionless, magnetized electron plasma, ions immobile.

    Linearized equation of motion:  m dv/dt = q (E + v x B0).
    Convention A, d/dt -> -i w:  -i w m v = q (E + v x B0)
    Convention B, d/dt -> +i w:  +i w m v = q (E + v x B0)
    With v x B0 = -[B0]x v ([a]x the cross-product matrix):
      A:  (-i w m I + q [B0]x) v = q E
      B:  (+i w m I + q [B0]x) v = q E
    J = N q v = sigma E, and Ampere's law gives
      A: eps = I + i sigma/(eps0 w),   B: eps = I - i sigma/(eps0 w).
    """
    w = 2.0 * math.pi * f
    Bx = np.array([[0.0, -B0vec[2], B0vec[1]],
                   [B0vec[2], 0.0, -B0vec[0]],
                   [-B0vec[1], B0vec[0], 0.0]])
    s = -1.0 if conv == "A" else +1.0
    M = s * 1j * w * M_E * np.eye(3) + Q * Bx
    mob = np.linalg.solve(M, Q * np.eye(3))          # v = mob @ E
    sigma = ne * Q * mob                              # J = sigma E
    if conv == "A":
        return np.eye(3) + 1j * sigma / (EPS0 * w)
    return np.eye(3) - 1j * sigma / (EPS0 * w)


# ---------------------------------------------------------- wave eigenmodes
def transverse_basis(khat):
    a = np.array([1.0, 0.0, 0.0]) if abs(khat[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    e1 = a - khat * khat.dot(a)
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(khat, e1)            # (e1, e2, khat) right-handed
    return e1, e2


def eigenmodes(f, ne, B0vec, khat, conv="A"):
    """Solve n^2 (k (k.E) - E) + eps E = 0 for the two transverse modes.

    Both time conventions reduce curl curl E = (w/c)^2 eps E to the same
    algebraic equation; the longitudinal component is eliminated to give a
    2x2 problem.  Returns a list of (n, E_hat 3-vector).
    """
    eps = eps_tensor(f, ne, B0vec, conv)
    e1, e2 = transverse_basis(khat)
    R = np.column_stack([e1, e2, khat])
    ep = R.T @ eps @ R
    Ett = ep[:2, :2]
    Etz = ep[:2, 2:3]
    Ezt = ep[2:3, :2]
    Ezz = ep[2, 2]
    Meff = Ett - (Etz @ Ezt) / Ezz
    vals, vecs = np.linalg.eig(Meff)
    out = []
    for i in range(2):
        n2 = vals[i]
        Et = vecs[:, i]
        Ez = -(Ezt @ Et)[0] / Ezz
        E3 = R @ np.array([Et[0], Et[1], Ez])
        res = n2 * (khat * khat.dot(E3) - E3) + eps @ E3
        assert np.linalg.norm(res) < 1e-9 * np.linalg.norm(E3), res
        out.append((np.sqrt(n2), E3))
    return out


def rotation_sense_about(Ehat, axis, conv="A", nsamp=64):
    """Mean (E(t) x dE/dt).axis over a period from the real time-domain field
    at a fixed point (conv A: E(t) = Re[E_hat exp(-iwt)], conv B: exp(+iwt)),
    normalised by mean |E|^2 w; positive = right-handed about axis.  Also the
    axial ratio of the transverse ellipse (1 = circular)."""
    w = 1.0
    ts = np.linspace(0.0, 2 * math.pi, nsamp, endpoint=False)
    sgn = -1.0 if conv == "A" else +1.0
    acc = 0.0
    norm = 0.0
    for t in ts:
        E = np.real(Ehat * np.exp(sgn * 1j * w * t))
        dE = np.real(Ehat * sgn * 1j * w * np.exp(sgn * 1j * w * t))
        acc += np.cross(E, dE).dot(axis)
        norm += E.dot(E) * w
    e1, e2 = transverse_basis(axis)
    Et = np.array([Ehat.dot(e1), Ehat.dot(e2)])
    pts = np.array([np.real(Et * np.exp(sgn * 1j * t)) for t in ts])
    cov = pts.T @ pts / len(ts)
    ev = np.sort(np.linalg.eigvalsh(cov))
    ar = math.sqrt(ev[1] / ev[0]) if ev[0] > 0 else float("inf")
    return acc / norm, ar


def XY(f, ne, bmag):
    X = (ne * E_CH ** 2 / (EPS0 * M_E)) / (2 * math.pi * f) ** 2
    Y = (E_CH * bmag / M_E) / (2 * math.pi * f)
    return X, Y


def rh_mode_index(f, ne, B0vec, khat, conv="A"):
    """(n, handedness, axial ratio) of the mode right-handed about khat."""
    best = None
    for n, E in eigenmodes(f, ne, B0vec, khat, conv):
        h, ar = rotation_sense_about(E, khat, conv)
        if best is None or h > best[1]:
            best = (n, h, ar)
    return best


# ------------------------------------------- single-electron gyration check
def electron_gyration_sense(B0vec, steps=4000):
    """RK4 on m dv/dt = q v x B0; mean (v x dv/dt).B0_hat (positive = right-handed about B0)."""
    wc = abs(Q) * np.linalg.norm(B0vec) / M_E
    dt = (2 * math.pi / wc) / 2000
    v = np.cross(B0vec, [1.0, 0.3, -0.2])
    v = v / np.linalg.norm(v)

    def acc(v):
        return (Q / M_E) * np.cross(v, B0vec)

    s = 0.0
    bh = B0vec / np.linalg.norm(B0vec)
    for _ in range(steps):
        k1 = acc(v)
        k2 = acc(v + 0.5 * dt * k1)
        k3 = acc(v + 0.5 * dt * k2)
        k4 = acc(v + dt * k3)
        s += np.cross(v, acc(v)).dot(bh)
        v = v + dt / 6 * (k1 + 2 * k2 + 2 * k3 + k4)
    return s / steps


def driven_electron_susceptibility(handed, w=1.0, wc=0.3, wp2=0.1, nu=0.02,
                                   tmax=600.0, dt=0.002):
    """Real time domain, normalised units (e = m = eps0 = 1, q = -1, N = wp2),
    B0 = wc z_hat.  Drive E(t) = (cos wt, handed sin wt, 0); handed = +1 is
    right-handed about B0.  Weak damping nu removes transients.  Returns the
    in-phase susceptibility <P.E>/<E.E> with -X/(1-Y) and -X/(1+Y) beside it."""
    q = -1.0
    B = np.array([0.0, 0.0, wc])
    v = np.zeros(3)
    P = np.zeros(3)
    t = 0.0

    def E_at(t):
        return np.array([math.cos(w * t), handed * math.sin(w * t), 0.0])

    def f(t, v):
        return q * (E_at(t) + np.cross(v, B)) - nu * v

    Ps, Es = [], []
    for _ in range(int(tmax / dt)):
        k1 = f(t, v)
        k2 = f(t + dt / 2, v + dt / 2 * k1)
        k3 = f(t + dt / 2, v + dt / 2 * k2)
        k4 = f(t + dt, v + dt * k3)
        vn = v + dt / 6 * (k1 + 2 * k2 + 2 * k3 + k4)
        P = P + 0.5 * dt * (wp2 * q * v + wp2 * q * vn)
        v = vn
        t += dt
        if t > tmax * 0.6:
            Ps.append(P.copy())
            Es.append(E_at(t))
    Ps = np.array(Ps)
    Es = np.array(Es)
    Ps = Ps - Ps.mean(axis=0)
    chi = np.mean(np.sum(Ps * Es, axis=1)) / np.mean(np.sum(Es * Es, axis=1))
    X = wp2 / w ** 2
    Y = wc / w
    return chi, -X / (1 - Y), -X / (1 + Y)


# ----------------------------------------------- engine geometry (item 5)
def parse_mf():
    src = (REPO / "src" / "core" / "atmosphere" / "MagneticField.cpp").read_text(encoding="utf-8")

    def grab(name):
        m = re.search(r"constexpr\s+double\s+" + name + r"\s*=\s*([-0-9.eE+]+)\s*;", src)
        if not m:
            raise RuntimeError(f"could not parse {name} from MagneticField.cpp")
        return float(m.group(1))

    return dict(lat=grab("kPoleLatDeg"), lon=grab("kPoleLonDeg"),
                b0=grab("kB0Tesla"), R=grab("kRefEarthRadiusM"))


def dipole_B(pos, mf):
    r = np.linalg.norm(pos)
    rh = pos / r
    la, lo = math.radians(mf["lat"]), math.radians(mf["lon"])
    m = np.array([math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la)])
    return mf["b0"] * (mf["R"] / r) ** 3 * (m - 3 * m.dot(rh) * rh)


# ------------------------------------------ wind-up handedness anchor (item 6)
WU_W = 1.0
WU_T = 2 * math.pi / WU_W
WU_TS = np.linspace(0.0, WU_T, 400, endpoint=False)


def _rot(v, axis, ang):
    axis = axis / np.linalg.norm(axis)
    return (v * math.cos(ang) + np.cross(axis, v) * math.sin(ang)
            + axis * axis.dot(v) * (1 - math.cos(ang)))


def _field(t, t1, t2, s):
    """Crossed transmit dipoles t1 (cos) and t2 (s sin): the transverse field
    at the receiver (the common propagation phase dropped)."""
    return np.outer(np.cos(WU_W * t), t1) + s * np.outer(np.sin(WU_W * t), t2)


def _received_phase(r1, r2, t1, t2, s):
    """Crossed receive dipoles with a quarter-period hybrid on r2; the hybrid
    sign that maximises the output (a matched antenna).  Returns
    (amplitude, phi_out) with V(t) = A cos(w t - phi_out), phi_out a delay."""
    best = None
    for h in (+1.0, -1.0):
        V = _field(WU_TS, t1, t2, s) @ r1 + h * (_field(WU_TS - WU_T / 4, t1, t2, s) @ r2)
        a = 2 * np.mean(V * np.cos(WU_W * WU_TS))
        b = 2 * np.mean(V * np.sin(WU_W * WU_TS))
        amp = math.hypot(a, b)
        if best is None or amp > best[0]:
            best = (amp, math.atan2(b, a))
    return best


def _repo_windup(k, xs, ys, xr, yr):
    """GnssCorrections::computePhaseWindUp's geometry (Wu et al. 1993, k = sat -> rx)."""
    Ds = xs - k * k.dot(xs) - np.cross(k, ys)
    Dr = xr - k * k.dot(xr) - np.cross(k, yr)
    Ds /= np.linalg.norm(Ds)
    Dr /= np.linalg.norm(Dr)
    return math.atan2(k.dot(np.cross(Ds, Dr)), Ds.dot(Dr))


def _wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def windup_anchor():
    """Antenna rotations of +5 deg right-handed about k: the repo model's
    response against a physical crossed-dipole pair's, for a field right- and
    left-handed about k.  RINEX phase has the sign of range, so a larger
    phi_out (delay) is a larger modelled phase."""
    lat, lon = math.radians(40.0), math.radians(10.0)
    up = np.array([math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat)])
    east = np.array([-math.sin(lon), math.cos(lon), 0.0])
    north = np.cross(up, east)
    rx, sat = 6371e3 * up, 26560e3 * up
    k = (rx - sat) / np.linalg.norm(rx - sat)
    ez = -sat / np.linalg.norm(sat)
    xs, ys = north.copy(), np.cross(ez, north)
    xr, yr = north.copy(), east.copy()
    beta = math.radians(5.0)
    w0 = _repo_windup(k, xs, ys, xr, yr)
    d_model_rx = _wrap(_repo_windup(k, xs, ys, _rot(xr, k, beta), _rot(yr, k, beta)) - w0)
    d_model_sat = _wrap(_repo_windup(k, _rot(xs, k, beta), _rot(ys, k, beta), xr, yr) - w0)
    e1 = north - k * k.dot(north)
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(k, e1)
    out = {}
    for s in (+1, -1):
        _, p0 = _received_phase(e1, e2, e1, e2, s)
        _, p_rx = _received_phase(_rot(e1, k, beta), _rot(e2, k, beta), e1, e2, s)
        _, p_sat = _received_phase(e1, e2, _rot(e1, k, beta), _rot(e2, k, beta), s)
        out[s] = (_wrap(p_rx - p0), _wrap(p_sat - p0))
    return d_model_rx, d_model_sat, out


# --------------------------------------------------------------------- main
def main():
    print("=" * 78)
    print("1. Electron gyration sense (RK4, m dv/dt = q v x B0, q = -e)")
    for Bv in (np.array([0, 0, BMAG]), np.array([BMAG, -BMAG, BMAG]) / math.sqrt(3)):
        g = electron_gyration_sense(Bv)
        check(f"electron right-handed about B0 = {Bv}", g > 0, f"mean (v x dv/dt).B0_hat = {g:+.3e}")

    print("\n2. Driven electron, real time domain")
    chi_r, chiR, chiL = driven_electron_susceptibility(+1)
    chi_l, _, _ = driven_electron_susceptibility(-1)
    print(f"  drive right-handed about B0: chi = {chi_r:+.5f}  (-X/(1-Y) = {chiR:+.5f})")
    print(f"  drive left-handed about B0 : chi = {chi_l:+.5f}  (-X/(1+Y) = {chiL:+.5f})")
    check("co-rotating drive sees -X/(1-Y)", abs(chi_r - chiR) < 2e-3 * abs(chiR))
    check("counter-rotating drive sees -X/(1+Y)", abs(chi_l - chiL) < 2e-3 * abs(chiL))

    X, Y = XY(F_L1, NE, BMAG)
    print("\n3. Plane-wave eigenmodes at L1, Ne = 1e12 el/m^3, |B0| = 3e-5 T")
    print(f"  X = {X:.6e}, Y = {Y:.6e}")
    B0vec = np.array([0.0, 0.0, BMAG])
    print(f"  {'conv':>4} {'theta':>6} {'n-(1-X/2)+X^2/8':>16} {'-XYcos/2':>14} {'axial':>8}")
    worst = 0.0
    for conv in ("A", "B"):
        for th in (0, 30, 60, 89, 91, 120, 150, 180):
            t = math.radians(th)
            khat = np.array([math.sin(t), 0.0, math.cos(t)])
            n, h, ar = rh_mode_index(F_L1, NE, B0vec, khat, conv)
            dn2 = n.real - (1 - X / 2) + X ** 2 / 8
            pred = -X * Y * math.cos(t) / 2
            rel = abs(dn2 - pred) / abs(pred)
            if th not in (89, 91):
                worst = max(worst, rel)
            print(f"  {conv:>4} {th:6.1f} {dn2:+16.6e} {pred:+14.6e} {ar:8.5f}")
            check(f"conv {conv}, theta {th}: RH-about-k mode matches -XYcos/2, not +XYcos/2",
                  abs(dn2 - pred) < abs(dn2 + pred))
    check("relative error below 1e-3 away from 90 deg", worst < 1e-3, f"worst {worst:.2e}")

    print("\n4. Group index of the RH-about-k mode (100 km slab, 10 TECU)")
    L = 1.0e5
    STEC = NE * L
    for th in (0.0, 60.0, 120.0, 180.0):
        t = math.radians(th)
        khat = np.array([math.sin(t), 0.0, math.cos(t)])

        def n_of(f):
            return rh_mode_index(f, NE, B0vec, khat, "A")[0].real

        h = F_L1 * 1e-4
        ng = ((F_L1 + h) * n_of(F_L1 + h) - (F_L1 - h) * n_of(F_L1 - h)) / (2 * h)
        nph = n_of(F_L1)
        code2 = (ng - 1 - X / 2 - 3 * X ** 2 / 8) * L
        phase2 = (nph - 1 + X / 2 + X ** 2 / 8) * L
        pred = 7527.0 * C * STEC * BMAG * math.cos(t) / F_L1 ** 3
        print(f"  theta {th:5.1f}: code {code2:+.6e} m, phase {phase2:+.6e} m, "
              f"+7527c STEC B cos/f^3 = {pred:+.6e} m")
        check(f"theta {th}: code excess = +7527c STEC B cos(theta_k)/f^3 within 0.5 %",
              abs(code2 / pred - 1) < 5e-3)
        check(f"theta {th}: phase/code = -1/2 within 0.5 %", abs(phase2 / code2 + 0.5) < 2.5e-3)

    print("\n5. Engine pinned geometry (HigherOrderIonosphereTest.MatchesCanonicalReference)")
    mf = parse_mf()
    rx = np.array([4.6e6, 0.8e6, 4.3e6])
    sat = np.array([1.5e7, 1.0e7, 2.0e7])
    la, lo, rr = 0.70, 0.18, (6371.0 + 450.0) * 1e3
    ipp = rr * np.array([math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la)])
    B = dipole_B(ipp, mf)
    k = (rx - sat) / np.linalg.norm(rx - sat)
    up = ipp / np.linalg.norm(ipp)
    east = np.array([-math.sin(lo), math.cos(lo), 0.0])
    north = np.cross(up, east)
    print(f"  B (ENU, nT) {B.dot(east)*1e9:+.1f} {B.dot(north)*1e9:+.1f} {B.dot(up)*1e9:+.1f}; "
          f"k (ENU) {k.dot(east):+.4f} {k.dot(north):+.4f} {k.dot(up):+.4f}; B.k = {B.dot(k):+.4e} T")
    stec = 50.0 * 1e16
    ne_s = stec / L

    def n_of_case(f):
        return rh_mode_index(f, ne_s, B, k, "A")[0].real

    h = F_L1 * 1e-4
    ng = ((F_L1 + h) * n_of_case(F_L1 + h) - (F_L1 - h) * n_of_case(F_L1 - h)) / (2 * h)
    Xs, _ = XY(F_L1, ne_s, 0.0)
    code2 = (ng - 1 - Xs / 2 - 3 * Xs ** 2 / 8) * L
    print(f"  eigenmode code I2 = {code2:+.6e} m; pinned golden = {PINNED_L1_CODE_I2:+.6e} m")
    check("pinned golden has the eigenmode's sign and size within 0.5 %",
          abs(code2 / PINNED_L1_CODE_I2 - 1) < 5e-3)

    print("\n6. Handedness anchor: the field-validated wind-up model")
    d_rx, d_sat, phys = windup_anchor()
    print(f"  model: receiver +5 deg about k -> {math.degrees(d_rx):+.3f} deg, "
          f"satellite +5 deg -> {math.degrees(d_sat):+.3f} deg")
    for s, lab in ((+1, "right-handed about k"), (-1, "left-handed about k")):
        print(f"  field {lab:21s}: receiver {math.degrees(phys[s][0]):+.3f} deg, "
              f"satellite {math.degrees(phys[s][1]):+.3f} deg")
    check("a field right-handed about k reproduces the model",
          abs(phys[+1][0] - d_rx) < 1e-6 and abs(phys[+1][1] - d_sat) < 1e-6)
    check("a field left-handed about k does not",
          abs(phys[-1][0] - d_rx) > 1e-3 and abs(phys[-1][1] - d_sat) > 1e-3)

    n_fail = sum(1 for _, ok in CHECKS if not ok)
    print("\n" + "=" * 78)
    print(f"{len(CHECKS) - n_fail} of {len(CHECKS)} checks pass")
    print("conclusion: code I2 = +7527 c STEC (B.k)/f^3 = -7527 c STEC (B.u)/f^3, "
          "k = satellite -> receiver, u = -k; phase I2 = -1/2 of it")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
