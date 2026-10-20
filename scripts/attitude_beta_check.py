#!/usr/bin/env python3
"""Independent check of the satellite beta angle and the nominal yaw frame.

Reference values for ``SatAttitude::computeBeta`` and the yaw-steering frame of
``SatAttitude::compute``, built without any code of this project:

* satellite positions from an SP3 file (ECEF, rotated to the inertial
  true-of-date frame with the Greenwich apparent sidereal angle),
* the Sun from the low-precision Astronomical Almanac series (about 0.01 deg),
* the orbit normal as r x (r(t+dt) - r(t-dt)) in that inertial frame, which is
  exact for a planar orbit and needs no velocity convention,
* a second, velocity-free source: the broadcast Keplerian inclination and
  node (RINEX 3 navigation file), h = (sin i sin W, -sin i cos W, cos i).

The standard sign convention is used: beta > 0 when the Sun is on the +h side
of the orbit plane, h = r x v (inertial).  Kouba (2009) nominal yaw is
psi = atan2(-tan beta, sin mu), mu the orbit angle from the orbit midnight,
psi positive about the nadir (+Z) axis from the velocity direction.

Modes
-----
``python scripts/attitude_beta_check.py table --sp3 F --nav F --sats G25,G26``
    Print the reference table (one row per satellite and epoch).
``python scripts/attitude_beta_check.py noon --sp3 F --sats G25,G26``
    Print the noon (mu = 180 deg) times and the beta at the noon.
``python scripts/attitude_beta_check.py csv --sp3 F --sats ... --out F``
    Write the inputs a C++ checker needs (ECEF position, ECEF velocity,
    ECEF Sun) and the reference beta, mu and yaw-steering X axis.
``python scripts/attitude_beta_check.py cssrcsv --sp3 F --sats ... --out F [--cssrlib DIR]``
    Same CSV as ``csv``, but every reference quantity comes from cssrlib
    (RTKLIB port): its Sun (``sunmoonpos_eci``), its IAU-1980 precession,
    nutation and sidereal rotation (``eci2ecef``) for the inertial orbit normal,
    and its nominal yaw frame (``orb2ecef``) for the X axis.  The Sun column
    carries the cssrlib Sun, so the C++ checker is fed the same Sun.  cssrlib has
    no beta routine and no noon-turn model: the beta here is the orbit-normal
    projection of its Sun, computed with its frames.
``python scripts/attitude_beta_check.py compare --ref F --cxx F``
    Compare a C++ checker output with the reference CSV.
``python scripts/attitude_beta_check.py follower --betas 2.44,-0.75``
    Noon turn of a rate-limited yaw follower (a time-stepped simulation of the
    controller, independent of the closed form in SatAttitude::noonTurnYaw):
    the orbit angles at which the turn starts and ends, for Block IIF and IIR.
``python scripts/attitude_beta_check.py windows --sp3 F --atx F``
    Every GPS Block IIF/IIR noon of the day: UTC time, beta, and whether the
    satellite turns (beta below the block threshold) with the turn start and end.
"""
import argparse
import math
import sys

import numpy as np

GPS_UTC = 18.0                       # leap seconds, 2024
OMEGA_E = 7.2921151467e-5            # rad/s
MU_E = 3.986005e14                   # GPS ICD value
JD_GPS0 = 2444244.5                  # 1980-01-06 00:00:00


# ----------------------------------------------------------------------------
# Time and frames
# ----------------------------------------------------------------------------
def jd_from_calendar(y, mo, d, h, mi, s):
    if mo <= 2:
        y -= 1
        mo += 12
    a = y // 100
    b = 2 - a + a // 4
    jd0 = math.floor(365.25 * (y + 4716)) + math.floor(30.6001 * (mo + 1)) + d + b - 1524.5
    return jd0 + (h + mi / 60.0 + s / 3600.0) / 24.0


