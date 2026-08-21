# SPEC — LV-6: the internal per-epoch level authority (`network.level_authority_csgf`)

**Session 2026-08-21.  Status: DERIVED (this doc) — census stage building this
session; the FOLD stage is deliberately deferred behind the census's tracking
verdict.  Owner of QUEUE item 5's "remaining" clause (the per-epoch
data-derived authority, INTERNAL only); the §30 contract itself
(`SPEC_level_information_budget.md`) is unchanged and this card conforms to
it.  Continues the LV ladder (LV-1..5 + S2H); adversary-reviewed before
build (§5).**

## §1 The derivation, from the record's own measurements

Constraints, each carrying its measurement:

- **C1 (the incumbent is an accident):** the level direction's designed
  authorities carry 0.25 (a00 anchor) / 0.01 (rx zero-mean datum)
  info/epoch; the rx-CB carry carries **~100** — "memory, not a datum" —
  and every tuning of that memory is falsified (freeze +20 TECU, loosen
  +25, remove −9/epoch collapse; the whole q axis closed, §18.1).
- **C2 (the information exists and tracks):** LV-4-alone tracked
  GIM within **±0.7 TECU from hour 1** (sd 1.81) — the admitted per-epoch
  code-GF information IS a level-tracking authority.  What was falsified
  is the fold-side VEHICLE (+32 % settled phase rms through the carry
  screen), never the information source (§24/§25.6).
- **C3 (8(b) transfer):** an authority must be **level-independent and
  code-anchored** (the ι̃ = ι + d_r,GF − d^s_GF realization — a solve-fed
  source stashes the level-locked consensus) and must act **per-epoch on
  the equilibrium**, not at event edges (three interventions exonerated
  the event channel).  The CS-GF kernel survives 8(b) precisely as
  "level-independent instrumentation".
- **C4 (the confound is a convention, not a defect):** a fleet aggregate
  of ι̃ reads true level **plus the fleet-mean receiver GF bias** (defect-2
  direction: ≈ −5 ns ≈ −15 slant TECU, std 21, fleet-clustered,
  quasi-static — measured drift −14.8 → −16.2 TECU over 10 h).  A LEVEL
  authority is a REALIZATION choice (§30: "the level is never 'estimated'
  … it is REALIZED"); a constant convention offset is what every
  realization carries (W6's s2z lands at 23–28 vs GIM 32.24 and is the
  DESIGNED owner there).  The failure mode to exclude is WALK/BREATHING,
  which is the memory-vehicle's crime, not the offset's.
- **C5 (internal only):** GIM stays diagnostics-only (standing directive).
  The CS-GF store is producer-internal by construction.
- **C6 (wire edge):** an authority that suddenly disagrees with the solved
  consensus steps the wire (the i3 consumer lesson).  A fold must be
  per-epoch from ignition at honest weight — never engaged mid-run — and
  the census stage has no wire surface at all.

**The candidate that satisfies all six**: a per-epoch, two-tier robust fleet
reduction of `ctx.carry.iono_cs` — per-station information-weighted mean of
vertical-mapped mature LOS states (tier 1: equalizes station weighting; the
per-station means are simultaneously a free d_r,GF-class diagnostic), then
the fleet MEDIAN across stations (tier 2: kills the fleet-clustered bias
tail and roster imbalance).  Call it **L_cs(t)**, in vertical TECU, with a
dispersion, LOS/station counts, and a trim census.

## §2 Staging — census first, fold later (the house pattern)

**Stage 1 (this card, this session): the CENSUS.**  Compute and trace
L_cs(t) every epoch; fold NOTHING.  Byte-identical OFF; solve-byte-identical
ON (measure-only).  This is the instrument that prices the fold before it
exists — the R2/withhold-card pattern, third use tonight.

**Stage 2 (a future card, gated on §4):** the FOLD — one row per epoch on
the level direction: under SH, an a00-direction prior at the census's
measured dispersion (replacing/augmenting the 15 ± 30 climatology); on the
EXT/GF postures, the S2H convention's per-epoch target (re-opening §29.6's
"non-zero-sum target" rejection is legitimate ONLY with an internal
per-epoch value — that rejection was written against a static external
number).  Not built here; do not build it without the census verdict and
its own adversary pass.

