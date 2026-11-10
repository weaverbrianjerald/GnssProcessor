# withhold cliff: retirement census and the same-build escalation re-grade

**STATUS**

- **id:** withhold cliff
- **topic:** raim-qc
- **status:** closed
- **summary:** G3 FAIL did NOT REPRODUCE: the cliff is an elapsed-time expiry, not therapy; a same-build 4 h pair shows zero paired cost; the consumer contract is the remaining blocker.
- **dates in the text:** 2026-08-21 to 2026-08-23
- **knobs / posture:** `raim_withhold_adapt_escalate`, `raim_withhold_free_erase`, `phase_leveling_max_gap_s`; the always-on leveling-carry retirement census (`n_cr_*` counters, no knob)
- **hashes named in the text** (commit / exe / byte-cert): `4f49fa2`, `0d25aa0`, exe `92FC13A7…`
- **run dirs named in the text:** `runs/fld63_relevel4h_`, `runs/fld63_relevel4h_ctlw`, `runs/_wd/whattrib`, `runs/_wd/whescal`, `runs/_wd/whattrib6h`, `runs/_wd/whescal6h`, `runs/fld63_wh_`
- **source:** `docs/archive/SPEC_withhold_era_release.md`: §8, §8.1, §8.2, §8.3, §8.4, §8.5, §9, §9.1, §10, §10.1, §11, §11.1, §11.2, §11.3, §11.4

**Outcome.** The question was whether the 900 s leveling-carry expiry (the cliff) was therapy the [A4 withhold](WITHHOLD-ERA-RELEASE.md) escalation removed. It is a timer: in the 4 h window the cliff does not occur (the named epoch is a 2-station dropout), and a same-build escalation arm recovers +23 epochs at a paired median σ̂₀ and a00 delta of zero. The 6 h pair limits that PASS: the cliff returns on storm eras, the level contrast widens once reset timing diverges, and instrument-silent level excursions occur in both arms. Promotion waits jointly with `network.emit_level_health` on the open [level health](../wire-contract/LEVEL-HEALTH-CHANNEL.md) card (§12.6).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_withhold_era_release.md -->
## §8 The successor card's PREMISE fails its own audit — the retirement census (2026-08-22)

§7 refused promotion on G3 and attributed the residual to a therapy the cliff
had been performing: *"The cliff, for all its brutality, was performing real
work: amputating the stressed zero-q carries.  The escalation removes the
amputation without replacing it."*  The obvious successor card is a targeted,
bounded replacement for that amputation.  Working it surfaced three independent
defects in the premise, each of which has to be closed before ANY mechanism is
justified.  This increment therefore builds the instrument, not the mechanism.

### §8.1 Every G3 number is on a RETIRED binary

`runs/fld63_relevel4h_{off,on,wh}` were produced 2026-08-21 at 00:05 / 00:08 /
09:17.  The carrier-phase wind-up **SIGN fix** landed the same evening
(`4f49fa2` 21:26 + consumer-gap closure `0d25aa0` 21:40) and is documented
solve-affecting on field data (H-RMS 3.18 → 1.30 cm).  Under standing invariant
#6, `wh`'s 2.42 versus anything measured on the post-fix build is a cross-build
comparison and inadmissible.  **The post-fix escalated arm has never been run.**

The post-fix control DOES exist — `runs/fld63_relevel4h_ctlw`
(`config/test_suite/_fld63_relevel4h_ctlw.json`, completed 2026-08-22 08:57),
knob-verbatim against the certified ON leg.  On the current build that same
config yields **453 emitted / 28 withheld, eras of 20 and 7 epochs, settled-tail
median emitted post-fit phase σ̂₀ 2.090, and ZERO leveling-fleet-expiry epochs.**
The deadlock reproduces — the fixture survives the binary change — but **the
cliff does not occur at all**, so the arm whose behaviour the therapy premise is
built on is not reproducible on the build any successor would run.

### §8.2 The old reference is DISQUALIFIED, not merely flattered

