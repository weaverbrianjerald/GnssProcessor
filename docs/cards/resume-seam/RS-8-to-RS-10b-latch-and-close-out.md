# RS-8 … RS-10b: the canonical-signal latch, the ZWD seam reseed and the v8 certification

**STATUS**

- **id:** RS-8 … RS-10b
- **topic:** resume-seam
- **status:** closed
- **summary:** RS-8's latch restore RECOMMEND_SCOPED (chord births at the seam 64 to 1); RS-10 turns `zwd_seam_reseed` OFF without a ZWD carry; RS-10b's re-arm candidate REFUSED on the v8 seed.
- **dates in the text:** 2026-09-13 → 2026-09-14
- **knobs / posture:** `network.state_snapshot_signal_capture` (write, pinned on both goldens of the time, retired 2026-09-15) + `network.resume_signal_canon_restore` (read, harness-armed), `network.zwd_seam_reseed` (OFF on lineages without a ZWD carry; regime narrowed), `network.zwd_carry_q` (fingerprinted, so untunable on a resume leg), `network.zwd_seam_reseed_q` (RECOMMEND_SCOPED; q 2.5e-3 REFUSED on v8); D5-v5 re-test KEEP_OFF stands
- **hashes named in the text** (commit / exe / byte-cert): commits `d4a56fa`, `e2bf262`, `9cf83b3`, `1bb0039`, `c1d94e8`, `1542441`, `574d1a4`; exes `B3E6D744`, `29952251`, `AF418A9F`; byte-certs `87a33bf0`, `0f510024`, `c7f63437`; fingerprint `545b8bdd2b638231`
- **run dirs named in the text:** `runs/evseed_field6h_v8`, `runs/golden_sim_campaign_zwdblk_refarm` (vs `_refarc`)
- **source:** `docs/archive/SPEC_event_harness.md` §21.20, §21.21, §21.22

**Outcome.** The questions: the L2X step at a resume (RS-8); the ZWD step and code re-references left after RS-9 (RS-10). Restoring the latch takes the L2X keys 25.63 to 0.62 m (Q1, Q2, Q4, Q5 PASS); Q3's other-keys clause is missed as written and was rewritten as "no worse". The ZWD step is the reseed itself; the v8 closing legs read 15 code re-references (bar 5, missed). Owed: the consumer's own latch. [CSL](../../refound/CSL_canonical_signal_latch_2026-09-20.md) later replaced the first-epoch count rule on the network stations; seams remain an owed add-back under the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 21.20 RESULTS — RS-8: the canonical-signal latch flips on 23 of 61 stations at a resume, not three; restoring it takes chord births at the seam 64 → 1 and every phase-bias key's level to the seed's (2026-09-13 ~14:30 local; worker on worktree-agent-a533ac4a027ca01be `d4a56fa` + merge; manager grade)

**Build.**  `PppModel::exportCanonicalSignals / restoreCanonicalSignals`; row `CN,<station idx>,<constellation>,<band>,
<C|L>,<signal>` under `network.state_snapshot_signal_capture` (61 stations × 6 entries = 366 rows on this lineage);
`network.resume_signal_canon_restore` (requires `resume_frame_restore`; INERT-warned on a CN-less snapshot) restores
each station's map before its first epoch so the `:427` guard treats it as decided; counters (stations / entries /
disagreements with what the first epoch would have decided) + run-end line; the rider re-emits the "Canonical
signals" line on `main_log` at every decision AND restore (`decided` / `restored` / `audited`); one call in
`ApplicationNetwork.cpp` after `networkTryWarmResume`, body in the seam; `tests/test_resume_signal_canon_restore.cpp`
(13).  Cold smoke `87a33bf0…` knobs absent AND armed (the ON snapshot gains exactly the 366 CN rows); seeds `v6` /
`v6raw` = the v5 recipes + the write knob, products byte-identical to `v5` / `v5raw`, ep0604 at the same tow,
fingerprint `545b8bdd2b638231`; `rs8_raw_ctl` `products.txt` byte-identical to `rs6_raw_both`'s (the ctl reading a
CN-bearing snapshot it ignores); Debug suite 2036 / 278 / 0 failed on the merged tree.  ⚠ The worker's worktree was
created 285 files behind main and reset to main's tip before any edit (feedback_agent_worktree_stale_base).