## §3 Census design

Site: the serial pre-PASS-B prepass slot (the LV-5 precedent), immediately
after PASS A — the CS-GF map was advanced this epoch and elevations are
still in hand.  Requires `network.carry_iono`-independent access: the CS-GF
advance today runs only under `carry_iono` + `iono_carry_source=smoothed_gf`
(`qc.params.iono_cs`).  The census therefore arms the ADVANCE separately:
`level_authority_csgf` sets `qc.params.iono_cs` true without arming the
carry (the kernel is instrumentation, 8(b)-sanctioned; the carry knob's
falsified inject path stays untouched and OFF).

Per epoch: for each (station i, sat) with `n ≥ level_authority_min_n`
(default 10 — the kernel's √K maturity), vertical-map
`v = val / (M(el)·m_per_tecu)` with the same thin-shell mapping the SH tie
rows use; per-station weighted mean over its LOS (w = 1/var); fleet median
over stations with ≥ `level_authority_min_los` (default 4) qualifying LOS.
Outputs: `L_cs` (TECU), dispersion (MAD across station means), n_sta,
n_los, n_immature.  Trace `network_level_authority.csv` (+ the per-station
means at a `--verbose`-class sub-knob? NO — one stream, station rows gated
by the same knob at one row per station per epoch: the d_r diagnostic is
half the card's value).  Counters: `n_lvlauth_epochs`, run-end
mean/min/max, `n_lvlauth_immature_skips`.  Teardown line (invariant #4).
Under SH, the trace also samples a00 (already in `ctx`/health channel) so
the in-run (L_cs − a00) series is on disk without a join.

Edges (invariant #7): the census inherits the CS-GF kernel's own reset
semantics (slip-funnel resets, gap re-sighting) — no new edges; the
maturity screen bounds what a fresh/reset LOS can contribute (n ≥ 10).
Withhold interaction: PASS A runs on withheld epochs, so the census KEEPS
measuring through withhold eras (deliberate — cite the withhold-era card:
instruments upstream of emission survive the era; note it in the trace
doc).

## §4 Pre-registered census gates (BEFORE any run)

- **G1 (sim, truth-graded — the only legal level venue):** on a ≥ 2 h
  SH-posture arm over a REGENERATED wsh-class fixture carrying the §29.5
  empirical receiver-bias distribution (`truth_rx_bias.csv` injection),
  corr(L_cs(t), censusOperator{truth}(t)) ≥ **+0.8** settled, offset ≈ the
  median of the injected biases (the C4 model, truth-graded), and
  |d/dt offset| ≤ **2 TECU/h** settled.
- **G2 (field, diagnostic-only):** on an ext63 arm, settled
  **L_cs(t) − censusOperator{GIM}(t)** (the sampling-matched reference —
  per-station GIM vertical at the same IPPs through the same two-tier
  reduction) is **stable within ±2 TECU** after the ≥ 2 h budget, AND the
  offset lands in the median-statistic raw-realization window
  **−10..−20 vertical TECU** (§5.1; EXT-CB arm sat-corrected by the run's
  own `.BIA`).  Landing outside −25..+5 falsifies the C4 confound model —
  STOP, record, no fold card.
- **G3 (byte-cert):** OFF path byte-identical (products + five CSVs on the
  fld63 recert pair); ON path solve-byte-identical to its control twin
  (the R2 pattern: armed vs control products + solve CSVs identical).
- **G4 (counters, invariant #4):** census epochs ≈ run epochs minus the
  min_n warmup (and after any warm resume — the store cold-starts by
  design); immature skips reported; an armed run with
  n_lvlauth_epochs = 0 past warmup is a miswire.
- **Stop rule:** G1 corr < +0.5 or G2 offset outside −25..+5 ⇒ the fleet
  reduction is not measuring the level — record, do not tune the reduction
  (no re-weighting sweeps), and the fold card does not open.

## §5 Adversary corrections (panel run before build; ALL adopted)

1. **C4 restated (the realization bookkeeping was wrong).**  The census
   observable is RAW code on every posture (`raw_observed` is copied before
   the modeled pipeline applies `.BIA`; the GF/IF pins constrain estimator
   states, never the raw code) — so ι̃ carries BOTH d_r,GF and d^s,GF
   everywhere.  The −15 TECU prediction chain was measured on a
   sat-OSB-corrected, fleet-MEAN instrument; the census is raw and MEDIAN.
   The honest G2 window is the **median-statistic, raw-realization**
   restatement: §25.6's measured hourly medians −22..−24 slant ≈
   **−14..−17 vertical** — accepted band −10..−20 kept, reasoning corrected.
   On EXT-CB the census subtracts the run's own loaded `.BIA` GF OSB per
   satellite (one lookup; internal to the run's inputs, C5-conformant) —
   removing the sat set-breathing term where products exist AND putting
   L_cs in the same realization as that posture's solve (the stage-2 wire
   edge needs this anyway); on W6/GF-datum it stays raw (the solve's iono
   carries the same d^s content, so comparisons cancel it).
2. **The G2 reference was the card's central flaw**: L_cs minus the
   PLANETARY GIM mean contains the fleet's geographic sampling term
   (several TECU, diurnal) plus sat-DCB set breathing — it measures the
   roster.  Fix adopted: grade **L_cs(t) − censusOperator{GIM}(t)** — the
   per-station GIM vertical at the SAME IPPs pushed through the SAME
   two-tier reduction (diagnostic-only script extension); under SH,
   additionally trace the SH field through the same operator so the in-run
   comparison is sampling-matched on disk.
3. **Arming side-effect (would have broken G3)**: setting the CS advance
   flips `cardsArmed()`, and the qualify pass's verdicts feed dormant D5
   v3/v4 solve consumers (`NetworkInstallCarry.cpp:152,692`) on lineages
   arming corroborate knobs without a slip card.  Adopted: every census
   config (BOTH A/B arms) pins `cycle_slip.gap_instrument: true`
   (instrumentation-only) so `cardsArmed()` is equal across the pair; the
   verdict-consuming dump columns stay out of G3's byte set.  The loader's
   auto-arm `!trace_slip_raw` hole (`GnssConfigLoadNetwork.cpp:5087,5165`)
   is a pre-existing latent bug FILED separately — not bundled here.
4. **Reduction mechanics**: iterate THIS EPOCH's `modeled[i]` rows (the
   `iono_cs` map is deliberately never pruned — stale entries forever);
   exact reuse of `computeIppGeometry` (free function) + `tecu_to_L1m` +
   per-row `getFrequency(system, sig1, sat)` (the pair's primary band is
   not always L1 — the (L2,L5) fallback is a 65 % mis-scale under the L1
   constant); constellation gate matched to the SH rows; placement BEFORE
   the RegisterColumns SkipEpoch.  Dispersion quoted as MAD, never formal σ
   (the kernel's var is documented optimistic).
5. **G1 venue corrected**: the wsh fixture/arm must be REGENERATED (runs/
   archived 2026-08-12, per-directive untrusted) — and regenerated WITH the
   §29.5 empirical receiver-bias distribution injected
   (`truth_rx_bias.csv`, sim support exists) so the entire C4 confound
   model becomes truth-gradable in sim before any field read.  G1's
   reference statistic = the census operator applied to TRUTH (not the
   coverage-weighted mean — median vs mean skew).
6. **G4 reworded**: census epochs ≈ run epochs MINUS the min_n warmup (and
   after any warm resume — the store cold-starts by design).
7. **§29.6 wording**: the legitimate stage-2 EXT route is feeding L_cs as
   the TARGET of the existing per-epoch a00 anchor — not resurrecting a
   datum-target mechanism (the old rejection was "uglier plumbing", not
   staticness).  §30.6's brdc-Klobuchar a00 knob is the unbuilt sibling —
   L_cs, if it gates through, is the better-sourced replacement for that
   design too.
8. **SEM attack self-refuted by the panel** (recorded so it is not
   re-raised): d_r is static per station — the median is a deterministic
   order statistic; the movers are set churn (~0.3–0.9 TECU/station),
   sat-DCB breathing (§5.1 kills it on EXT-CB), and median-rank wander
   under diurnal VTEC re-ordering — which the §5.2 sampling-matched
   reference cancels from the GRADE even though it stays in L_cs itself.

## §6 Results

(filled after the arms)