§7 already called armed-OFF's 1.274 "cliff-flattered".  It is worse than that.
Settled tail (emitted), `fld63_relevel4h_on`: **11 of 211 epochs (5.2 %) sit at
σ̂₀_pf_phase < 0.10**, and 162 epochs report σ̂₀ ≡ 1.0000 exactly; the off / wh /
ctlw legs have **0.0 %** below 0.10.  The cliff epoch itself emits at σ̂₀ 1.0000
with **σ̂₀_pf_phase = 0.0000 on 2 372 post-fit rows** — a post-fit phase residual
of identically zero is the free-ambiguity signature, i.e. the statistic went
**vacuous**, not good.  Excluding only the sub-0.10 epochs moves 1.274 → 1.346,
so the distortion is a sustained ~1 h post-cliff regime, not a few outliers.

### §8.3 σ̂₀_pf_phase is blind to the cost of a mass re-level

CLAUDE.md invariant #1's re-coupling clause already says a one-epoch code
re-level walks the level *"with every internal instrument silent"*.  On an
availability-matched paired contrast of the SH `a₀₀` channel over the settled
tail, the arm G3 called BEST carries the LARGEST level drift and the arm G3
FAILED roughly halves it.  ⚠ That contrast is itself cross-build (§8.1) and
`a₀₀` is diagnostic-class, so it is an arm-ranking hint, **not** a graded
result — it is recorded here as a reason to distrust a single-instrument verdict,
not as a verdict.  The general point stands on its own: **G3 graded a
level-affecting intervention on the one channel that cannot see level error.**

### §8.4 What the "cliff" actually is — and what landed

The obvious suspect is the post-harvest prune
`if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = local_carry.erase(it);`
(`NetworkRunContext.cpp`).  **It is not the cliff, and it cannot be.**  The
harvest refreshes `last_seen_s` for every arc it stashes and runs immediately
before the prune in the same block, so the prune's candidate set is by
construction DISJOINT from the arcs observed this epoch: it can only retire arcs
that were not harvested at all — set satellites, absent stations, harvest
failures.  It records **departures**, and it is structurally incapable of
amputating a carry that is being observed.

The actual mechanism is one seam earlier and erases nothing:

```
if (gap > pl_max_gap_s) continue;      // ApplicationNetworkAssembly, the inject
```

The carried prior is simply **not injected**.  The arc solves FREE for that
epoch and the harvest then re-stashes it from one epoch of code.  That is
exactly the observable §1.3 reported — `pl prires rows 753 → 0`, since a prires
row exists only where a prior folded — and it is why nothing in the carry map
looks amputated afterwards.  During a withhold era the epoch returns `SkipEpoch`
textually above the leveling seam, so `last_seen_s` freezes while `pl_now_s`
advances and the whole fleet crosses the threshold on ONE epoch.  At 900 s / 30 s
that is era length 30; ON's era was 29 and the 30th epoch crossed, while ctlw's
20-epoch era never reached it and exited by EMIT.

**Calling a 30-epoch timer "therapy" requires evidence that it selects on
quality.  It selects on elapsed time**, and what it does to the selected arcs is
force a one-epoch code re-level — which is the standing invariant-#1
re-coupling mechanism, i.e. the known *walk* generator, not a known cure.

Landed instead of a mechanism — the **leveling-carry retirement census**
(`n_cr_*` on `NetworkServiceCounters` plus one `SlipQcCounters` field; always on,
pure counters, no CSV row, no solve effect), reported as **four families that are
never summed**, because their units are not commensurable:

| family | what it costs | channels |
|---|---|---|
| **(B) THE CLIFF** — inject-side stale skip | nothing erased; one epoch of FREE solve, then a code re-level | `n_cr_inject_stale` (+ epochs, max, week/TOW, **distinct stations**) |
| **(A) departures** — permanent | the arc is gone | `n_cr_gap_prune` (+ epochs, max, week/TOW, fleet-before), `n_cr_sh_conv_flush` (whole-fleet one-shot at SH convergence — the largest single removal in the producer), `n_cr_zwd_gap_prune`, `n_cr_rpb_gap_prune` |
| **(C) one-solve refloats** — quality-selected | one solve, next epoch | `n_cr_raim_attrib` (T4), `n_cr_raim_consensus` (T3) |
| **(D) one-solve frame skips** | one solve of continuity; the same epoch re-stashes any still-tracked arc | `n_cr_eq34_reref`, `n_cr_phase_datum_reref`, `datum_slip_carry_erases` |
| **response** | — | `n_wh_free_erases` |

Three details that decide whether a reading is trustworthy:

- **(C) is a run-constant either/or, not a split.**  `raim_attribution` is a
  config read, so exactly one of the two channels is reachable for a whole run;
  the other's zero means "path not armed", never "path found nothing".  The
  run-end line prints which arm is live.
