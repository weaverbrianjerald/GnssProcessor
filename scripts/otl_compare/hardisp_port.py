#!/usr/bin/env python3
"""Pure-Python port of the IERS HARDISP ocean tide loading routine, as shipped in
NGS M-PAGES v1.2 (Lib/IersHardisp: HARDISP.F, ADMINT.F, TDFRPH.F, SHELLS.F, SPLINE.F,
EVAL.F, ETUTC.F, TOYMD.F, JULDAT.F, LEAP.F).

No Fortran compiler is installed on the development machine, so the reference routine is
ported line by line.  The 342-constituent tables (TAMP and IDD of ADMINT.F) are parsed
from the Fortran source itself, never retyped.  The port is checked against the two test
cases printed in the HARDISP.F header (Onsala and Reykjavik, 24 hourly epochs) by
`python hardisp_port.py --selftest`.

The M-PAGES wrapper evaluates one epoch per call (HARDISP.F, "Convert to subroutine"):
the epoch is a UTC calendar time whose seconds are truncated to an integer
(IT(5) = SECI with IT an INTEGER array), and SAMP = 0, so the recursion returns the first
sample only.  This port evaluates the same single-epoch quantity.

Arithmetic is double precision.  HARDISP keeps AMP, EVAL, SPLINE and RECURS in REAL
(single precision); the resulting differences are at the 1e-9 m level.

Usage as a library:
    from hardisp_port import Hardisp
    h = Hardisp(admint_path)
    u, s, w = h.evaluate(2024, 129, 3, 15, 30, amp, phs)   # amp, phs: [11][3] in BLQ row
                                                           # order (Up, West, South), phase lag deg
"""
import math
import re
import sys

DR = 0.01745329252          # DATA DR in HARDISP.F / DTR in ADMINT.F
NT = 342

# Cartwright-Tayler numbers of the 11 BLQ constituents (HARDISP.F, DATA IDT):
# M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa
IDT = [
    (2, 0, 0, 0, 0, 0), (2, 2, -2, 0, 0, 0), (2, -1, 0, 1, 0, 0), (2, 2, 0, 0, 0, 0),
    (1, 1, 0, 0, 0, 0), (1, -1, 0, 0, 0, 0), (1, 1, -2, 0, 0, 0), (1, -2, 0, 1, 0, 0),
    (0, 2, 0, 0, 0, 0), (0, 1, 0, -1, 0, 0), (0, 0, 2, 0, 0, 0),
]

DEFAULT_ADMINT = ("D:/GnssProcessorSupport/reference_software/mpages/M-PAGES_v1.2/"
                  "Lib/IersHardisp/ADMINT.F")

# ETUTC.F: TT-UTC step table from 1972.0 on (SI = 1 s each)
ST = [1972.5, 1973.0, 1974.0, 1975.0, 1976.0, 1977.0, 1978.0, 1979.0, 1980.0, 1981.5,
      1982.5, 1983.5, 1985.5, 1988.0, 1990.0, 1991.0, 1992.5, 1993.5, 1994.5, 1996.0,
      1997.5, 1999.0, 2006.0, 2009.0, 2012.5, 2015.5, 2017.0]


def _fortran_data_block(text, name):
    """Numbers of `DATA <name>/ ... /` from fixed-form Fortran source."""
    lines = []
    for ln in text.splitlines():
        if ln.startswith(("*", "c", "C")):
            continue
        lines.append(ln[6:] if len(ln) > 6 else "")
    body = "\n".join(lines)
    m = re.search(r"DATA\s+" + name + r"\s*/(.*?)/", body, re.S)
    if not m:
        raise ValueError("DATA " + name + " not found")
    return [float(x) for x in re.split(r"[,\s]+", m.group(1).strip()) if x]


def leap(iy):
    v = 1 - (iy % 4 + 3) // 4
    if iy % 100 == 0 and iy % 400 != 0:
        v = 0
    return v