def gast_rad(jd_gps):
    """Greenwich apparent sidereal angle for a GPS-scale JD (UT1 ~ UTC)."""
    jd_ut = jd_gps - GPS_UTC / 86400.0
    d0 = jd_ut - 2451545.0
    t = d0 / 36525.0
    gmst = (280.46061837 + 360.98564736629 * d0 + 0.000387933 * t * t
            - t * t * t / 38710000.0)
    # equation of the equinoxes, principal term
    omega_m = math.radians(125.04452 - 1934.136261 * t)
    l_sun = math.radians(280.4665 + 36000.7698 * t)
    dpsi = -17.20 * math.sin(omega_m) - 1.32 * math.sin(2 * l_sun)      # arcsec
    eps = math.radians(23.4393 - 0.0130 * t)
    gast = gmst + dpsi / 3600.0 * math.cos(eps)
    return math.radians(gast % 360.0)


def rot_ecef_to_eci(vec, theta):
    c, s = math.cos(theta), math.sin(theta)
    return np.array([c * vec[0] - s * vec[1], s * vec[0] + c * vec[1], vec[2]])


def rot_eci_to_ecef(vec, theta):
    c, s = math.cos(theta), math.sin(theta)
    return np.array([c * vec[0] + s * vec[1], -s * vec[0] + c * vec[1], vec[2]])


def sun_eci(jd_gps):
    """Unit vector to the Sun in the equatorial frame of date (Almanac)."""
    n = jd_gps - GPS_UTC / 86400.0 - 2451545.0
    L = math.radians((280.460 + 0.9856474 * n) % 360.0)
    g = math.radians((357.528 + 0.9856003 * n) % 360.0)
    lam = L + math.radians(1.915) * math.sin(g) + math.radians(0.020) * math.sin(2 * g)
    eps = math.radians(23.439 - 0.0000004 * n)
    return np.array([math.cos(lam), math.cos(eps) * math.sin(lam), math.sin(eps) * math.sin(lam)])


# ----------------------------------------------------------------------------
# SP3 and navigation readers
# ----------------------------------------------------------------------------
def read_sp3(path, sats):
    """Return {sat: [(jd_gps, ecef_m ndarray), ...]} for the requested sats."""
    out = {s: [] for s in sats}
    jd = None
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            if line.startswith('*'):
                p = line[1:].split()
                jd = jd_from_calendar(int(p[0]), int(p[1]), int(p[2]),
                                      int(p[3]), int(p[4]), float(p[5]))
            elif line.startswith('P') and jd is not None:
                sat = line[1:4]
                if sat in out:
                    xyz = np.array([float(line[4:18]), float(line[18:32]),
                                    float(line[32:46])]) * 1000.0
                    out[sat].append((jd, xyz))
    return out


def read_nav_gps(path, sats):
    """Return {sat: [dict(toe_gps_s, i0, idot, omega0, omegadot, week), ...]}."""
    out = {s: [] for s in sats}
    with open(path, 'r', errors='replace') as fh:
        lines = fh.read().splitlines()
    i = 0
    while i < len(lines) and 'END OF HEADER' not in lines[i]:
        i += 1
    i += 1

    def num(txt):
        txt = txt.strip()
        return float(txt.replace('D', 'E')) if txt else float('nan')

    while i < len(lines):
        ln = lines[i]
        if ln[:1] == 'G' and ln[1:3].isdigit() and ln[:3] in out:
            rec = [ln]
            for k in range(1, 8):
                rec.append(lines[i + k])
            f = [[num(r[4 + 19 * j:4 + 19 * (j + 1)]) for j in range(4)] for r in rec[1:]]
            out[ln[:3]].append(dict(
                toe=f[2][0], omega0=f[2][2], i0=f[3][0], omegadot=f[3][3],
                idot=f[4][0], week=f[4][2]))
            i += 8
        else:
            i += 1
    return out


# ----------------------------------------------------------------------------
# Reference quantities
# ----------------------------------------------------------------------------
def orbit_eci(series, k):
    """Inertial position, orbit normal and sidereal angle at sample k."""
    jd, rk = series[k]
    jm, rm = series[k - 1]
    jp, rp = series[k + 1]
    r = rot_ecef_to_eci(rk, gast_rad(jd))
    r_m = rot_ecef_to_eci(rm, gast_rad(jm))
    r_p = rot_ecef_to_eci(rp, gast_rad(jp))
    h = np.cross(r, r_p - r_m)
    return jd, r, h / np.linalg.norm(h), gast_rad(jd)