- **The attribution handle is week + TOW**, not continuous GPS seconds, so it
  pastes straight into a trace filter.  The cliff channel additionally records
  the **number of distinct stations** in its worst epoch — a fleet-wide era and
  a single station dropping out produce similar counts and completely different
  conclusions.
- **The fleet-before denominator is taken after the harvest**, so the prune's
  percentage is `departed / (departed + refreshed-live)` — availability, not
  carry health.  It is not a "fraction amputated" and the run-end line says so.

### §8.5 The ordered next steps (none of them a new mechanism)

1. **Attribute the event.** Re-run the certified ON config on the current exe
   with the census live and read the four families at the cliff epoch.  The
   timer reading predicts a single-epoch `n_cr_inject_stale` near the carried
   fleet, spanning MANY stations, with family (A) small — the carry is not
   erased, only bypassed.  If instead a quality channel (C) dominates, the
   therapy premise gets its first actual support.  If (A) carries the mass, the
   arcs genuinely departed and neither reading holds.
2. **Re-run the escalated arm post-fix** (`escalate: 12` + `free_erase` over
   ctlw), pinning the exe hash AND every `config_extends[].hash_fnv1a64` from
   the run manifest — §7's byte-cert pinned the exe only, and the config CHAIN
   is what demonstrably drifted between arms.  Grade it against **ctlw's
   measured 2.090**, which is the only same-build reference that exists.
3. **Only then** decide whether a targeted retirement is needed.  If it is, the
   per-entry statistic already exists and is correctly sited — the standardized
   prior-residual row `(e, fold_var, post_var) → (r, s, t)` built one statement
   before the carry is overwritten — but note it has **no knee and no small
   tail**: ~18.5 % of rows sit above 4σ̂₀ every epoch, so a 4σ̂₀ retirement
   erases ~139 entries EVERY epoch against the cliff's 753 ONCE, roughly 40× the
   amortized blast radius.  Every threshold on that ladder is a knob, not a
   physics boundary.  **§2-R's objection 2 applies to any successor that keys
   off it.**

**Disposition unchanged from §7** (`raim_withhold_adapt_escalate` +
`raim_withhold_free_erase` remain default-OFF research knobs); what changes is
that the successor card is **BLOCKED on §8.5 steps 1–2**, and the "cliff as
therapy" reading is downgraded from a finding to an untested hypothesis with a
disqualified evidence base.

## §9 §8.5 step 1 EXECUTED — the cliff does NOT reproduce, and the timer model is confirmed

`runs/_wd/whattrib` (`_fld63_wh_attrib4h.json` = the certified ON config,
knob-verbatim, product path redirected; exe `92FC13A7…`, post-wind-up-fix).

**Availability and eras (current binary):** **453 emitted / 28 withheld**, eras of
**1, 20 and 7** epochs.  The 20-epoch era runs TOW 269190 → 269760.

**The §1.3 cliff does not occur.**  20 epochs × 30 s = 600 s never reaches the
900 s `phase_leveling_max_gap_s` threshold, so the fleet never crosses it; the era
ends at 269760 by **EMIT**, exactly as §1.5 describes the healthy exit.  This is a
direct confirmation of §8.4's timer model — the crossing requires a ≥ 30-epoch
era, and 20 < 30.

**The retirement census attributes the epoch precisely.**  Its worst inject-side
stale event is at **week 2313 TOW 269760.0 — the very epoch §1.3 names** — but it
is **21 carried priors across 2 stations**, against an 853-entry carry.  By this
card's own pre-registered reading, a count concentrated on two station indices is
a station dropout, **not** a fleet-wide mass re-level.  The whole-run cliff
channel is 139 skips over 11 epochs.

**And the cliff was never a major retirement channel on this posture:**

| family | count (4 h, 61 sta) |
|---|---|
| [C] T4 attribution refloats | **6 564** |
| [D] eq34 chain-datum re-reference | **6 107** |
| [A] departures (900 s prune) | 528 (worst 24 of 853 = 2.8 %) |
| [B] **the cliff** (inject-side stale) | **139** (worst 21, 2 stations) |
| [A] SH-convergence flush | 0 |
| [D] phase-datum / datum-slip re-reference | 0 / 0 |
| [RESPONSE] A4 free-erase | 0 |

