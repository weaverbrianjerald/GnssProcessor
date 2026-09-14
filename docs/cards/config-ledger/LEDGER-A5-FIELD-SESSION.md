# A5 — the 2026-09-09 field session — the gap re-entry response, bridge v3, and the E1 probe

**STATUS**

- **id:** A5
- **topic:** config-ledger
- **status:** closed
- **summary:** the 2026-09-09 field session — the gap re-entry response, bridge v3, and the E1 probe
- **dates in the text:** 2026-09-09 → 2026-09-10
- **knobs / posture:** `network.cycle_slip.gap_reentry_relevel`, `network.cycle_slip.relevel_delta_bridge`, `network.cycle_slip.relevel_bridge_gf`, `network.raim_arc_persist_epochs`, `network.cycle_slip.oracle_relevel`, `network.carry_iono_in_block`
- **hashes named in the text** (commit / exe / byte-cert): `be523008`
- **run dirs named in the text:** `runs/fldf3_pl2_gapfree2`, `runs/fldf3_pl2_bridge`, `runs/golden_sim_campaign_noarcrefloat`, `runs/golden_sim_campaign`, `runs/golden_sim_campaign_zwdblk`, `runs/fldf3_pl2_ctl_new`, `runs/fldf3_pl_smoke`
- **source:** `docs/archive/CONFIG_promotion_ledger.md` — A5

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: CONFIG_promotion_ledger.md -->
## A5 — the 2026-09-09 field session: the gap re-entry response, bridge v3, and the E1 probe

Four JSON rows moved or landed. All four are OFF; nothing was promoted. Full records:
`docs/SPEC_slip_qc_hardening.md` §12 (the response), `docs/SPEC_amb_iono_split_overcount.md` §6.16
(the sim discriminators and the ZWD sink census), `docs/SPEC_prior_residual_census.md` §17 (the RNEG
re-partition that made the field prires readout usable again).

