# RULE F3: the LLI-epoch verdict and its anchor clause

**STATUS**

- **id:** RULE F3
- **topic:** slip
- **status:** closed
- **summary:** At a receiver-flagged epoch with no MW verdict, F3 implicates the flagged bands, so S3's level residual under `all` returns from 0.335 to 0.005 cyc; the anchor clause keeps references together.
- **dates in the text:** none
- **knobs / posture:** `network.cycle_slip.common_band_scope` (the gate; no new knob) with `network.cycle_slip.if_phase_td_pairs` (`first` / `all`); counters `cb_lli_verdicts`, `cb_lli_narrowed`, `cb_lli_anchor_colocate`
- **hashes named in the text** (commit / exe / byte-cert): `420634f` (main's exe, the OFF reference); products `4c96e34a4aebc4a9` (quiet 240-epoch null), `4e0b5606` / `4e0b56068e44a862` (L2 control), `fd217f07` / `fd217f072a2eaf98` (S2); exes `gnss_processor_iftdpairs_v6.exe` (`B94E7600D4159…`), `gnss_processor_iftdpairs_v7.exe` (`35D7DDF8BDFD5…`)
- **run dirs named in the text:** `runs/evleg_t_s3_fix`, `runs/golden_sim_campaign_zwdblk_cbscope4`, `runs/evleg_u_anch_fix_cl`
- **source:** `docs/archive/SPEC_event_harness.md` §14.12 (14.12.1 to 14.12.6)

**Outcome.** The question: under `all`, the IF-TD pairs holding an LLI-flagged band fired unscoped at the LLI epoch, where every MW sample is withheld and no verdict exists; can the receiver's bits form one? F3 implicates only the flagged bands, and S3 and S1 under `all` equal their `first` arms digit for digit. The Study-7 storm arm then found F3 splitting references on anchor arcs (mean 7.6 against 3.5); with the anchor clause the anchor leg matches the null. That arm, re-run with the clause, made both knobs RECOMMEND_ON on the sim golden ([Study 7](../slip/STUDY-7-common-band-narrowing.md) §16.8); the current goldens set both on both beds.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 14.12 RULE F3 — the LLI-EPOCH verdict: the epoch the rule could not see

### 14.12.1 The gap §14.10.4 found

At an epoch where the receiver flags a band, `MwSlipDetector::detect`'s
uncombined branch withholds **every** MW sample of that satellite (§14.11).  So
`flagged_pairs` is empty, the §14.2 rule returns FALLBACK 1 — "no MW pair fired,
not a verdict at all" — and **the drains run UNSCOPED**.

That was harmless for as long as the LLI drain was the only actor at such an
epoch: it frees exactly the bands the receiver flagged, i.e. it is already
band-scoped by its own evidence.  It stops being harmless the moment a SIBLING
drain can fire in the same epoch.  Under `if_phase_td_pairs = all` the IF-TD
pairs that contain the flagged band do fire there — the IF-TD detector does not
skip LLI epochs — and unscoped, each frees BOTH of its bands.  On a triple-band
arc with an L5 flag that is (1,5) ∪ (2,5) = all three, so every LLI epoch becomes
an all-band free.  Measured on the §14.10 **S3** leg (L5Q −31 with the flag, the
LLI path alone):

| S3 leg | level residual @+45 | slant-iono peak | +1 verdict |
|---|---|---|---|
| `first` (only the LLI drain acts) | **0.005 cyc** | 0.004 m | `keep implicated=L5Q` |
| `all`, no F3 | **0.335 cyc** (the §9 all-band floor) | 0.143 m | `keep implicated=L5Q`, unchanged |

The verdict at +1 was right the whole time; the damage was done at +0, where
there was no verdict to obey.

### 14.12.2 The rule

Gated by `network.cycle_slip.common_band_scope`; no new knob, OFF stays
byte-identical.  When LLI bits are set for a (station, satellite) at this epoch
**and the normal path formed no verdict** (no MW pair flagged — on the uncombined
path, all withheld), form one:

- `action = keep`, `implicated = lli_signals ∩ live_bands`;
- **no exoneration step.**  The receiver's bits ARE the scope — today's LLI drain
  already trusts them that far, and there is no MW evidence at this epoch to
  weigh anything against.  `exonerated` stays empty so the `bands_kept` census
  keeps meaning "spared by positive exoneration";
- basis `lli=L5Q;mw=withheld`, outcome @ref CommonBandOutcome::LliOnly, counters
  `cb_lli_verdicts` / `cb_lli_narrowed` (knob-independent), run-end clause
  extended;
- every drain at that epoch — the LLI free and every IF-TD pair refloat — then
  frees (its bands ∩ implicated), through the one existing hook, exactly as the
  drains already do under a verdict.

**If any MW pair DID flag at an LLI epoch the normal path runs, unchanged**: F3's
guard is `flagged_pairs.empty()`, and the §14.2 rule already implicates every
band carrying a loss-of-lock bit, so there is nothing for F3 to add there.

Two degenerate cases are explicit: a flagged band that is not tracked leaves
`implicated = live_bands` and `narrowed = false`, so a caller cannot restrict a
drain by accident; and every live band flagged is today's response, also
`narrowed = false`.

**⚠ Under `first` + scope ON this also scopes the (L1C+L2W) IF-TD drain** when the
LLI is on L1C or L2W — today that drain frees both of its bands at an LLI epoch.
F3 is therefore not an `all`-only rule, and its first effect on the existing ON
posture is a narrowing the ON lineage has never run.

**Deliberately NOT applied: the §14.5 ANCHOR CLAUSE.**  An F3 verdict is not
refused on an arc that anchors a family of the satellite, because the LLI drain
ALREADY frees exactly the flagged bands there today — so F3 changes only the
sibling drains, and refusing to scope them would restore the very all-band free
the rule exists to remove.  Whether that trade costs reference co-location is
measurable rather than assumed: the always-on split-referenced-satellite census
(§14.5) reads it on every arm.

### 14.12.3 Legs and gates

See §14.12.4 for the measured table.  Pre-registered, same seed / station /
satellite / arms as §14.10.4:

| leg | pre-registered |
|---|---|
| **S3 FIX** | +0 `keep implicated=L5Q` with the IF-TD refloat rows scoped to L5Q; +1 `keep implicated=L5Q` as before; level residual @+45 and iono peak equal to the `first` values (0.005 cyc / 0.004 m) |
| **S1 FIX** | +0 `keep implicated=L5Q`; +1 `all … lli_shadow` unchanged; residual table unchanged from the S1+F2 row |
| **S2 FIX / L2 FIX** | products byte-identical to the pre-F3 `all` runs (`fd217f07…` / `4e0b5606…`) — neither leg has an LLI, so F3 cannot reach them |
| **null 70 / null 240** | `all` ≡ `first` on products / solution / ZWD / DD-fractional; OFF ≡ main's `420634f` exe |

### 14.12.4 RESULT — the gap closes, and `all` becomes a pure addition on every leg

Final exe `gnss_processor_iftdpairs_v6.exe` (SHA-256 `B94E7600D4159…`), same seed /
station / satellite / arms; every knob explicit; all three arms of every leg
rebuilt from FRESH event dicts and their schedules verified before a single run.

| leg | ON (scope, first) | FIX (scope, all) |
|---|---|---|
| **S3** LLI only | +0 `keep rule=lli_epoch;implicated=L5Q;narrowed=1;basis=lli=L5Q;mw=withheld`; +1 `keep implicated=L5Q` | **identical rows**, plus the two scoped drains: `T1_if_slip … pair=L1C+L5Q;cb_kept=L1C` and `… pair=L2W+L5Q;cb_kept=L2W` |
| **S1** storm shape | +0 the same `lli_epoch` row; +1 `all … mw(L1C+L5Q)lli_shadow(n=10)` | identical rows + the same two scoped drains at +0 |
| **S2** equal-count | +0 `keep implicated=L2W` (the defect) | +0 `all reason=no_exoneration`, `iftd(L1C+L5Q)=flagged` |
| **L2** single band | +0 `keep implicated=L2W` | identical verdict |

Per-band DD level residual of the injected arc (event − null, cycles):

| leg / arm | L2_d@+1 | L5_d@+1 | L2_d@+45 | L5_d@+45 | oth_d rms +14..45 | ZWD_d peak | iono_d peak | T4 |
|---|---|---|---|---|---|---|---|---|
| S3 ON | 0.001 | 0.032 | 0.005 | 0.008 | 0.000182 | −0.0006 m | 0.0037 m | 0 |
| **S3 FIX** | **0.001** | **0.032** | **0.005** | **0.008** | **0.000182** | **−0.0006 m** | **0.0037 m** | 0 |
| S1 ON | −2.214 | −2.280 | −0.367 | −0.378 | 0.021447 | −0.0026 m | −0.331 m | 0 |
| **S1 FIX** | **−2.214** | **−2.280** | **−0.367** | **−0.378** | **0.021447** | **−0.0026 m** | **−0.331 m** | 0 |

**S3 FIX is now S3 ON digit for digit** — the §14.12.1 cost (0.335 cyc / 0.143 m)
is gone, and the pre-registered `first` values (0.005 cyc / 0.004 m) are met
exactly.  **S1 FIX is S1 ON digit for digit** too.

Byte-identity:

| comparison | products / solution / ZWD / DD-fractional |
|---|---|
| **OFF vs main's `420634f` exe**, L2 control leg | **IDENTICAL** (events too) |
| **OFF vs main's exe**, quiet 240-epoch null | **IDENTICAL** (events, rx-states and the consumer trace too; products `4c96e34a4aebc4a9`) |
| `all` vs `first`, quiet 70-epoch null | **IDENTICAL** |
| `all` vs `first`, **S3** and **S1** | **IDENTICAL** — with F3 the widening no longer changes the solve on an LLI leg at all |
| `all` vs `first`, L2 control | **IDENTICAL** (`4e0b56068e44a862`) |
| S2 FIX vs the pre-F3 `all` run | **IDENTICAL**, events included (`fd217f072a2eaf98`) — no LLI on that leg, so F3 cannot reach it |

So after F3 the only legs on which `all` changes the solve at all are the ones
where a *new pair catches a slip the first pair could not see* — S2.  That is the
contract the card was aiming at.

Run-end clause (`runs/evleg_t_s3_fix`):

```
Network (common-band scope): verdicts 480, narrowed 4 (bands kept 8), all-band 476
  (no exoneration 470 / inconsistent 6 / anchor co-locate 0), IF-TD vetoes 0,
  immature pairs 720, LLI shadows 0, LLI-epoch verdicts 1 (1 narrowed),
  weak silences 3; split-referenced satellites mean 3.4 / max 5 of 52.3 anchored
  [knob on].
Network (IF-TD pairs [all]): 5 pair kind(s), eval 202015 / flag 2 / refloat 2 total —
  L1C_L2W eval 39814 / flag 0 / refloat 0, L1C_L5Q eval 67338 / flag 1 / refloat 1,
  L1C_L7Q eval 27525 / flag 0 / refloat 0, L2W_L5Q eval 39813 / flag 1 / refloat 1,
  L5Q_L7Q eval 27525 / flag 0 / refloat 0.
```

The **split-referenced satellite census reads 3.4 / max 5 on both arms**, i.e. the
`first` value — so scoping the LLI epoch did not separate a satellite's references,
which is the measurement the deliberate omission of the anchor clause (§14.12.2)
was left open for.

Unit suite 1897 cases / 259 suites, 1880 passed, 17 fixture-gated skips, 0
failures (+27 over main's 1870; F3 adds 5 rule/hook cases to
`tests/test_common_band_scope.cpp`).

⚠ **A harness-builder trap, recorded because it cost a leg.**  `event_leg.py
make-leg` arms ONE event; a multi-event leg patches `leg.json` afterwards.  A
builder that hoists the event list out of the arm loop and then `pop()`s the
per-event offset out of it mutates the shared dicts, so the SECOND arm built gets
a different schedule — here S2's second slip moved from +0 to +1, turning the
equal-count trap into a two-epoch slip and producing a plausible-looking but
different verdict.  Build the schedule fresh per arm, and **print every leg's
`(signal, cycles, lli, tow − t0)` before running any of them** (the batch-6
builder does).

Still owed before any promotion: the storm-bed arm.  Both knobs stay default
(`first` / OFF).

### 14.12.5 The ANCHOR CLAUSE inside F3 — iteration 2's defect, re-entered through the LLI door

The Study-7 storm arm (`runs/golden_sim_campaign_zwdblk_cbscope4`) cleared every
§14.10/§14.11 gate — the 4.99 h fallout gone (0 T4 rows / 0 clock decouples / 0
declared clock breaks at 4.9917), unanswered exonerations 1 → 0, T4 86 → 11, ZWD
0.0071 → 0.0064, L1C ref-free 89.6 → 90.1 %, cold handovers 59 → 51, consumer
pooled p50 0.2067 → 0.1673 (better than OFF's 0.1741) — with ONE defect, and it
was F3's:

| quantity | §14.9 (cbscope3) | cbscope4, F3 without the clause |
|---|---|---|
| split-referenced satellites, mean of 52.8 | 3.5 (6.6 %) | **7.6 (14.4 %)** |
| single-family anchor handovers | 57 | **103** |
| …of the new ones, on an arc carrying an F3 verdict that epoch | — | **49** |
| single-family handovers from the NORMAL path's `keep` rows | 0 | **0** |

§14.12.2 had left the clause off deliberately, on the argument that the LLI drain
already frees exactly the flagged bands so F3 changes only the sibling drains.
That argument is sound for the DRAIN and wrong for the DATUM: narrowing on an arc
that anchors a family still frees only the flagged band's family, so the election
hands THAT family over alone and the satellite's per-frequency references
separate.  The §14.10 S3 leg could not see it — **S21000SIM anchors nothing**,
which is exactly why §14.3 chose it.

**The fix** is the §14.5 clause applied inside F3, identically: if the arc anchors
ANY family of the satellite the LLI-epoch verdict is `all` with reason
`anchor_colocate`, so every family moves to one successor.  Counted in the shared
`cb_anchor_colocate` (which stays the honest total) AND in its own
`cb_lli_anchor_colocate`, because the storm arm needed exactly that split to
attribute the regression to the F3 path.  The clause fires only on a verdict that
WOULD have narrowed, so an LLI on an untracked band is not charged a co-locate it
never earned.  The event row keeps `rule=lli_epoch` on the co-locate row too —
without it the two paths' refusals are indistinguishable in the trace.

### 14.12.6 RESULT — the anchor-arc leg (S36000SIM / G04, L5Q −31 with LLI at +10)

Exe `gnss_processor_iftdpairs_v7.exe` (SHA-256 `35D7DDF8BDFD5…`), quiet seed
ep0665, 70 epochs, knobs explicit.  G04's families anchor at S36000SIM (§14.8).

| arm | +0 verdict | +0 PHASE_ANCHOR handovers | later | split-ref mean / max |
|---|---|---|---|---|
| null (any arm) | — | — | — | **3.4 / 5** |
| **OFF** | (no verdict) | 1 → S60000SIM | +1: 1 → S31000SIM | 4.3 / 6 |
| ON `first`, pre-clause | `keep rule=lli_epoch;implicated=L5Q` | 1 → S60000SIM | +1: 1 → S31000SIM | 4.3 / 6 |
| ON `first`, **clause** | `all rule=lli_epoch;reason=anchor_colocate` | 1 → S60000SIM | +1: 1 → S31000SIM | 4.3 / 6 |
| **FIX `all`, pre-clause** | `keep rule=lli_epoch;implicated=L5Q`, drains scoped (`cb_kept=L1C`, `cb_kept=L2W`) | **1 → S60000SIM** | +1 normal path fires its own clause, 1 → S31000SIM | **4.3 / 6** |
| **FIX `all`, clause** | `all rule=lli_epoch;reason=anchor_colocate`, drains unscoped | **3 → S60000SIM, all one successor** | +1 `keep implicated=L5Q`, no further handover | **3.4 / 5 = the null** |

Both pre-registered predictions hold: pre-clause **one** family hands over and the
split census rises above the null; with the clause **all three** families hand over
to the **same** successor at +10 and the census equals the null's.

**The OFF control is what makes the attribution exact.**  OFF — knob entirely off —
gives the same 1 handover and the same 4.3 / 6 as the ON arm with or without the
clause.  So the single-family handover at an LLI epoch on the `first` lineage is
**today's behaviour**, not a card effect: the LLI drain is band-scoped by the
receiver's own bits and always was.  F3's leak was `all`-SPECIFIC — it scoped the
IF-TD pair drains that would otherwise have freed all bands and co-located the
families — and the clause removes exactly that.  With the clause the `all` arm is
the **only** posture on this leg that keeps a satellite's references together
(3.4 / 5 against OFF's 4.3 / 6).

Regression set, all byte-identical including `network_events.csv`:

| comparison | products / solution / ZWD / DD-frac / events |
|---|---|
| S3 at S21, ON and FIX, vs §14.12.4 | **IDENTICAL** (S21 anchors nothing, so the clause cannot fire) |
| S1 at S21, ON and FIX, vs §14.12.4 | **IDENTICAL** |
| OFF null 70 vs §14.12.4 | **IDENTICAL** |
| OFF quiet 240-ep null vs main's `420634f` exe | **IDENTICAL** (products `4c96e34a4aebc4a9`, + rx-states and the consumer trace) |
| `all` vs `first`, quiet 70-ep null | **IDENTICAL** on products / solution / ZWD / DD-frac |

Run-end clause (`runs/evleg_u_anch_fix_cl`):

```
Network (common-band scope): verdicts 480, narrowed 4 (bands kept 8), all-band 477
  (no exoneration 470 / inconsistent 6 / anchor co-locate 1), IF-TD vetoes 0,
  immature pairs 720, LLI shadows 0, LLI-epoch verdicts 1 (0 narrowed,
  1 anchor co-locate), weak silences 3; split-referenced satellites mean 3.4 /
  max 5 of 52.3 anchored [knob on].
```

Unit suite 1900 cases / 259 suites, 1883 passed, 17 fixture-gated skips, 0
failures (+3: the clause on an anchor arc, the unchanged non-anchor arc, and the
guard that a null narrowing is not charged a co-locate).

**Residual, recorded not fixed** (from the Study-7 arm): 2 spurious-implicate
`keep` rows remain — S31/G18 at 3.675 h with (L1C+L5Q) silent at n = 36, thr
10.6 WL, and S27/E30 at 5.2333 h with (L5Q+L7Q) silent at n = 10, thr 1.2 WL.
Both are YOUNG-LANE silences, not the F2 shadow: the maturity bar is 10 samples
and these lanes met it while their gates stood at 10.6 and 1.2 wide-lane cycles.
The `weak_silence` counter already reports the first class; the second is a
mature-but-thin lane.  Ledger residual, not a §14.12 item.

Still owed before promotion: nothing new from §14.12.5 — the storm arm is the gate
and it is the manager's to re-run with the clause.  Both knobs stay default
(`first` / OFF).