T4 attribution and the eq34 re-reference each retire ~47× what the cliff does.
Any claim that the cliff was performing load-bearing therapy has to explain how a
channel two orders of magnitude smaller than T4 — and quality-blind — was doing
the work.

**Method note:** `_fld63_relevel4h_ctlw` and `_fld63_relevel4h_on` were confirmed
to differ ONLY in output paths, a snapshot path and `trace_dd_fractional`, i.e.
they are the same solve posture.  The census arm and the attribution arm therefore
returned byte-identical counters, which is a determinism cross-check rather than a
coincidence.

**Consequence for the successor card:** §8's premise is not merely unsupported, its
motivating EVENT is absent on the current binary.  A targeted-retirement mechanism
has nothing to target here.  The remaining live question is §8.5 step 2 — whether
the escalation's quality cost is real once there is no cliff to remove.

### §9.1 The honest reference for that test

Same exe, same posture, measured: settled tail (TOW ≥ 266400, emitted only)
median `sigma0_pf_phase` = **2.0902** over 213 epochs, with **zero** vacuous
(< 0.10) epochs in that tail — 19 such epochs exist in the run but all fall in
ignition.  This is the bar §7 should have graded against; its 1.274 came from the
cliff-flattered leg (§8.2).

## §10 §8.5 step 2 EXECUTED — G3 does NOT reproduce; the promotion refusal is re-opened

`runs/_wd/whescal` (`_fld63_wh_escal_postfix.json` = `_fld63_relevel4h_wh.json`
knob-verbatim, product path redirected; same exe `92FC13A7…` as the §9 reference,
so this is a same-build pair — the comparison §7 could not make).

| | ON reference (§9) | ESCALATED | §7's pre-fix figures |
|---|---|---|---|
| emitted / withheld | 453 / 28 | **476 / 5** | 451 / 30 → 478 / 3 |
| eras | 1, 20, 7 | **five 1-epoch** | 29 → three 1-epoch |
| escalated → landed / exhausted | — | **5 → 5 / 0** | 3 → 3 / 0 |
| free-erase count | — | **2** | 0 (unexercised) |
| settled median σ̂₀_pf_phase | 2.0902 | 2.2117 | 1.274 → 2.42 |

**Gate (1): PASS, decisively.** The 20-epoch era collapses to isolated
single-epoch withholds; total withholds 28 → 5; availability +23 epochs (+5.1 %).
Every escalated epoch LANDED (5/5, 0 exhausted), so the budget-limited model is
confirmed a second time, on a different binary.  The stop rule did not fire.  The
free-erase half — recorded in §7 as unexercised because the escalation prevented
any era long enough to contain a detector slip — **fired twice here**, so that
channel is no longer unvalidated wiring.

**Gate (2): §7's quality objection DOES NOT REPRODUCE.**  The whole-population
medians (2.0902 → 2.2117, +5.8 %) compare DIFFERENT POPULATIONS — the escalated
arm emits 23 epochs the reference withheld — which is the same flattery error
§8.2 identified, running in the opposite direction.  Availability-matched, on the
**213 settled epochs both arms emitted**:

```
ON  median sigma0_pf_phase = 2.0902
ESC median sigma0_pf_phase = 2.1168
paired median delta        = +0.0000
```

**A paired median delta of exactly zero.**  On the epochs that were already being
emitted, the escalation changes nothing — consistent with §7's own G5 proof that
the verdict is byte-untouched until the first escalated epoch, and with the C5
single-trip rule.  The entire whole-population shift is the arithmetic of adding
the 23 recovered epochs, whose own median σ̂₀ is **3.6781** (max 6.2207): they are
the sick epochs, exactly as the `emitted_contaminated` class in §3 anticipated.

⇒ **§7's attribution was wrong, and its verdict rested on it.**  §7 read 2.42 vs
1.274 as the escalation degrading the settled solution because it had removed the
cliff's therapy.  On the current binary there is no cliff (§9), and the
availability-matched cost to the healthy population is zero.  The 2.42 was an
artifact of a retired binary graded against a cliff-flattered reference.

### §10.1 What this does and does NOT license

**Does:** re-open the promotion question.  The measured trade on this posture is
**+23 epochs of product (+5.1 % availability) at zero cost to the epochs already
being emitted**, with the recovered epochs themselves noisy (median σ̂₀ 3.68 vs
2.09).  That is a consumer-side decision — gap versus degraded epoch — and it is
the RT/service-posture call §7's disposition already named, now with the
"it also poisons the good epochs" objection removed.

