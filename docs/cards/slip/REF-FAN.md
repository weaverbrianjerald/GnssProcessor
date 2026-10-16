# REF-FAN: the reference-slip fan-out under `if_phase_td_pairs = all`

**STATUS**

- **id:** REF-FAN
- **topic:** slip
- **status:** closed
- **summary:** Reference arm on additional IF-TD chains: collateral refloats 6 to 0, flood held. ARMED on both refound goldens via the RF-0 base; inert at dual frequency, live at three bands (L5).
- **dates in the text:** 2026-09-13
- **knobs / posture:** `network.cycle_slip.if_phase_td_ref_arm_all` (requires `if_phase_td_pairs = all` and `if_phase_td_ref_gate`)
- **hashes named in the text** (commit / exe / byte-cert): `87a33bf0`, `9bd2d51`, `e6b04a4`, `bea5df33`, `1c217798`, `e6071f43`
- **run dirs named in the text:** `runs/evleg_fp1_`
- **source:** `docs/archive/SPEC_event_harness.md`, sections 22.6, 22.7

**Outcome.** Question: does testing each additional IF-TD chain's own reference satellite stop a slip on it from refloating the chain's other satellites? F1, F3 (in substance) and F4 PASS; F2 passes its primary bars (E1 consumer mean |Δ3D| 1.94 to 0.72 m), missing the E5 consumer max (+53 %) and the E3/G24 clock at +45. The field pin owed against the retired Card-4 golden is moot: the RF-0 base armed the knob on both refound goldens, inert at dual frequency. Closed; the L5 counter check is in [OPEN_ITEMS](../../status/OPEN_ITEMS.md). Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 22.6 THE REFERENCE-SLIP FAN-OUT under `if_phase_td_pairs = all` — found by FP-1 on the warm-resuming field seed; the REF-FAN card pre-registered (2026-09-13 ~09:45 local, manager; FP-1's legs still running, no fix leg read)

**The finding (FP-1 worker, interim report at 7 of 12 legs; `runs/evleg_fp1_*` from `evseed_field6h_v5` ep0604,
exe `gnss_processor_rs7.exe` 23C628B3…).**  With the field seed warm-resuming cleanly (61/61 emitted on every
leg, 0 withheld — defect 1 of §20.1 is cured on v5) the §20 identity rows all reproduce (E1 PIN → `{L5X}`; E2 PIN
→ `all` via `iftd(L2W+L5X)=ref_arc`, the clause's named cost; the PIN null's `ref-arc refusals 6 [L1C_L5Q 1,
L1C_L5X 1, L1W_L5X 2, L2W_L5Q 2]` = §22.4 exactly).  But the pre-registered G-A cannot see what the additional
chains DO with a slip on their own reference: at GOPE00CZE, G24 is the reference of both L5 chains for k ∈
[−9, +41] (§20.1), so E1's −99 cyc L5X slip reads `abs_dev = 0.000000` on G24's own rows and **−31.8 m / −283.9 m
on G11, G25 and G32** — six false IF-TD refloats on innocent arcs, 0 in the OFF arm; one of them (G32/L1C)
re-realizes a satellite phase-bias datum network-wide by **13.7 m** (declared: disc 1 → 2, σ 0.039 → 0.449).  No
backstop: card R's counters are identical in all four legs, T4 is structurally 0 on `carry_invariant`, and the
collateral satellites have no scope verdict to narrow.  The mirror image of §22.1: the clause made an UNTESTED
reference refuse to exonerate; nothing makes a SLIPPED reference stop implicating its chain.

**Why it exists (the §14.10.3 flood guard).**  The reference arm (tier T1r, `IfPhaseSlipDetector.cpp` ~689–800:
the median of the non-reference between-epoch DDs is the reference's own step; on a fire the reference's IF arc
is reset, `ref_slip` traced, and — for a coherent step — the chain's `prev_grad` re-based so the pooled outlier
test sees only per-satellite residuals) runs on the FIRST chain only (`~977: run_ref_arm = first_pair_chain`),
because per-chain re-runs through the LEGACY candidate condition |median| > 0.5·λ_IF flooded on the wide
combinations ((L2W,L5Q): noise amplification 16.6, λ_IF 0.125 m — 5 351 candidates / 64 fires on a quiet null,
§14.10.3).  An additional chain therefore ADOPTS the first chain's reference when that satellite is in its set
(`~934–957`) and otherwise ELECTS its own (`selectReference`) — and in neither case is a reference slip ever
attributed: an adopted reference's fire in the first chain does not reach the additional chain (whose fan-out then
goes to the pooled test), and an own-elected reference is never tested at all.  On DOY-129 the L5 chains
own-elect at 53 % of station-epochs (the (1,2) reference is a no-L5 IIR-M in 98 % of those), G24 at GOPE among
them.  Card R's EMPIRICAL gate (`if_phase_td_ref_gate`, armed on both goldens: per-chain EWMA dispersion scale,
k = 6, coherence, warm-up abstain, thin-widen) postdates that flood measurement and scales with each chain's own
noise — the legacy threshold was the flood, not the arm.