def fdiv(a, b):
    """Fortran integer division (truncation toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def toymd(it1):
    """TOYMD.F: (year, day-of-year) -> (year, month, day)."""
    def idn(m):
        return (fdiv(367 * (m - 2 - 12 * fdiv(m - 14, 12)), 12) + 29) % 365

    def mon(jj, m):
        return fdiv(12 * (jj - 29 - m), 367) + 2 + fdiv(jj - 200, 169)
    y, doy = it1[0], it1[1]
    mo = mon(doy, leap(y))
    d = doy - idn(mo) - leap(y) * fdiv(9 + mo, 12)
    return (y, mo, d)


def juldat(it):
    y, m, d = it
    return (fdiv(1461 * (y + 4800 + fdiv(m - 14, 12)), 4)
            + fdiv(367 * (m - 2 - 12 * fdiv(m - 14, 12)), 12)
            - fdiv(3 * fdiv(y + 4900 + fdiv(m - 14, 12), 100), 4) + d - 32075)


def etutc(year):
    """TT-UTC [s] for years from 1972 (ETUTC.F)."""
    if year < 1972.0:
        raise ValueError("port covers 1972 and later only")
    delta = 42.184
    for st in ST:
        if year >= st:
            delta += 1.0
        if year < st:
            return delta
    return delta


class Hardisp:
    def __init__(self, admint_path=DEFAULT_ADMINT):
        text = open(admint_path, encoding="latin-1").read()
        self.tamp = _fortran_data_block(text, "TAMP")
        idd = _fortran_data_block(text, "IDD")
        assert len(self.tamp) == NT and len(idd) == 6 * NT, (len(self.tamp), len(idd))
        self.idd = [tuple(int(v) for v in idd[6 * i:6 * i + 6]) for i in range(NT)]
        self._tdkey = None
        self._d = self._dd = None

    # --- TDFRPH.F -----------------------------------------------------------
    def _setup_args(self, itm):
        if self._tdkey == itm:
            return
        self._tdkey = itm
        y, doy, hh, mm, ss = itm
        itm2 = toymd(itm)
        jd = juldat(itm2)
        dayfr = hh / 24.0 + mm / 1440.0 + ss / 86400.0
        year = y + (doy + dayfr) / (365.0 + leap(y))
        delta = etutc(year)
        djd = jd - 0.5 + dayfr
        t = (djd - 2451545.0 + delta / 86400.0) / 36525.0
        f1 = 134.9634025100 + t * (477198.8675605000 + t * (0.0088553333 + t * (0.0000143431 + t * (-0.0000000680))))
        f2 = 357.5291091806 + t * (35999.0502911389 + t * (-0.0001536667 + t * (0.0000000378 + t * (-0.0000000032))))
        f3 = 93.2720906200 + t * (483202.0174577222 + t * (-0.0035420000 + t * (-0.0000002881 + t * 0.0000000012)))
        f4 = 297.8501954694 + t * (445267.1114469445 + t * (-0.0017696111 + t * (0.0000018314 + t * (-0.0000000088))))
        f5 = 125.0445550100 + t * (-1934.1362619722 + t * (0.0020756111 + t * (0.0000021394 + t * (-0.0000000165))))
        d = [0.0] * 6
        d[0] = 360.0 * dayfr - f4
        d[1] = f3 + f5
        d[2] = d[1] - f4
        d[3] = d[1] - f1
        d[4] = -f5
        d[5] = d[2] - f2
        fd1 = 0.0362916471 + 0.0000000013 * t
        fd2 = 0.0027377786
        fd3 = 0.0367481951 - 0.0000000005 * t
        fd4 = 0.0338631920 - 0.0000000003 * t
        fd5 = -0.0001470938 + 0.0000000003 * t
        dd = [0.0] * 6
        dd[0] = 1.0 - fd4
        dd[1] = fd3 + fd5
        dd[2] = dd[1] - fd4
        dd[3] = dd[1] - fd1
        dd[4] = -fd5
        dd[5] = dd[2] - fd2
        self._d, self._dd = d, dd

    def tdfrph(self, idood):
        freq = sum(idood[i] * self._dd[i] for i in range(6))
        phase = sum(idood[i] * self._d[i] for i in range(6))
        phase = math.fmod(phase, 360.0)
        if phase < 0.0:
            phase += 360.0
        return freq, phase

    # --- SHELLS.F (ascending sort of x carrying the permutation k, ties by k) ---
    @staticmethod
    def shells(x):
        n = len(x)
        k = list(range(1, n + 1))
        order = sorted(range(n), key=lambda i: (x[i], k[i]))
        return [x[i] for i in order], [k[i] for i in order]

    # --- SPLINE.F (cubic spline second derivatives S; |NN|<=3 gives 0) ---
    @staticmethod
    def spline(n, x, u):
        s = [0.0] * n
        if n <= 3:
            return s

        def q(u1, x1, u2, x2):
            return (u1 / x1 ** 2 - u2 / x2 ** 2) / (1.0 / x1 - 1.0 / x2)
        q1 = q(u[1] - u[0], x[1] - x[0], u[2] - u[0], x[2] - x[0])
        qn = q(u[n - 2] - u[n - 1], x[n - 2] - x[n - 1], u[n - 3] - u[n - 1], x[n - 3] - x[n - 1])
        a = [0.0] * n
        s[0] = 6.0 * ((u[1] - u[0]) / (x[1] - x[0]) - q1)
        n1 = n - 1
        for i in range(1, n1):
            s[i] = (u[i - 1] / (x[i] - x[i - 1]) - u[i] * (1.0 / (x[i] - x[i - 1]) + 1.0 / (x[i + 1] - x[i]))
                    + u[i + 1] / (x[i + 1] - x[i])) * 6.0
        s[n - 1] = 6.0 * (qn + (u[n1 - 1] - u[n - 1]) / (x[n - 1] - x[n1 - 1]))
        a[0] = 2.0 * (x[1] - x[0])
        a[1] = 1.5 * (x[1] - x[0]) + 2.0 * (x[2] - x[1])
        s[1] = s[1] - 0.5 * s[0]
        for i in range(2, n1):
            c = (x[i] - x[i - 1]) / a[i - 1]
            a[i] = 2.0 * (x[i + 1] - x[i - 1]) - c * (x[i] - x[i - 1])
            s[i] = s[i] - c * s[i - 1]
        c = (x[n - 1] - x[n1 - 1]) / a[n1 - 1]
        a[n - 1] = (2.0 - c) * (x[n - 1] - x[n1 - 1])
        s[n - 1] = s[n - 1] - c * s[n1 - 1]
        s[n - 1] = s[n - 1] / a[n - 1]
        for j in range(1, n1 + 1):
            i = n - 1 - j
            s[i] = (s[i] - (x[i + 1] - x[i]) * s[i + 1]) / a[i]
        return s

    # --- EVAL.F ----------------------------------------------------------------
    @staticmethod
    def eval_spline(y, nn, x, u, s):
        if y <= x[0]:
            return u[0]
        if y >= x[nn - 1]:
            return u[nn - 1]
        k1 = k2 = 0
        for k in range(1, nn):
            if x[k - 1] < y and x[k] >= y:
                k1, k2 = k - 1, k
        dy = x[k2] - y
        dy1 = y - x[k1]
        dk = x[k2] - x[k1]
        deli = 1.0 / (6.0 * dk)
        ff1 = s[k1] * dy * dy * dy
        ff2 = s[k2] * dy1 * dy1 * dy1
        f1 = (ff1 + ff2) * deli
        f2 = dy1 * ((u[k2] / dk) - (s[k2] * dk) / 6.0)
        f3 = dy * ((u[k1] / dk) - (s[k1] * dk) / 6.0)
        return f1 + f2 + f3

    # --- ADMINT.F --------------------------------------------------------------
    def admint(self, ampin, phin):
        """Amplitude and phase [deg] at each of the 342 lines for one component.
        `phin` is the HARDISP phase argument (the negated BLQ lag)."""
        rl, aim, rf = [], [], []
        for ll in range(11):
            kk = self.idd.index(IDT[ll])
            rl.append(ampin[ll] * math.cos(DR * phin[ll]) / abs(self.tamp[kk]))
            aim.append(ampin[ll] * math.sin(DR * phin[ll]) / abs(self.tamp[kk]))
            fr, _ = self.tdfrph(self.idd[kk])
            rf.append(fr)
        rf, key = self.shells(rf)
        nlp = sum(1 for v in rf if v < 0.5)
        ndi = sum(1 for v in rf if 0.5 < v < 1.5)
        nsd = sum(1 for v in rf if 1.5 < v < 2.5)
        rl = [rl[k - 1] for k in key]
        aim = [aim[k - 1] for k in key]
        seg = [(0, nlp), (nlp, nlp + ndi), (nlp + ndi, nlp + ndi + nsd)]
        sr = [self.spline(b - a, rf[a:b], rl[a:b]) for a, b in seg]
        si = [self.spline(b - a, rf[a:b], aim[a:b]) for a, b in seg]
        amp, ph = [], []
        for i in range(NT):
            n1 = self.idd[i][0]
            if n1 == 0 and nlp == 0:
                continue
            f, p = self.tdfrph(self.idd[i])
            if n1 == 0:
                p += 180.0
            if n1 == 1:
                p += 90.0
            a, b = seg[n1]
            re_ = self.eval_spline(f, b - a, rf[a:b], rl[a:b], sr[n1])
            am_ = self.eval_spline(f, b - a, rf[a:b], aim[a:b], si[n1])
            amp.append(self.tamp[i] * math.sqrt(re_ ** 2 + am_ ** 2))
            p += math.atan2(am_, re_) / DR
            if p > 180:
                p -= 360.0
            ph.append(p)
        return amp, ph

    def evaluate(self, year, doy, hour, minute, second, amp, phs):
        """Single-epoch HARDISP: returns (up, south, west) in metres.
        amp, phs: [11][3] in BLQ row order (Up, West, South); phs are BLQ phase lags [deg]."""
        self._setup_args((year, doy, hour, minute, int(second)))
        out = []
        for comp in range(3):
            a = [amp[c][comp] for c in range(11)]
            p = [-phs[c][comp] for c in range(11)]
            am, ph = self.admint(a, p)
            out.append(sum(am[j] * math.cos(DR * ph[j]) for j in range(len(am))))
        return out[0], out[2], out[1]       # U, S, W


# --- self-test: the two cases printed in the HARDISP.F header ---------------------
ONSALA_AMP = [
    [.00352, .00123, .00080, .00032, .00187, .00112, .00063, .00003, .00082, .00044, .00037],
    [.00144, .00035, .00035, .00008, .00053, .00049, .00018, .00009, .00012, .00005, .00006],
    [.00086, .00023, .00023, .00006, .00029, .00028, .00010, .00007, .00004, .00002, .00001]]
ONSALA_PHS = [
    [-64.7, -52.0, -96.2, -55.2, -58.8, -151.4, -65.6, -138.1, 8.4, 5.2, 2.1],
    [85.5, 114.5, 56.5, 113.6, 99.4, 19.1, 94.1, -10.4, -167.4, -170.0, -177.7],
    [109.5, 147.0, 92.7, 148.8, 50.5, -55.1, 36.4, -170.4, -15.0, 2.3, 5.2]]
ONSALA_EXP = [(0.003094, -0.001538, -0.000895), (0.001812, -0.000950, -0.000193),
              (0.000218, -0.000248, 0.000421), (-0.001104, 0.000404, 0.000741),
              (-0.001668, 0.000863, 0.000646), (-0.001209, 0.001042, 0.000137),
              (0.000235, 0.000926, -0.000667), (0.002337, 0.000580, -0.001555),
              (0.004554, 0.000125, -0.002278), (0.006271, -0.000291, -0.002615),
              (0.006955, -0.000537, -0.002430), (0.006299, -0.000526, -0.001706),
              (0.004305, -0.000244, -0.000559), (0.001294, 0.000245, 0.000793),
              (-0.002163, 0.000819, 0.002075), (-0.005375, 0.001326, 0.003024),
              (-0.007695, 0.001622, 0.003448), (-0.008669, 0.001610, 0.003272),
              (-0.008143, 0.001262, 0.002557), (-0.006290, 0.000633, 0.001477),
              (-0.003566, -0.000155, 0.000282), (-0.000593, -0.000941, -0.000766),
              (0.001992, -0.001561, -0.001457), (0.003689, -0.001889, -0.001680)]
REYK_AMP = [
    [.02359, .01481, .00448, .00419, .00273, .00033, .00088, .00005, .00081, .00034, .00034],
    [.00514, .00280, .00089, .00078, .00106, .00074, .00035, .00018, .00004, .00001, .00003],
    [.00209, .00077, .00051, .00021, .00151, .00066, .00047, .00019, .00014, .00008, .00006]]
REYK_PHS = [
    [78.5, 102.3, 76.3, 104.1, -52.0, -160.4, -52.6, -128.0, -174.8, -175.7, -178.1],
    [54.2, 93.8, 38.9, 96.9, 2.3, -12.5, 3.1, -31.6, -144.4, -122.6, -173.5],
    [156.2, -167.1, 141.9, -164.9, 155.9, 178.7, 155.6, -168.5, 177.7, 178.7, 179.7]]
REYK_EXP = [(-0.005940, -0.001245, -0.000278), (0.013516, -0.001086, 0.003212),
            (0.029599, -0.000353, 0.005483), (0.038468, 0.000699, 0.005997),
            (0.038098, 0.001721, 0.004690), (0.028780, 0.002363, 0.001974),
            (0.013016, 0.002371, -0.001369), (-0.005124, 0.001653, -0.004390),
            (-0.021047, 0.000310, -0.006225), (-0.030799, -0.001383, -0.006313),
            (-0.032056, -0.003048, -0.004549), (-0.024698, -0.004288, -0.001314),
            (-0.010814, -0.004794, 0.002623), (0.005849, -0.004416, 0.006291),
            (0.020857, -0.003208, 0.008766), (0.030226, -0.001413, 0.009402),
            (0.031437, 0.000594, 0.007996), (0.024079, 0.002389, 0.004844),
            (0.009945, 0.003606, 0.000663), (-0.007426, 0.004022, -0.003581),
            (-0.023652, 0.003601, -0.006911), (-0.034618, 0.002505, -0.008585),
            (-0.037515, 0.001044, -0.008270), (-0.031544, -0.000402, -0.006125)]


def blq_columns(rows_amp, rows_phs):
    """BLQ rows [3][11] -> HARDISP [11][3]."""
    return ([[rows_amp[c][k] for c in range(3)] for k in range(11)],
            [[rows_phs[c][k] for c in range(3)] for k in range(11)])


def selftest(admint_path=DEFAULT_ADMINT):
    h = Hardisp(admint_path)
    worst = 0.0
    for label, amp_rows, phs_rows, expected in (("Onsala", ONSALA_AMP, ONSALA_PHS, ONSALA_EXP),
                                                 ("Reykjavik", REYK_AMP, REYK_PHS, REYK_EXP)):
        amp, phs = blq_columns(amp_rows, phs_rows)
        mx = 0.0
        for i, exp in enumerate(expected):
            # 2009-06-25 (day 176) 01:10:45 UTC plus i hours
            tot = 1 + i
            day = 176 + tot // 24
            hour = tot % 24
            u, s, w = h.evaluate(2009, day, hour, 10, 45, amp, phs)
            d = max(abs(u - exp[0]), abs(s - exp[1]), abs(w - exp[2]))
            mx = max(mx, d)
        print(f"{label}: max |port - HARDISP.F header| over 24 epochs = {mx:.2e} m "
              f"(the header prints 6 decimals)")
        worst = max(worst, mx)
    ok = worst < 1.5e-6
    print("selftest", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        path = DEFAULT_ADMINT
        for a in sys.argv[1:]:
            if a.endswith(".F"):
                path = a
        sys.exit(selftest(path))
    print(__doc__)
