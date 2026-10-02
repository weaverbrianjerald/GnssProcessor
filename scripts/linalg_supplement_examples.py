"""Numerical examples for the linear algebra supplement of the mathematical reference.

Every number quoted in docs/report/linear_algebra.tex (Examples 1 to 13) is
printed by this script. It needs only numpy and runs in under a minute. One
simulated epoch: eight GPS L1 satellites (azimuth and elevation below),
parameters east, north, up and receiver clock c*dt in metres, float
ambiguities in cycles. Each example has its own random seed, so one example
can be changed without disturbing the others.

    python scripts/linalg_supplement_examples.py
"""
import math

import numpy as np

np.set_printoptions(precision=4, suppress=True, linewidth=110)

C = 299_792_458.0                    # speed of light [m/s]
LAM = C / 1575.42e6                  # GPS L1 wavelength [m]
AZ_DEG = np.array([15, 70, 120, 160, 210, 250, 300, 340], float)
EL_DEG = np.array([62, 25, 40, 15, 55, 30, 18, 80], float)
AZ, EL = np.radians(AZ_DEG), np.radians(EL_DEG)
M = len(AZ)
LOS = np.column_stack([np.cos(EL) * np.sin(AZ), np.cos(EL) * np.cos(AZ), np.sin(EL)])
G = np.column_stack([-LOS, np.ones(M)])          # row i: [-u_i^T, 1]


def cond(A):
    """2-norm condition number sigma_max / sigma_min."""
    s = np.linalg.svd(A, compute_uv=False)
    return s[0] / s[-1]


def header(title):
    print("\n" + "=" * 78 + "\n" + title + "\n" + "=" * 78)


# --------------------------------------------------------------------------- 1 Norms
def example_1_robust(trials=4000, sigma=0.3, fault=20.0, k=1.5):
    header("Example 1: +20 m NLOS error on S4 (elevation 15 deg); L2 vs Huber vs L1")
    rng = np.random.default_rng(1)
    P = G @ np.linalg.solve(G.T @ G, G.T)        # hat matrix, an orthogonal projector
    r = 1.0 - np.diag(P)                         # redundancy numbers, sum = m - n
    print("  redundancy numbers:", r, " sum:", round(r.sum(), 6))

    def wls(y, w):
        return np.linalg.solve(G.T @ (w[:, None] * G), G.T @ (w * y))

    def huber(y):
        x = wls(y, np.ones(M))
        for _ in range(30):
            v = np.abs(y - G @ x) / sigma
            x = wls(y, np.where(v <= k, 1.0, k / np.maximum(v, 1e-12)))
        return x

    def least_abs(y):                            # L1 by iteratively reweighted LS
        x = wls(y, np.ones(M))
        for _ in range(200):
            x = wls(y, 1.0 / np.maximum(np.abs(y - G @ x), 1e-6))
        return x

    keep = np.arange(M) != 3
    runs = {"LS, no fault": [], "LS": [], "Huber, k = 1.5 sigma": [],
            "L1 (least abs. dev.)": [], "LS, S4 excluded": []}
    for _ in range(trials):
        e = rng.normal(0.0, sigma, M)
        y = e.copy()
        y[3] += fault
        runs["LS, no fault"].append(wls(e, np.ones(M)))
        runs["LS"].append(wls(y, np.ones(M)))
        runs["Huber, k = 1.5 sigma"].append(huber(y))
        runs["L1 (least abs. dev.)"].append(least_abs(y))
        runs["LS, S4 excluded"].append(np.linalg.lstsq(G[keep], y[keep], rcond=None)[0])
    for name, xs in runs.items():
        d = np.linalg.norm(np.array(xs)[:, :3], axis=1)
        print(f"  {name:22s} RMS 3-D position error {np.sqrt(np.mean(d ** 2)):7.3f} m")
    bias = np.linalg.solve(G.T @ G, G[3]) * fault
    print("  LS bias caused by the fault (E, N, U, clock) [m]:", bias)
    print(f"  fault left in S4's own residual: r_4 x 20 m = {r[3] * fault:.2f} m")
    print(f"  expected w-test statistic {fault * math.sqrt(r[3]) / sigma:.1f} (critical 3.29),"
          f" MDB for S4 {sigma * math.sqrt(17.075 / r[3]):.2f} m")


