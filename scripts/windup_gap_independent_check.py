#!/usr/bin/env python3
"""Independent check of the whole-cycle counts of the phase wind-up gap bridge.

The bridge (estimation.phase_windup_gap_bridge) carries the accumulated wind-up angle
across a data gap of 300 s to 900 s by walking it through virtual epochs and keeps the
whole-cycle count N that a restart would have lost: the bridged angle is r_now + 2 pi N,
r_now the epoch's own atan2 angle.  This script recomputes N with code that shares nothing
with the project: its own SP3 reader and Lagrange interpolation, its own low-precision
Sun, its own receiver frame and its own dipole-angle routine (the formulation of RTKLIB
and cssrlib, nominal yaw steering), and its own RINEX epoch reader.

Definition of the independent count.  The accumulator of a (station, satellite) pair is
seeded with the raw angle at its origin (the first epoch the pair is processed, or the
epoch of the last restart) and unwrapped from epoch to epoch afterwards.  The continuous
angle phi(t) of that accumulation therefore equals the raw angle plus a whole number of
cycles at every epoch: phi(t) = r(t) + 2 pi N(t).  The independent count across a gap is
N(t_now) with phi built from the origin on a 15 s grid, which is finer than the bridge's
30 s steps with their middles.  N splits into the count K carried into the gap (the
accumulator before it) and the count J gained inside it (the raw angle crossing the
+-pi cut): N = K + J.

Inputs are the outputs of two runs of the same exe, one with the bridge on and one with it
off (the control): the run-end lines of the bridge run (the attempts it lists and its
counters), the `wu_restart` events of `logs/network_events.csv` of both runs (the restarts,
complete; the bridge run's are the origins of its accumulators and its refused attempts,
the control's are all attempts), the station coordinates and RINEX paths of the arm
config, and the SP3 of the arm.  Nothing of the project's code is called.

Modes
  --selftest        analytic and synthetic checks, no data needed
  validate-trace    compare the raw angle of this script with the `wrapped_rad` column of a
                    `windup_trace.csv` written by the project (single-receiver run)
  check             the table: one row per attempt (see `check --help`)

Guard-set limit.  The bridge refuses a walk only in the Earth's umbra and in the noon-turn
window of a GPS Block IIF/IIR; it bridges Galileo satellites at low beta and BeiDou
satellites in orbit-normal mode on the nominal yaw-steering model.  The table therefore
carries the smallest |beta| of each gap, so that the attempts a nominal model may not
describe are visible.
"""
import argparse
import csv
import datetime
import math
import os
import re
import sys

import numpy as np

TWO_PI = 2.0 * math.pi
GPS_EPOCH = datetime.datetime(1980, 1, 6)
LEAP_S = 18.0                      # GPST - UTC in 2024
AU_M = 149597870700.0
OMEGA_E = 7.2921151467e-5          # rad/s
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
GRID_S = 15.0                      # the independent integration step (the bridge's half step)
MASK_DEG = 15.0                    # elevation mask of the field golden
HORIZON_LO_S, HORIZON_HI_S = 300, 900   # the bridged range [lo, hi) of the golden
BETA_FLAG_DEG = 5.0                # |beta| below this: the nominal yaw model is in doubt
YAW_RATE_IIF_DPS = 0.11            # GPS Block IIF maximum yaw rate (deg/s), the lower of IIF and IIR


# --------------------------------------------------------------------------- time and frames
def gps_seconds(y, mo, d, h, mi, s):
    """Seconds since 1980-01-06 00:00:00 of a GPS-time calendar stamp."""
    return (datetime.datetime(y, mo, d, h, mi) - GPS_EPOCH).total_seconds() + s


def jd_utc(gps_sec):
    """Julian date (UTC scale) of GPS seconds; UT1 is taken as UTC."""
    return 2444244.5 + (np.asarray(gps_sec, dtype=float) - LEAP_S) / 86400.0


def wrap_pi(x):
    """Wrap to [-pi, pi] (arrays or floats)."""
    return x - TWO_PI * np.round(x / TWO_PI)


def sun_ecef(gps_sec):
    """Sun position (m, ECEF) at GPS seconds, shape (n, 3): the low-precision series of the
    Astronomical Almanac (about 0.01 degree) rotated by the mean sidereal angle."""
    jd = jd_utc(np.atleast_1d(gps_sec))
    d0 = jd - 2451545.0
    t = d0 / 36525.0
    m = np.radians(357.5277233 + 35999.05034 * t)
    lon = np.radians(280.460 + 36000.770 * t + 1.914666471 * np.sin(m) + 0.019994643 * np.sin(2.0 * m))
    r = AU_M * (1.000140612 - 0.016708617 * np.cos(m) - 0.000139589 * np.cos(2.0 * m))
    eps = np.radians(23.439291 - 0.0130042 * t)
    x = r * np.cos(lon)
    y = r * np.cos(eps) * np.sin(lon)
    z = r * np.sin(eps) * np.sin(lon)
    th = np.radians((280.46061837 + 360.98564736629 * d0 + 0.000387933 * t * t
                     - t * t * t / 38710000.0) % 360.0)
    c, s = np.cos(th), np.sin(th)
    return np.stack([c * x + s * y, -s * x + c * y, z], axis=1)


def geodetic(xyz):
    """(latitude, longitude) in radians of an ECEF point on WGS-84 (fixed-point iteration)."""
    x, y, z = (float(v) for v in xyz)
    e2 = WGS84_F * (2.0 - WGS84_F)
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1.0 - e2))
    for _ in range(8):
        n = WGS84_A / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
        lat = math.atan2(z + e2 * n * math.sin(lat), p)
    return lat, lon


def local_axes(xyz):
    """(north, west, up) unit vectors of the local frame at an ECEF point."""
    lat, lon = geodetic(xyz)
    sl, cl, sp, cp = math.sin(lon), math.cos(lon), math.sin(lat), math.cos(lat)
    north = np.array([-sp * cl, -sp * sl, cp])
    west = np.array([sl, -cl, 0.0])
    up = np.array([cp * cl, cp * sl, sp])
    return north, west, up