**Q1 PASS.**  At the first IF-TD-traced epoch (277350) the canon leg's L2X tracker set is the seed's — 15 arcs on
{GAMB00PYF, INEG00MEX, OAFA00ARG}, arc for arc — against the ctl's 12 on {CPVG00CPV, INEG00MEX} (§21.16 reproduced);
emitted L2X satellites at +1 11 = the seed's (ctl 10).  The restore line's disagreement count: **34 of 366 entries
across 23 of 61 stations** — the latch does not flip on three stations, it flips on twenty-three: mostly L1 code /
phase ties (`C1C` vs `C1W`, `L1C` vs `L1W`, decided by one or two satellites), plus the three L2 flips §21.16 named
(GAMB L2X→L2W, OAFA L2X/C2X→L2W/C2W, CPVG L2W→L2X) and a `C2W`↔`C2L` at CEBR00ESP.  The cold seed's rider caught the
defect in the act: `GAMB00PYF (10 sats): … L2X(L2:10)` at 00:00 against `L2L keeps L2X vs L2W(10)` at 05:02.

**Q2 PASS.**  eq34 re-references at 277320: ctl 8 (2 L5Q + 6 L2X), **canon 2** (G09 and G27 on L5Q — the seed's own
two), zero L2X; the RS-3 restore line identical on both.

**Q3 — the level.**  Raw pair, per-key |Δ| at +1 vs `v6raw`: the six L2X keys **25.63 → 0.62 m** p50 (max 70.6 →
1.53; bar ≤ 2.0, PASS with 41× margin) — and the OTHER 115 keys **1.4957 → 0.6009 m** p50 (max 4.66 → 2.25).  The
pre-registered "within 0.05 m of the control" bar assumed the restore touched L2X only; it touches the channel
23 stations contribute, so the whole realization moves — toward the seed, 2.5× better.  The bar is rewritten as "no
worse than the control" and PASSES; the 1.50 m "raw PB at +1 ex L2X" figure that RS-6/RS-7/RS-9 carried was the
latch's own cost on the other 20 stations.  Raw PB at +1 is graded over all 129 keys from here on.

| wire pair (`rs8_null_*` vs `v6`), leg − seed | ctl | **canon** | seed's own |
|---|---|---|---|
| chord births at +1 | 64 | **1** | 2 |
| D5 flags at +1 | 38 | **29** | — |
| clock contrast p50 at +1 / +60 | 0.306 / 2.846 | **0.160 / 0.886** | — |
| W6 code bias p50 at +1 | 0.086 | **0.046** | — |
| ZWD p50 / p95 (window) | 0.659 / 5.03 | 0.446 / 4.24 | — |
| PB wire p50 at +1 | 0.0004 | 0.0004 | — |
| a₀₀ at +1; worst from +5 | +0.43; 4.49 @ +39 | +0.88; 2.80 @ +56 | — |
| σ̂₀ code ratio (G3) | 1.561 | **1.136** | — |
| consumer p50 from +5 (report-only) | 7.86 | 9.26 m | — |

**The structural headline: 64 → 1 chord births at the seam** — the ctl leg was re-birthing the ambiguities of every
station whose canonical channel had just flipped; the canon leg births 1 against the continuous run's 2.  (Even the
pooled channels §21.19 retired move: clock contrast window p50 1.65 → 1.06, at +60 2.85 → 0.89 — the latch was also
the largest seed of the two realizations' divergence.)

**Q4 PASS** on every clause (above).  **Q5 PASS:** ctl 62 `decided` per leg; canon 61 `restored` + 61 `audited` + 1
`decided` — the stray `decided` is the closed-loop CONSUMER's own `PppModel`, built outside `buildStation`, not in the
producer roster and not covered by the snapshot: the consumer re-decides its canonical channels at every warm resume
too (scope note; the consumer is not snapshotted at all and its own cold start dominates a 30-minute window, §18.14).
The probe `rs8_probe_v5` (read knob armed against the CN-less v5 snapshot): one INERT warn, the run-end
`0 restored under an ARMED read` clause, 62 `decided` lines, exit 0.

**Decision.**  Both knobs RECOMMEND_SCOPED: the write knob pinned on both goldens (write side, byte-identical), the
read knob harness-armed on every leg; the harness seed becomes `v8` (CN + RL rows) at the certification.  Two owed
items named, not built: the latch's tie-break itself (34 flips on one-satellite ties over a 5 h gap is a design
smell of `decideConstellationCanonicals` on the CONTINUOUS run too — a per-station canonical channel decided by one
epoch's count; owed as its own card with a field A/B), and the consumer's uncovered latch.