def beta_mu_psi(r, h_hat, s_hat):
    """beta (deg), mu (deg, 0 = midnight, 180 = noon) and Kouba nominal yaw (deg)."""
    sin_b = float(np.dot(h_hat, s_hat))
    beta = math.asin(sin_b)
    s_p = s_hat - sin_b * h_hat
    p_hat = s_p / np.linalg.norm(s_p)
    q_hat = np.cross(h_hat, p_hat)
    r_hat = r / np.linalg.norm(r)
    mu = math.atan2(-float(np.dot(r_hat, q_hat)), -float(np.dot(r_hat, p_hat)))
    psi = math.atan2(-math.tan(beta), math.sin(mu))
    return math.degrees(beta), math.degrees(mu) % 360.0, math.degrees(psi)


def keplerian_beta(rec, jd_gps, s_hat):
    """Beta from the broadcast inclination and node (no velocity, no SP3)."""
    week_s = (jd_gps - JD_GPS0) * 86400.0
    sow = week_s % 604800.0
    tk = sow - rec['toe']
    if tk > 302400.0:
        tk -= 604800.0
    if tk < -302400.0:
        tk += 604800.0
    inc = rec['i0'] + rec['idot'] * tk
    node_ecef = rec['omega0'] + (rec['omegadot'] - OMEGA_E) * tk - OMEGA_E * rec['toe']
    raan = node_ecef + gast_rad(jd_gps)
    h = np.array([math.sin(inc) * math.sin(raan), -math.sin(inc) * math.cos(raan), math.cos(inc)])
    return math.degrees(math.asin(float(np.dot(h, s_hat))))


def velocity_ecef(series, k):
    """ECEF velocity by a 5-point central difference of the SP3 positions."""
    dt = (series[k + 1][0] - series[k][0]) * 86400.0
    r = [series[k + j][1] for j in (-2, -1, 1, 2)]
    return (-r[3] + 8.0 * r[2] - 8.0 * r[1] + r[0]) / (12.0 * dt)


def noon_crossings(series, sat):
    """(time_utc_h, beta_deg) at every mu = 180 deg crossing of the series."""
    out = []
    prev = None
    for k in range(1, len(series) - 1):
        jd, r, h, _ = orbit_eci(series, k)
        s_hat = sun_eci(jd)
        beta, mu, _ = beta_mu_psi(r, h, s_hat)
        if prev is not None and prev[1] < 180.0 <= mu and mu - prev[1] < 90.0:
            f = (180.0 - prev[1]) / (mu - prev[1])
            jd_x = prev[0] + f * (jd - prev[0])
            hr = ((jd_x - 0.5) % 1.0) * 24.0 - GPS_UTC / 3600.0
            out.append((hr, prev[2] + f * (beta - prev[2])))
        prev = (jd, mu, beta)
    return out


# ----------------------------------------------------------------------------
# Rate-limited yaw follower (independent of the closed-form noon turn)
# ----------------------------------------------------------------------------
GPS_ORBIT_RATE = 0.00836                       # deg/s, as in Kouba (2009)
BLOCK_RATE = {'IIF': 0.11, 'IIR': 0.20}        # maximum yaw rate, deg/s


def follower_turn(beta_deg, rate_deg_s, dt=0.25):
    """Simulate a yaw controller that tracks the nominal yaw at most ``rate`` deg/s.

    Returns (start_deg, end_deg) of the orbit angle between which the controller
    saturates, or None when it follows the nominal yaw throughout.
    """
    tb = math.tan(math.radians(beta_deg))
    rate = math.radians(rate_deg_s)
    mu_dot = math.radians(GPS_ORBIT_RATE)

    def nominal(mu):
        return math.atan2(-tb, math.sin(mu))

    def wrap(x):
        return (x + math.pi) % (2.0 * math.pi) - math.pi

    mu = math.radians(165.0)
    psi = nominal(mu)
    start = end = None
    while mu < math.radians(215.0):
        mu_next = mu + mu_dot * dt
        diff = wrap(nominal(mu_next) - psi)
        if abs(diff) <= rate * dt:
            psi += diff
            if start is not None and end is None:
                end = mu_next
        else:
            psi += math.copysign(rate * dt, diff)
            if start is None:
                start = mu
        mu = mu_next
    if start is None:
        return None
    return math.degrees(start), math.degrees(end if end is not None else mu)


