# R0 + 1(d): `network.phase_anchor_carry_reref`, the PB-anchor carry re-reference

**STATUS**

- **id:** R0 + 1(d)
- **topic:** network-vehicle
- **status:** closed
- **summary:** VALIDATED 2026-08-14 (n = 1): marginalizing the stale PB-anchor carry at each anchor handover closes the clock churn on the 24-sta sim fixture (Σ|dev| 160.457 → 1.529 m); R0's instruments are solve-neutral.
- **dates in the text:** 2026-08-14
- **knobs / posture:** `network.phase_anchor_carry_reref` (default OFF); posture `_simnet24_inj_null_pdp` plus this knob
- **hashes named in the text** (commit / exe / byte-cert): `7a908e23` (the pre-change exe, the §5.07 snapshot)
- **run dirs named in the text:** `runs/pareref_{off,on}` (configs `config/test_suite/_simnet24_inj_pareref_{off,on}.json`); `runs/pareref_on` is the null-calibration fixture
- **source:** `docs/archive/SPEC_pivot_visibility_gauge_defect.md` §5.08, 5.08.1, 5.08.2, 5.08.3, 5.08.4, 5.08.5, 5.08.6, 5.1

**Outcome.** R0 adds the missing instruments (unconditional PBGAUGE rows, within-epoch response counters, σ̂₀_first, the clock-impulse meter) with a bit-identical solve. 1(d) marginalizes each handed-over PB key for one epoch and erases the family's leveling carries. On 30/30 identical handovers the datum-free C contrast falls 18.6553 → 0.5035 m (the §4 clean-election bound) and T3 consensus 20.1 → 0.56 %. A review closed two holes. Not established: replication, field reach, transform over delete. [R2 sticky](../network-vehicle/PIVOT-R2-STICKY.md) later composed the disarmed continuous-handover pairing; the knob is in the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) posture and armed in both goldens and working bases ([EV7](../../refound/EV7_all_sat_clock_decouple_2026-09-21.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_pivot_visibility_gauge_defect.md -->
## 5.08 R0 + 1(d) LANDED AND VALIDATED — the carry re-reference closes the churn

**Built, golden-gated and A/B-validated 2026-08-14, same session as §5.07.** Exe pair: pre-change
`7a908e23…` (the §5.07 snapshot) vs the post-change build; arms `runs/pareref_{off,on}` from
`config/test_suite/_simnet24_inj_pareref_{off,on}.json` (the `_simnet24_inj_null_pdp` posture, one
knob differing). Unit suite 1408/1408 (1 known fixture-gated skip).

### 5.08.1 R0 — all five items, plus two collaterals

1. **PBGAUGE rows are UNCONDITIONAL** (the event sink is always passed; the ledger stays
   armed-only) — §5.07.4 defect 5 closed.
2. **Run-end counters for the three within-epoch responses**
   (`we_refloat_sat/arc/exclude_sta_total`) + a `T2 within-epoch responses` teardown line;
   previously only the withhold had a first-class counter.
3. **σ̂₀_first is traced on every assessed epoch** — 4 columns appended to
   `network_epoch_status.csv` (schema-gated on `raim_within_epoch`, the adm/prires precedent):
   `sigma0_first, n_we_refloat_sats, we_clk_move_max_m, we_clk_move_max_sat`. The null sample R4
   needs now exists on disk.
4. **`n_we_refloat_sats`** carries the tier's OWN whole-sat refloat count (`es.n_refloat_sats` is
   left as the clock-COUPLE set for schema compatibility, with the distinction documented at the
   write site).
5. **The §5.07.2 impulse-channel meter**: the primary solve's clocks are captured after the first
   assess and `(final − first)` is emitted per moved satellite (`we_clock_move` T2 rows) with
   epoch/run maxima. On the OFF arm it reads **28.487 m (G12 @ 305880)** — the tier's own measure
   of the impulse it injects, confirming §5.07.2's decomposition with no arm needed. Collaterals:
   `handover_refused_var` was written-never-read (now on the teardown handover line); the 1(d)
   counters get their own dead-wiring warn.