# --------------------------------------------------------------------------- the angle
def windup_angle(sat, sun, rx):
    """Raw wind-up angle (rad, in (-pi, pi]) between the effective dipoles of a satellite and
    a receiver, nominal yaw-steering attitude (Wu et al. 1993, as in RTKLIB and cssrlib).

    sat, sun: (n, 3) ECEF metres; rx: (3,) ECEF metres.  Satellite frame: z toward the
    Earth's centre, y = z x (toward the Sun), x = y x z.  Receiver frame: x north, y west.
    The angle is positive when the satellite dipole turns toward the receiver dipole
    counter-clockwise about the line of sight from the satellite to the receiver.
    """
    sat = np.atleast_2d(sat)
    sun = np.atleast_2d(sun)
    k = rx[None, :] - sat
    k /= np.linalg.norm(k, axis=1)[:, None]
    ez = -sat / np.linalg.norm(sat, axis=1)[:, None]
    es = sun - sat
    es /= np.linalg.norm(es, axis=1)[:, None]
    ey = np.cross(ez, es)
    ey /= np.linalg.norm(ey, axis=1)[:, None]
    ex = np.cross(ey, ez)
    north, west, _ = local_axes(rx)
    north = np.broadcast_to(north, k.shape)
    west = np.broadcast_to(west, k.shape)
    ds = ex - k * np.sum(k * ex, axis=1)[:, None] - np.cross(k, ey)
    dr = north - k * np.sum(k * north, axis=1)[:, None] + np.cross(k, west)
    return np.arctan2(np.sum(k * np.cross(ds, dr), axis=1), np.sum(ds * dr, axis=1))


def elevation_deg(sat, rx):
    """Elevation (degrees) of satellites (n, 3) seen from the ECEF point rx."""
    _, _, up = local_axes(rx)
    d = np.atleast_2d(sat) - rx[None, :]
    return np.degrees(np.arcsin(np.clip(d @ up / np.linalg.norm(d, axis=1), -1.0, 1.0)))


def in_umbra(sat, sun):
    """Boolean per row: the satellite is inside the Earth's cylindrical shadow."""
    sat = np.atleast_2d(sat)
    sun = np.atleast_2d(sun)
    u = sun / np.linalg.norm(sun, axis=1)[:, None]
    along = np.sum(sat * u, axis=1)
    perp = np.linalg.norm(sat - along[:, None] * u, axis=1)
    return (along < 0.0) & (perp < WGS84_A)


def beta_deg(sat, sun):
    """Sun elevation above the orbit plane (degrees); sat (n, 3) ECEF metres on an even time
    grid of GRID_S, sun (n, 3).  The orbit normal is r x (inertial velocity), the inertial
    velocity v_ecef + omega x r with v_ecef the central difference of the positions."""
    v = np.gradient(sat, GRID_S, axis=0)
    w = np.array([0.0, 0.0, OMEGA_E])
    h = np.cross(sat, v + np.cross(w[None, :], sat))
    h /= np.linalg.norm(h, axis=1)[:, None]
    s = sun / np.linalg.norm(sun, axis=1)[:, None]
    return np.degrees(np.arcsin(np.clip(np.sum(h * s, axis=1), -1.0, 1.0)))


# --------------------------------------------------------------------------- accumulation
def continuous_angle(raw):
    """The accumulated angle of a raw angle series on a fine grid: raw[0] plus the wrapped
    steps.  The result minus raw is 2 pi times an integer at every index."""
    steps = wrap_pi(np.diff(raw))
    return raw[0] + np.concatenate([[0.0], np.cumsum(steps)])


def whole_cycles(phi, raw):
    """Integer N with phi = raw + 2 pi N, with the largest departure from an integer."""
    q = (phi - raw) / TWO_PI
    n = np.round(q)
    return n.astype(int), float(np.max(np.abs(q - n))) if q.size else 0.0


def count_across(raw, i_origin, i_prev, i_now):
    """(K, J, N, defect, max_half_step_cycles) of an attempt on the grid indices.

    K: whole cycles of the accumulator at i_prev, J: gained across the gap, N = K + J
    at i_now; defect: the largest distance of (phi - raw)/2pi from an integer (0 for a
    consistent series); the last value is the largest 15 s step inside the gap in cycles.
    """
    phi = continuous_angle(raw[i_origin:i_now + 1])
    r = raw[i_origin:i_now + 1]
    n_all, defect = whole_cycles(phi, r)
    k = int(n_all[i_prev - i_origin])
    n = int(n_all[i_now - i_origin])
    half = float(np.max(np.abs(wrap_pi(np.diff(raw[i_prev:i_now + 1]))))) / TWO_PI if i_now > i_prev else 0.0
    return k, n - k, n, defect, half


# --------------------------------------------------------------------------- SP3
class Sp3:
    """Positions of one SP3 file (G and E satellites), metres ECEF, on its own epochs."""

    def __init__(self, path, systems="GE"):
        times, rows = [], {}
        with open(path, "r", errors="replace") as f:
            for line in f:
                if line.startswith("*"):
                    p = line[1:].split()
                    times.append(gps_seconds(int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4]), float(p[5])))
                elif line.startswith("P") and line[1] in systems:
                    sat = line[1:4].replace(" ", "0")
                    try:
                        xyz = [float(line[4:18]), float(line[18:32]), float(line[32:46])]
                    except ValueError:
                        continue
                    bad = any(abs(v) < 1e-9 or abs(v) > 99999.0 for v in xyz)
                    rows.setdefault(sat, {})[len(times) - 1] = None if bad else [v * 1000.0 for v in xyz]
        self.t = np.array(times)
        self.dt = float(self.t[1] - self.t[0])
        self.pos = {}
        for sat, d in rows.items():
            a = np.full((len(self.t), 3), np.nan)
            for i, v in d.items():
                if v is not None:
                    a[i] = v
            self.pos[sat] = a

    def interpolate(self, sat, t, order=10):
        """Lagrange interpolation of the satellite position at GPS seconds t (n,): NaN
        where a node is missing; the nodes of the nearest window (extrapolation at the ends)."""
        t = np.atleast_1d(np.asarray(t, dtype=float))
        a = self.pos[sat]
        n_nodes = order + 1
        i0 = np.clip(np.floor((t - self.t[0]) / self.dt).astype(int) - order // 2 + 1, 0, len(self.t) - n_nodes)
        idx = i0[:, None] + np.arange(n_nodes)[None, :]
        tn = self.t[idx]
        out = np.zeros((len(t), 3))
        for j in range(n_nodes):
            w = np.ones(len(t))
            for m in range(n_nodes):
                if m != j:
                    w *= (t - tn[:, m]) / (tn[:, j] - tn[:, m])
            out += w[:, None] * a[idx[:, j]]
        return out


# --------------------------------------------------------------------------- RINEX epochs
def read_phase_presence(path, wanted, systems="GE"):
    """{sat: sorted array of GPS seconds} of the epochs at which a RINEX 3 observation file
    holds a non-zero phase observation of the satellite (only the satellites in `wanted`)."""
    types = {}
    out = {s: [] for s in wanted}
    with open(path, "r", errors="replace") as f:
        cur = None
        for line in f:
            lab = line[60:].strip()
            if lab == "SYS / # / OBS TYPES":
                if line[0] != " ":
                    cur = line[0]
                    types[cur] = line[7:60].split()
                else:
                    types[cur].extend(line[7:60].split())
            elif lab == "END OF HEADER":
                break
        phase_cols = {s: [i for i, t in enumerate(v) if t.startswith("L")] for s, v in types.items()}
        t_epoch, skip = None, 0
        for line in f:
            if line.startswith(">"):
                p = line[1:].split()
                try:
                    flag = int(p[6]) if len(p) > 6 else 0
                    t_epoch = gps_seconds(int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4]), float(p[5]))
                    skip = 1 if flag > 1 else 0
                except (ValueError, IndexError):
                    t_epoch, skip = None, 1      # a malformed epoch record: its lines are ignored
                continue
            if skip or t_epoch is None:
                continue
            sat = line[:3]
            if sat not in out:
                continue
            for i in phase_cols.get(sat[0], []):
                field = line[3 + 16 * i:3 + 16 * i + 14].strip()
                if field:
                    try:
                        if float(field) != 0.0:
                            out[sat].append(t_epoch)
                            break
                    except ValueError:
                        pass
    return {s: np.array(sorted(set(v))) for s, v in out.items()}


