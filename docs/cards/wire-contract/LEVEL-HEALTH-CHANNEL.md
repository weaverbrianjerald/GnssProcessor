# level health: the `network.emit_level_health` channel and its consumer contract

**STATUS**

- **id:** level health
- **topic:** wire-contract
- **status:** superseded
- **summary:** Phase A flagged era-3 (7/7) and passed era-2 (16/16), the causal limb missed slides; Phase B never built. Superseded: the re-founding drops SH from the producer, so the a00 level it watches is gone.
- **dates in the text:** 2026-08-23
- **knobs / posture:** `network.emit_level_health`, `network.level_health_dev_tecu`, `network.level_health_ref_epochs`, `network.level_health_ref_max_age_s`, `network.level_health_confirm_epochs`, `network.level_health_relevel_hold_s`
- **hashes named in the text** (commit / exe / byte-cert): exe `92FC13A7…`
- **run dirs named in the text:** `runs/_wd`, `runs/_wd/whattrib`, `runs/_wd/whescal`
- **source:** `docs/archive/SPEC_withhold_era_release.md`: §12, §12.1, §12.2, §12.3, §12.4, §12.5, §12.6, §12.7, §12.8, §12.9

**Outcome.** Can a self-referenced flag token on the H heartbeat rank the SH level's health, which σ̂₀ anti-ranks? The Phase A replay flagged era-3's 7 toxic epochs and passed era-2's 16 clean ones, but the causal limb missed the 276150 slide (`lvlretro` required); VA-3/5/7/9 went unreported; Phase B was never built. Superseded: [the re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) drops SH from the producer (`estimate_iono_sh: false`), so the watched a₀₀ level is gone and the goldens emit 720 of 720 epochs; Phase B and the joint [A4 withhold](../raim-qc/WITHHOLD-ERA-RELEASE.md) promotion are moot. Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_withhold_era_release.md -->
## §12 The WITHHOLD CONSUMER CONTRACT — the level-health channel (`network.emit_level_health`)

**Session 2026-08-23.  Status: DERIVED (this card); implementation + validation
pre-registered below.  Owner of §10.1 limit (3) / the §11.3 blocker.  Evidence
base: the same-build 6 h pair (`runs/_wd/whattrib` / `runs/_wd/whescal`, exe
`92FC13A7…`), whose recovered-epoch split, anti-ranking measurement, and clean-
epoch baseline are this card's design inputs and its labeled truth set.**

### §12.1 The contract gap, named exactly (all measured on the 6 h pair)

A consumer of this product today has NO channel that ranks level health:

1. **σ̂₀ ANTI-RANKS it.**  The five worst-level epochs (−21.5…−14.1 TECU)
   carry σ̂₀ 3.10–4.19, all BELOW era-2's level-clean 269430 (σ̂₀ 6.22).  A
   consumer downweighting by σ̂₀ keeps the poisoned epochs and drops clean ones.
2. **The `emitted_contaminated` class rides the trigger AND lives only in the
   events trace** (`NetworkRaim.cpp:889` — a T2 event row, gated in practice
   on the events CSV being open).  Era-3's 7 toxic epochs never crossed the
   3.0 trigger (final σ̂₀ 2.27–2.97; the within-epoch T2 refloats ABSORBED it
   — 3 `we_refloat_*` actions, measure/refloat rows only), so they carry no
   class anywhere, and even the classified epochs are invisible to a product
   consumer.  Two defects: trigger-blind AND trace-gated.
3. **Every other armed instrument is silent through a −21 TECU excursion**:
   formal a00 std unchanged (0.588–0.594), no datum flag, n_adapt 0–1.
4. **The blindness is POSTURE-WIDE, not an escalation defect**: the stock
   reference arm silently emitted its own toxic epoch (276150, d_local
   −11.42, σ̂₀ 2.73), and both arms' post-mass-re-level epochs run +5.7…+12.7
   TECU hot with σ̂₀ pinned at the exact-1.0 zero-redundancy sentinel and no
   marking of any kind (the §11-caveat open limb).