### 21.21 RESULTS — RS-10 census: the ZWD "step" is `zwd_seam_reseed` itself on a lineage that carries no ZWD, and the consumer's code re-references are all declared (2026-09-13 ~15:00 local; read-only Opus census on `rs9_null_lin` / `rs9_raw_lin` vs `v7` / `v7raw`, scripts `scratchpad/rs10_*.py`; manager grade; the single-knob confirmation leg pre-registered below)

**(a) The frame is clean, and the sink has no memory to restore.**  The Z row holds the ABSOLUTE ZWD (`NetworkRunContext
.cpp:5055`, `value_abs = x_lin + x_r`) and the B9 inject forms `η = λ(value_abs − x_lin(tropo))` with `x_lin(tropo) ≡ 0` on
both sides (the producer never writes its solution back into the station StateManager) — measured |Z row − seed
ZWD(ep0604)| = **0.0000 m on all 61 stations**; RS-9's `x_lin` family is falsified for the ZWD (lin 0.288 vs ctl 0.300 m
at +1).  What the field lineage lacks is a ZWD MEMORY at all: `carry_zwd` off, no F3 station block ⇒ the seed's
`carried` flag is 0 on all 43 889 ZWD rows — the ZWD is a per-epoch white datum sink on every continuous field epoch,
with its own epoch-to-epoch step **0.107 m p50 (rms 0.72)**.  The reseed pins that sink to one draw at q = 1e-9 m²/s
(σ 1.7e-4 m over 30 s — the measured increment implies q ≈ 1.7e-2, ~4000× in σ): the leg's `zwd` prior family runs
σ̂₀ p50 **11.2** / max|t| 23 (cb 3.2, rxcb 4.1 in the same file; the seed has no `zwd` family), folds every epoch and
is refused by the data — and from +2 the harvest re-stashes the LEG's own posterior (fold variance 2.3e-2 → 2.3e-5 m²):
the diagonal ZWD carry documented FALSIFIED on continuous runs (consumer 0.204 → 0.723 m rms), running for 480 epochs.
The receiver-clock "re-split" at +1 is the same event: across stations r(ΔZWD, ΔCLK) = **−0.897**, slope −1.71 (= −⟨mf_w⟩),
R² 0.80; the residual after the trade is 1.36 m rms — and 1.39 m in the reseed-OFF arm, flat over +1..+30 while the
totals grow (rms ΔCLK 3.08 → 6.62).  Against the estimator's own formal σ (seed σ(ZWD) p50 0.31, σ(CLK) 0.57 m):
reseed ON |ΔZWD|/σ 1.05, |ΔCLK|/σ 1.61; reseed OFF **0.68 and 0.99** — two realizations of the same estimator.  The
movers are weak-geometry stations (Spearman with the seed's own per-station step +0.77: MAC1, VOIM, PARK, KIT3, CEBR,
CAS1), not the rx-CB set (R² 0.015).  Over the hour the double count leaks to the wire: ZWD leg − seed p50 **1.338 vs
0.578 m** at +60, satellite-clock contrast p95 **10.29 vs 5.44** (the RS-6-era nozwd contrast, not single-knob; D5 trips
at +1 37 → 53, diagnostic-only under the RS-2b hold).  **Verdict: a datum-sink restart — and the reseed is the
perturbation, not the cure.**  `network.zwd_seam_reseed` is OFF on any lineage that does not carry the ZWD: its ledger
regime is narrowed from `phase_leveling` to `phase_leveling_local_block or carry_zwd` (the two mechanisms that give
the ZWD a carried flag; the sim golden has the block, the field golden neither), the posture row follows, and
`event_leg.py` arms it only on such lineages (`--extra` overrides).  Rejected: raising `zwd_carry_q` to soften the
prior — it would still run a ZWD carry for 480 epochs on the one lineage where that carry is falsified.