# --------------------------------------------------------------------------- run outputs
RE_ATTEMPT = re.compile(r"Network \(wind-up gap bridge\) attempt: (\S+) (\S+) at TOW (\d+) s, gap (\d+) s: "
                        r"(\w+)(?:, (-?\d+) whole cycle\(s\) kept)?")
RE_SUMMARY = re.compile(r"Network \(wind-up gap bridge\): (\d+) gap\(s\) in the bridged range \[(\d+), (\d+)\) s at "
                        r"(\d+) station\(s\): (\d+) BRIDGED \((\d+) of them over a nonzero whole-cycle count.*?\), "
                        r"(\d+) refused \(no satellite state (\d+), umbra (\d+), noon turn (\d+), half step of a "
                        r"quarter cycle or more (\d+), closure with the epoch's own angle (\d+)\); (\d+) gap\(s\) "
                        r"at or beyond the horizon restarted as before; (\d+) virtual epoch\(s\) evaluated")
RE_NOT_LISTED = re.compile(r"Network \(wind-up gap bridge\): (\d+) further attempt\(s\) not listed")


def read_run_end(path):
    """The wind-up gap bridge run-end lines of a run's stdout: the counters and the listed
    attempts, as {'summary': dict or None, 'attempts': [dict], 'not_listed': int}."""
    res = {"summary": None, "attempts": [], "not_listed": 0}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = RE_ATTEMPT.search(line)
            if m:
                res["attempts"].append({
                    "station": m.group(1), "sat": m.group(2), "tow": int(m.group(3)), "gap": int(m.group(4)),
                    "outcome": m.group(5), "cycles": int(m.group(6)) if m.group(6) is not None else None})
                continue
            m = RE_SUMMARY.search(line)
            if m:
                g = [int(x) for x in m.groups()]
                res["summary"] = dict(zip(
                    ["attempts", "lo", "hi", "stations", "bridged", "bridged_stepped", "refused", "no_state",
                     "umbra", "noon_turn", "step", "closure", "beyond", "virtual"], g))
                continue
            m = RE_NOT_LISTED.search(line)
            if m:
                res["not_listed"] = int(m.group(1))
    return res


def read_restarts(events_csv):
    """The wind-up restarts of a network_events.csv as [(station, sat, gps_sec, gap_s)]."""
    out = []
    with open(events_csv, "r", newline="") as f:
        for row in csv.DictReader(f):
            if row["tier"] == "T0_windup" and row["event"] == "wu_restart":
                out.append((row["station"], row["sat"], int(row["week"]) * 604800.0 + float(row["tow"]),
                            float(row["statistic"])))
    return out


# --------------------------------------------------------------------------- the check
class Context:
    """The SP3, the Sun and the satellite positions on the fine grid of one day."""

    def __init__(self, sp3_path, start_gps, hours):
        self.sp3 = Sp3(sp3_path)
        self.t0 = start_gps
        n = int(round(hours * 3600.0 / GRID_S)) + 1
        self.t = self.t0 + GRID_S * np.arange(n)
        self.sun = sun_ecef(self.t)
        self._sat = {}
        self._series = {}

    def index(self, gps_sec):
        return int(round((gps_sec - self.t0) / GRID_S))

    def sat(self, sat):
        if sat not in self._sat:
            self._sat[sat] = self.sp3.interpolate(sat, self.t)
        return self._sat[sat]

    def umbra(self, sat):
        if ("umbra", sat) not in self._sat:
            self._sat[("umbra", sat)] = in_umbra(self.sat(sat), self.sun)
        return self._sat[("umbra", sat)]

    def series(self, sat, rx_key, rx):
        """(raw angle, elevation) on the grid for a receiver."""
        key = (sat, rx_key)
        if key not in self._series:
            s = self.sat(sat)
            self._series[key] = (windup_angle(s, self.sun, np.asarray(rx, dtype=float)),
                                 elevation_deg(s, np.asarray(rx, dtype=float)))
        return self._series[key]


def station_table(arm_json):
    """{station: (rinex_path, ecef)} of an arm config."""
    import json
    with open(arm_json, "r") as f:
        cfg = json.load(f)
    return {s["name"]: (s["rinex_path"], np.array(s["position_ecef"], dtype=float)) for s in cfg["network"]["stations"]}


def processed_indices(presence, elev, umbra, ctx):
    """Set of grid indices at which the receiver model would process the satellite: a phase
    observation, an elevation of MASK_DEG or more, and the satellite outside the umbra (the
    golden rejects an eclipsing satellite)."""
    if presence is None or len(presence) == 0:
        return set()
    idx = np.round((presence - ctx.t0) / GRID_S).astype(int)
    idx = idx[(idx >= 0) & (idx < len(elev))]
    return set(int(i) for i in idx[(elev[idx] >= MASK_DEG) & ~umbra[idx]])