**Does NOT:** license a blanket promotion.  Three limits, stated:

1. **σ̂₀_pf_phase is level-blind** (§8.3, and invariant #1's own re-coupling
   clause).  This pair was not graded on a level-aware channel, so "zero paired
   cost" means zero on THIS instrument, not zero.  A level contrast on the
   same-build pair is the owed measurement.
2. **One 4 h run, one posture** — the same limitation §7 had.
3. The **23 recovered epochs are genuinely worse**, and a consumer that cannot
   tell an `emitted_contaminated` epoch from a clean one inherits that silently.
   The class exists in the epoch-status record; whether the consumer reads it is
   a separate contract question.

**Disposition: UNVALIDATED→SCOPED-research stands, but the stated REASON changes.**
The knobs are not held back by a measured quality cost — that cost did not
reproduce.  They are held back pending (1) a level-aware grade and (3) the
consumer contract.  The §2-R successor mechanism remains DEAD: with no cliff and
no measured therapy, there is nothing for it to replace.
## §11 §10.1 limit (1) EXECUTED — the LEVEL-aware grade (2026-08-23)

Analysis-only, on the existing same-build pair (`runs/_wd/whattrib` /
`runs/_wd/whescal` — the arms' own CWDs; the `runs/fld63_wh_*` dirs hold only
products).  Both arms already carried `logs/network_vtec_sh.csv` (a00 = the
`n=0,m=0,trig=C` row; 81 rows/emitted epoch — row counts 453×81 and 476×81
match the dispositions exactly), so no synthesis was needed, and
`network_ionosphere.csv` was not touched (frame-broken, amendment 4).  GIM was
used DIAGNOSTICS-ONLY per amendment 6; the grade is the paired arm-vs-arm
contrast.

**Amendment-6 compliance**: 4 h run ≥ the 2 h convergence budget; the contrast
is confined to the settled tail TOW ≥ 266400 (241 ep = 2.00 h wall; 213
commonly-emitted).  Paired deltas are EXACTLY 0.0 on all 74 settled common
epochs before the first post-shared-withhold divergence at TOW 268650 — G5
byte-identity independently confirmed on the level channel — so the informative
window is the 139 post-divergence common epochs (1.38 h; a property of when the
first withhold fell, not of run length).

### §11.1 The level gate on commonly-emitted epochs: PASS

Paired a00 (esc − att, TECU): all-213 median **+0.0000** (mean −0.110,
p16/p84 −1.415/+0.971); post-divergence-139 median **−0.222**
(delta-of-medians −0.546), p16/p84 −2.430/+1.948, halves −0.72/+0.16 —
bounded breathing, no ramp.  Context: each arm's own 30-s |Δa00| median is
0.49 TECU (p95 3.3–4.2), and the −0.55 TECU shift is ~10× under the posture's
documented −4…−15 wander band.  σ̂₀ cross-check reproduces §10 exactly
(2.0902 / 2.1168 / paired +0.0000).  **§10.1 limit (1) is discharged: "zero
paired cost" now holds on a level-aware channel at ~0.5 TECU resolution.**

### §11.2 The recovered epochs SPLIT — and era-3 is the new finding