| knob | verdict | evidence (measured, 12 h DOY-129 63-sta field bed unless noted) |
|---|---|---|
| `network.cycle_slip.gap_reentry_relevel` (+ `_lli`, `_unmeasurable`) | **UNVALIDATED → KEEP_OFF** (regime unchanged) | Round 1 INERT (live-state walk: 396 verdicts, "0 arc(s) reached with a live ambiguity (775 had none)", products byte-identical). Round 2 (`runs/fldf3_pl2_gapfree2`, single knob) proven live: **129 arcs freed**, SHIFTED→bridged **88 → 0**, unmeasurable-bridged 24 → 5, BSHM's 271200 step −2612 → −0.028 m, fleet ZWD span p50 0.797 → 0.660 m, DD arc-wander tail 47.0 → 38.2 %, T4a 2032 → 1092. **But the float consumer settled rms 3.178 → 4.946 m (+56 %)** and WROC00POL's ZWD span 3.6 → 15.6 m — a refloat re-levels from ONE epoch of code, and 129 re-seeds / 12 h cost more than the 88 poisons. Keep the instrument half. |
| `network.cycle_slip.relevel_delta_bridge` | **KEEP_OFF** (verdict unchanged; first FIELD number) | `runs/fldf3_pl2_bridge` (= the same control + this knob + `relevel_bridge_gf`): 282 pends created / **52 replaced** (max \|ΔN\| 52.3 cyc), 43 corroboration-rejected, 109 expired unpaired. FLAT: consumer rms 3.178 → 3.131 (−1.5 %) but p50 2.418 → 2.823 (+17 %); ZWD span p50 0.797 → **0.904**, > 10 m 3 → **4**; T4a 2032 → 1801, T3c 1105 → 896. 18 % of pends replaced is too small a population to move the wire. |
| `network.cycle_slip.relevel_bridge_gf` (+ `_sigma_floor_m`) | **KEEP_OFF** (new row) | Same arm: "GF rows captured 103 (E1+E4 float-plane applies 49, wrong-WL plane rejects 0); **EWL integers filled 0 same-epoch + 0 belated, band-3 replaces 0**". The v3 half never actuated, so that arm grades v2 only. |
| `network.raim_arc_persist_epochs` | **UNVALIDATED — PROBE, do not arm** (new row) | SIM, E1 vs E0 on the slipped campaign bed (`runs/golden_sim_campaign_noarcrefloat`, single knob, 720/720): T4 one-solve arc refloats **22 968 → 0**, ZWD truth rms **2.098 → 0.933 m**, sat-clock contrast 8.05 → 2.76 m, D3 arc-age slope gone, corr(log ZWD, event rate) 0.765 → 0.123 — at the price of consumer p50 +46 %, ref-free WL 81.5 → 65.4 %, σ̂₀(phase) 1.42 → 2.69, T3c 0.816. ⚠ **AMENDED 2026-09-10** (§6.17e, `runs/golden_sim_campaign{,_zwdblk}_oracle_e1`): co-armed with the sim-only truth-fed slip repair `network.cycle_slip.oracle_relevel`, the pair is **worse than E1 alone on every channel E1 improved** — ZWD 0.933 → **1.819 m**, consumer settled p50 2.559 → **3.781 m**, ref-free WL 65.4 → **35.4 %** (zwdblk posture 1.239 / 3.334 / 39.7) — while sat-clock contrast (3.16 m) and code bias (0.36 m) keep E1's gain. Two riders: starving T4 measurably RECOVERS carries the response needs (oracle repairs 1 161 → 1 549, `no_carry` 3 149 → 1 396 on an identical 13 485-actuation population — T4 was deleting the carry out from under a repair in ~21 % of truth slips), and with BOTH the erases starved and every detected slip repaired to truth the ambient per-band DD error **rises to 6.09 cyc** (E0 1.65), i.e. the erases were also the only mechanism periodically re-randomising the frozen (ρ, I) split.  The fix is neither this gate nor bounded identification (§7 item 22 superseded) — see §7 item 33. ⚠ `raim_attribution: false` is a byte-identical no-op under `raim_within_epoch` (loader force-re-arm) — its row now says so. ⚠ **AMENDED AGAIN 2026-09-10 afternoon — the K ladder, graded on the WIRE** (§6.18b/§6.18c, `runs/golden_sim_campaign_zwdblk{,_p10,_p40,_e1}` = K 1 / 10 / 40 / 1e6 on the ZWD-in-block posture): **the ladder INVERTS and no K tunes out of it.** Producer, monotone with K — ZWD truth rms 1.188 → 0.977 → 0.931 → **0.818 m**, sat-clock contrast **11.79 → 2.66 m (−77 %)**, clock vf −92 %, T4 events 9 871 → 0. Consumer, the other way — settled p50 1.085 → 2.296 → 2.727 (×2.5) and 2.320 at K = 1e6. The wire says why: within-tenure **WANDER rises monotonically with K** (Gal L1C 0.249 → 0.793 cyc, GPS L1C 0.507 → 0.944) while per-epoch **jitter is FLAT** (~0.07 cyc), step size is flat and **handover counts are identical in every arm** — K changes the CORRELATION of the steps, not their number or size — and the motion is a 2-parameter (range, iono) walk at R² 0.993–0.996 whose shape is invariant in K. Arc-level-removed the clock channel is flat at 5–12 cm across the ladder (the 4.4× clock gain is ALL arc level) while the arc-removed PHASE-clock **degrades** on every Galileo band; within each run the consumer's |error|-vs-arc-age correlation **flips sign** (−0.335 at K = 1 → +0.20…+0.42 at K ≥ 10). ⇒ **the T4 erases are the crude cure for the anchors' walk**, and the fix is stopping the walk (`network.carry_iono_in_block`, §7 items 35 / 37). Carry-forward rule: `sat-clock contrast` is NOT a consumer-relevant metric by itself — grade a consumer-facing change on the per-satellite PHASE-clock (C + P). ⚠ K = 1 is a Sep-9 build (only K = 10 → 1e6 is same-build); the arms carry no `ambiguity_trace`; every wire number quoted is integer-free (the wire grader's static-truth defect, fixed the same day). |

**Method notes worth carrying forward.** (1) A response acting at a gap re-entry, a re-admission or a
first sighting must key on the CARRY and actuate by KEY — the ambiguity state is pruned with the gap
and re-created only in PASS B, so a live-state walk is silently inert; pre-register the freed count
and read it BEFORE any science number (it is what caught round 1). (2) Both bridge rows and the
gap-re-entry row were graded against a control produced by a DIFFERENT exe — legal only because the
OFF path was re-certified byte-identical on that build (`products.txt` sha256 `be523008…` reproduced
by `runs/fldf3_pl2_ctl_new`, and a 20-epoch knob-OFF smoke reproducing `runs/fldf3_pl_smoke`'s
`E06007B0…`).