def first_processed(ok, i_min=0):
    """Smallest index of the set `ok` that is at least i_min, or None."""
    cand = [i for i in ok if i >= i_min]
    return min(cand) if cand else None


def nominal_yaw_rate_dps(sat, sun):
    """Rate (degrees per second) of the nominal yaw angle psi = atan2(-tan beta, sin mu) on the
    grid, index i for the step from i to i+1.  A satellite cannot follow a rate above its
    maximum yaw rate (GPS Block IIF 0.11, IIR 0.20 degrees per second): the noon-turn
    exposure the bridge's guard is meant to cover, here from the geometry alone.  mu is the
    orbit angle from the orbit midnight; the rate is the same for either origin of the angle."""
    v = np.gradient(sat, GRID_S, axis=0)
    h = np.cross(sat, v + np.cross(np.array([0.0, 0.0, OMEGA_E])[None, :], sat))
    h /= np.linalg.norm(h, axis=1)[:, None]
    s_hat = sun / np.linalg.norm(sun, axis=1)[:, None]
    beta = np.arcsin(np.clip(np.sum(h * s_hat, axis=1), -1.0, 1.0))
    s_pl = s_hat - np.sum(s_hat * h, axis=1)[:, None] * h
    s_pl /= np.linalg.norm(s_pl, axis=1)[:, None]
    e_r = sat / np.linalg.norm(sat, axis=1)[:, None]
    nu = np.arctan2(np.sum(h * np.cross(s_pl, e_r), axis=1), np.sum(s_pl * e_r, axis=1))   # from noon
    psi = np.arctan2(-np.tan(beta), -np.sin(nu))
    return np.degrees(np.abs(wrap_pi(np.diff(psi)))) / GRID_S