def ils_success(Q, n=1_000_000, seed=2):
    """Monte Carlo success rates of rounding and integer least squares (2-D)."""
    rng = np.random.default_rng(seed)
    L = np.linalg.cholesky(Q)
    Qi = np.linalg.inv(Q)
    cand = np.array([(i, j) for i in range(-4, 5) for j in range(-4, 5)], float)
    ok_round = ok_ils = 0
    for _ in range(n // 100_000):
        a = rng.standard_normal((100_000, 2)) @ L.T     # float solutions, true integer 0
        ok_round += np.count_nonzero(np.all(np.rint(a) == 0, axis=1))
        d = a[:, None, :] - cand[None, :, :]
        q = np.einsum("nkd,de,nke->nk", d, Qi, d)       # squared Q^-1 norm to candidates
        ok_ils += np.count_nonzero(np.all(cand[np.argmin(q, axis=1)] == 0, axis=1))
    return ok_round / n, ok_ils / n


def ils_cell(Q, reach=4):
    """Pull-in region of integer least squares around 0: Voronoi cell, Q^-1 metric."""
    Qi = np.linalg.inv(Q)
    poly = np.array([[-3.0, -3.0], [3.0, -3.0], [3.0, 3.0], [-3.0, 3.0]])
    for i in range(-reach, reach + 1):
        for j in range(-reach, reach + 1):
            if i == 0 and j == 0:
                continue
            z = np.array([i, j], float)
            nrm, rhs = Qi @ z, 0.5 * z @ Qi @ z         # keep the half-plane nrm.x <= rhs
            out = []
            for p, q in zip(poly, np.roll(poly, -1, axis=0)):
                fp, fq = nrm @ p - rhs, nrm @ q - rhs
                if fp <= 0:
                    out.append(p)
                if fp * fq < 0:
                    out.append(p + fp / (fp - fq) * (q - p))
            poly = np.array(out)
    return poly


def decorrelate_2d(Q, span=6):
    """Integer unimodular Z minimizing cond(Z Q Z^T); a 2-D stand-in for LAMBDA."""
    best = None
    vals = range(-span, span + 1)
    for a in vals:
        for b in vals:
            for c in vals:
                for d in vals:
                    if abs(a * d - b * c) != 1:
                        continue
                    Z = np.array([[a, b], [c, d]], float)
                    k = cond(Z @ Q @ Z.T)
                    if best is None or k < best[0] - 1e-12:
                        best = (k, Z)
    Z = best[1]
    for i in range(2):                              # sign convention: first entry positive
        if Z[i, np.flatnonzero(Z[i])[0]] < 0:
            Z[i] = -Z[i]
    return Z


def q_2d(rho, s1=0.30, s2=0.26):
    return np.array([[s1 * s1, rho * s1 * s2], [rho * s1 * s2, s2 * s2]])


def example_2_rounding_vs_ils():
    header("Example 2: rounding (L-infinity cells) vs integer least squares (Q^-1 metric)")
    for rho in (0.96, 0.98, 0.99):
        pr, pi = ils_success(q_2d(rho))
        print(f"  corr {rho:.2f}: cond {cond(q_2d(rho)):6.1f}   P(rounding) {pr:.3f}   P(ILS) {pi:.3f}")
    Q = q_2d(0.96)
    Z = decorrelate_2d(Q)
    Qz = Z @ Q @ Z.T
    pr, pi = ils_success(Qz)
    corr = Qz[0, 1] / math.sqrt(Qz[0, 0] * Qz[1, 1])
    print("  Z =", Z.astype(int).tolist(),
          f"  det Q = {np.linalg.det(Q):.4e}, det Qz = {np.linalg.det(Qz):.4e}")
    print(f"  after Z: std {np.sqrt(np.diag(Qz))}, corr {corr:+.3f}, cond {cond(Qz):.1f},"
          f" P(rounding) {pr:.3f}, P(ILS) {pi:.3f}")


def example_3_dop():
    header("Example 3: DOP as matrix norms")
    Qx = np.linalg.inv(G.T @ G)
    Gp = np.linalg.pinv(G)
    print("  singular values of G:", np.linalg.svd(G, compute_uv=False))
    print(f"  GDOP = sqrt(tr((G^T G)^-1)) = {math.sqrt(np.trace(Qx)):.4f}")
    print(f"  ||G^+||_F                   = {np.linalg.norm(Gp, 'fro'):.4f}")
    print(f"  ||G^+||_2 = 1 / sigma_min   = {np.linalg.norm(Gp, 2):.4f}")
    print(f"  HDOP {math.sqrt(Qx[0, 0] + Qx[1, 1]):.3f}   VDOP {math.sqrt(Qx[2, 2]):.3f}"
          f"   TDOP {math.sqrt(Qx[3, 3]):.3f}")


def example_4_determinant():
    header("Example 4: determinant vs condition number of H^T W H (E, N, U, clock, ZWD)")
    H = np.column_stack([G, 1.0 / np.sin(EL)])     # simple 1/sin(e) wet mapping
    print(f"  cond(G^T G) = {cond(G.T @ G):.1f};  with the zenith-delay column {cond(H.T @ H):.0f}")
    for sigma in (0.3, 1.0, 3.0, 5.0):
        N = H.T @ H / sigma ** 2
        print(f"  sigma {sigma:3.1f} m: det {np.linalg.det(N):10.3e}   cond {cond(N):9.3e}")


# ------------------------------------------------------------ 2 Hermitian matrices
def example_5_error_ellipse(sigma=0.3):
    header("Example 5: horizontal error ellipse (code only, sigma 0.3 m)")
    Qx = sigma ** 2 * np.linalg.inv(G.T @ G)
    Qen = Qx[:2, :2]
    lam, V = np.linalg.eigh(Qen)                  # ascending
    k95 = math.sqrt(-2.0 * math.log(0.05))        # sqrt of the 95 % chi-square, 2 dof
    major = V[:, 1]
    azimuth = math.degrees(math.atan2(major[0], major[1])) % 180.0
    print("  Q_EN [m^2]:", np.round(Qen, 4).tolist())
    print(f"  eigenvalues {lam[::-1]} m^2 -> 1-sigma semi-axes {np.sqrt(lam[::-1])} m")
    print(f"  95 % semi-axes (x {k95:.4f}) {k95 * np.sqrt(lam[::-1])} m,"
          f" major axis azimuth {azimuth:.1f} deg")
    print(f"  sigma_U {math.sqrt(Qx[2, 2]):.3f} m")


def example_6_double_differences(sigma=0.003, n=7):
    header("Example 6: double-difference covariance, 8 satellites (7 DDs), sigma 3 mm")
    one = np.ones((n, n))
    Qdd = 2 * sigma ** 2 * (np.eye(n) + one)
    Qinv = (np.eye(n) - one / (n + 1)) / (2 * sigma ** 2)
    print("  eigenvalues [mm^2]:", np.linalg.eigvalsh(Qdd) * 1e6)
    print(f"  Sherman-Morrison inverse: max |Q^-1 Q - I| = {np.abs(Qinv @ Qdd - np.eye(n)).max():.1e}")
    L = np.linalg.cholesky(Qdd)
    white = np.linalg.solve(L, np.linalg.solve(L, Qdd).T)
    print(f"  whitened covariance L^-1 Q L^-T: max |. - I| = {np.abs(white - np.eye(n)).max():.1e}")
    naive = 1.0 / (4 * sigma ** 2)
    print(f"  diagonal weights instead: common mode weighted x{naive * 2 * sigma ** 2 * (n + 1):.1f},"
          f" other directions x{naive * 2 * sigma ** 2:.2f}")


def example_7_antenna_array(elements=4, snapshots=4000):
    header("Example 7: 4-element array; jammer 40 deg JNR 40 dB; satellite -15 deg SNR -20 dB")
    rng = np.random.default_rng(7)

    def steer(theta_deg):                         # half-wavelength uniform linear array
        return np.exp(1j * np.pi * np.arange(elements) * math.sin(math.radians(theta_deg)))

    def cnoise(*shape):
        return (rng.standard_normal(shape) + 1j * rng.standard_normal(shape)) / math.sqrt(2)

    a_j, a_s = steer(40.0), steer(-15.0)
    jnr, snr = 10 ** 4.0, 10 ** -2.0
    X = (cnoise(elements, snapshots)
         + np.outer(a_j, math.sqrt(jnr) * cnoise(snapshots))
         + np.outer(a_s, math.sqrt(snr) * np.exp(2j * np.pi * rng.random(snapshots))))
    R = X @ X.conj().T / snapshots                # sample covariance, Hermitian
    lam, U = np.linalg.eigh(R)
    print(f"  ||R - R^H|| = {np.linalg.norm(R - R.conj().T):.1e}; eigenvalues:", lam)
    P = np.eye(elements) - np.outer(U[:, -1], U[:, -1].conj())

    def gain(v):
        return 10 * math.log10(np.linalg.norm(P @ v) ** 2 / np.linalg.norm(v) ** 2)

    print(f"  projection: jammer direction {gain(a_j):.1f} dB, satellite direction {gain(a_s):.2f} dB")

    def sjnr(w):
        s = snr * abs(np.vdot(w, a_s)) ** 2
        j = jnr * abs(np.vdot(w, a_j)) ** 2
        return 10 * math.log10(s / (j + np.linalg.norm(w) ** 2))

    Ri = np.linalg.inv(R)
    for name, w in (("single element", np.eye(elements)[:, 0]),
                    ("beam steered at satellite", a_s / elements),
                    ("projection, then beam", P @ a_s),
                    ("MVDR", Ri @ a_s / np.vdot(a_s, Ri @ a_s))):
        print(f"  {name:26s} output SJNR {sjnr(w):6.1f} dB")
    print(f"  clean-sky reference: SNR + 10 log10(M) = {-20 + 10 * math.log10(elements):.1f} dB")


# ----------------------------------------------------------------------------- 3 SVD
def example_8_weakest_direction():
    header("Example 8: the weakest direction of the geometry")
    _, s, Vt = np.linalg.svd(G, full_matrices=False)
    v = Vt[-1] if Vt[-1][2] > 0 else -Vt[-1]
    Qx = np.linalg.inv(G.T @ G)
    print("  singular values:", s)
    print("  weakest direction v4 (E, N, U, clock):", v)
    print("  range change along v4, G v4 [m]:", G @ v)
    print(f"  corr(up, clock) {Qx[2, 3] / math.sqrt(Qx[2, 2] * Qx[3, 3]):+.3f}")


def rotation(yaw, pitch, roll):
    y, p, r = np.radians([yaw, pitch, roll])
    Rz = np.array([[math.cos(y), -math.sin(y), 0], [math.sin(y), math.cos(y), 0], [0, 0, 1]])
    Ry = np.array([[math.cos(p), 0, math.sin(p)], [0, 1, 0], [-math.sin(p), 0, math.cos(p)]])
    Rx = np.array([[1, 0, 0], [0, math.cos(r), -math.sin(r)], [0, math.sin(r), math.cos(r)]])
    return Rz @ Ry @ Rx


def euler(R):
    return np.degrees([math.atan2(R[1, 0], R[0, 0]), math.asin(-R[2, 0]),
                       math.atan2(R[2, 1], R[2, 2])])


def wahba(r, b):
    """Rotation minimizing sum ||r_i - R b_i||^2, by the SVD of B = sum r_i b_i^T."""
    B = sum(np.outer(ri, bi) for ri, bi in zip(r, b))
    U, s, Vt = np.linalg.svd(B)
    return U @ np.diag([1.0, 1.0, np.linalg.det(U @ Vt)]) @ Vt, s


def example_9_attitude(trials=5000, noise=0.003):
    header("Example 9: attitude from two antenna baselines (Wahba's problem)")
    rng = np.random.default_rng(9)
    truth = np.array([37.0, 2.5, -1.2])                  # yaw, pitch, roll [deg]
    Rt = rotation(*truth)
    b = np.array([[1.5, 0.0, 0.0], [0.0, 1.0, 0.0]])     # body frame: forward, side [m]
    err = []
    for _ in range(trials):
        r = b @ Rt.T + rng.normal(0.0, noise, b.shape)   # RTK baselines, local frame
        R, s = wahba(r, b)
        err.append(euler(R) - truth)
    print("  RMS error yaw / pitch / roll [deg]:", np.sqrt(np.mean(np.square(err), axis=0)))
    print("  singular values of B (last trial):", s)
    Cd = Rt + 1e-3 * rng.standard_normal((3, 3))        # drifted attitude matrix
    U, _, Vt = np.linalg.svd(Cd)
    Cn = U @ Vt
    print(f"  ||C^T C - I||_F: {np.linalg.norm(Cd.T @ Cd - np.eye(3)):.1e}"
          f" -> {np.linalg.norm(Cn.T @ Cn - np.eye(3)):.1e}")


def example_10_common_mode(stations=12, days=730):
    header("Example 10: common-mode error by PCA (synthetic network, up component, mm)")
    rng = np.random.default_rng(10)
    cme = np.zeros(days)
    for t in range(1, days):
        cme[t] = 0.95 * cme[t - 1] + rng.normal()
    cme = 2.5 * (cme - cme.mean()) / cme.std()
    response = rng.uniform(0.8, 1.2, stations)
    X = np.outer(cme, response) + rng.normal(0.0, 2.0, (days, stations))
    X -= X.mean(axis=0)
    U, s, Vt = np.linalg.svd(X, full_matrices=False)
    share = s ** 2 / np.sum(s ** 2)
    X1 = X - s[0] * np.outer(U[:, 0], Vt[0])

    def rms(A):
        return np.sqrt(np.mean(A ** 2, axis=0)).mean()

    print(f"  variance share of PC1..PC3: {share[:3]}")
    print(f"  mean station RMS {rms(X):.2f} mm -> {rms(X1):.2f} mm after removing PC1")
    print(f"  |corr(PC1, true common mode)| {abs(np.corrcoef(U[:, 0], cme)[0, 1]):.3f}")
    print(f"  |corr(V1, true station response)| {abs(np.corrcoef(Vt[0], response)[0, 1]):.3f}")


# -------------------------------------------------------------------- 4 Pseudoinverse
def single_epoch(sig_p, sig_phi, clock_in_seconds=False, seed=11):
    """Single-epoch float code + phase solution by several routes."""
    rng = np.random.default_rng(seed)
    Gc = G.copy()
    if clock_in_seconds:
        Gc[:, 3] *= C                                    # parameter dt [s], partial c
    A = np.block([[Gc, np.zeros((M, M))], [Gc, LAM * np.eye(M)]])
    w = np.r_[np.full(M, sig_p ** -2), np.full(M, sig_phi ** -2)]
    x_true = np.r_[3.0, -2.0, 5.0, 12.0 / C if clock_in_seconds else 12.0,
                   rng.integers(-20, 21, M)]             # ambiguity corrections [cycles]
    y = A @ x_true + np.r_[rng.normal(0, sig_p, M), rng.normal(0, sig_phi, M)]
    sw = np.sqrt(w)
    Aw, yw = A * sw[:, None], y * sw                     # whitened system
    N, b = Aw.T @ Aw, Aw.T @ yw                          # normal equations
    _, s, Vt = np.linalg.svd(Aw, full_matrices=False)
    Qx = (Vt.T / s ** 2) @ Vt                            # (A^T W A)^-1 without forming N
    x_ref = np.linalg.lstsq(Aw, yw, rcond=None)[0]       # SVD least squares, double
    x_code = np.linalg.lstsq(Gc / sig_p, y[:M] / sig_p, rcond=None)[0]
    L = np.linalg.cholesky(N)
    s_N = np.linalg.svd(N, compute_uv=False)
    Q32, R32 = np.linalg.qr(Aw.astype(np.float32))
    solves = {
        "double, Cholesky on N": np.linalg.solve(L.T, np.linalg.solve(L, b)),
        "double, pinv(N) b": np.linalg.pinv(N) @ b,
        "double, pinv(W^1/2 A) W^1/2 y": np.linalg.pinv(Aw) @ yw,
        "single, inv(N) b": (np.linalg.inv(N.astype(np.float32))
                             @ b.astype(np.float32)).astype(float),
        "single, QR on W^1/2 A": np.linalg.solve(R32, Q32.T @ yw.astype(np.float32)).astype(float),
    }
    return dict(N=N, w=w, Aw=Aw, Qx=Qx, x_ref=x_ref, x_code=x_code, solves=solves,
                truncated=int(np.sum(s_N <= 1e-15 * s_N[0])))


def example_11_12_code_and_phase():
    header("Examples 11-12: single-epoch code + phase, float ambiguities")
    cases = (("geodetic: sigma_P 0.3 m, sigma_phi 3 mm", 0.3, 0.003, False),
             ("low-cost: sigma_P 3 m, sigma_phi 2 mm", 3.0, 0.002, False),
             ("geodetic, clock parameter in SECONDS", 0.3, 0.003, True))
    for label, sp, sf, secs in cases:
        d = single_epoch(sp, sf, secs)
        print(f"\n  {label}")
        print(f"    cond(W) {d['w'].max() / d['w'].min():.2e}   cond(W^1/2 A) {cond(d['Aw']):.2e}"
              f"   cond(N) {cond(d['N']):.2e}   pinv(N) zeroes {d['truncated']} of"
              f" {len(d['N'])} singular values")
        print("    |position(code + phase) - position(code only)| ="
              f" {np.abs(d['x_ref'][:3] - d['x_code'][:3]).max():.1e} m")
        for name, x in d["solves"].items():
            print(f"    {name:32s} position error {np.abs(x[:3] - d['x_ref'][:3]).max():.2e} m")
        if secs:
            print("    pinv(N) position [m]:", d["solves"]["double, pinv(N) b"][:3],
                  "  reference:", d["x_ref"][:3])
        else:
            ev = np.linalg.eigvalsh(d["Qx"][4:, 4:])
            print(f"    eigenvalues of Q_a [cycles^2]: {ev}")
            print(f"    cond(Q_a) {ev[-1] / ev[0]:.5e}   1 + (sigma_P / sigma_phi)^2 ="
                  f" {1 + (sp / sf) ** 2:.5e}")


def example_13_clock_network(noise=0.01):
    header("Example 13: clock network with a datum defect (pinv vs pivot S-basis)")
    rng = np.random.default_rng(13)
    us = 1e-6 * C                                        # one microsecond in metres
    rx = np.array([95.2, -410.7, 188.9]) * us            # receiver clocks
    sv = np.array([-212.4, 35.8, 468.0, -87.3, 150.6, -301.9, 12.5, 243.1]) * us
    nr = len(rx)

    def solve_both(sats):
        rows, y = [], []
        for r in range(nr):
            for j, s in enumerate(sats):
                a = np.zeros(nr + len(sats))
                a[r], a[nr + j] = 1.0, -1.0              # y_rs = dt_r - dt^s
                rows.append(a)
                y.append(rx[r] - sv[s] + rng.normal(0.0, noise))
        A, y = np.array(rows), np.array(y)
        x_pinv = np.linalg.pinv(A) @ y                   # minimum-norm solution
        x_piv = np.r_[0.0, np.linalg.lstsq(A[:, 1:], y, rcond=None)[0]]  # receiver 1 := 0
        return A, x_pinv, x_piv

    sats = list(range(len(sv)))
    A, xp, xs = solve_both(sats)
    print(f"  {A.shape[1]} parameters, rank {np.linalg.matrix_rank(A)},"
          f" smallest singular value {np.linalg.svd(A, compute_uv=False)[-1]:.1e}")
    print(f"  pinv: sum of all clock estimates {xp.sum():.1e} m")
    d_pinv, d_piv = xp[nr + 1:] - xp[nr], xs[nr + 1:] - xs[nr]
    print(f"  estimable dt^s - dt^1: max |pinv - pivot| {np.abs(d_pinv - d_piv).max():.1e} m")
    print(f"  receiver clocks [km]: pinv {xp[:nr] / 1e3}   pivot {xs[:nr] / 1e3}")
    Qp = np.linalg.pinv(A.T @ A)
    Qs = np.zeros_like(Qp)
    Qs[1:, 1:] = np.linalg.inv(A[:, 1:].T @ A[:, 1:])
    print(f"  trace of the covariance (unit variance): pinv {np.trace(Qp):.3f}, pivot {np.trace(Qs):.3f}")
    k = int(np.argmax(sv))
    x_all = np.r_[rx, sv]
    _, xp2, xs2 = solve_both([s for s in sats if s != k])
    jump_p, jump_s = xp2[:nr] - xp[:nr], xs2[:nr] - xs[:nr]
    print(f"  satellite {k + 1} (clock {sv[k] / us:+.1f} us) sets:")
    print(f"    receiver-clock change, pinv : {jump_p} m = {jump_p.mean() / us:+.2f} us")
    print(f"    predicted (x_k - mean) / (n - 1): {(sv[k] - x_all.mean()) / (len(x_all) - 1):.1f} m")
    print(f"    receiver-clock change, pivot: {jump_s} m")


if __name__ == "__main__":
    example_1_robust()
    example_2_rounding_vs_ils()
    example_3_dop()
    example_4_determinant()
    example_5_error_ellipse()
    example_6_double_differences()
    example_7_antenna_array()
    example_8_weakest_direction()
    example_9_attitude()
    example_10_common_mode()
    example_11_12_code_and_phase()
    example_13_clock_network()