def atx_blocks(path, date):
    """{prn: block} for the GPS satellites whose ATX entry covers ``date`` = (y, m, d)."""
    def parse(line):
        f = line[:60].split()
        return tuple(int(v) for v in f[:3]) if len(f) >= 3 else None

    out = {}
    with open(path, 'r', errors='replace') as fh:
        block = prn = None
        start = end = None
        for line in fh:
            if 'TYPE / SERIAL NO' in line:
                block, prn, start, end = line[:20].strip(), line[20:23], None, None
            elif 'VALID FROM' in line:
                start = parse(line)
            elif 'VALID UNTIL' in line:
                end = parse(line)
            elif 'END OF ANTENNA' in line:
                if (prn and block and prn.startswith('G') and block.startswith('BLOCK')
                        and (start is None or start <= date) and (end is None or date <= end)):
                    out[prn] = block[6:].strip()
                block = prn = None
    return out


# ----------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------
def cmd_table(a):
    sats = a.sats.split(',')
    sp3 = read_sp3(a.sp3, sats)
    nav = read_nav_gps(a.nav, sats) if a.nav else {}
    print('sat   hr_gps  beta_eci  beta_kep  mu_deg  psi_kouba')
    for sat in sats:
        ser = sp3[sat]
        for k in range(1, len(ser) - 1, a.step):
            jd, r, h, _ = orbit_eci(ser, k)
            s_hat = sun_eci(jd)
            beta, mu, psi = beta_mu_psi(r, h, s_hat)
            kep = float('nan')
            if nav.get(sat):
                rec = min(nav[sat], key=lambda q: abs(((jd - JD_GPS0) * 86400.0) % 604800.0 - q['toe']))
                kep = keplerian_beta(rec, jd, s_hat)
            print(f'{sat}  {((jd - 0.5) % 1) * 24:6.2f}  {beta:8.3f}  {kep:8.3f}  {mu:6.1f}  {psi:8.2f}')


def cmd_noon(a):
    sats = a.sats.split(',')
    sp3 = read_sp3(a.sp3, sats)
    for sat in sats:
        for hr, beta in noon_crossings(sp3[sat], sat):
            print(f'{sat} noon {hr:6.3f} h UTC  beta {beta:7.3f} deg')


def cmd_csv(a):
    sats = a.sats.split(',')
    sp3 = read_sp3(a.sp3, sats)
    nav = read_nav_gps(a.nav, sats) if a.nav else {}
    with open(a.out, 'w') as fo:
        fo.write('sat,jd,rx,ry,rz,vx,vy,vz,sx,sy,sz,beta_ref,beta_kep,mu_ref,psi_ref,'
                 'ix,iy,iz\n')
        for sat in sats:
            ser = sp3[sat]
            for k in range(2, len(ser) - 2, a.step):
                jd, r_i, h, theta = orbit_eci(ser, k)
                s_hat = sun_eci(jd)
                beta, mu, psi = beta_mu_psi(r_i, h, s_hat)
                kep = float('nan')
                if nav.get(sat):
                    rec = min(nav[sat], key=lambda q: abs(((jd - JD_GPS0) * 86400.0) % 604800.0 - q['toe']))
                    kep = keplerian_beta(rec, jd, s_hat)
                pos = ser[k][1]
                vel = velocity_ecef(ser, k)
                sun_e = rot_eci_to_ecef(s_hat * 1.495978707e11, theta)
                # Reference nominal X axis from the Kouba angle, in ECEF:
                # x = cos(psi) v_hat - sin(psi) h_hat, v_hat = h_hat x r_hat.
                r_hat = r_i / np.linalg.norm(r_i)
                v_hat = np.cross(h, r_hat)
                p = math.radians(psi)
                x_i = math.cos(p) * v_hat - math.sin(p) * h
                x_e = rot_eci_to_ecef(x_i, theta)
                fo.write(','.join([sat, f'{jd:.8f}'] + [f'{v:.6f}' for v in pos]
                                  + [f'{v:.9f}' for v in vel]
                                  + [f'{v:.3f}' for v in sun_e]
                                  + [f'{beta:.6f}', f'{kep:.6f}', f'{mu:.4f}', f'{psi:.4f}']
                                  + [f'{v:.9f}' for v in x_e]) + '\n')