**Build (default-OFF, byte-identical when off; the `first` posture untouched by construction).**  Knob
`network.cycle_slip.if_phase_td_ref_arm_all` (requires `if_phase_td_pairs = all` AND `if_phase_td_ref_gate` —
warn + disarm otherwise; the legacy candidate condition must never gate a wide chain again): on an additional
chain, (i) OWN-ELECTED reference ⇒ run the reference arm on that chain through the empirical gate with the chain's
own dispersion state (`cs.ref_scale_*` are per chain already); (ii) ADOPTED reference ⇒ inherit the first chain's
verdict for that satellite this epoch: a first-chain `ref_slip` fire on the adopted satellite is a `ref_slip` on
this chain too (reset THIS pair's reference arc `<sat>_<this chain's phase_if_key>`, re-base this chain's
`prev_grad` by this chain's own median when the verdict is FireRebase, skip the pooled attribution of the fan-out);
no fire ⇒ exactly today.  Counters per chain: candidates / fires (own) / inherited fires / fan-out suppressed;
run-end line; `network_if_slip.csv` rows for the additional chains' reference arm carry the chain key and
`ref_slip`.  Nothing changes on the first chain, on `pairs = first`, or when the knob is off.

**Gates, frozen (legs 60 epochs from `evseed_field6h_v5` ep0604, PIN posture = scope + pairs `all` + the clause
[the FP-1 arm], fix off vs on, one exe; events as §20 at GOPE00CZE, +10):**
* **F1 the flood (null off/on):** additional-chain reference-arm fires network-wide over 60 epochs ≤ 2× the first
  chain's own count over the same window (report both, and the candidates); products byte-identical when nothing
  fires, otherwise differing only at attributed epochs (list them).
* **F2 the fan-out (E1, E5, E3/G24 off/on):** collateral IF-TD refloats on the non-reference satellites of the L5
  chains at k ∈ [+9, +12] → 0 (from 6 on E1); the reference's own arc flagged `ref_slip` on each L5 chain at +10
  (E1 and E5: the L5X arc; E3: both bands, and the band-scope verdict for G24 stays all-band or narrows to the
  slipped bands, never to L2W); phase-bias datum re-realizations caused by collateral refloats → 0 (the 13.7 m
  G32/L1C one); consumer event − null max |Δ3D| over +0..+50 and mean over +30..+50 on ≤ OFF; the station frame
  (CLK / ZWD / the injected LOS) at +45 on ≤ off.
* **F3 the control (E3/G25 off/on, G25 not a reference):** products byte-identical, or differing only where an
  inherited first-chain fire acted (explain each).
* **F4 byte-identity:** cold smoke on the field golden (pairs `first`) knob absent vs armed `87a33bf0…`; the
  fix-off legs reproduce FP-1's PIN legs' `products.txt` byte-for-byte (same posture, new binary); suite green.
Passing F1–F4 re-opens the FP-1 pin (G-A..G-G re-run on the fixed posture, §20's bars); failing F1 names the
flood's true owner (the empirical gate on wide chains) and the pin stays an exception with this mechanism.

### 22.7 RESULTS — REF-FAN: the reference arm on the additional chains holds the flood and removes the fan-out; RECOMMEND_SCOPED, not yet pinned; FP-2 (the §20 matrix on the fixed posture) pre-registered (2026-09-13 ~19:10 local; worker on worktree-agent-a789fceb19843b4fb `9bd2d51`, merged `e6b04a4`; manager grade)

**Build.**  `network.cycle_slip.if_phase_td_ref_arm_all` (default OFF; loader disarms it without `if_phase_td_pairs = all`
AND `if_phase_td_ref_gate`, and the detector refuses the legacy candidate condition on a `|`-keyed chain — two locks
against re-opening the §14.10.3 flood): an additional chain with an OWN-elected reference runs the reference arm
through card R's empirical gate with that chain's own dispersion state; an ADOPTED reference inherits the first
chain's verdict for the epoch (this pair's reference arc reset, the pooled fan-out suppressed); per-chain counters
(opportunities / candidates / own fires / inherited fires / fan-out rows suppressed) + a run-end line;
`tests/test_iftd_ref_arm_all.cpp` (13).  Cold smoke on the field golden (pairs `first`, knob inert) `87a33bf0…` both
arms; the fix-off legs reproduce FP-1's PIN legs byte-for-byte (`null_off` `bea5df33…`, `e3g25_off` `1c217798…`); Debug
suite 2036 / 277 / 0 failed.  ⚠ Process: the ten legs were first generated across three harness versions (RS-9, RS-8
and the RS-10 conditional landed on `event_leg.py` mid-batch — feedback_harness_defaults_shared_state) and an orphaned
driver tree re-ran four legs in place (killed name-scoped); every graded leg was regenerated onto one harness
(`e6071f43…`), diff-verified single-knob per pair.  A free determinism control: `e3g25_on` run twice (once with a
second producer live) is byte-identical.

**F1 PASS — the flood is held by the empirical gate.**  Null legs, 60 epochs × 60 stations: additional-chain fires
**1** (own; `G27/SCTB00ATA` on `L2W;L5X` at k = −7, a natural reference slip the OFF arm never tested) against the
first chain's 1 — bar ≤ 2×.  The counters price §14.10.3 exactly: `GPS|L2W_L5Q` 9 candidates → 0 fires, `GPS|L2W_L5X`
85 → 1 — 93 of 94 legacy candidates on the wide combinations suppressed.  Products differ from that fire on (59
epochs, the first differing `H` record stamped at k = −7; nothing differs at −10 / −9).

**F2 PASS on every primary bar — the fan-out is gone.**  Collateral IF-TD refloats at GOPE00CZE, k = +0:

| event | OFF collateral | ON collateral | ON: the reference's own arcs reset | the innocent 13.3 m `G32/L1C` PB re-realization |
|---|---|---|---|---|
| E1 L5X −99 | 6 (31.8 m ×3, 283.9 m ×3) | **0** | 2 (`G24 L1C;L5X`, `G24 L2W;L5X`) | OFF present, **ON absent** |
| E5 L5X −31 + LLI | 6 (9.96 ×3, 88.9 ×3) | **0** | 2 | OFF present, ON absent |
| E3/G24 +29 / +29 | 6 + 1 legitimate (`G24 L1C+L2W` 14.05 m) | **0** + the same legitimate one, value identical | 2 | OFF present, ON absent |

The band-scope verdict for G24 is unchanged (E1 both arms `implicated=L5X; exonerated=L1C;L2W; … ref_arc`; E3/G24
all-band in both — the clause's named cost untouched).  Event − null within each arm: blast radius collapses 7.5× on
every event (worst other satellite 13.34 → 1.79 m); the station frame at +45 improves on E1 (clk −8.17 → −5.07, ZWD
3.48 → 1.16) and E5 (−4.48 → −1.59, 2.50 → 0.36); consumer mean |Δ3D| over +30..+50 **1.94 → 0.72 (E1), 2.20 → 1.01
(E5), 1.34 → 0.99 (E3/G24)**.  Two honest misses, reported not smoothed: consumer MAX |Δ3D| over +0..+50 E1 2.44 →
2.52 (+3 %) and **E5 2.93 → 4.47 (+53 %)**; the E3/G24 station clock at +45 −1.19 → +1.50 m.

**F3 PASS in substance.**  The E3/G25 detections are bit-identical between arms (`L1C+L2W` 14.0445, `L1C+L5X` 3.1508,
`L2W+L5X` 83.1726 m) and the station frame identical to four decimals; the products differ from k = −7 on — the same
natural `G27` fire as in every pair including the null, not the G25 event — so the pre-registered "byte-identical"
wording cannot hold for a mechanism that acts on natural reference slips network-wide; the bar is read event − null
within each arm.  **F4 PASS** (above).

**Invariant #4, both ways.**  `inherited` fires = **0 in every leg**: the adopted-reference path is built and
unit-tested but never fired on this lineage — the L5 chains own-elect (the (1,2) reference is a no-L5 IIR-M at 98 % of
those station-epochs), so the whole field result comes from the own-elected arm; the inheritance half is validated
on a lineage where an L5-capable satellite is the first chain's reference, or not at all.

**Decision.**  RECOMMEND_SCOPED (regime: `if_phase_td_pairs = all` with `if_phase_td_ref_gate`); pinned nowhere yet —
the field golden runs pairs `first` (out of regime), the sim golden pins pairs `all` + the clause — its storm arm ran in the certification (§21.22 part 3): neutral or
better on every field, the additional chains now with opportunities, so the knob is PINNED there and the exception lifted.  **FP-2 pre-registered:**
the §20 matrix on the fixed field posture — scope + pairs `all` + the clause + `ref_arm_all` — from the v8 harness
seed (every RS restore in both arms; the harness held still for the batch): {null, E1, E2, E3/G24, E3/G25, E5} ×
{OFF, PIN}, §20's G-A..G-E bars verbatim with the G25 +40 window rule, the fan-out census of §20.2 (collateral must
read 0 on every G24 leg), the E5 consumer-max and the E3/G24 clock-at-+45 misses above re-read on the second
realization; G-A..G-E passing ⇒ the G-G twin (6 h, the four knobs) and, on its bars, the field pin of all four
together.  A second event bed (another station / satellite where the injected satellite is a chain reference) is the
named guard against pinning on one site.