- **Era-2's 16 (TOW 269220–269760): level-CLEAN.**  d_local vs the 5 nearest
  clean emitted neighbors each side: median −0.26, max |1.37| TECU — inside
  the clean-epoch baseline (p16/p84 −0.80/+0.82).  σ̂₀_pf_phase decays
  4.44 → 1.06 across the era (median 3.6781 = §10's number).
- **Era-3's 7 (TOW 273270–273450): level-FAIL, and INSTRUMENT-SILENT.**
  Emitted a00 22.11–33.63 vs a local trend of ~43.6 — **d_local −9.96 to
  −21.48 TECU**, a dip-and-recover of −17/+14 TECU inside 4 minutes,
  physically impossible as global-mean ionosphere.  In the ESC arm these
  epochs **never tripped**: pre-fit σ0 2.0–3.0 (below the 3.0 trigger),
  n_adapt 0–1, σ̂₀_pf_phase 2.89–4.23, formal a00 std unchanged (0.6235 vs
  0.6234), no datum change — while the ATT arm at the same TOWs read σ0
  **14.5–20.2**, exhausted 5 adaptations, and withheld.  The era-2 escalation
  changed the trajectory such that the era-3 event expressed as an UNDETECTED
  EMITTED LEVEL EXCURSION instead of a detected withhold — §8.3's
  level-blindness made concrete.
- **σ̂₀ ANTI-RANKS level health**: the five worst-level epochs (−21.5…−14.1)
  carry σ̂₀ 3.10–4.19, all BELOW era-2's level-clean 269430 (σ̂₀ 6.22).  A
  consumer downweighting by σ̂₀ keeps the poisoned epochs and drops the clean
  ones.
- **A NEW classification gap**: the 7 era-3 epochs are contaminated but carry
  no `emitted_contaminated` class — that flag rides the trigger, and they
  never tripped.  The consumer contract cannot be satisfied by σ̂₀ thresholds
  OR by the class flag as built.
- Post-era: paired deltas +4.83, +3.28, +3.95, −3.56, −8.85 over the final 5
  epochs, the att arm's a00 climbing 38.7 → 54.6 in 2 min — the run ends
  150 s after era-3, MID-EVENT.  Re-convergence is unobserved.

### §11.3 Disposition

**The level grade is no longer the blocker; the consumer contract is — with a
concrete, measured failure mode.**  Promotion of `escalate=12 + free_erase`
stays blocked on §10.1 limit (3), now sharpened: the recovered class is not
merely "worse by σ̂₀ 3.68" — it can be level-toxic at −20 TECU while silent on
every armed instrument, and σ̂₀ mis-orders the two classes.  One limb is NOT
closable on this pair: era-3's post-era re-convergence (and its cost) needs a
window extending ≥1 h past TOW 273600, or a later start.  Gate (1) does not.
Scratch analysis: `wh_level_grade.py` (session scratchpad; numbers above are
the record).


### §11.4 The 6 h pair EXECUTED — era-3 answered, and two findings §11 did not predict (2026-08-23)

`_fld63_wh_attrib6h` / `_fld63_wh_escal6h` (knob-verbatim 6 h clones, same-build
pair on the §7.8-certified exe; `runs/_wd/whattrib6h` / `whescal6h`).
**Hours 0–4 reproduce the 4 h record EXACTLY across the build change** — era
TOWs identical (1/20/7), σ0 identical to six decimals (33.378601 at 268620),
a00 identical at matched settled TOWs, the same +23 recovered epochs.

**Era structure (6 h):** ATT (reference, 720 epochs TOW 259200-280770, 661 emitted / 59 sigma0_withheld): era-1 268620 (1 ep, sigma0 33.38), era-2 269190-269760 (20 ep, sigma0 27.6-30.5), era-3 273270-273450 (7 ep, sigma0 14.51-20.21, all 5 adaptations exhausted), era-4 275400 (1 ep, sigma0 7.78), era-5 277260-278130 (30 ep, sigma0 7.2 peaking 23.8). ESC (683 emitted / 37 withheld, 8 era_enter/8 era_exit per run summary): five 1-ep eras in hours 0-4 (268620, 269190, 269250, 269310, 269370 — each exits 'escalate_landed' after exactly 1 withheld epoch), then 275070 (1), 276150 (1), and era-8 278220-279090 (30 ep, n_adapt climbing 3->12, 29 escalation-exhausted epochs). HOURS 0-4 REPRODUCE THE 4 H RECORD EXACTLY despite the cross-build: identical era TOWs (1/20/7 at 268620 / 269190-269760 / 273270-273450), identical sigma0 to 6 decimals (33.378601 at 268620; 14.513363 at 273270), identical a00 at matched settled TOWs (37.2403 +/- 0.6253 at 266400 in both builds), and the same +23 recovered epochs in hours 0-4 (16 of era-2's 20 + all 7 of era-3) — zero cross-build drift detected. HOURS 4-6 NEW: the reference's +31 withholds are 275400 plus a 30-ep storm era 277260-278130; the escal arm postpones the storm era by 960 s (it emits ATT's whole era-5 window by adapting 4-5x/epoch, 28/30 of those epochs T2-flagged emitted_contaminated) but then takes its OWN 30-ep era at 278220-279090 that even 12 re-solves cannot land. ERA-3 RECURS at exactly 273270-273450 in the reference and is again fully emitted-through (never a withhold) in the escal arm. EXIT MODE OF BOTH 30-EP ERAS IS THE 900 s CLIFF, NOT RECOVERY: 30 ep x 30 s = phase_leveling_max_gap_s = 900, and both run summaries log the section-1.3 mass re-level at the exit epoch (ATT: 535 carried priors inject-skipped across 61 stations at TOW 278160; ESC: 556 across 61 stations at 279120); the first post-era epoch emits with sigma0 pinned at exactly 1.0000 and sigma0_pf_phase 0.000-0.02 (zero-redundancy fresh-relevel signature), sigma0 staying ~1.0 for ~15 min after. The ESC era_exit row's 'escalate_landed / 30 withheld' is coincident with — and actually caused by — the carry expiry.

**Era-3 re-convergence:** ERA-3 ANSWER: yes, both arms re-converge, fast, but by different paths and with a persistent low-level decorrelation afterward. ESC arm (emitted through the event): the dip bottoms at 22.11 TECU (TOW 273360, d_local -21.48) and is already back on trend INSIDE the era window — 273480 (one epoch after era end) reads 43.58 vs local trend 43.6-44.7 (d_local -1.10); one +5.6 breathing spike at 273540, formal a00 std flat at 0.588-0.594 throughout. ATT arm (withheld through the event): resumes at 273480 still LOW (38.75, own-arm d_local -6.62), climbs 38.75 -> 54.59 over 5 epochs — the 4 h record's 38.7->54.6 climb was the front half of an under-then-overshoot — peaking at d_local +7.71 at 273600, then snaps back to trend at 273630 (46.96, d_local -0.97). So ATT re-convergence: ~6 epochs (3 min) with a -6.6/+7.7 TECU under/overshoot; ESC: <=1 epoch, no overshoot beyond its +/-2 breathing. PAIRED DELTA (esc-att, common emitted): first sustained |delta|<1.5 TECU (5 consecutive ep) at TOW 274020 — 19 ep (9.5 min) after era end; but agreement does not re-lock to the hours-2-4 regime: a second +5..+7.5 divergence appears 274290-274500 with neither arm withholding. HOURS 4-6 PAIRED STATS (177 common emitted, all nonzero): median -2.54, mean -1.17, p16/p84 -7.89/+6.15, min/max -14.7/+12.6; per-30-min medians +4.76 / -2.22 / +0.16 / -7.39 — event-driven decorrelation, not a monotone drift. The -7.4 tail (n=56 epochs after both resets, TOW>=279120: median -7.39, p16/p84 -10.9/-3.6) is dominated by the ASYNCHRONOUS mass re-levels (ATT re-levels at 278160, ESC at 279120): each reset re-seeds all zero-q levels from one epoch of code and the two arms land ~7 TECU apart (ATT tail ~45-46, ESC ~38-39, pre-storm trend ~40-44; which is right is unadjudicated — no external reference used per method rules). Cost framing: the 4-6 h paired divergence is not attributable to escalation as a level 'cost' — it is trajectory decorrelation from divergent era/reset histories in an active field, and the per-arm 30 s |dA00| breathing itself doubles in hours 4-6 (ATT median 0.49->0.81, p95 3.1->4.9; ESC 0.49->0.66, p95 4.2->3.8).

**Recovered-epoch classes (6 h):** 54 recovered (esc emitted where att withheld; run totals 683 vs 661 emitted = net +22 after the 32 anti-recovered). ESC-arm d_local (5 nearest clean emitted neighbors each side, dispute epochs excluded; clean-epoch baseline n=144: median +0.005, p16/p84 -0.91/+0.64, p2.5/97.5 -2.69/+2.35, max |5.98|). CLASSES: (a) era-2's 16 (269220-269760): LEVEL-CLEAN — d_local median -0.4, max |1.37|, sigma0 decaying 4.14->1.09, sigma0_pf_phase 6.22->1.06; reproduces the 4 h record. (b) era-3's 7 (273270-273450): LEVEL-TOXIC AND STILL CONSUMER-SILENT — d_local -9.96..-21.48 TECU (numerically identical to the 4 h record), final sigma0 2.27-2.97 (below the 3.0 trigger), n_adapt 0-1, sigma0_pf_phase 2.89-4.23, formal a00 std unchanged (0.588-0.594), ZERO emitted_contaminated rows. One sharpening vs the 4 h 'never tripped' wording: the within-epoch T2 tier did act 3 times (we_refloat_sat G17 10.83->2.27 at 273270; we_refloat_arc G12 at 273330 and 273450) and those refloats ABSORBED the trigger — measure/refloat rows only, nothing a consumer sees; the A4 trigger never crossed and the class flag never set. (c) 275400 single: clean (+1.48). (d) era-5's 30 (277260-278130): 28 clean-to-marginal (d_local within +/-3.2) plus 2 toxic-by-threshold — 277380 (-7.01) and 277410 (-6.39) — but this class is NOT silent: n_adapt 4-5 on nearly every epoch, final sigma0 3.0-5.8, and 28/30 carry T2 emitted_contaminated flags (277380 flagged; 277410 is one of the 2 unflagged). ANTI-RECOVERED (32: att emitted where esc withheld) — the new symmetry finding: the REFERENCE arm silently emits its own level-toxic epochs. 276150: ATT d_local -11.42 at sigma0 2.73, n_adapt 2 — an instrument-silent toxic emission in the STOCK arm, same signature as era-3. And ATT's post-reset epochs 278490-278610 run d_local +5.7..+12.7 with sigma0 pinned at/near the exact-1.0 zero-redundancy sentinel and n_adapt 0 — the mass re-level transient is level-hot and structurally invisible to sigma0. T2 totals: ATT 52 emitted_contaminated rows, ESC 72.

**Level gate re-stated:** Re-run per section-11.1 method (paired esc-att a00 on commonly-emitted settled epochs, TOW>=266400; GIM untouched; network_ionosphere.csv untouched). HOURS 2-4 (212 common): median +0.0000, p16/p84 -1.380/+0.974 — reproduces section 11.1's PASS numbers (record: +0.0000, -1.415/+0.971) with byte-zero deltas on all pre-268650 epochs (first nonzero paired delta at TOW 268650, same divergence epoch as the 4 h record). FULL 6 h SETTLED TAIL (389 common): median +0.0000, mean -0.57, p16/p84 -4.58/+3.95, p05/p95 -9.4/+7.1, halves medians +0.0000/-2.23; post-divergence 315: median -0.78, p16/p84 -5.23/+5.10. Paired sigma0_pf delta median +0.0000 (ATT settled median 3.124, ESC 3.586). VERDICT: the hours-2-4 gate PASSES identically, but the section-11.1 'zero paired cost at ~0.5 TECU resolution' claim DOES NOT EXTEND to hours 4-6: the paired contrast widens to +/-5-8 TECU with a -7.4 TECU tail offset after the two asynchronous 900-s mass re-levels. However the widening is symmetric-cause (storm + divergent reset timing), sits inside the posture's documented -4..-15 wander band and inside each arm's own doubled breathing, shows no monotone ramp, and the reference arm cannot serve as a level truth there (it resets FIRST and its post-reset epochs are themselves +12.7 TECU hot at sigma0=1.0). The gate as an escalation-cost instrument is CONFOUNDED once era histories diverge enough to move reset timing; on the windows where the arms share an era history it still reads zero cost.

**What changes §11's reading:**
1. **Withholding through era-3 was WORSE for the level than emitting through
   it** — the reference arm resumed −6.6 low and overshot +7.7 over 6 epochs
   while the escalation arm was back on trend inside one epoch.  The
   adapt-and-EMIT doctrine gets direct level-channel support.
2. **Instrument-silent level toxicity is PRODUCER-WIDE, not an escalation
   cost**: the stock reference arm silently emitted −11.4 TECU at σ0 2.73
   (TOW 276150) and its post-reset epochs ran +5.7…+12.7 TECU hot at the
   σ0 = 1.0 zero-redundancy sentinel.  The §12 channel is owed by the
   producer, not by the escalation knob.
3. **The 900-s cliff is BACK in hours 4–6** — both arms' 30-epoch storm eras
   exit by carry expiry (mass re-level of 535/556 priors), not recovery, and
   the post-reset transient is itself level-hot and σ0-invisible.  §9's
   "cliff does not reproduce" was a property of the 4 h window, not of the
   posture.
4. Era-3's silence sharpens: the within-epoch T2 tier DID act (3 refloats
   that ABSORBED the trigger) — measure/refloat rows only, nothing a
   consumer sees.