**Solve neutrality is PROVEN, not asserted**: same config, pre- vs post-change exe —
`network_solution.csv` and `network_dd_fractional.csv` **bit-identical** (sha256),
`network_events.csv` identical after stripping the 235 new R0 rows, `network_epoch_status.csv`
identical after stripping the 4 appended columns.

### 5.08.2 1(d) — `network.phase_anchor_carry_reref` (default OFF)

Exactly §5.07.5 R1: `networkElectPhaseAnchors` populates a per-epoch `phase_anchor_reref`
(sat,signal) set alongside `handovers_out` (parsed from the registered PB key, both cold and
continuous handovers — the continuous pin reverts at T+1 by construction, §5.07.3, so the carry is
stale either way); the D2/C1 fold marginalizes those PB keys for one epoch on BOTH branches,
exactly as `eq34_reref`; the family's phase-leveling local carries are erased at every station
(`networkApplyPhaseAnchorCarryReref`); the VCE monitor and the prior-residual census mirror the
skip (`mech = "phase_anchor_reref"` Frame label). Loader INERT-guards mirror the handover knob's
ladder; NOT in the warm-resume fingerprint (sixth forward-only exemption). Counted at run end;
armed-and-silent warns (invariant #4).

### 5.08.3 The single-knob A/B — every instrument, one direction

Handover schedule **identical 30/30** across the arms (control); gauge audit identical (1
legitimate satellite-left removal, 0 avoidable — control). The 1(d) wiring: 30/30 handovers
marginalized, 171 local carries erased.

| channel | OFF (= §5.0 baseline) | ON 1(d) | factor |
|---|---|---|---|
| CLK_STEP marks / episodes | 15 / 7 | **1 / 1** | — |
| Σ\|dev\| | 160.457 m | **1.529 m** | **105×** |
| datum-free C contrast rms | 18.6553 m | **0.5035 m** | **37×** |
| terminal `clock_std_m` | 15.283 | **0.812** | 18.8× |
| B sat-code-bias truth rms | 1.5725 m | **0.0752 m** | 21× |
| B variance factor | 5526 | **34.7** | 159× |
| rx DCB contrast rms | 4.4141 m | **1.2083 m** | 3.7× |
| T3 cell rate | 6.69 % | **3.66 %** | −45 % |
| T3 consensus rate | 20.1 % | **0.56 %** | 36× |
| within-epoch sat refloats | 8 | **0** | — |
| we clock impulse (max) | 28.487 m | **0** | — |
| σ̂₀_first (terminal) | 2.16 | 1.39 | — |
| consumer 3D final / settled median | 1.649 / 1.450 m | 0.491 / 0.541 m | (§4.1 caveat) |

The one surviving mark is **G12 @ 302430 (1.53 m) — the second epoch of the run, an ignition
transient present in both arms**, not a handover. Every §5.0.1 episode is gone. The ON arm's C
contrast (0.5035) lands AT the §4 clean-election bound (0.518, datum S06 which loses nothing) —
consistent with the §5.07 claim that the entire churn was the un-transformed carry, and achieved
WITHOUT electing a special datum. The tier had nothing left to refloat (8 → 0) and the A4 ceiling
never armed a withhold in either arm.

### 5.08.4 What this changes upstream

- **§5.05's third root is CLOSED on this fixture.** The four null interventions stand as measured
  — they were all design-matrix-side; the first carry-side intervention removes the channel.
- **QUEUE item 4's two blockers are both cleared**: σ̂₀_first now exists on every assessed epoch,
  and `runs/pareref_on` is a VERIFIED null-calibration fixture — at the nine assessed handover
  epochs σ̂₀_first reads 1.37 / 9.35 / 3.80 / 5.88 / 6.99 / 2.30 / 3.29 / 13.27 on the OFF arm
  (the carry tension, twice past the 6.0 A4 ceiling that `sigma0_adapt` laundered — §5.07.4 defect
  1 visible in its own instrument) versus **1.00–1.32 on the ON arm**, i.e. indistinguishable from
  background. The design-side re-pin still happens at those epochs; it is now statistically silent.
- **Item 0 (pin bridge) and item 2 (datum election)** now address exposure to an edge whose carry
  side is fixed — re-rank accordingly (QUEUE amended).

### 5.08.5 What is still NOT established

- **n = 1** — same fixture, arc, constellation and roster salt as everything above. No
  independent-seed replication; no ≥2 h arm; GPS-only.
- **Marginalize (delete) is now MEASURED sufficient here; transform > delete remains unmeasured**
  — the S-transform would additionally preserve one epoch of carry information per handed-over
  family (30 family-epochs here). With the channel at 1.5 m residual Σ|dev| the headroom is small;
  build the transform only if a bigger fixture shows the one-epoch refloat cost. (It is specified
  in **§8**, QUEUE item 1(b), for the CLOCK-PIVOT edge.)
- **Field reachability untested** — unchanged from §5.07.6. Note the flagship field lineages run
  eq34, whose chain election owns `eq34_reref` already; 1(d) covers the prioritized-anchor posture.
- ~~The continuous-handover + 1(d) pairing is legal but unmeasured.~~ **RESOLVED by review
  (§5.08.6): the pairing is loader-DISARMED** — under the ledger pin the frame step lands at T+1
  (the §5.07.3 revert) while the mark covers T, so the pair would marginalize the valid fold and
  miss the stale one. The reref wins the conflict (it alone closed the churn; the continuous
  handover measured null on the same channel), so `phase_datum_handover_continuous` is disarmed
  with a warn whenever both are set. Compose them only after an R2-style sticky target or a
  T+1-pending mark exists AND is validated.

### 5.08.6 Adversarial review of the diff — two confirmed holes, both closed

A 4-dimension / 2-skeptics-per-finding review of the landed diff (7 findings, 4 survived
refutation) confirmed two real edges of the NEW mechanism — both on postures the validation arms
did not exercise — and two minors. All fixed in the same landing:

1. **The continuous pairing mistimed the mark** (major, confirmed twice independently) — see the
   strike-through above. Fixed with a fourth INERT-guard; the validated arms are untouched (they
   ran cold handovers, where mark-at-T is provably correct).
2. **A bridge-exit re-pin was never marked** (major). The bridged-key skip in the election
   `continue`s before any incumbent bookkeeping, so one bridged epoch evaporates the incumbent
   record and the eventual bridge-exit re-pin — a full frame step against a still-stashed carry —
   read as a *first sighting* (standing invariant #7's named failure, verbatim). Fixed by marking
   every **incumbent-less pin**: a family with no surviving carry makes every consumer a no-op,
   while one WITH a surviving carry (bridge exit, or a long-gap return inside the prune horizon)
   is exactly the stale-frame case. First pins at run start mark harmlessly (prev_carry is empty).
3. **A withheld epoch evaporated the mark while the incumbent swap persisted** (minor). The set
   was cleared at the next election, but a withheld epoch skips the stash — its prev_carry still
   holds retired-frame values that would then fold unmarked. Fixed by moving the clear to the
   carry-stash tail: the mark now survives exactly until a solve commits the new frame (SH-gate
   epochs stash and clear correctly; A4/rank-gate epochs persist the mark).
4. **The eq34-only posture disarmed with a factually wrong diagnostic** (minor) — the guard ladder
   now tests eq34 first.

Refuted classes, for the record: the signed-counter vs absolute-column `we_clk_move` split
(documented contract, not a collision), and the guard-ladder-escape scenario (independently
blocked by the `NetworkKnobs` belt-and-braces resolution).

**Post-fix re-verification (same session):** suite 1412/1412; the OFF arm is byte-identical
through the review fixes on all four graded CSVs (the golden chain holds transitively), and the ON
arm's `network_solution.csv` is **bit-identical** to the pre-fix ON arm — every incumbent-less
mark on this fixture was the predicted no-op (126 marks = 30 handovers + 96 first sightings with
no surviving carry), so every number in §5.08.3 stands unchanged for the shipped semantics.

### 5.1 The real fix, not yet built

Both remedies above are avoidance; the defect itself is §1's one-edge event. The correct fix keeps
the network's shared phase-bias set from depending on any single station's visibility at all — the
set follows *network* coverage, the pivot supplies only the gauge pin, and a pivot that loses a
satellite hands that satellite's pin to another station rather than deleting the column. That is
close to what `phase_datum_prioritized` attempts; its measured harm here (§4) says the election
needs the anti-churn treatment (sticky anchors, hysteresis) before it is usable, not that the idea
is wrong.

---