def calendar_from_jd(jd):
    """Calendar [y, m, d, h, mi, s] of a Julian date (Meeus, Gregorian)."""
    z = math.floor(jd + 0.5)
    f = jd + 0.5 - z
    alpha = math.floor((z - 1867216.25) / 36524.25)
    b = z + 1 + alpha - math.floor(alpha / 4) + 1524
    c = math.floor((b - 122.1) / 365.25)
    d = math.floor(365.25 * c)
    e = math.floor((b - d) / 30.6001)
    day = b - d - math.floor(30.6001 * e)
    month = e - 1 if e < 14 else e - 13
    year = c - 4716 if month > 2 else c - 4715
    secs = round(f * 86400.0)
    return [int(year), int(month), int(day), secs // 3600, (secs % 3600) // 60, float(secs % 60)]


def cmd_cssrcsv(a):
    """Reference CSV built from cssrlib alone (Sun, inertial frame, nominal yaw frame)."""
    sys.path.insert(0, a.cssrlib)
    from cssrlib.gnss import epoch2time, gpst2utc
    from cssrlib.peph import eci2ecef, orb2ecef, sunmoonpos_eci

    sats = a.sats.split(',')
    sp3 = read_sp3(a.sp3, sats)
    erpv = np.zeros(5)

    def inertial(jd, r_ecef):
        tutc = gpst2utc(epoch2time(calendar_from_jd(jd)))
        u, _ = eci2ecef(tutc, erpv)
        return u.T @ r_ecef, u, tutc

    with open(a.out, 'w') as fo:
        fo.write('sat,jd,rx,ry,rz,vx,vy,vz,sx,sy,sz,beta_ref,beta_kep,mu_ref,psi_ref,'
                 'ix,iy,iz\n')
        for sat in sats:
            ser = sp3[sat]
            for k in range(2, len(ser) - 2, a.step):
                jd, r_ecef = ser[k]
                r_i, u, tutc = inertial(jd, r_ecef)
                r_m, _, _ = inertial(ser[k - 1][0], ser[k - 1][1])
                r_p, _, _ = inertial(ser[k + 1][0], ser[k + 1][1])
                h = np.cross(r_i, r_p - r_m)
                h /= np.linalg.norm(h)
                sun_i, _ = sunmoonpos_eci(tutc, True, False)
                s_hat = sun_i / np.linalg.norm(sun_i)
                beta, mu, psi = beta_mu_psi(r_i, h, s_hat)
                sun_e = u @ sun_i
                x_e = orb2ecef(epoch2time(calendar_from_jd(jd)), r_ecef)[0]
                vel = velocity_ecef(ser, k)
                fo.write(','.join([sat, f'{jd:.8f}'] + [f'{v:.6f}' for v in r_ecef]
                                  + [f'{v:.9f}' for v in vel]
                                  + [f'{v:.3f}' for v in sun_e]
                                  + [f'{beta:.6f}', 'nan', f'{mu:.4f}', f'{psi:.4f}']
                                  + [f'{v:.9f}' for v in x_e]) + '\n')


def cmd_compare(a):
    ref = [ln.strip().split(',') for ln in open(a.ref)][1:]
    cxx = [ln.strip().split(',') for ln in open(a.cxx)][1:]
    assert len(ref) == len(cxx), 'row count differs'
    worst_b, worst_x, worst_k, n_sign = 0.0, 0.0, 0.0, 0
    for r, c in zip(ref, cxx):
        b_ref, b_kep = float(r[11]), float(r[12])
        x_ref = np.array([float(v) for v in r[15:18]])
        b_cxx = float(c[2])
        x_cxx = np.array([float(v) for v in c[3:6]])
        worst_b = max(worst_b, abs(b_cxx - b_ref))
        if not math.isnan(b_kep):
            worst_k = max(worst_k, abs(b_kep - b_ref))
        worst_x = max(worst_x, math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(x_ref, x_cxx)))))))
        if abs(b_ref) > 1.0 and b_cxx * b_ref < 0:
            n_sign += 1
    print(f'rows {len(ref)}  max|beta_cxx - beta_ref| {worst_b:.4f} deg  '
          f'max|beta_kep - beta_ref| {worst_k:.4f} deg  '
          f'max angle(X_cxx, X_ref) {worst_x:.4f} deg  sign mismatches (|beta|>1) {n_sign}')