**(b) The seam re-references — 14 keys at +1 (24 over +1/+2), all DECLARED, all GPS code bias.**  G06 C1W/C2W/C5X, G12
C1W, G19 C1W/C2W, G24 C1W/C2W, G25 C1W/C2W/C5X, G32 C1W/C2W/C5X (ctl = the same minus G06 C2W); the phase-bias (29)
and clock (6) discontinuity totals are identical to the continuous reference — the consumer's whole seam cost is code
bias.  Declared 14/14: each leg's `code_disc` counter = the seed's + 1 at 277320 while the seed's own is unchanged;
producer-wide the leg bumps 81 of 120 keys at +1 against the seed's 7.  The consumer's test IS the declared counter,
no threshold (`RealTimeEngineSsrBreaks.cpp:169–175` → `SsrCorrectionStore.cpp:268–276` → `StateManager::noteSsr
Discontinuity`, any change in either direction), and its response is ONE re-inflation of the constellation's receiver
code-bias states — no ambiguity reset, no re-reference; 14 fires = one GPS-wide rx-CB re-inflation.  Magnitude: |leg −
seed| at +1 for those keys p50 **0.099 m** (wire arm bit-identical to the raw arm ⇒ estimator level), 14× the seed's own
per-epoch step for the same keys (0.007 m).  Mechanism: the producer's emit jump test (`NetworkProductEmitter.cpp:368–
381`, threshold 0.25 λ = 48–64 mm) tripped by a genuine realization move — 71 of the 81 bumps exceed it (0 false
negatives) — plus 10 `changed_channels` triggers from a channel-set change at the leg's first epoch (G|C1W 29 → 30
gaining G18; G|C2X G31 → G32) that the seed never had, i.e. RS-8's latch, retired with it.  Not the RS-3 datum (121
CBP + 32 CBS + 30 CBI restored, zero re-seed rows), not the receiver side (RS-9 moves it 12 mm the wrong way), not a
per-signal datum shift (signed channel means ≤ 1.2 mm against sd 0.09–0.25 m).  What remains after RS-8 is a
~5–10 cm per-key OSB realization move at the first post-resume fold, sitting at the jump test's own threshold and
answered by a benign consumer re-inflation — the seam's residual floor, read at the certification's closing splice.

**The confirmation leg, pre-registered (running):** `rs10_null_nozwd` = `rs9_null_lin`'s posture with
`zwd_seam_reseed` off (the one differing key, verified by config diff; same exe B3E6D744), graded against
`rs9_null_lin` and `v7`: the `zwd` prior family absent (n_folded 0); at +1 ZWD p50 ≤ 0.22 / p95 ≤ 1.5 m (from 0.288 /
2.32), receiver-clock rms ≤ 1.8 m (from 3.08), the ZWD-trade-removed residual 1.37 ± 0.10 m; the wire at +1 unchanged
(satellite-clock contrast p50 ≤ 0.34 / p95 ≤ 0.85, cb p50 ≤ 0.10, pb p50 ≤ 0.001); at +60 ZWD p50 ≤ 0.60 and
satellite-clock contrast p95 ≤ 6.0; D5 trips at +1 reported with re-taps 0; |ΔZWD|/σ and |ΔCLK|/σ at +1 ≈ 0.68 / 0.99.

**Confirmation leg RESULT (`rs10_null_nozwd`, single knob verified by config diff — `resume_signal_canon_restore` pinned false
in both arms because RS-8's harness default landed between the two legs; the v7 seed has no CN rows anyway): 11 of 12 bars
PASS.**  The `zwd` prior family is absent (0 rows, 0 folds; ON σ̂₀ 11.2); at +1 ZWD p50 **0.180** (ON 0.288) / p95 1.26 (2.32),
receiver-clock rms **1.65** (3.08), the trade-removed residual 1.356 (1.354); wire at +1 unchanged within the six-leg scatter —
code bias 0.092 (0.097), phase bias 0.0004 (0.0004), satellite-clock contrast p95 0.817 (0.816) and p50 0.368 vs the 0.34 bar
(FAIL as written: the bar was another leg's observed value with no margin; six legs span 0.275–0.368, sd 0.029, and the OFF − ON
gap is 0.46× the seed's own per-epoch step) ; at +60 ZWD p50 **0.584** (ON 1.338) and satellite-clock contrast p95 **5.36**
(10.29); |ΔZWD|/σ 0.65 and |ΔCLK|/σ 0.93 at +1 (the predicted 0.68 / 0.99); D5 trips at +1 51 (37), re-taps 0.  Removal is
confirmed on the producer's state channel and on the wire over the hour.  **And one unregistered channel goes the other way,
reproducibly:** the grader's report-only in-run consumer (a cold consumer started at the seam in every leg) reads 3D p50 over
k 5–32 **14.7 m OFF vs 8.8 ON** and |up| 12.1 vs 6.4 (p95 19.4 vs 7.9), crossing over by k 33–60 (7.3 vs 7.6) — on two
independent OFF arms (`rs10_null_nozwd`, `rs6_null_both_nozwd`) and two ON arms (`rs9_null_lin`, `rs9_null_ctl`), the
continuous consumer at 8.9.  That is the benefit `event_leg.py` armed the knob for ("the consumer vertical it feeds"), which
the census had no channel to see: the stiff prior double-counts the sink for the producer but hands a COLD consumer a
~15-minute smoother start.  The stateful-consumer splice (§21.14's RS-6 probe) saw no difference either way.  The two ends of
the stiffness axis each win one side — a genuine trade, not a defect — so the decision (OFF on non-carrying lineages) stands on
the producer evidence with the cold-consumer transient recorded as its named cost, and the bracket is pre-registered as one
config-only pair from the same seed: `network.zwd_carry_q` 3.8e-4 (p50-matched to the sink's own step) and 1.7e-2
(rms-matched), reseed ARMED, each a single knob vs `rs9_null_lin`; bars R1' (prior present, n_folded 3721, σ̂₀ p50 ≤ 2.0,
max|t| p50 ≤ 5 — an inert q FAILS), R2' / R4' at the OFF arm's levels (ZWD +1 ≤ 0.22 / p95 ≤ 1.5, rx-clock rms ≤ 1.8; +60
ZWD ≤ 0.60, contrast p95 ≤ 6.0), R3' wire within the six-leg scatter, C' the in-run consumer k 5–32 |up| p50 ≤ 7 m and 3D
p50 ≤ 9.5 m (the ON arm's levels) with k 33–60 not worse than either arm.  A q that holds both ends re-arms the reseed on
this lineage at that q; if neither does, the trade is decided by the stateful splice, not by the in-run consumer.

**The bracket could not run, and why is the finding (RS-10b pre-registered).**  `rs10_null_q4` was REFUSED at the resume:
`network.zwd_carry_q` is IN the warm-resume fingerprint (`NetworkRunContext.cpp:743`, under "carry q's … must COLD-start"),
while `zwd_seam_reseed` / `_epochs` are deliberately not — the seam response's only weight is borrowed from a continuous-run
carry parameter, so the one knob a resume response exists to tune is the one that cannot be tuned on a resume leg, which is
why 1e-9 was never questioned on this lineage.  RS-10b: `network.zwd_seam_reseed_q` (default −1 = use `zwd_carry_q`,
byte-identical when unset; read only inside the seam window; NOT fingerprinted, like every RS-x response knob), then the
bracket exactly as pre-registered above on the v7 seed (`rs10b_null_{on,q4,q2}`; the ON leg must reproduce `rs9_null_lin`
byte-for-byte on the new binary).

**RS-10b RESULTS — the bracket ran; the reseed's damage was its stiffness, and no single q holds both ends (2026-09-13 ~20:40
local; worker on worktree-agent-aaa93a7f642ccfdb7 `e2bf262`, merged `9cf83b3`; manager grade).**  `network.zwd_seam_reseed_q`
(default −1 = inherit `zwd_carry_q`, byte-identical; must be > 0 when set; one resolved `zwdQEff(seam_live)` read by the B9
diagonal fold, the B9-in-block fold and the harvest's prior-residual `fold_var`; NOT fingerprinted) — cold smoke `87a33bf0…`
both arms, suite 2060 / 280 / 0 failed, the ON reference reproduces `rs9_null_lin` byte-for-byte, and all four legs
warm-resume under the unchanged fingerprint.  The five arms on `v7` ep0604 (the six-leg scatter of a +1 p50 is ±0.03 m;
R2′'s bars were set at the OFF arm's own values without margin and are restated as multiples of the seed's own per-epoch
step, 0.107 m ZWD / 0.331 m receiver clock):

| leg − seed | ON 1e-9 | q 3.8e-4 | q 2.5e-3 | q 1.7e-2 | reseed OFF |
|---|---|---|---|---|---|
| `zwd` prior σ̂₀ p50; folds dropped by the var guard | 11.2; 215 | 1.32; 6 | 0.60; 1 | **0.24; 0** | absent |
| ZWD +1 p50 (× the seed's step) | 2.68× | 3.00× | 2.82× | 2.07× | 1.68× |
| rx-clock +1 rms (× step); max | 9.3×; 17.1 m | 9.0×; 16.3 | 7.9×; 12.6 | **5.9×; 5.9** | 5.0×; 5.9 |
| ZWD +1 max | 9.88 m | 9.44 | 7.31 | **3.25** | 2.48 |
| ZWD +60 p50 / contrast +60 p95 | 1.338 / 10.29 | 0.895 / **4.01** | 0.618 / 4.05 | **0.471** / 4.94 | 0.584 / 5.36 |
| wire at +1 (clock contrast p50 / cb / pb) | 0.327 / 0.097 / 0.0004 | 0.324 / 0.101 / – | 0.330 / 0.100 / – | 0.345 / 0.094 / – | 0.368 / 0.092 / 0.0004 |
| cold in-run consumer k 5–32 3D / \|up\| p50 | 8.8 / 6.4 | **6.4 / 2.3** | 7.3 / 3.1 | 12.2 / 9.8 | 14.7 / 12.1 |
| cold consumer k 33–60 3D / \|up\| | 7.6 / 3.9 | 6.3 / 1.5 | 7.1 / **0.97** | 7.0 / 1.9 | 7.3 / 3.3 |

The over-stiffness signature is monotone in q (tails, σ̂₀, dropped folds) and at 1.7e-2 the seam tails equal the OFF arm's
while the producer at +60 is BETTER than OFF (ZWD 0.471 vs 0.584) — §21.21's "the reseed is the perturbation, not the cure"
was a statement about q = 1e-9, not about the mechanism.  The cold consumer runs the other way: the best arms are the soft
ones (3.8e-4, 2.5e-3) and 1.7e-2 breaks it.  The crossover lies in (2.5e-3, 1.7e-2): q 2.5e-3 holds every consumer bar and
misses the producer end on exactly one, ZWD at +60 0.618 vs the 0.60 bar (3 %; the OFF arm itself 0.584), with σ̂₀ 0.60,
contrast p95 4.05 and the tails half-way down.  **Decision.**  No q holds both pre-registered ends, so by the
pre-registration the tie-break is the stateful-consumer splice, which is indifferent to the reseed (§21.14) — the reseed
stays OFF on lineages that carry no ZWD; `network.zwd_seam_reseed_q` is registered RECOMMEND_SCOPED (validated,
byte-identical, the only way the response can ever be tuned) and q 2.5e-3 is the re-arm candidate on record, re-tested
on the v8 seed as a second realization (the one 3 % miss sits inside the pooled p50's own scatter).  Two instrument
notes for the record: R1′d (max|t| p50) never passes at any q and is non-monotonic — it is not on the stiffness axis; and
the census counts 3720 of 3721 folds at 2.5e-3 because its own `var > 0` guard drops one station-epoch (the ON-path
run-end counter reads 3721 on every armed leg).

**RS-10b v8 RE-TEST — the re-arm candidate q = 2.5e-3 is REFUSED on the second realization (2026-09-14 ~01:30 local; manager-run
leg `evleg_rs10b_v8_q3` = `cert_null_v8` + `zwd_seam_reseed: true, zwd_seam_reseed_q: 0.0025`, single-knob pair by config
diff, both from `evseed_field6h_v8` ep0604; `scripts/ev_seam_grade.py` against the v8 seed; consumer channel REPORTED only,
§24.1).**  On the v8 seed — RS-8 + RS-9 in the snapshot — the reseed-OFF seam is far cleaner than the v7 OFF arm the bracket
was bracketed against (ZWD leg − seed +1 p50 0.086 vs 0.180; +60 0.092 vs 0.584), and the reseed at the crossover q is now
worse than OFF on every ZWD number and neutral on the wire:

| leg − seed | q 2.5e-3 (v8) | reseed OFF (v8, `cert_null_v8`) |
|---|---|---|
| ZWD +1 p50 / p95 | 0.169 / **2.17** | 0.086 / 0.42 |
| ZWD +2 p50 / p95 | 0.164 / 1.08 | 0.105 / 0.36 |
| ZWD +60 p50 / p95 | **0.296 / 4.70** | 0.092 / 0.60 |
| ZWD pooled +5..+60 p50 / p95 | 0.210 / 2.63 | 0.087 / 0.42 |
| sat-clock contrast +1 p50; +60 p50 / p95 | 0.140; 0.268 / 0.945 | 0.152; 0.255 / 0.929 |
| W6 code bias +1 / +60 p50 | 0.052 / 0.461 | 0.052 / 0.415 |
| phase bias +1 / +60 p50 | 0.0004 / 0.097 | 0.0004 / 0.109 |
| D5 flags at +1 (re-taps 0) | 37 | 45 |
| `zwd` prior family | 3720 folds, Σw·ê² 85.8 % of the prior side | absent (0 rows) |
| a₀₀ +1 / +60 (TECU) | +0.95 / +0.12 | +1.08 / −0.07 |
| in-run cold consumer p50 from +5 (reported) | 4.28 m | 8.94 m |

The reseed's prior is a perturbation of at least the size of the seam it means to smooth: at +60 it leaves the ZWD 3× further
from the continuous run with a 4.7 m p95 tail, while the wire channels are inside the six-leg scatter either way.  The
cold-consumer number runs the other way again (4.3 vs 8.9 m) and, per §24, cannot be read.  **Decision.**  No re-arm: the
reseed stays OFF on lineages that carry no ZWD memory; `network.zwd_seam_reseed_q` stays RECOMMEND_SCOPED (validated,
byte-identical, the only way the response can be tuned) with this re-test on its row; the crossover of §21.21 was a property
of the v7 seed's seam, not of the mechanism.

### 21.22 CERTIFICATION — the night's merges on main's own binaries; the v8 harness seeds; the closing legs graded at the seam epoch (2026-09-13 ~21:30 local; certification worker on main, parts 1–2 at `1bb0039`; part 3 re-certifies the final HEAD; manager grade)

**Part 1 (main `1bb0039`; `c1d94e8` binary-identical).**  Debug and Release rebuilt first attempt (both new test TUs compiled in
without a `CMakeLists.txt` touch); Debug suite **2049 cases / 279 suites / 2048 passed / 1 documented skip / 0 failed**
(the six resume suites 48/48); Release exe `29952251…`; smokes `_cert1_smoke_base` and `_cert1_smoke_nowrite` (every write
knob false) BOTH `87a33bf0…` — fourteen write-row families go exactly to zero under the write knobs and every other tag is
count-identical (the census delta vs `_rs6_smoke_off` is CN 366, RC 61, SD 376, RL 61 only); no unknown-knob warning;
the canonical-signal rider on `main_log` 61 + the consumer's 1.

**Part 2 — the v8 harness seeds.**  `runs/evseed_field6h_v8` / `v8raw` = the v5 / v5raw recipes under today's harness
(config diff = the two new write knobs + paths): `products.txt` **byte-identical to v5 / v5raw** (`0f510024…` / `c7f63437…`),
ep0604 at tow 277290.00000810623, fingerprint `545b8bdd2b638231`, the ep0604 census = v5's in every tag plus **RL 61 and
CN 366** (D 256 · ZL 4 · EQD 6 · EQP 176 · EQS 126 · EQT 302 · CBP 121 · CBS 32 · CBI 30 · ER 147 · PF 147 · PS 1 · RC 61 ·
SD 467 · QK/QP 150 · QS 383 · QZ 61); v8raw the same minus the emit layer.  **v8 is the harness seed from here.**

**The closing legs (`cert_null_v8` / `cert_raw_v8`, 60 epochs, the harness defaults and nothing else: every RS restore
armed, `resume_coupling_restore` and `zwd_seam_reseed` set false by the harness on this lineage, `resume_border_restore`
absent).**  Both 61/61, no cold start, riders 61 `restored` + 61 `audited` + 1 `decided` (the consumer's).  Restore counts:
RS-2 4 blocks / 699 chord rows re-based / 0 un-basable; RS-2b 45 trips held, retired at 277320; RS-3 6 / 176 / 126 / 302
and CB 121 / 32 / 30, **eq34 re-references at +1 = 2**, re-seed discs 0; RS-5 147 / 147 / 1 (raw arm 0 / 0 / 0 with the
"0 under an ARMED read" clause — its seed writes no emit layer); RS-6 61 + 467; **RS-8 366 restored, 34 would have been
decided differently** (§21.20 to the count); **RS-9 61 restored / 61 seeded / 0 carried-but-unseeded**; RS-4 null arm 126
tested / 0 over threshold / 0 marked, raw arm 113 of 126 over (89.7 %) / 1 marked — the mark reads the raw level exactly
where it is meant to.

| seam epoch, leg − seed | +1 | +2 | reference (the card that set it) |
|---|---|---|---|
| PB wire p50 | 0.0004 | 0.0008 | 0.000 (RS-5) |
| clock contrast p50 | 0.152 | 0.190 | 0.160 (RS-8 canon) |
| W6 code bias p50 | 0.052 | 0.084 | 0.046 (RS-8 canon) |
| ZWD p50 | **0.086** | 0.105 | 0.180 (RS-10 nozwd) — halved again by RS-8 |
| Δa₀₀ (TECU) | +1.08 | +1.49 | within 2 |
| chord births leg / seed | **1 / 2** | 5 / 2 | 1 (RS-8 canon; the control read 64) |
| D5 flags (held, 0 re-taps) | 45 | 1 | posture (RS-9 in, reseed out) |

The raw arm's solve channels are bit-for-bit the null arm's (only the emit layer differs).  rx-CB withheld share over the
leg **11.39 %** (417/3660) vs the seed's 11.01 % — on the seed's own number (RS-9's lin leg read 9.78); the withholding set 7
on both, ±1 (OAFA00ARG in, LCK400IND out).  L2X tracker set at the first IF-TD epoch **= the seed's, arc for arc**
({GAMB00PYF 5, OAFA00ARG 6, INEG00MEX 4}).  **Raw PB at +1 over ALL keys** (126 present at the epoch): p50 **0.736 m**, p95
1.79, max 2.22 — the 11 L2X keys read 0.30 (no longer the outlier family; the "ex L2X" grading is retired), the other 115
0.78 against RS-8's 0.60 on the canon leg (+30 %; posture delta — RS-9 in, the reseed out — worth a second look, not a
regression).  **Stateful splice** (`_cert_rp_splice_v8`, 665 epochs; the instrument first reproduced RS-9's 24 on
`_rs9_rp_splice_lin`): at the seam epoch **PB resets 0, code re-references 15, clock 0** — the code channel down from
22–24 (−37 %) with RS-8's 10 channel-set bumps retired; the window's PB count (29) equals the continuous wire's.

**The D5-v5 re-test (§23.2's named pair, `d5v5_v8_{off,on}`, 120 epochs, one differing key).**  V2 every clause PASS
(born flags 74 → 0, born-skipped 1824, carried 16 → 17, births 1873 → 1805, **re-taps 90 → 17**, carry-side lifetime 49.1 →
51.0 ep); wire: clock contrast 0.0020 and ZWD 0.0018 PASS, PB wire |ON − OFF| p50 **0.0137 m vs the 0.01 bar** (FAIL, 1.37×;
p95 0.18, max 6.3); withheld 0 = 0; a₀₀ 0.002 TECU.  **The consumer bar is indeterminate:** de-biased by the window MEDIAN
ON/OFF = −3.9 % (PASS), by the window MEAN +3.0 to +4.1 % — every reading inside ±5.4 % and the SIGN flips with an
analyst choice, while the direct arm-to-arm 3D separation (mean 1.22 m) is an order of magnitude larger than the
dispersion difference.  Not resolved by same-realization pairing either: the knob reshapes the chord population and with
it the realization, and this float consumer cannot rank two realizations.  **KEEP_OFF stands** (§23.2); the mechanism's
win is real (re-taps −80 %) and a promotion needs a consumer-side instrument that is not a single realization's
dispersion — a truth-referenced multi-realization grade, owed.

**Part 3 — the final HEAD (`1542441` for binaries and configs; `574d1a4` docs-only past it).**  Debug and Release rebuilt
first attempt; Debug suite **2073 cases / 281 suites / 2072 passed / 1 documented skip / 0 failed** (`IfTdRefArmAll` 13/13,
`ZwdSeamReseedQ` 11/11); Release exe `AF418A9F…`; the smoke pair from the REGENERATED field golden (now pinning
`raim_carry_invariant`) `_cert3_smoke_base` and `_cert3_smoke_nowrite` BOTH `87a33bf0…` with the re-host live (20 seam
passes, 18 163 cells, σ̂₀ 1.000, zero opportunity) — the posture summary moved SCOPED 39/46 → 40/47 and the loader's old
"post-fit RAIM is INERT" warning is gone; no unknown-knob warnings.  **The REF-FAN sim storm arm** (`runs/golden_sim_campaign_
zwdblk_refarm` vs `_refarc`, the §22.5 protocol; config tracked as `config/test_suite/_golden_sim_campaign_zwdblk_refarm.json`)
is neutral or better on every printed field — ZWD 0.0059 both, consumer p50 0.0807 → 0.0806, T4 events 11 = 11, refloats
8315 → 8313, the whole band-scope census byte-identical — and the additional chains now report opportunities (2 each on
the Galileo (1,7) and (5,7) chains, 4 inherited fires against the first chains' 2, the F1 bar exactly); the knob is pinned
on `golden_sim_tri_w6` and its exception lifted (§22.7).  ⚠ `simnet_storm_grade.py` needs the elapsed-seconds line in
`exitcode.txt` that `ev_launch.ps1` writes and a plain `.cmd` launcher does not.

