# GL — the gross line-of-sight screen

**STATUS**

- **id:** GL
- **topic:** raim-qc
- **status:** open
- **summary:** the gross line-of-sight screen
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.gross_los_screen`
- **hashes named in the text** (commit / exe / byte-cert): `87a33bf0`, `7702af1`, `8a018a3`, `2997163e`
- **run dirs named in the text:** `runs/_gl_on6`, `runs/_gl`
- **source:** `docs/archive/SPEC_event_harness.md` — 24.18

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.18 Card GL — the gross line-of-sight screen (pre-registered before any arm is read)

**Diagnosis first (code read, cited file:line):** why the post-fit RAIM pool filed ZERO rows for DJIG/G06 over k 210..240 of `_cm_pvb`
(code residuals −75 … −142 m on three signals; `network_raim.csv` 964 rows in the run, none for that satellite or station in the window;
MDB `mdb_exceedance` twice, action `observe`) and for BSHM00ISR in hours 5–6 of `_cm_c10` — the pool's membership rules on the leveling
vehicle (`pl_amb`; `raim_carry_invariant` auto-disabled), any age / elevation / tie / redundancy gate, and MDB's action table.
**Build (default OFF, byte-identical OFF, never fingerprinted; an EVENT at both edges with counters — invariant #7):** the minimal ACTING
response the diagnosis names — preferably a scope or action change to the existing screen (e.g. MDB `observe` → an acting mode, or the
pool admitting the untied low-elevation LOS) rather than a new mechanism; if a new screen is needed: per (station, satellite) LOS, code
residuals beyond k·σ (k = 8) on ≥ 2 signals for ≥ 2 consecutive epochs ⇒ the LOS is declared GROSS (event, decoupling edge): its
ambiguities refloated, its per-LOS iono re-seeded from the SH field, its code rows deweighted (×100 σ) until the residual returns under
3σ for 2 epochs (event, re-coupling edge); the epoch always emits.  Counters: LOS declared / cleared / currently gross, rows deweighted,
by station and satellite.  Gates: a new unit-test file, the Debug suite, smokes OFF / ON (`87a33bf0…` OFF; ON on the smoke should be
byte-identical too unless the smoke carries a gross LOS — report), `--verify`.  **Arm:** c10's posture 6 h with the screen ON
(`runs/_gl_on6`) against `_cm_c10`, hour by hour.  **Bars (frozen):** station code-rms max < 5 m in every hour; DJIG/G06 and BSHM bounded
(their LOS residuals < 5 m within 5 epochs of the declaration); (i) p50 in hours 3–6 within 0.2 m of hour 2 (the drift test); (ii) and the
consumer's per-hour p50 not worse than c10's; availability identical; the counters live.  **Frozen reading:** all bars ⇒ the gross-LOS
admission WAS the drift's channel and the screen joins the posture; max bounded but (i) still drifts ⇒ the drift has a second channel
in the bulk (recorded, next read); the screen declares > 5 % of LOS ⇒ the threshold is wrong — report the |v| / σ distribution, do not
tune blind.

**§24.18 RESULTS (2026-09-14 ~09:00 local; worker on worktree-agent-a5f7e6e737d28c009 `7702af1` / `8a018a3`, merged; exe `2997163e…`; suite
2109 / 286 (2092 + 17 fixture-gated, 0 failed); smokes OFF `87a33bf0…`, ON identical; `--verify` PASS; manager grade).**
**Diagnosis (`runs/_gl/diagnosis.md`, cited):** every ACTING post-fit tier in this producer pools carried-ambiguity PHASE rows — the
admission test `if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;` (`NetworkRaim.cpp:328`) is +1 on a phase row and 0 on every code row —
so a code row can never enter the σ̂₀ pool, the median-polish table, the consensus or the attribution: a gross CODE error is not
under-weighted, it is UNREACHABLE.  MDB (`NetworkMdb.cpp:286`) publishes a reliability number that never reads the residual, with the action
column the literal `"observe"`; the assembly leaves CODE rows "deliberately untouched" (`ApplicationNetworkAssembly.cpp:231`).  DJIG/G06's
31-epoch arc ran C5Q −141.9 m against a worst PHASE residual of 0.024 m (5 821×): normalised, its phase never reached the k = 6 bar (4.5–5.7)
while its code cleared it ~20× every epoch; `network_raim.csv` zero rows for the pair, ever; its pinned signals were outside the pool by
construction (elected anchor at its first epoch).  BSHM is the other half: the phase tiers DO see it (475 RAIM rows, 242 T4 refloats in
hours 5–6) and their only response — refloat an ambiguity — cannot bound a 449 m code row; its fault is a 197 m ZWD with `std_m` 0.05.
**Build:** `network.gross_los_screen` + 9 companions (`NetworkGrossLos` seam; the actuator = the CODE counterpart of `phase_sigma_scale` in
`networkSplitStation_`; called inside SB_RAIM after `networkPostFitRaim`; `GROSSLOS` events at both edges; every denominator counted).
Three guards, each measured: `gross_los_warmup_epochs` 120 (unguarded, the 20-epoch cert smoke declared 39.5 % of LOS-epochs — a cold
post-fit code residual is the un-converged station block — `sigma0_pf_code` 3.2 → 2 334, 9 of 20 epochs withheld); `gross_los_max_station_frac`
0.5 (BSHM's over-bar fraction reaches 0.88–1.00; deweighting every code row of a station removes what determines its clock); and the
deweight UNDO in the screen's own statistic (`8a018a3`: the ×100 fold made a declared LOS read 1.28σ at |v| 144.7 m and release one epoch
later — 425 declarations / 422 releases in a limit cycle; fixed, DJIG/G06 declared once and held).
**The arm (`runs/_gl_on6` = c10 + the screen, 6 h, 720/720 emitted; 351 561 LOS-epochs, 2.15 % over the bar, 70 declarations / 19 releases /
19 expiries, 28 092 code rows folded at σ × 100, 150 refloats, stand-down held 314 times — BSHM 71):**

| hour | gl_on6 (i) p50 / p90 / max m | c10 (i) p50 / p90 / max | (ii) gl / c10 | consumer p50 gl / c10 |
|---|---|---|---|---|
| 1 | 0.873 / 1.647 / 3.42 | 0.873 / 1.647 / 3.42 | 17 / 17 | 10.02 / 10.02 |
| 2 | 1.090 / 2.120 / 3.46 | 1.089 / 2.120 / 3.46 | 8 / 8 | 1.94 / 1.94 |
| 3 | 1.488 / 2.301 / **3.32** | 1.502 / 3.064 / 63.1 | 10 / 13 | 1.89 / 2.78 |
| 4 | 2.081 / 2.963 / **5.37** | 1.909 / 3.841 / 85.0 | 12 / 13 | **25.45** / 1.41 |
| 5 | 1.893 / 3.058 / **5.89** | 2.012 / 4.671 / 91.0 | 12 / 10 | 2.37 / 2.04 |
| 6 | 1.672 / 2.704 / **6.77** | 2.138 / 6.155 / 83.6 | 7 / 10 | 3.70 / 4.37 |

Hours 1–2 reproduce the control to 0.001 m (the OFF path is inert until the horizon opens — a same-build confirmation).  **Bars:** station
max < 5 m FAILS by a hair (5.4 / 5.9 / 6.8 against 85 / 91 / 84 — a 13–16× collapse of the tail); (i) within 0.2 m of hour 2 FAILS (+0.40 /
+0.99 / +0.80 / +0.58, the trend reversing after hour 4 where the control rises monotonically); (ii) better in 4 of 5 acting hours;
availability identical; counters live; **the consumer FAILS hour 4 (25.5 m)**.  **Reading: the second — the max is bounded and (i) still
drifts, so the bulk has a second channel (R8 below).**  Two readings §24.18 could not anticipate: (a) the "LOS residual < 5 m" bar is not
measurable for a deweight — a deweight bounds INFLUENCE, not the residual (DJIG/G06 stays at ~144 m in both arms; at arc end 2.9 vs 14.9 m);
grade this response class on the station rms / (i) / (ii) / consumer; (b) **the hour-4 consumer is the missing third guard**: at tow 271200
the screen declared TEN lines of sight, every one satellite G17 (SEYG NYA2 CEBR LEIJ MIZU LCK4 KIT3 ULAB URUM SVTL) — a satellite wrong at
ten stations is a SATELLITE fault, and deweighting it network-wide took the consumer to 56 m decaying over an hour.  The whole-satellite
stand-down is the same rule one axis over, and the population RAIM's `raim_min_stations = 3` consensus already exists to route it.
**Status: BUILT, default OFF, byte-identical, NOT consumer-safe without §24.19; not armed anywhere.**