def cmd_follower(a):
    print('beta_deg  block  start_deg    end_deg  duration_s')
    for b in [float(x) for x in a.betas.split(',')]:
        for block, rate in BLOCK_RATE.items():
            res = follower_turn(b, rate, a.dt)
            if res is None:
                print(f'{b:8.2f}  {block:5s}  no turn')
            else:
                print(f'{b:8.2f}  {block:5s}  {res[0]:9.3f}  {res[1]:9.3f}  {(res[1] - res[0]) / GPS_ORBIT_RATE:9.1f}')


def cmd_windows(a):
    with open(a.sp3, 'r', errors='replace') as fh:
        head = fh.readline().split()
    date = (int(head[0][3:]), int(head[1]), int(head[2]))
    blocks = atx_blocks(a.atx, date)
    sats = sorted(s for s, b in blocks.items() if b.startswith(('IIF', 'IIR')))
    sp3 = read_sp3(a.sp3, sats)
    print('sat   block   noon_utc_h  beta_deg  turns   turn_start_utc  turn_end_utc')
    for sat in sats:
        if not sp3[sat]:
            continue
        block = 'IIF' if blocks[sat].startswith('IIF') else 'IIR'
        rate = BLOCK_RATE[block]
        beta0 = math.degrees(math.atan(GPS_ORBIT_RATE / rate))
        for hr, beta in noon_crossings(sp3[sat], sat):
            turn = abs(beta) < beta0
            when = ''
            if turn:
                res = follower_turn(beta, rate)
                if res:
                    t0 = hr + (res[0] - 180.0) / GPS_ORBIT_RATE / 3600.0
                    t1 = hr + (res[1] - 180.0) / GPS_ORBIT_RATE / 3600.0
                    when = f'{t0:14.3f}  {t1:12.3f}'
            print(f'{sat}  {block:5s}  {hr:10.3f}  {beta:8.3f}  {"yes" if turn else "no ":5s}  {when}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='mode', required=True)
    for name, fn in (('table', cmd_table), ('noon', cmd_noon), ('csv', cmd_csv)):
        p = sub.add_parser(name)
        p.add_argument('--sp3', required=True)
        p.add_argument('--nav')
        p.add_argument('--sats', required=True)
        p.add_argument('--step', type=int, default=6, help='sample stride (SP3 epochs)')
        if name == 'csv':
            p.add_argument('--out', required=True)
        p.set_defaults(fn=fn)
    p = sub.add_parser('cssrcsv')
    p.add_argument('--sp3', required=True)
    p.add_argument('--sats', required=True)
    p.add_argument('--step', type=int, default=6, help='sample stride (SP3 epochs)')
    p.add_argument('--out', required=True)
    p.add_argument('--cssrlib', default='D:/GnssProcessorSupport/reference_software/cssrlib/src',
                   help='directory that holds the cssrlib package')
    p.set_defaults(fn=cmd_cssrcsv)
    p = sub.add_parser('follower')
    p.add_argument('--betas', required=True)
    p.add_argument('--dt', type=float, default=0.25)
    p.set_defaults(fn=cmd_follower)
    p = sub.add_parser('windows')
    p.add_argument('--sp3', required=True)
    p.add_argument('--atx', required=True)
    p.set_defaults(fn=cmd_windows)
    p = sub.add_parser('compare')
    p.add_argument('--ref', required=True)
    p.add_argument('--cxx', required=True)
    p.set_defaults(fn=cmd_compare)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    sys.exit(main())