Constraint set carried in (each pre-paid): GIM/external data is diagnostics-
only — the instrument must be SELF-referenced; the level VALUE stays untracked
(LV-6) — this card FLAGS and must not fix, and must not touch the rx-CB
carry's memory in either direction (both falsified); consumer SUPERSEDES never
adds (M4 v2); never gate a DECLARATION on a QC/trace knob (legend card);
emit-side additions fingerprint-SAFE or explicitly fingerprinted; adapt-and-
EMIT — a contract that blanket-rejects the recovered class re-creates the
withhold (16 of 23 recovered epochs were level-clean and must stay usable);
invariant #7 (both edges, counters, identities); invariant #4 (armed-but-inert
must be diagnosable at run end).

### §12.2 The mechanism — one channel, three classes, causal + retro-confirm

**What is measured.**  After each ACCEPTED solve, the producer reads its own
co-estimated global-mean ionosphere `a00 = x_s(vtecCoeffColumn(0,0,'C',L1))`
— the identical quantity `network_vtec_sh.csv` records and §11 graded on —
and forms the CAUSAL self-trend deviation

    d_causal(t) = a00(t) − median{ a00(t_i) }

over the last `K = level_health_ref_epochs` (default 5) EMITTED epochs whose
own class was `ok`, with content age ≤ `level_health_ref_max_age_s` (default
900 s).  Self-exclusion of flagged epochs is load-bearing: without it the
reference tracks the dip and the recovery false-flags at +14 TECU; with it
the era-3 dip is measured against the pre-era trend, exactly as §11.2's
centered d_local was.  Fewer than K valid entries ⇒ INDETERMINATE (never a
guess, never a block).  Cost: one median over ≤ 5 doubles per epoch — no
O(dim) work, no solve interaction.

**The classes** (an epoch carries a possibly-empty flag SET; `ok` = none):

- **`c` — trigger contamination.**  The EXISTING criterion verbatim
  (`raim_within_epoch` armed && post-adaptation σ̂₀ > `raim_sigma0_trigger`,
  `NetworkRaim.cpp:884`), promoted from trace-event-only to the product
  channel.  The T2 event row is unchanged; the class is set on the ctx
  scratch UNCONDITIONALLY of any trace stream.  Covers §12.1's known class.
- **`s` — level-suspect.**  |d_causal| > `level_health_dev_tecu` (default
  **6.0** TECU) with a valid reference.  Threshold provenance: above the
  144-epoch clean-emitted baseline's max |d_local| = 5.98 (p97.5 = 2.69) and
  1.7× UNDER the era-3 toxic floor (9.96) — margin both ways; ≈ 7.5× the
  clean p16/p84 half-width (±0.8); above each arm's own 30 s breathing p95
  (3.1–4.9) even in the doubled hours-4-6 regime.  Two-sided: the post-reset
  excursion is POSITIVE (+12.7).
- **`r` — fresh-re-level transient.**  Set at the mass re-level edge — this
  epoch's inject-side stale-skip count ≥ 50 % of carried leveling priors
  (the §1.3 signature; measured 535/61-sta and 556/61-sta at exactly the two
  era-exit epochs) — and HELD for `level_health_relevel_hold_s` (default
  900 s, the measured σ̂₀≈1.0 zero-redundancy transient).  This gives the
  900-s cliff edge its consumer MARK (the §11-caveat open limb); the wired
  RESPONSE at that edge remains a separate owed card — this card only flags.
- **`na` — indeterminate**: reference warming/starved, or no SH co-estimation
  in the posture.  `r`, `c`, `s`, and `na` epochs are all excluded from the
  reference.  ⚠ Declared consequence: a long all-`c` era (era-5: 28/30) can
  starve the reference into `na` — honest indeterminacy, never dip-tracking.

**What is emitted, at which edge.**  A trailing key=value token on the H
heartbeat line of every EMITTED epoch (withheld H lines carry none — a
withheld epoch delivers nothing to class):

    H,<week>,<tow>,<status>,<sigma0>,<n_sats>,<withheld>,lvl=<flags>[:<dev>]

