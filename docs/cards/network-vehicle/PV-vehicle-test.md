# PV — the VEHICLE test — carry_invariant vs leveling, pre-registered

**STATUS**

- **id:** PV
- **topic:** network-vehicle
- **status:** open
- **summary:** the VEHICLE test — carry_invariant vs leveling, pre-registered
- **dates in the text:** 2026-09-09 → 2026-09-14
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): `7e9918d`, `eee1b90`, `13c1d7ef`, `c1a13fb`, `87a33bf0`
- **run dirs named in the text:** `runs/_cm_pva`, `runs/_cm_pvb`
- **source:** `docs/archive/SPEC_event_harness.md` — 24.11

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.11 Card PV — the VEHICLE test (pre-registered 2026-09-14 before any arm is read; the author's go)

The F3 field ladder (SPEC_amb_iono §6.15.4, 2026-09-09) already found the converging leveling posture on the field —
`carry_invariant: false` + `relevel_free_epoch` + `carry_coupling_gauge_project` + the sim golden's phase-datum family
(`config/test_suite/_fldf3_pl2.json`: 1440/1440 solved, 0 withholds, ZWD span 0.8 m at σ 8 mm, DD-tropo arc MAD 1.22 → 0.07 m,
DD wander 10 → 0.12 cyc, float cold consumer 6.6 → 3.8 m) — and named the shipped vehicle's (I, N, ZWD) weakly-determined
direction from the DD side; §24.9 names the same direction from the code / wire side.  pl2 was never read with §24's
instruments.  **Two arms, 2 h cold from the CM control's start, the §24.8 traces on:** **PV-A** = `_fldf3_pl2` as-is on its own
chain (the known-converging reference; `runs/_cm_pva`); **PV-B** = the field golden exactly as `_cm_a0` ran it + the vehicle
delta ONLY (the pl2 `network` block: `carry_invariant` and its sub-family off, the EWL/UWL lanes off, `relevel_free_epoch`,
`carry_coupling_gauge_project`, `phase_datum_eq34: false` + `phase_datum_prioritized` + `phase_datum_handover_continuous` +
`phase_anchor_carry_reref` + `phase_datum_min_stations: 2`; `runs/_cm_pvb`) — the single-variable A/B against A0.  Every other
knob that differs between the flattened pl2 chain and the golden is classified (VEHICLE / GOLDEN-NEWER / CHAIN-ERA) and
recorded before B runs; a knob the loader refuses with the vehicle off is recorded with its line and set off (it is then part
of the vehicle delta by the loader's own definition).  **Reads:** §24.8's (i)–(v) by `scripts/cm_grade.py` on the last hour,
plus the cold closed-loop consumer's 3D at 1 h and 2 h and its p50 over the last 30 min (reported, no bar — §24.1).
**Bars (frozen = §24.8's):** (i) per-station code residual rms p50 < 1.0 m AND (ii) zero stations with |ZWD| > 0.5 m AND
(iii) D_phase declared within-arc rms < 0.5 m; availability within 5 % of A0.  **Frozen reading:** PV-B crosses all three ⇒ the
vehicle IS the wire defect — the field golden moves to the leveling vehicle (a posture card: goldens, harness seeds and the
vehicle-specific machinery re-based) and the consumer's remaining error is read on a determined block; PV-B leaves (ii)
physical but (i) / (iii) at metres ⇒ a second, code-side defect independent of the vehicle — the §24.8 knob arms are re-run
on the leveling vehicle, where a knob can show; PV-B diverges or withholds where PV-A converges ⇒ the golden carries a knob
incompatible with the vehicle — the classified diff names the candidates and the bisect is one arm per class.  Card ZP's
audit trace (§24.10) is read on both vehicles beside this card.

**§24.11 RESULTS (2026-09-14 ~00:00 local; worker on worktree-agent-a0bb119d8e53672db, commits 7e9918d → eee1b90, fast-forwarded to
main; exe `13c1d7ef…` = main's Release at c1a13fb, smoke `87a33bf0…`; manager grade).**  Pre-registration committed 23:38, first arm
launched 23:44.  PV-A had to be FLATTENED (`_fldf3_pl2` is the 9th file of its chain — the loader's 8-level ceiling); PV-B =
`_cm_a0/arm.json` + exactly 16 vehicle keys (+ 2 paths).  The classified diff of the pl2 chain against the golden: 52 leaf keys —
VEHICLE 16, GOLDEN-NEWER 13 (kept), CHAIN-ERA 17 (kept; the load-bearing ones: `frequency_mode` triple → dual, the EXT-CB external-BIA
posture against the golden's W6 OSB, `cb_kaula_prior` off, SH degree 15 + layer 2 against 8 with the retired 32.24 ± 2 GIM pin,
`apriori_clock_anchor_sigma_m` 0.02), RUN-SCOPE 6 (`runs/_cm_pvb/posture_diff.md`).  All three arms cold, 240/242 emitted, 0 withheld
— no S-basis null-space withhold anywhere (the old chain's divergence did not recur); loader refusals none; `raim_carry_invariant`
auto-DISABLED with the vehicle off (`pl_amb` is the pool there), `emit_pb_filter` and `state_snapshot_carry` INERT.  Vehicle lines live
on PV-B: 394 free-epoch latches / 250 inject-site skips, Card-1f gauge projection on, 61 anchor handovers (60 continuous), 139
phase-anchor reref family-epochs.

| arm (last hour) | (i) code rms p50 / p90 / max m | (ii) stations \|ZWD\| > 0.5 m (p50; worst) | (iii) D_phase / D_code / clock LEVEL p50 m | (iv) LOS-mean SS / dispersive | block-Baarda G | consumer 3D p50 last 30 min (at 2 h) | wall |
|---|---|---|---|---|---|---|---|
| A0 (carry_invariant) | 4.67 / 11.02 / 21.69 | 56 (3.21; NIUM −38.4, CAS1 −20.5, COCO +19.8) | 2.74 / 1.20 / 4.09 | 82.2 % / 16.1 % (shape REFUTED) | 9.958 | 7.80 (8.93) | 581 s |
| PV-A (pl2 as-is, EXT-CB) | not readable (below) | 13 (0.241; BSHM +4.3, MAC1 −4.2, KIT3 +1.9) | 1.07 / — / 0.06 † | — | 1.562 | 2.26 (2.18) | 156 s |
| **PV-B (golden + vehicle)** | **1.79 / 3.28 / 7.08** | **13 (0.285; BOGT −3.5, GOPE +2.5, GAMB −2.1)** | **0.95 / 0.13 / 0.74** | 97.8 % / 5.6 % (shape ADMITTED) | **1.375** | **1.19 (0.55)** | 298 s |

† PV-A's chain pins `apriori_clock_anchor_sigma_m: 0.02` — its clock LEVEL is not comparable.  Availability identical (99.2 %) in all
three.  prires rxcb 2.106 → 1.086; the tropo-shaped share of the code residual 1.4 % → 15.2 % (the residual acquires a mapping-function
shape as the gross error leaves); SH a₀₀ 30.3 → 27.6 TECU, in band.  0 consumer resets and 0 fixes in all three.

**Instrument gap (found in source):** `NetworkFrameDiag.cpp:300–330` classifies a row as CODE only if it touches a `CB:` shared column;
a posture that estimates no satellite code biases (PV-A's EXT-CB: "0 code biases") has none, so every code row falls to
`pf_rows_unattributed` (0 code / 235 335 phase rows) and the wire census has 0 code / 0 cb arcs — (i), (iv), D_code and the CB channel
are structurally empty on PV-A.  The phase side has the fallback ("for a run whose satellite phase biases are NOT estimated"); the code
side needs the same — assigned to card ZP's worker (trace-only).  Also: `cm_grade.py` wants `<rundir>/seed.json` (copied from the arm's
config); its refusal regex false-positives on the `RXCB-DATUM` loader line.

**Grade: no arm crosses any bar; PV-B halves all three** — (i) 2.6×, (ii) 4.3× in count and 11× in p50, (iii) 2.9× on D_phase, 9.4× on
D_code, 5.5× on the clock LEVEL — at identical availability, and the cold consumer settles at 1.19 m instead of 7.80 (0.55 m at the 2 h
mark).  **The second frozen reading applies:** the vehicle is the dominant defect on every read but not the whole of it — with the station
block determined, code residuals of 1.8 m p50, 13 stations with |ZWD| > 0.5 m (three of them, GAMB / GOPE / BOGT, with §24.6's DCB
signature) and a wire 0.95 m off CODE within arcs survive independently of the vehicle.  Readings one and three are excluded (PV-B
neither crosses nor withholds, and it is better than PV-A on every shared read — the golden's newer pins and the W6 posture are not the
problem).  **Decision:** the field golden's move to the leveling vehicle is a posture card and waits for the second defect to be named,
so the goldens re-base once; §24.12 runs the knob arms on the leveling vehicle now, with PV-B as the control.

