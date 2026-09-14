# F3 24 h — the 24 h F3 arm — the settled number is NOT met, and the reason is an EDGE, not the mechanism

**STATUS**

- **id:** F3 24 h
- **topic:** ionosphere
- **status:** closed
- **summary:** the 24 h F3 arm — the settled number is NOT met, and the reason is an EDGE, not the mechanism
- **dates in the text:** 2026-09-05
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/nonint_losblock24h_hv`, `runs/nonint_losblock24h`, `runs/nonint_los24h_hv`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` — §6.9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.9 The 24 h F3 arm (2026-09-05, `runs/nonint_losblock24h_hv`: F3 + handover ceiling 1e-2, 2879/2879 epochs) — the pre-registered settled number is NOT met, and the reason is an EDGE, not the mechanism

Pre-registered (§6.8): settled L1C med|e| 0.14–0.15 cyc / round0 84–86 %.  Measured (`scripts/simnet_amb_integer.py`, 7,851,033 rows, 0 dropped):

| window | L1C med\|e\| | round0 | WL(1,2) | EWL(2,5) | Gal WL(1,5) / EWL(5,7) |
|---|---|---|---|---|---|
| full 24 h | 0.283 | 64.2 % | 97.7 % | 100.0 % | 97.0 % / 100.0 % |
| **last 25 % (h18–24)** | **0.425** | **53.2 %** | 97.9 % | 100.0 % | 97.2 % / 100.0 % |
| quiet hours h2–h5, h12–h16 (hourly, stride 4) | 0.18–0.23 | 74–83 % | 98 % | 100 % | — |
| mature chords (all 4 legs ≥ 30 ep), h13–h16 | **0.160–0.168** | **86–89 %** | 100 % | — | — |

Mechanism signatures hold all day: first-inject σ ratio 0.720/0.722/0.728/0.722 (all bands; correct recursion 0.707), settled |e|/σ 1.2–1.3, |e| vs chord age slope −0.46 (runs ≥ 200), F3 run-end `172320 block(s) whitened, 7,793,292 row(s) emitted, 69,410 column(s) marginalized, 0 fallbacks`, T4 attribution 0 until the run-end day-edge burst (49 at h23; base 63/122/103/48/… + 68), refsat_reset 2482 (identical to base), handovers 948 continuous / 87 cold (base 1030 / 5).  The LANES are solved on every hour of the day (WL 96.6–99.1 %, EWL 99.5–100 %).

**What breaks the settled quarter — two ~4 h DEGRADATION EPISODES, both trailing an eclipse re-admission cluster by 1–2 h** (hourly L1C med|e|: h0 1.26, h1 0.26, h2–5 0.19–0.27, **h6–8 0.42/0.49/0.38**, h9–16 0.32→0.18, h17 0.27, **h18–21 0.46/0.58/0.56/0.42**, h22–23 0.38/0.31).  Conditioned grades (`scratchpad/arms/{mature,coldho,readmit,station_episode}_grade.py` on the run dir):

| split (L1C, stride 4) | quiet h15 | episode h19 | verdict |
|---|---|---|---|
| all four legs ≥ 30 epochs old (tracer re-keying to young low-PRN refs removed) | 0.160 / 89 % | **0.515 / 50 %** | young legs are NOT the driver: mature chords degrade too |
| no `handover_cold` on sat_i/ref within 120 ep (97–99 % of rows) | 0.182 / 83 % | 0.585 / 47 % | cold handovers (2–3 % of rows, 87/day) are NOT the driver |
| no satellite RE-ADMITTED from eclipse exclusion within 240 ep on sat_i/ref ("clean") | 0.175 / 86 % | **0.680 / 45 %** (h19 clean n=5122; touched 0.561 / 48 %, 82 % of rows) | the degradation is NETWORK-WIDE — chords that do not touch a returned satellite degrade as much as those that do |

28 re-admission events over 54 satellites (gap ≥ 10 epochs of network-wide absence); the two clusters are h3.3–6.9 (E02, G01, G26, G16, E30, G25 …) and h16.6–18.9 (E25, G01, G26, E02, G16).  The episodes start ~1–2 h into each cluster and decay over ~4 h.  The wire's clock spread is unchanged through them (epoch-status `clock_std_m` 2.0–2.1 m on both arms every hour; `worst_dev_m` 7.1–7.5 vs 6.9–7.0; code σ̂0 1.1 vs 1.7), so this is not a clock re-realization the DD sees.

**Consumer (seam_tools, hourly 3D rms, F3+hv vs the 24 h control):** h0–2 1.60/0.58/0.24 vs 2.76/1.36/0.42 (F3 3–5× faster ignition); **h3–h7 1.81/1.10/1.19/1.40/1.11 vs 1.23/0.98/0.48/0.12/0.12** — the base recovers from the h3–4 pivot-transit cluster in 1 h, F3 takes until h9 (0.31) / h10 (0.16); h12–23 0.06–0.19 vs 0.05–0.15 (comparable).  The second episode (h18–21) costs the consumer nothing (0.06–0.10) — the DD instrument sees a network-wide level error the single-station consumer partly absorbs.