class Attempts:
    """The universe of attempts of a bridge run and everything the independent count needs."""

    def __init__(self, a):
        self.bridge = read_run_end(os.path.join(a.bridge_run, "stdout.txt"))
        self.ctl_restarts = read_restarts(os.path.join(a.control_run, "logs", "network_events.csv"))
        self.brg_restarts = read_restarts(os.path.join(a.bridge_run, "logs", "network_events.csv"))
        self.stations = station_table(os.path.join(a.bridge_run, "arm.json"))
        self.ctx = Context(a.sp3, Sp3(a.sp3, systems="G").t[0], a.hours)
        week0 = int(self.ctx.t0 // 604800) * 604800

        def in_range(gap):
            return HORIZON_LO_S <= round(gap) < HORIZON_HI_S

        self.ctl_att = {(s, p, round(t), round(g)) for s, p, t, g in self.ctl_restarts if in_range(g)}
        self.brg_ref = {(s, p, round(t), round(g)) for s, p, t, g in self.brg_restarts if in_range(g)}
        self.listed = {(x["station"], x["sat"], x["tow"] + week0, x["gap"]): x for x in self.bridge["attempts"]}
        self.universe = sorted(self.ctl_att | set(self.listed), key=lambda k: (k[2], k[0], k[1]))
        self.last_restart = {}
        for s, p, t, g in sorted(self.brg_restarts, key=lambda r: r[2]):
            self.last_restart.setdefault((s, p), []).append(t)
        self.presence = {}
        if not a.no_rinex:
            need = {}
            for s, p, t, g in list(self.universe) + list(self.brg_restarts) + list(self.ctl_restarts):
                need.setdefault(s, set()).add(p)
            for s, sats in need.items():
                if s in self.stations and os.path.exists(self.stations[s][0]):
                    self.presence[s] = read_phase_presence(self.stations[s][0], sorted(sats))
        self._proc = {}

    def processed(self, s, p):
        """Grid indices at which the pair would be processed (see processed_indices)."""
        if (s, p) not in self._proc:
            raw, el = self.ctx.series(p, s, self.stations[s][1])
            self._proc[(s, p)] = processed_indices(self.presence.get(s, {}).get(p), el, self.ctx.umbra(p), self.ctx)
        return self._proc[(s, p)]

    def origin(self, s, p, t_prev, el):
        """(grid index, kind, alternative index or None) of the accumulator origin before t_prev:
        the last restart of the bridge run, else the first processed epoch (the alternative is
        the first epoch of visibility, to show how much the origin matters)."""
        prior = [t for t in self.last_restart.get((s, p), []) if t <= t_prev + 0.5]
        if prior:
            return self.ctx.index(prior[-1]), "restart", None
        vis = np.nonzero(el >= MASK_DEG)[0]
        i_vis = int(vis[0]) if len(vis) else 0
        fp = first_processed(self.processed(s, p))
        if fp is None:
            return i_vis, "visibility", None
        return fp, "first_processed", i_vis

    def row(self, key):
        s, p, t_now, gap = key
        ctx = self.ctx
        raw, el = ctx.series(p, s, self.stations[s][1])
        i_now, i_prev = ctx.index(t_now), ctx.index(t_now - gap)
        i_o, kind, i_alt = self.origin(s, p, t_now - gap, el)
        i_o = min(i_o, i_prev)
        k, j, n, defect, half = count_across(raw, i_o, i_prev, i_now)
        sensitive = False
        if i_alt is not None and i_alt < i_o:
            sensitive = count_across(raw, min(i_alt, i_prev), i_prev, i_now)[2] != n
        sat_pos, sun = ctx.sat(p), ctx.sun
        beta = beta_deg(sat_pos, sun)[i_prev:i_now + 1]
        rate = nominal_yaw_rate_dps(sat_pos, sun)[i_prev:i_now]
        lst = self.listed.get(key)
        if lst is not None:
            outcome, b_cyc = lst["outcome"], lst["cycles"]
        else:
            outcome, b_cyc = ("restart" if key in self.brg_ref else "bridged"), None
        if outcome == "bridged":
            agree = ("yes" if b_cyc == n else "NO") if b_cyc is not None else "not listed"
        else:
            agree = "refused"
        return {
            "station": s, "sat": p, "tow": int(t_now % 604800), "gap_s": gap,
            "bridge_outcome": outcome, "listed": lst is not None,
            "bridge_cycles": "" if b_cyc is None else b_cyc,
            "K_ind": k, "J_ind": j, "N_ind": n, "agree": agree,
            "umbra_ind": bool(np.any(ctx.umbra(p)[i_prev:i_now + 1])),
            "min_abs_beta_deg": round(float(np.min(np.abs(beta))), 2),
            "max_yaw_rate_dps": round(float(np.max(rate)) if len(rate) else 0.0, 3),
            "max_step_cyc": round(half, 3), "origin_kind": kind, "origin_sensitive": sensitive,
            "origin_tow": int((ctx.t0 + GRID_S * i_o) % 604800), "defect": round(defect, 6),
            "in_control": key in self.ctl_att,
            "flag": ("low_beta" if np.min(np.abs(beta)) < BETA_FLAG_DEG else "")}

    def structure(self):
        """The control's restarts against the processed-epoch model: a restart of gap g at t
        needs the pair processed at t - g and at t and at no epoch in between."""
        res = {"events": 0, "match": 0, "mismatch": []}
        for s, p, t, g in self.ctl_restarts:
            if s not in self.presence or p not in self.presence[s]:
                continue
            ok = self.processed(s, p)
            i_now, i_prev = self.ctx.index(t), self.ctx.index(t - g)
            between = [i for i in range(i_prev + 2, i_now, 2) if i in ok]
            res["events"] += 1
            if i_now in ok and i_prev in ok and not between:
                res["match"] += 1
            else:
                res["mismatch"].append((s, p, int(t % 604800), int(round(g)), i_prev in ok, i_now in ok, len(between)))
        return res


def run_check(a):
    att = Attempts(a)
    rows = [att.row(key) for key in att.universe]
    struct = att.structure() if att.presence else {"events": 0, "match": 0, "mismatch": []}
    write_outputs(a, rows, att.bridge, att.universe, struct)
    return 0


def summarize(rows, bridge, struct):
    """The counts of a table as a dict (printed by write_outputs, kept for the card)."""
    br = [r for r in rows if r["bridge_outcome"] == "bridged"]
    ref = [r for r in rows if r["bridge_outcome"] != "bridged"]
    lst = [r for r in br if r["listed"]]
    unl = [r for r in br if not r["listed"]]
    sm = bridge["summary"] or {}
    gps_exposed = [r for r in br if r["sat"][0] == "G" and r["max_yaw_rate_dps"] > YAW_RATE_IIF_DPS]
    low_beta = [r for r in br if r["sat"][0] != "G" and r["min_abs_beta_deg"] < BETA_FLAG_DEG]
    return {
        "attempts": len(rows), "counter_attempts": sm.get("attempts"),
        "bridged": len(br), "not_bridged": len(ref),
        "counter_bridged": sm.get("bridged"), "counter_refused": sm.get("refused"),
        "listed_bridged": len(lst), "unlisted_bridged": len(unl),
        "listed_agree": len([r for r in lst if r["agree"] == "yes"]),
        "listed_disagree": len([r for r in lst if r["agree"] == "NO"]),
        "listed_bridge_nonzero": len([r for r in lst if r["bridge_cycles"] != 0]),
        "listed_ind_nonzero": len([r for r in lst if r["N_ind"] != 0]),
        "unlisted_ind_nonzero": len([r for r in unl if r["N_ind"] != 0]),
        "counter_nonzero": sm.get("bridged_stepped"),
        "bridged_ind_nonzero": len([r for r in br if r["N_ind"] != 0]),
        "bridged_J_nonzero": len([r for r in br if r["J_ind"] != 0]),
        "bridged_K_nonzero": len([r for r in br if r["K_ind"] != 0]),
        "max_step_cyc": max([r["max_step_cyc"] for r in br] or [0.0]),
        "max_defect": max([r["defect"] for r in rows] or [0.0]),
        "origin_restart": len([r for r in br if r["origin_kind"] == "restart"]),
        "origin_first_processed": len([r for r in br if r["origin_kind"] == "first_processed"]),
        "origin_visibility": len([r for r in br if r["origin_kind"] == "visibility"]),
        "origin_sensitive": len([r for r in br if r["origin_sensitive"]]),
        "gps_bridged_above_iif_yaw_rate": [(r["station"], r["sat"], r["tow"], r["max_yaw_rate_dps"]) for r in gps_exposed],
        "non_gps_bridged_low_beta": [(r["station"], r["sat"], r["tow"], r["min_abs_beta_deg"]) for r in low_beta],
        "refused_in_umbra_ind": len([r for r in ref if r["umbra_ind"]]), "refused": len(ref),
        "bridged_in_umbra_ind": len([r for r in br if r["umbra_ind"]]),
        "struct_events": struct["events"], "struct_match": struct["match"],
    }


def write_outputs(a, rows, bridge, universe, struct):
    if a.out_csv:
        with open(a.out_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    sm = summarize(rows, bridge, struct)
    lines = [
        f"attempts in the universe: {sm['attempts']} (bridge run-end counters: {sm['counter_attempts']})",
        f"bridged {sm['bridged']} (run-end {sm['counter_bridged']}; {sm['listed_bridged']} listed with their cycles, "
        f"{sm['unlisted_bridged']} not listed); not bridged (restart or refused) {sm['not_bridged']} "
        f"(run-end refused {sm['counter_refused']}); run-end lines list {len(bridge['attempts'])} attempts, "
        f"{bridge['not_listed']} not listed",
        f"agreement of the listed bridged attempts: {sm['listed_agree']} of {sm['listed_bridged']}; "
        f"disagreements: {sm['listed_disagree']}",
        f"listed with nonzero bridge cycles: {sm['listed_bridge_nonzero']}; with nonzero independent N: "
        f"{sm['listed_ind_nonzero']}",
        f"not listed: independent N nonzero in {sm['unlisted_ind_nonzero']} of {sm['unlisted_bridged']}; "
        f"run-end count of nonzero over all bridged: {sm['counter_nonzero']}; independent nonzero over all bridged: "
        f"{sm['bridged_ind_nonzero']}",
        f"independent J (cycles gained inside the gap) nonzero: {sm['bridged_J_nonzero']}; K (carried into the gap) "
        f"nonzero: {sm['bridged_K_nonzero']}",
        f"origin of the accumulator, bridged attempts: last restart {sm['origin_restart']}, first processed epoch "
        f"{sm['origin_first_processed']}, first visible epoch {sm['origin_visibility']}; N changes with the "
        f"origin (first processed against first visible) in {sm['origin_sensitive']}",
        f"largest 15 s step inside a bridged gap: {sm['max_step_cyc']:.3f} cycles (the bridge refuses 0.25 per half step); "
        f"largest series defect: {sm['max_defect']:.2e}",
        f"independent umbra in the gap: not bridged {sm['refused_in_umbra_ind']} of {sm['refused']}; "
        f"bridged {sm['bridged_in_umbra_ind']} of {sm['bridged']}",
        f"guard-set limit: GPS bridged with a nominal yaw rate above {YAW_RATE_IIF_DPS} deg/s (Block IIF limit) "
        f"{len(sm['gps_bridged_above_iif_yaw_rate'])}; Galileo and other bridged with min |beta| below "
        f"{BETA_FLAG_DEG:.0f} deg {len(sm['non_gps_bridged_low_beta'])}",
    ]
    for r in sm["gps_bridged_above_iif_yaw_rate"]:
        lines.append(f"   GPS above the yaw rate: {r}")
    for r in sm["non_gps_bridged_low_beta"]:
        lines.append(f"   low beta, not GPS: {r}")
    if struct["events"]:
        lines.append(f"control restarts matching the processed-epoch model: {struct['match']} of {struct['events']}")
    print("\n".join(lines))
    if a.out_summary:
        with open(a.out_summary, "w") as f:
            f.write("\n".join(lines) + "\n")
    if a.out_struct and struct["mismatch"]:
        with open(a.out_struct, "w") as f:
            f.write("station sat tow gap prev_processed now_processed processed_between\n")
            for m in struct["mismatch"]:
                f.write(" ".join(str(x) for x in m) + "\n")
    if a.out_md:
        with open(a.out_md, "w") as f:
            f.write("| station | satellite | TOW | gap s | bridge | bridge cycles | independent cycles | agreement |\n")
            f.write("| --- | --- | --- | --- | --- | --- | --- | --- |\n")
            for r in rows:
                f.write(f"| {r['station']} | {r['sat']} | {r['tow']} | {r['gap_s']} | {r['bridge_outcome']} | "
                        f"{r['bridge_cycles']} | {r['N_ind']} | {r['agree']} |\n")


# --------------------------------------------------------------------------- validation against a project trace
def run_validate(a):
    """Compare the raw angle with the `wrapped_rad` column of a project windup_trace.csv."""
    sp3_first = Sp3(a.sp3, systems="G").t[0]
    ctx = Context(a.sp3, sp3_first, a.hours)
    rx = np.array(a.ecef, dtype=float)
    diffs, by_sat = [], {}
    with open(a.trace, "r", newline="") as f:
        for row in csv.DictReader(f):
            sat = row["satellite"]
            if sat not in ctx.sp3.pos:
                continue
            t = int(row["week"]) * 604800.0 + float(row["tow_s"])
            i = ctx.index(t)
            if not (0 <= i < len(ctx.t)):
                continue
            raw, el = ctx.series(sat, "rx", rx)
            d = float(wrap_pi(raw[i] - float(row["wrapped_rad"])))
            diffs.append(d)
            by_sat.setdefault(sat, []).append(d)
    d = np.abs(np.array(diffs))
    print(f"rows compared: {len(d)}; |difference| of the raw angle in cycles: median {np.median(d) / TWO_PI:.2e}, "
          f"95th percentile {np.percentile(d, 95) / TWO_PI:.2e}, maximum {d.max() / TWO_PI:.2e}")
    worst = sorted(((float(np.max(np.abs(v))) / TWO_PI, s) for s, v in by_sat.items()), reverse=True)[:8]
    print("largest per-satellite maxima (cycles):", ", ".join(f"{s} {x:.2e}" for x, s in worst))
    return 0


# --------------------------------------------------------------------------- selftest
def selftest():
    fails = []

    def check(name, ok, detail=""):
        print(("ok   " if ok else "FAIL ") + name + (f"  [{detail}]" if detail else ""))
        if not ok:
            fails.append(name)

    # 1. Sun: the equinox and solstice instants of 2024 (UTC), declination and distance.
    for label, stamp, dec_want, tol in [
            ("March equinox", (2024, 3, 20, 3, 6, 0.0), 0.0, 0.03),
            ("June solstice", (2024, 6, 20, 20, 51, 0.0), 23.4, 0.06),
            ("September equinox", (2024, 9, 22, 12, 44, 0.0), 0.0, 0.03),
            ("December solstice", (2024, 12, 21, 9, 20, 0.0), -23.4, 0.06)]:
        g = gps_seconds(*stamp) + LEAP_S
        s = sun_ecef(np.array([g]))[0]
        # the declination does not depend on the sidereal rotation
        dec = math.degrees(math.asin(s[2] / np.linalg.norm(s)))
        check(f"sun declination at the {label}", abs(dec - dec_want) < tol, f"{dec:+.3f} deg")
    g = gps_seconds(2024, 1, 3, 0, 0, 0.0) + LEAP_S      # near perihelion
    check("sun distance near perihelion", abs(np.linalg.norm(sun_ecef(np.array([g]))[0]) / AU_M - 0.9833) < 0.0008)

    # 2. Sidereal rotation: one sidereal day turns the Earth through 2 pi (the Sun's ECEF
    # longitude advances by 2 pi - 2 pi / 365.25 in a sidereal day).
    g0 = gps_seconds(2024, 5, 8, 0, 0, 0.0)
    s0, s1 = sun_ecef(np.array([g0, g0 + 86164.0905]))
    lon0, lon1 = math.atan2(s0[1], s0[0]), math.atan2(s1[1], s1[0])
    adv = (lon1 - lon0) % TWO_PI
    check("Sun ECEF longitude after one sidereal day", abs(adv - TWO_PI / 365.25) < 2e-3 or abs(adv - TWO_PI / 365.25 + TWO_PI) < 2e-3, f"{adv:.5f} rad")

    # 3. Geodetic frame: equator, prime meridian; a mid-latitude point.
    lat, lon = geodetic([WGS84_A, 0.0, 0.0])
    check("geodetic latitude and longitude on the equator", abs(lat) < 1e-12 and abs(lon) < 1e-12)
    # a point at 45 degrees geodetic latitude and 800 m height, built from the forward formulas
    e2 = WGS84_F * (2.0 - WGS84_F)
    n45 = WGS84_A / math.sqrt(1.0 - e2 * math.sin(math.radians(45.0)) ** 2)
    pt = [(n45 + 800.0) * math.cos(math.radians(45.0)) * math.cos(math.radians(30.0)),
          (n45 + 800.0) * math.cos(math.radians(45.0)) * math.sin(math.radians(30.0)),
          (n45 * (1.0 - e2) + 800.0) * math.sin(math.radians(45.0))]
    lat45, lon30 = geodetic(pt)
    check("geodetic latitude and longitude of a point built from the forward formulas",
          abs(math.degrees(lat45) - 45.0) < 1e-7 and abs(math.degrees(lon30) - 30.0) < 1e-9,
          f"{math.degrees(lat45):.9f} {math.degrees(lon30):.9f}")

    # 4. The angle, three analytic cases: satellite at the zenith of an equatorial receiver,
    # the Sun to the east, north and west.  Then D_s = 2 x_s, D_r = 2 north, and the angle
    # is that of the satellite x axis to north about k = -radial: east -> -90 deg, north -> 0,
    # west -> +90 deg.
    rx = np.array([WGS84_A, 0.0, 0.0])
    sat = np.array([[26560e3, 0.0, 0.0]])
    for name, sun, want in [("east", [0.0, 1.5e11, 0.0], -90.0), ("north", [0.0, 0.0, 1.5e11], 0.0),
                            ("west", [0.0, -1.5e11, 0.0], 90.0)]:
        ang = math.degrees(float(windup_angle(sat, np.array([sun]), rx)[0]))
        check(f"wind-up angle, satellite at zenith, Sun to the {name}", abs(ang - want) < 0.01, f"{ang:+.4f} deg")

    # 5. Covariance: rotating the whole geometry about the polar axis (the receiver, satellite
    # and Sun together) leaves the angle unchanged, because the receiver frame follows its longitude.
    def rz(v, th):
        c, s_ = math.cos(th), math.sin(th)
        return np.array([c * v[0] - s_ * v[1], s_ * v[0] + c * v[1], v[2]])

    rx2 = np.array([4000e3, 1000e3, 4800e3])
    sat2 = np.array([[15000e3, 12000e3, 18000e3]])
    sun2 = np.array([[1.1e11, 9.0e10, 3.0e10]])
    base = float(windup_angle(sat2, sun2, rx2)[0])
    rot = float(windup_angle(np.array([rz(sat2[0], 0.7)]), np.array([rz(sun2[0], 0.7)]), rz(rx2, 0.7))[0])
    check("wind-up angle is invariant under a rotation of the whole geometry about the pole", abs(wrap_pi(rot - base)) < 1e-9)

    # 6. Unwrapping and the whole-cycle count on a synthetic ramp that crosses the cut.
    t = np.arange(0, 2400.0, GRID_S)
    phi_true = 0.1 * TWO_PI + 1.7 * TWO_PI * (t / 2400.0)         # 1.7 cycles in 40 min, first cut at 0.5
    raw = wrap_pi(phi_true)
    cont = continuous_angle(raw)
    check("continuous angle reproduces a ramp through the cut", np.max(np.abs(cont - phi_true)) < 1e-9 or
          np.max(np.abs(wrap_pi(cont - phi_true))) < 1e-9)
    n_all, defect = whole_cycles(cont, raw)
    check("the accumulated angle minus the raw angle is a whole number of cycles", defect < 1e-9)
    # A gap straddling the first crossing of +pi: the raw angle wraps down, the continuous one
    # does not, so the gap gains one cycle (J = +1); after the cut the accumulator carries K = +1.
    cross = int(np.argmax(np.abs(np.diff(raw)) > math.pi))
    k, j, n, _, half = count_across(raw, 0, cross - 10, cross + 10)
    check("gap straddling the +pi cut: K = 0, J = +1", (k, j, n) == (0, 1, 1), f"K {k} J {j} N {n}")
    k, j, n, _, _ = count_across(raw, 0, cross + 20, cross + 40)
    check("gap after the cut: K = +1, J = 0", (k, j, n) == (1, 0, 1), f"K {k} J {j} N {n}")
    k, j, n, _, _ = count_across(raw, 0, 2, 20)
    check("gap before the cut: K = 0, J = 0", (k, j, n) == (0, 0, 0))
    # A falling angle crossing -pi gives the other sign, the case of the two bridged gaps
    # of the field arms that kept -1 cycle.
    raw_dn = wrap_pi(-phi_true)
    cross = int(np.argmax(np.abs(np.diff(raw_dn)) > math.pi))
    k, j, n, _, _ = count_across(raw_dn, 0, cross - 10, cross + 10)
    check("falling angle through -pi: J = -1", (k, j, n) == (0, -1, -1), f"K {k} J {j} N {n}")

    # 7. Lagrange interpolation of a circular orbit sampled at 300 s, evaluated at 15 s.
    class Fake(Sp3):
        def __init__(self):
            self.t = 1.4e9 + 300.0 * np.arange(48)
            self.dt = 300.0
            w, r = TWO_PI / 43082.0, 26560e3
            ang = w * (self.t - self.t[0])
            self.pos = {"G99": np.stack([r * np.cos(ang), r * np.sin(ang) * math.cos(0.96), r * np.sin(ang) * math.sin(0.96)], axis=1)}
    fk = Fake()
    tq = fk.t[0] + 15.0 * np.arange(1, 900)
    w, r = TWO_PI / 43082.0, 26560e3
    aq = w * (tq - fk.t[0])
    exact = np.stack([r * np.cos(aq), r * np.sin(aq) * math.cos(0.96), r * np.sin(aq) * math.sin(0.96)], axis=1)
    err = np.max(np.linalg.norm(fk.interpolate("G99", tq) - exact, axis=1))
    check("Lagrange interpolation of a circular orbit", err < 1e-3, f"{err:.2e} m")

    # 8. Umbra and beta on constructed geometry.
    sun_far = np.array([[1.5e11, 0.0, 0.0]])
    check("umbra: satellite behind the Earth", bool(in_umbra(np.array([[-26560e3, 0.0, 0.0]]), sun_far)[0]))
    check("no umbra: satellite beside the shadow axis", not bool(in_umbra(np.array([[-26560e3, 7000e3, 0.0]]), sun_far)[0]))
    check("no umbra: satellite on the sunlit side", not bool(in_umbra(np.array([[26560e3, 0.0, 0.0]]), sun_far)[0]))
    # Beta on a circular orbit of inclination 55 degrees, built in the inertial frame and
    # rotated to the Earth-fixed frame with the sidereal angle: a Sun along the orbit normal
    # gives 90 degrees, a Sun in the orbit plane 0, a Sun at 30 degrees from the plane 30.
    tt = GRID_S * np.arange(60)
    ang = TWO_PI / 43082.0 * tt
    inc = math.radians(55.0)
    pos_i = 26560e3 * np.stack([np.cos(ang), np.sin(ang) * math.cos(inc), np.sin(ang) * math.sin(inc)], axis=1)
    normal = np.array([0.0, -math.sin(inc), math.cos(inc)])
    th = OMEGA_E * tt

    def to_ecef(v):
        c, s_ = np.cos(th), np.sin(th)
        return np.stack([c * v[:, 0] + s_ * v[:, 1], -s_ * v[:, 0] + c * v[:, 1], v[:, 2]], axis=1)

    pos_e = to_ecef(pos_i)
    for label, direction, want in [("along the orbit normal", normal, 90.0),
                                   ("in the orbit plane", np.array([1.0, 0.0, 0.0]), 0.0),
                                   ("30 degrees from the plane",
                                    math.cos(math.radians(30.0)) * np.array([1.0, 0.0, 0.0])
                                    + math.sin(math.radians(30.0)) * normal, 30.0)]:
        sun_e = to_ecef(np.tile(direction * 1.5e11, (len(tt), 1)))
        b = beta_deg(pos_e, sun_e)[5:-5]
        check(f"beta of a Sun {label}", float(np.max(np.abs(b - want))) < 0.1, f"{b[0]:+.3f} deg")

    # 9. The readers, on small constructed files: the run-end lines, the restart events and
    # the RINEX phase presence (one malformed event record, a zero value, a code-only line).
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        so = os.path.join(tmp, "stdout.txt")
        with open(so, "w") as f:
            f.write("[2313, 1.000 (x)] [main_log] [info] Network (wind-up gap bridge): 3 gap(s) in the bridged range "
                    "[300, 900) s at 2 station(s): 2 BRIDGED (1 of them over a nonzero whole-cycle count, the integer "
                    "step a restart would have applied), 1 refused (no satellite state 0, umbra 1, noon turn 0, half "
                    "step of a quarter cycle or more 0, closure with the epoch's own angle 0); 4 gap(s) at or beyond "
                    "the horizon restarted as before; 90 virtual epoch(s) evaluated.\n")
            f.write("x Network (wind-up gap bridge) attempt: ABCD00XXX G14 at TOW 278100 s, gap 570 s: bridged, -1 whole cycle(s) kept\n")
            f.write("x Network (wind-up gap bridge) attempt: ABCD00XXX G20 at TOW 262920 s, gap 720 s: umbra\n")
            f.write("x Network (wind-up gap bridge): 5 further attempt(s) not listed.\n")
        re_ = read_run_end(so)
        check("run-end parser: counters", re_["summary"] is not None and re_["summary"]["attempts"] == 3
              and re_["summary"]["bridged_stepped"] == 1 and re_["summary"]["umbra"] == 1, str(re_["summary"]))
        check("run-end parser: attempts", [(x["sat"], x["tow"], x["gap"], x["outcome"], x["cycles"]) for x in re_["attempts"]]
              == [("G14", 278100, 570, "bridged", -1), ("G20", 262920, 720, "umbra", None)] and re_["not_listed"] == 5)
        ev = os.path.join(tmp, "network_events.csv")
        with open(ev, "w", newline="") as f:
            f.write("week,tow,tier,event,sat,station,statistic,threshold,action,detail\n")
            f.write("2313,260430.000,T0_windup,wu_restart,E18,SCTB00ATA,839.999990,900.000000,refloat_carry,x\n")
            f.write("2313,260430.000,T0_gap,arc_resume,E18,SCTB00ATA,,,open,x\n")
        rs = read_restarts(ev)
        check("restart events parser", rs == [("SCTB00ATA", "E18", 2313 * 604800.0 + 260430.0, 839.99999)], str(rs))
        rn = os.path.join(tmp, "t.rnx")
        with open(rn, "w") as f:
            f.write("     3.05           OBSERVATION DATA    M (MIXED)           RINEX VERSION / TYPE\n")
            f.write("G    4 C1C L1C D1C S1C                                      SYS / # / OBS TYPES\n")
            f.write("E    2 C1C L1C                                              SYS / # / OBS TYPES\n")
            f.write("                                                            END OF HEADER\n")
            f.write("> 2024 05 08 00 00  0.0000000  0  3\n")
            f.write("G01  22000000.000 7 118000000.123 7        -1.234 7        45.000 7\n")
            f.write("G02  22000000.000 7\n")
            f.write("E05  23000000.000 7           0.000 7\n")
            f.write(">                              4206\n")
            f.write("G01  22000000.000 7 118000000.123 7\n")
            f.write("> 2024 05 08 00 00 30.0000000  0  1\n")
            f.write("G01  22000000.000 7 118000030.456 7\n")
        pr = read_phase_presence(rn, ["G01", "G02", "E05"])
        t0 = gps_seconds(2024, 5, 8, 0, 0, 0.0)
        check("RINEX presence: phase present, code-only absent, zero value absent, malformed record skipped",
              list(pr["G01"]) == [t0, t0 + 30.0] and len(pr["G02"]) == 0 and len(pr["E05"]) == 0, str({k: list(v) for k, v in pr.items()}))

    print("selftest:", "PASS" if not fails else "FAIL: " + ", ".join(fails))
    return 0 if not fails else 1


# --------------------------------------------------------------------------- command line
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="analytic and synthetic checks, no data needed")
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("check", help="the table of the bridge attempts of a bridge run against its control run")
    c.add_argument("--bridge-run", required=True, help="run directory with the bridge on (stdout.txt, arm.json, logs/)")
    c.add_argument("--control-run", required=True, help="run directory of the same exe with the bridge off")
    c.add_argument("--sp3", required=True)
    c.add_argument("--hours", type=float, default=24.0, help="length of the grid from the first SP3 epoch")
    c.add_argument("--no-rinex", action="store_true", help="skip the RINEX epoch reader (origins from elevation only)")
    c.add_argument("--out-csv")
    c.add_argument("--out-md")
    c.add_argument("--out-summary", help="the printed summary, as a text file")
    c.add_argument("--out-struct", help="the control restarts the processed-epoch model does not reproduce")
    v = sub.add_parser("validate-trace", help="compare the raw angle with a project windup_trace.csv")
    v.add_argument("--trace", required=True)
    v.add_argument("--ecef", type=float, nargs=3, required=True)
    v.add_argument("--sp3", required=True)
    v.add_argument("--hours", type=float, default=24.0)
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd == "check":
        return run_check(a)
    if a.cmd == "validate-trace":
        return run_validate(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