`<flags>` ∈ concatenations of `c`/`s`/`r`, or `ok`, or `na`; `<dev>` = signed
d_causal ("%.2f" TECU), present whenever computed (including `ok`) so a
consumer may apply its own tighter threshold.  Verified forward-compatible:
the current reader parses H fields f[1..6] positionally and ignores trailing
fields (`NetworkProductIO.cpp:237-243`); the '#' legend is extended (enforced
by `HeaderLegendDeclaresEveryEmittedField`).  The S contract line gains

    lvl=<thr_tecu>:<live_classes>        e.g.  lvl=6.0:csr

declaring the threshold AND which class letters are live in this run (with
`raim_within_epoch` off, `c` is structurally absent and the declaration says
`sr`) — so "no c epochs" and "c not monitored" are distinguishable, and the
declaration is gated ONLY on the emit knob, never on a QC/trace knob (legend-
card rule).  `NetworkProducerStatus` gains optional `level`/`level_dev`
fields (empty default; `parseState`-style unknown-tolerance preserved), and
the consumer-replay H-rider forwards them.

**Retroactive limb (the causal-vs-centered answer, stated).**  At emit time
the producer knows: a00(t), the trailing clean reference, σ̂₀, n_adapt, the
T2 verdict, and the re-level edge (the inject-skip census runs in assembly,
BEFORE emit).  It cannot know whether a sub-threshold drift is an onset, nor
whether a flag is the field legitimately moving — those need future epochs.
So: `N = level_health_confirm_epochs` (default 5; 0 disables the limb)
emitted epochs later, the CENTERED verdict (5 clean neighbors each side —
§11.2's d_local exactly) re-evaluates the `s` decision; where it differs
from what was emitted, the next H line carries a supersession token

    lvlretro=<tow>:<class>[;<tow>:<class>...]

draining a pending queue with the `ura_retro_bump` mark==land discipline
(`NetworkSolPrep.cpp:644-672`): a correction is serialized only when it
lands on an actual emit.  Retro semantics are ACCEPTABLE to this contract by
precedent — `ura_retro_bump` already flags a past epoch's contamination at
the next emit, and M4 v2's supersession is the contract's update primitive
(the consumer REPLACES its stored class for that TOW, never accumulates).
The two consumer postures are stated explicitly: a hard-RT consumer operates
on the causal token alone — which on the labeled truth set catches 7/7 with
LAG 0 (era-3's first epoch is already −9.96) — accepting the un-measured
miss class (a slow sub-threshold onset; no example exists on this pair); a
consumer integrating longer than N·rate (150 s at 30 s) honors `lvlretro`.
The retro limb exists to BOUND the causal limb's miss class, not to carry
the detection.

**Events + counters (invariants #7/#4).**  An `s`-flagged stretch is an era
at both edges: `LVL/era_enter` (dev at entry, reference value + age) and
`LVL/era_exit` (length, min/max dev, class mix) into the events trace when
open — the product tokens are the consumer-side record regardless.  Run-end
teardown line prints the full census: `n_lvl_ok`, `n_lvl_suspect`,
`n_lvl_contam`, `n_lvl_relevel`, `n_lvl_na`, `n_lvl_retro_upgrade`,
`n_lvl_retro_downgrade`, `n_lvl_era_entries`, `lvl_era_max_len`.
Armed-but-inert is diagnosable: all-`na` with n_emitted matching `n_lvl_na`
(plus the loader warn) = no SH model / never-warm; token census vs counter
mismatch = mis-wiring.

**What this card does NOT do** (scope fence, each constraint-traceable): no
withhold, no deweight, no adaptation trigger — flag-only, so the no-deadlock
property is structural; no level fix and no rx-CB carry-memory change (LV-6 /
amendment-6 corollary — the per-epoch level AUTHORITY is the separate open
card); no GIM anywhere in-line; no per-station granularity (a00 is the global
mean — regional excursions cancelling in the mean are invisible; declared).

### §12.3 Config surface

| knob | type | default | notes |
|---|---|---|---|
| `network.emit_level_health` | bool | **false** | master; auto-enables `emit_epoch_status_record` (the R4/`ura_retro_bump` auto-enable precedent — the H line is the carrier) |
| `network.level_health_dev_tecu` | double | 6.0 | `s` threshold, two-sided |
| `network.level_health_ref_epochs` | int | 5 | K, trailing clean-reference size |
| `network.level_health_ref_max_age_s` | double | 900.0 | reference content-age horizon; overlaps the 900 s cliff by design — post-cliff epochs go `r`/`na`, never silently re-referenced |
| `network.level_health_confirm_epochs` | int | 5 | N; 0 = causal-only |
| `network.level_health_relevel_hold_s` | double | 900.0 | `r` hold after the mass-re-level edge |

Loader: warn-not-interlock when SH co-estimation is absent (channel runs
all-`na`) and when `raim_within_epoch` is off (`c` structurally absent; the
S-line declaration reflects it).  Summary toggle-log line per convention.

**Fingerprint class: fingerprint-SAFE** (measure+emit only — the trace-class
family).  The solve is untouched (VB-2 certifies byte-identical solve CSVs
armed-vs-not); `LevelHealthState` (ring buffer, retro queue, era scratch,
the T2 per-epoch flag) lives in `NetworkRunContext`, is NOT serialized into
snapshots, and a warm resume re-warms in K epochs, visible as K `na` tokens.
Legal on a warm resume.  ⚠ The product FILE differs when armed (tokens +
S declaration + legend) — same class of product delta as arming the H
heartbeat itself; arm only when consumers reading the file are current (the
existing RTA-10 rule extends verbatim).

### §12.4 The consumer contract (normative)

1. `lvl` is the ONLY level-health ranking.  σ̂₀ MUST NOT be used to rank
   level health — measured anti-ranking on this product class.
2. `ok` epochs are full-service, INCLUDING recovered ones (16 of 23 measured
   clean) — a consumer that rejects the recovered class wholesale is out of
   contract (it re-creates the withhold).
3. `s` / `c` / `r` epochs: response is the consumer's risk policy; the
   contract mandates only that iono-sensitive long-memory state MUST NOT
   integrate an `s`-class epoch unaccounted.  `<dev>` is provided for
   consumers applying tighter thresholds.
4. `lvlretro` SUPERSEDES the named TOW's causal class (replace, never add).
   Consumers with integration commitment > N·rate honor it; hard-RT
   consumers run causal-only with the documented miss class.
5. Unknown trailing tokens are ignored (existing grammar rule); pre-RTA-10
   and current readers parse the file unchanged.

### §12.5 Pre-registered validation (BEFORE any build) — the 6 h pair is the truth set

**Phase A — offline classifier replay** (`scripts/wh_level_flag_replay.py`
over the recorded `logs/network_vtec_sh.csv` + dispositions + events of BOTH
arms; the classifier is pure arithmetic over recorded series, so it runs
before a line of C++; gates on the CAUSAL statistic, centered only in VA-7):

- **VA-1 (sensitivity, HARD)**: era-3's 7 ESC-emitted epochs (TOW
  273270–273450) all class `s` causally — **7/7**.  Predicted margin: min
  |d_local| 9.96 vs thr 6.0.
- **VA-2 (specificity, HARD)**: era-2's 16 recovered epochs (269220–269760)
  — **zero** `s`.  Predicted: max |d_local| 1.37.
- **VA-3 (clean baseline)**: ≤ 2 causal `s` false-flags per arm over each
  arm's settled clean-emitted population (centered baseline max 5.98 < 6.0
  predicts 0; the causal-vs-centered difference is what is under test).
  Every flag individually adjudicated in the report.
- **VA-4 (symmetry / stock arm)**: ATT 276150 (−11.42) flags `s`; ATT
  post-reset 278490–278610 carry `r` (330–450 s < the 900 s hold), `s`
  additionally where |d| > 6 with a valid reference.
- **VA-5 (known-class unification)**: every epoch with a T2
  `emitted_contaminated` event carries `c` — ESC 72/72, ATT 52/52; era-5
  shows `c` on 28/30.
- **VA-6 (era-5 marginals, REPORT-ONLY)**: 277380/277410 (−7.0/−6.4)
  expected `s` at 6.0 — recorded, not gated (labels soft: the era-5
  d_local bracket spans 15–30 min, the coarse-reference caveat).
- **VA-7 (retro consistency)**: N=5 centered confirm yields ZERO downgrades
  on era-3 and ZERO upgrades on era-2; total retro corrections reported.
- **VA-8 (latency, REPORT)**: first-flag epoch == first toxic epoch for
  era-3 (predicted lag 0); dev at first flag reported.
- **VA-9 (indeterminate census)**: every `na` attributable to warmup,
  reference starvation, or the post-900s hole — no unexplained `na`.

Failure handling, pre-registered: one fallback threshold only (6.0 → 8.0);
K/max_age iterate IN THIS SPEC with the change recorded.  If no setting
passes VA-1 AND VA-2 simultaneously, the causal limb is FALSIFIED → STOP,
record, the retro-only variant becomes the candidate, promotion stays
blocked.  No silent tuning.

**Phase B — wired** (after build, suite green incl. the new config +
product-IO round-trip + legend tests):

- **VB-1 (structural byte-identity, invariant #6)**: both 6 h configs rerun
  knob-absent on the new exe — products + five solve CSVs byte-identical to
  the recorded pair.
- **VB-2 (measure-only)**: armed rerun — all solve CSVs byte-identical to
  VB-1; products differ ONLY in `lvl`/`lvlretro` tokens, the S `lvl=`
  declaration, and the legend line.
- **VB-3 (wiring == replay)**: the armed rerun's per-epoch token census
  equals Phase A's classifier output EXACTLY.
- **VB-4 (counters, invariant #4)**: teardown totals == token census;
  era events paired enter/exit.

### §12.6 The promotion gate this unblocks

§11.3's blocker (§10.1 limit 3) closes when Phase A + Phase B pass.  Then
`raim_withhold_adapt_escalate: 12` + `raim_withhold_free_erase: true`
promote as a SCOPED LEDGER ENTRY (CONFIG_promotion_ledger), **jointly and
only jointly** with `emit_level_health: true` (+ its auto-enabled H
heartbeat) — the recovered class's toxic tail is consumer-distinguishable
exactly when the channel is on the wire, so the escalation never ships
without it.  No loader-default flips (fingerprint discipline; the standing
no-blanket-promotion rule).  New network configs derive from a base arming
the triple explicitly.  Re-blocking condition: a field lineage where the
channel's false-flag rate exceeds VA-3's bound, or a toxic class that
evades ALL THREE flags (would also revive deferred item 4 below).

### §12.7 Deferred alternatives — with revival conditions

1. **Make σ̂₀ level-aware** — DEAD on LV-6 (the fold does not open; corr
   +0.681).  Revive only if the per-epoch level-authority card lands.
2. **Level-triggered withhold / blanket recovered-class rejection** — never
   (re-creates the withhold; 16/23 clean).  A level-triggered ADAPTATION is
   a lawful successor ONLY after the level-authority card exists (it would
   touch the level mechanism, out of this card's fence).
3. **GIM-referenced monitor** — prohibited in-line (standing directive);
   stays the offline diagnostic.  Revive only if the directive changes.
4. **WE-absorption suspicion** (count within-epoch refloats as a `c`-adjacent
   signal — era-3's trigger was absorbed by 3 `we_refloat_*` actions) —
   counterfactual and unmeasurable cleanly today.  Revive if a toxic class
   ever evades `s`, `c`, AND `r` simultaneously.
5. **Formal-std channel** — measured flat (0.588–0.594) through −21 TECU;
   structurally blind to a stiff carry.  No revival short of the carry
   variance model changing (carry-aware covariance card territory).
6. **Per-station level flags** — deferred until a consumer needs regional
   selectivity AND the LV-6 tier-2-median suspect is resolved; a00 stays
   the global-mean instrument until then.
7. **The wired RESPONSE at the 900-s mass-re-level edge** (re-validate /
   re-seed the re-coupled levels — invariant #1/#7) — NOT this card; this
   card ships the edge's consumer MARK (`r`) and its census, which is that
   card's motivating instrument.  Eligible immediately.

### §12.8 Implementation map

`GnssConfig.hpp` (6 getters + members), `GnssConfigLoadNetwork.cpp` (parse
beside the RTA-10 block at :4866; auto-enable + 2 warns + summary line),
`NetworkRunContext.hpp` (`LevelHealthState`: a00 ring buffer, retro queue,
era scratch, per-epoch `t2_tripped` + `relevel_edge` flags; knob snapshot; 9
counters), `NetworkRaim.cpp:884` (set `t2_tripped` beside the existing T2
event — trace-independent), assembly inject-skip census → `relevel_edge`
(PASS-B, `ApplicationNetworkAssembly.cpp`), class computation + retro drain
in `NetworkSolPrep.cpp` (beside the `ura_retro_bump` drain it imitates),
token attach in `NetworkProductIO.cpp::writeEpochStatus` (+ legend + S line
+ `NetworkProducerStatus.level`/`level_dev` + parser), H-rider forward in
`ApplicationConsumerReplay.cpp`, teardown line in `NetworkTeardown.cpp`.
Tests: `tests/test_level_health_config.cpp` (defaults, auto-enable, warns,
counter zero-init) + `test_network_product_io` extensions (H token
round-trip, unknown-token tolerance, legend completeness).  New `.cpp`
seams, if any, get manual `CMakeLists.txt` entries.


### §12.9 Phase A EXECUTED (2026-08-23) — PASS on the era-3 class, and two spec defects caught before any C++

`scripts/wh_level_flag_replay.py` over the recorded 6 h pair (the card's own
pre-registered zero-C++ gate):

**PASS — the sharp-excursion class:** escalation arm, causal limb alone,
defaults (K=5, 900 s, 6.0 TECU): **era-3 toxic 7/7 flagged `s`**
(d −9.0…−20.5) and **era-2 clean 16/16 passed**.  Bonus: it also catches
277410 — the era-5 toxic epoch the T2 class MISSED — and the post-era
recovery overshoots (+7.6/+6.6, two-sided as designed).  Flag census:
ESC 16/683 `s` (2.3 %), ATT 52/661 (7.9 % — an eventful tail).

**DEFECT 1 (spec text, fixed in place): the `na` self-exclusion was a
bootstrap deadlock.**  As first written, `na` epochs were excluded from the
reference — so the reference could never warm (first replay: 1160 `na`,
0 `ok`).  Amended: WARMING epochs with no `c`/`r`/`s` flag seed the
reference; the load-bearing exclusion is `c`/`r`/`s` only.

**DEFECT 2 (mechanism, resolves an open design question): the causal limb
alone MISSES the GRADUAL-slide class, and the retro-confirm limb is therefore
REQUIRED, not optional.**  The reference arm's own toxic epoch 276150
(centered d_local −11.4) is a 7-epoch slide (a00 35.2 → 23.3, each step
under the 6.0 per-epoch threshold) followed by a one-epoch snap back.  In the
causal replay the slide epochs either enter the reference (poisoning it) or
are c-excluded (starving it → 276150 lands `na`), and the RECOVERY gets
flagged +10…+13 instead of the trough.  The centered/retro measure catches
276150 trivially — so the `lvlretro` supersession limb is the mandatory
detector for slides, and a consumer reading only the causal token must be
told (S-line declaration) that slides confirm at +N.  The 52-flag ATT census
also confirms the post-reset transients flag correctly (`r`+`s` overlap).

Verdict: the card proceeds to Phase B implementation with the two amendments;
the promotion ledger entry (escalate=12 + free_erase JOINTLY with
emit_level_health) stands as written.