**Reading (HYPOTHESIS, discriminating arms queued):** under the diagonal over-counting carry every level was frozen, so a satellite returning from exclusion with a cold re-realization (the E2 gap of SPEC_synthetic §18.4 — the re-coupling edge marks but never re-validates) moved nothing the DD could see; under an honest carry the returned satellite's shared states enter every station's coupling regressors (`M_j·ŝ_prev`) with a STALE `ŝ_prev`, and the F3 whitening (rows = L⁻¹·[I|M|y], dense over the block) mixes that one bad shared term into every ambiguity of the station — which is exactly the network-wide shape measured.  Under F1 (diagonal rows) the same stale term would hurt only the columns coupled to it.  Discriminators: (a) `runs/nonint_losblock24h` (F3 at the 1e-4 ceiling, every handover cold) — running; (b) `runs/nonint_los24h_hv` (F1 24 h, same ceiling) — queued behind it: if F1 shows no network-wide episodes, the fix is the invariant-#7 re-coupling edge on the BLOCK: at a satellite's re-admission (col_added after a gap ≥ N epochs / the CTA "first sighting"), marginalize its shared terms out of every block's regressors (or feed the re-admission into `carry_coupling_reinit_sources`, Card 1g) for K epochs and count it.  Until then: **F3 stays default-OFF; on a quiet window it is at the CRLB (mature chords 0.16 cyc / 89 %), and the 24 h settled-quarter prediction is FALSIFIED as written because the pre-registration assumed no edge response was missing.**

**Arm 2 — F3 at the lineage's 1e-4 ceiling (`runs/nonint_losblock24h`, every PB-gauge handover cold: 20 continuous / 1015 cold):** full 24 h L1C 0.322 / 62.2 %, last quarter 0.449 / 52.5 %; first-inject 0.720/0.722/0.728/0.722, |e|/σ 1.2, T4 37 (day-edge), 0 fallbacks; the re-admission-conditioned hourly grade reproduces arm 1 to the digit (clean chords h6 0.571 / h19 0.658 vs 0.538 / 0.680).  So the ceiling is NOT the lever for the DD episodes.  It IS the lever for the consumer: h5–h7 1.86/2.57/1.64 m (arm 1 1.19/1.40/1.11; base 0.48/0.12/0.12) and h18–21 0.18–0.24 m (arm 1 0.06–0.10) — every cold handover erases the family's carries and re-pins at 0, and the consumer pays for each.  The two findings separate cleanly: the handover ceiling governs the WIRE's continuity (fix: re-express it on the honest-σ scale, ledger row); the ~4 h network-wide DD episodes after each re-admission cluster are a different edge (F1 control pending).

**F1 24 h control (`runs/nonint_los24h_hv`, F1 + ceiling 1e-2, 2879/2879, 744 continuous / 291 cold handovers, T4 0):** full 24 h L1C 0.853 / 33.1 %, last quarter 0.739 / 36.3 %, WL 90–92 %, EWL 100 %; hourly L1C 0.56–1.0 cyc all day (F1's own station-common ZWD-geometry residue, §6.7), first-inject 0.588/0.650/0.660/0.665, |e|/σ 0.9–1.1.  Its re-admission-episode response is a +0.15–0.30 cyc bump on a 0.7 cyc floor (clean chords h13 0.67 → h19 0.90, h8 1.02) against F3's +0.35–0.45 cyc on a 0.19 cyc floor (h15 0.18 → h19 0.68).  READING: the post-re-admission edge response exists for ANY honest carry (it is the E2 re-coupling gap the STABILITY campaign named, invisible under the frozen over-counted carry), and the F3 block roughly doubles its absolute size, consistent with the dense L⁻¹ mixing of a stale shared-border term into every ambiguity of the station — but F1 cannot separate "smaller edge" from "hidden under its own floor".  Consequences: (1) F3 delivers the CRLB on quiet windows (mature chords 0.16 cyc / 89 %, lanes 100 %) and is the vehicle to keep; (2) the next card is the block's invariant-#7 RE-COUPLING EDGE at satellite re-admission (col_added after a gap / CTA first sighting): marginalize the returned satellite's shared terms out of every block's regressors for K epochs (or feed the re-admission into `carry_coupling_reinit_sources`, Card 1g), counted, pre-registered on this bed as "no clean-chord degradation after the h16.6–18.9 cluster"; (3) the handover ceiling is a separate wire-continuity card (arm 2).  Until (2) lands F3 stays default-OFF for settled work, and the 24 h number to quote is the full-window 0.283 cyc / 64 % (quiet hours 0.18–0.23 / 74–83 %), not the settled quarter.

Pre-registration audit: §6.8's "24 h F3+hv 0.14–0.15 / 84–86 %" was derived from the 3 h settled window, which contains no eclipse re-admission cluster; the 24 h bed has two.  The quiet-hour mature-chord reading (0.160 / 89 %) is the number the prediction actually described.

