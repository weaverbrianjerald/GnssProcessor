# COLD-FB — the cold phase-datum anchor fallbacks (`phase_datum_candidacy_usable`) — pre-registration, G1–G3 and iteration 1

**STATUS**

- **id:** COLD-FB
- **topic:** network-vehicle
- **status:** parked
- **summary:** the cold phase-datum anchor fallbacks (`phase_datum_candidacy_usable`) — pre-registration, G1–G3 and iteration 1
- **dates in the text:** 2026-09-11 → 2026-09-12
- **knobs / posture:** `network.phase_datum_candidacy_usable`, `network.phase_datum_candidacy_min_age_ep`
- **hashes named in the text** (commit / exe / byte-cert): `cc689d5`, `90566ebf`, `c763c168`, `f3f37bc3`, `852c4dc3`, `690927f6`, `c5b5df834d185ac8`, `6a83728` … (+2 more in the text)
- **run dirs named in the text:** `runs/evseed_quiet6h_v2`
- **source:** `docs/archive/SPEC_event_harness.md` — 15, 15.1, 15.2, 15.3, 15.4, 15.5, 15.6, 15.7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 15. The COLD anchor fallbacks — KEEP_OFF after TWO iterations: the rule works, the per-family scope was wrong, and fixing it proved the SPLIT was not the consumer's cost (`network.phase_datum_candidacy_usable` + `…_satellite`; census 2026-09-11 on `golden_sim_campaign_zwdblk_mwevid`, built + graded on `coldfb`)

The anchor-slip handover falls back to a COLD re-realization when the elected
successor has no usable carried value (68 of 305 on the storm arm with both
slip cards; 50 of 261 on the anchor-only arm).  Two censuses, both from the
observation trace (`network_slip_raw.csv`, every station × satellite × epoch,
so an arc's age is the time since the satellite rose at that station; the
residual trace cannot be used — pinned arcs never appear in it):

| population | successor arc age, epochs (p10 / p50 / p90) |
|---|---|
| 68 cold handovers | 12 / **39** / 158 |
| 237 continuous handovers | 65 / 236 / 474 |

- 21 of the 68 fall in the arm's first 30 minutes, where EVERY arc is young
  (cold start): unavoidable, and a warm resume never sees them.
- Of the remaining 47, **43 had three or more mature alternatives** (another
  station holding the same satellite for ≥ 60 epochs) and **in 0 of 43 cases
  was the chosen successor the highest-elevation mature candidate** — the
  successors sat at 16–34° with 10–65-epoch arcs while mature candidates
  stood at 78–80°.  Cold handovers come in whole-satellite groups (13 pairs,
  10 triples: every band of the satellite handed over in one epoch — the
  all-band responses the common-band scope of §14 removes for single-band
  slips).

**Hypothesis for the card:** the successor election does not test carry
usability (maturity) as a CANDIDACY condition; it applies its priority (which
appears to favour a station already holding the satellite's sibling bands, or
list order) and only afterwards discovers the carry is unusable and falls back
cold.  **Pre-registered fix:** candidacy requires a usable carried value
(mature arc, finite value_abs, not itself freed this epoch); the normal
priority then ranks the usable candidates; a family with no usable candidate
keeps today's cold path.  **Gates (as pre-registered; §15.3–§15.6 report each one, and §15.5 replaces the leg population the last sentence names — the census proxy did not predict carry usability):** OFF byte-identical; on the storm arm the
cold count falls from 68 to ≤ 25 (the cold-start 21 remain), continuous
handovers land on arcs of age ≥ 60, consumer / ZWD / T4 not worse than §13.6;
the harness leg is the §12.7 single-event anchor slip at a station whose
satellite has a young low successor and a mature high one available.

### 15.1 The card as built — pre-registration, frozen before any leg (2026-09-11 evening; worktree `coldfb` / branch `worktree-coldfb` off main `cc689d5`)

`network.phase_datum_candidacy_usable` (bool, default **false**) +
`network.phase_datum_candidacy_min_age_ep` (int, default **60**; 0 = no age test).

When the knob is ON **and a handover is about to happen** — an incumbent EXISTS
for the key and the sticky branch did not keep it (absent, lost the key, or
banned by an injector / anchor-slip directive) — the candidate sweep gains a
FIRST pass over the SAME coverage-ranked list that accepts only candidates whose
carried value is USABLE:

```
usable(i, amb) := the continuous branch's OWN predicate on the ledger entry
                    (finite value, 0 < var <= phase_datum_handover_max_var_m2,
                     the ratio ceiling when armed, and NOT a forced-cold directive)
                ∧ (i, amb) not in this epoch's freed sets (relevel.free_ambs, carry_slipped_ambs)
                ∧ (min_age_ep == 0 ∨ age unknown ∨ n_loop_epochs − LocalCarry::birth_ep ≥ min_age_ep)
```

The predicate is ONE lambda with two readers — the candidacy pass and the
continuous branch that follows it — because a screen that drifted from the
decision it previews would elect a station the branch then refuses, which is the
defect with extra steps.  The existing coverage order ranks the usable
candidates and the FIRST is elected; with none usable the election falls through
to today's two sweeps unchanged, counted.  Two deliberate non-entries: a kept
sticky incumbent (no handover), and an incumbent-LESS key — a first sighting or
bridge exit has no outgoing frame, the continuous branch cannot run for it, and
every pin there is a 0-pin whichever station takes it.  The hygiene eligibility
test rides inside the pass, so the elected station is always one today's FIRST
sweep would also have accepted — just, possibly, a later-ranked one.

**Property, asserted in a test and stated in the header:** with the knob ON a
handover goes COLD only if no tracking candidate satisfies the continuous
branch's own predicate — `handover_cold` then counts exactly the unavoidable
ones.

Counters on `NetworkAnchorTallies` (invariant #4): `candidacy_usable_elected`,
`candidacy_rank_changed` (the elected station differs from the one today's sweep
would pick — the number that proves the pass did something), `candidacy_no_usable_fallback`,
`candidacy_skipped_unusable` split four ways (`no_entry` / `var_refused` /
`freed` / `young`); a run-end clause reports all of them and WARNS when the pass
never ran or never changed an election.  Every handover row gains
`cand_skipped=<n>;cand_first_unusable=<station|->` when armed — the PBGAUGE
per-family row and the PHASE_ANCHOR anchor-slip row both — so a cold-handover
census re-runs from the trace alone.  Tests: `tests/test_candidacy_usable.cpp`,
17 cases in two new suites (`CandidacyUsableConfig`, `CandidacyUsableElection`).

### 15.2 Step 1 — the family census, from the seed's own traces (no runs)

`scripts/ev_coldfb_census.py` reproduces the election's coverage rank from
`network_phase_anchor.csv` (the incumbent of every family) and
`network_slip_raw.csv` (age = consecutive 30 s epochs the satellite has been
seen at that station; elevation at the epoch), and lists, per anchored family,
the top-ranked non-incumbent candidate against the best MATURE (≥ 60 ep) HIGH
(> 60°) one below it.  Seed `evseed_quiet6h` ep0665 (tow 279150), 60 stations,
`phase_datum_pref_elev_deg` 15, `phase_datum_min_stations` 2.

153 families are anchored; **102** can be assessed (GPS L5Q and Galileo L7Q are
absent from `network_slip_raw.csv`, which carries only the L1C+L2W and L1C+L5Q
pairs — a signal-coverage limit of that trace, not of the election), and **70 of
the 102 qualify**: the top-ranked non-incumbent candidate is young or low while a
mature high one sits below it.  The four chosen (three by rank, plus the §12.7
family G10/S29000SIM, which qualifies on its own):

| family | incumbent (age/el) | trk | top candidate (rank/age/el) | mature alternative (rank/age/el) |
|---|---|---|---|---|
| E09 L1C+L5Q | S14000SIM 402 ep / 48.5° | 20 | S02000SIM 0 / 35 ep / 15.7° | S04000SIM 11 / 275 ep / 60.1° |
| E10 L1C+L5Q | S09000SIM 665 ep / 12.8° | 19 | S56000SIM 0 / 37 ep / 16.0° | S25000SIM 2 / 654 ep / 63.4° |
| G25 L1C+L2W | S59000SIM 114 ep / 34.3° | 16 | S60000SIM 0 / 38 ep / 14.7° | S56000SIM 1 / 256 ep / 67.0° |
| G10 L1C+L2W (§12.7) | S29000SIM 665 ep / 35.5° | 21 | S06000SIM 0 / 56 ep / 21.7° | S24000SIM 3 / 312 ep / 72.3° |

### 15.3 G1 — OFF byte-identity: PASS, on both halves

Quiet seed `evseed_quiet6h` ep0665 (tow 279150), 60-epoch legs, event at +10.

| comparison | result |
|---|---|
| base exe (unmodified worktree, `90566ebf…`) OFF-null vs ON exe (`c763c168…`) OFF-null | **IDENTICAL** — `products.txt` `f3f37bc3…`, `network_dd_fractional.csv` `852c4dc3…`, `network_events.csv` `690927f6…`, `network_solution.csv`, `network_zwd.csv`, and the run-end counter block character-for-character |
| ON exe, knob OFF vs knob ON **at the default `min_age_ep` 60** (`coldfb_null_off` vs `coldfb_null_on`) | **SOLVE IDENTICAL** — `products.txt`, `network_dd_fractional.csv`, `network_solution.csv`, `network_zwd.csv` byte-for-byte; `network_events.csv` differs ONLY by the two trace fields the armed knob appends to each handover row (273 rows both, `…from=S46000SIM` vs `…from=S46000SIM;cand_skipped=0;cand_first_unusable=-`).  But see §15.4: that identity is the maturity bar being INERT on a warm-resume leg, not the pass agreeing with today's sweep (FIXED 2026-09-12, §15.4.1 — re-measured on the v2 seed the same arm gives 36/36 elected, 11 rank-changed, `handover_cold` 3 → 0) |
| ON exe, knob OFF vs knob ON at `min_age_ep` 0 (`coldfb_null_off` vs `coldfb_null_on0`) | DIFFERS, and attributes exactly: 3 of 24 handovers changed successor (`candidacy_rank_changed` 3), `handover_cold` 3 → **0** |

The third row is the §14.4 precedent, not a failure: a knob that acts must be
graded against a **knob-ON null**, which is why the leg set carries three nulls.

### 15.4 The first finding, and it is about the HARNESS: the maturity bar is structurally inert on a warm-resume leg

`coldfb_null_on` (knob ON, `min_age_ep` at its 60 default) is byte-identical to
the OFF null, and its run-end clause says why:

```
the usable-first candidacy pass ran on 24 handover election(s) — 0 elected a candidate
whose carry is USABLE … and 24 found NO usable candidate and fell through to today's
sweeps.  414 (candidate, family) pair(s) were skipped: 0 no ledger entry, 24 refused by
the variance/ratio ceiling, 0 freed this epoch, 390 younger than 60 epochs.
```

**390 of 414 skips are `young`, and every one of them is an artifact of the
resume.**  `LocalCarry::birth_ep` is deliberately NOT snapshotted (the documented
restored-entry convention: a restored entry reads 0 ⇒ mature).  But the harvest's
own first-stash-wins rule treats `birth_ep == 0` as *not yet stamped* and stamps
it with the CURRENT `n_loop_epochs` — so the first post-resume harvest re-brands
every restored arc as **born at the resume**, and on a leg shorter than the bar
no arc can ever clear it.  The pass degrades gracefully (it falls through to
today's election, which is exactly the honest cold path), but it is measuring
nothing.

Consequences, and they are not only the harness's:

* every harness leg of this card runs the ON arm at `min_age_ep: 0`, which
  exercises the LEDGER half of the predicate — the half a 60-epoch leg can see.
  The legs are named `_on0`; `coldfb_null_on` and `coldfb_e09_on` are kept as the
  documented inert controls (27/27 fall-through, 445 young skips).
* on a production **warm-resume lineage** the bar is inert for its first
  `min_age_ep` epochs after every resume, in the safe direction.  The G3 storm
  arm is a COLD 6 h run, where `birth_ep` is the true first stash and the bar is
  live from epoch 60 on — the regime the default was sized for.
* ~~**owed:** either snapshot `birth_ep` (and `birth_s` with it), or give the
  candidacy bar a resume-aware clock.  Until then any card whose predicate reads
  `birth_ep` must state which side of a resume it was measured on.~~  **FIXED
  2026-09-12 — see §15.4.1.**  A card whose predicate reads `birth_ep` no longer
  has to say which side of a resume it was measured on; it does have to be
  built against an exe carrying the fix (everything before `worktree-birthfix`
  measured the inert bar).

#### 15.4.1 FIXED (2026-09-12) — the RESTORED sentinel, and the bar is live on a resume leg at its DEFAULT

**Mechanism: a sentinel, not a format bump.**  `LocalCarry::kBirthEpRestored`
(−1) / `kBirthSRestored` (−1.0) — the warm resume stamps every restored entry
with them (`makeRestoredLocalCarry`, `NetworkRunContext.cpp:1250` for the `L`
rows and `:1279` for `Z`), and the harvest's first-stash-wins rule now tests
`!= 0` instead of `> 0` (`localCarryStampBirth`, `NetworkRunTypes.hpp:531`,
called at all three stash sites — `NetworkRunContext.cpp:3636` ambiguity,
`:4056` in-block iono, `:4242` in-block rx clock).  `0` still means "never
stashed"; `−1` means "restored, true age not snapshotted ⇒ MATURE"; the two are
now DISTINGUISHABLE, which is the entire defect.  **Nothing is added to the
snapshot record or to the warm-resume fingerprint**, so the fix applies to every
seed already on disk — which is also why the alternative (snapshot `birth_ep` at
a format bump) was NOT taken: it could not have been A/B'd on
`evseed_quiet6h_v2` at all, because that seed was written before it.

**The reader audit came back clean: all three readers already tested `> 0` /
`<= 0`, so none needed a logic change.**  The two `birth_ep` readers — the F3
block admission gate (`ApplicationNetworkAssembly.cpp:3499` / `:3525`) and the
candidacy maturity bar (`NetworkPhaseDatum.cpp:325`) — were merged onto ONE
predicate, `localCarryYoung` (`NetworkRunTypes.hpp:505`), so they cannot drift
apart.  The `birth_s` reader (the sim-only slip oracle's window floor,
`NetworkSlipOracle.cpp:267`) keeps LOSING the window's `max` for the arc's whole
restored life instead of being floored at the resume second from the first
post-resume harvest on — the behaviour its own header always documented.
`relevel_s` was never affected and needs no sentinel: it is stamped only on
CREATE and on FREE (`NetworkYoungInflate::stamp`), never on an ordinary
re-stash, so a restored entry's 0 survives every harvest and reads mature by
`varAdd`'s own `relevel_s > 0` test.  Unit suite:
`tests/test_restored_arc_age.cpp` (`RestoredArcAge`, 10 cases).

**Measured.**  `runs/evseed_quiet6h_v2` ep0665 (fingerprint `c5b5df834d185ac8`,
WARM RESUME confirmed: 2637 ambiguity carries + 60 Z rows + 355 D2 shared
carries restored, `state_snapshot_require` on), 60-epoch NULL legs, `--extra`
arming `phase_datum_candidacy_usable` + `phase_datum_candidacy_min_age_ep`
(QC/election class, NOT fingerprinted — the loader accepted it on the resume
leg, as §18.1 rule (2) allows).  Two exes differing ONLY by this fix: OLD
`1B37E94BB6DA82FE…` (main `6a83728`), FIXED `06580E71AA86A37F…`.

| channel (run-end clause) | knob OFF (either exe) | ON @ 60, OLD exe | ON @ 60, FIXED exe |
|---|---|---|---|
| elections the pass entered | — | 39 | 36 |
| elected a candidate whose carry is USABLE | — | **0** | **36** |
| of which NOT today's pick (`candidacy_rank_changed`) | — | 0 | **11** |
| found NO usable candidate, fell through | — | **39** | **0** |
| (candidate, family) pairs skipped | — | 679 | 10 |
| … no ledger entry | — | 0 | 0 |
| … refused by the variance/ratio ceiling | — | 46 | 3 |
| … freed this epoch | — | 31 | 7 |
| … **younger than 60 epochs** | — | **602** | **0** |
| PB-GAUGE anchor handovers | 39 | 39 | 36 |
| … CONTINUOUS / **COLD** | 36 / **3** | 36 / **3** | 36 / **0** |
| handover rows carrying a screen decision (`cand_skipped > 0`) | — | 0 of 39 | **9 of 36** (`cand_first_unusable` = S31000SIM ×3, S02000SIM ×2, S29000SIM ×2, S56000SIM ×2) |
| consumer 3D, final epoch (held-out SZZ000SIM) | 0.833 m | 0.833 m | 0.760 m |

The OLD-exe ON leg is the §15.3 inert control reproduced on the v2 seed: same
39/36/3 handovers as the OFF null, `cand_skipped=0` on every traced row, and
**602 of 679 skips are `young`** (the SPEC's original 390 of 414 was the same
phenomenon on the pre-promotion `evseed_quiet6h` seed).

**OFF-path byte-identity (invariant #6).**  Same legdir, same `leg.json`, the
two exes: `products.txt` `f6588a96d157dad836214f3eafde379133d4fc2c8dffe3d7fc774364a86952b6`,
`network_dd_fractional.csv` `5842740049dd86f0…`, `network_events.csv`
`0272423d6276ab2f…`, plus `network_solution.csv`, `network_zwd.csv`,
`network_epoch_status.csv`, `network_phase_anchor.csv` — all IDENTICAL, and the
run-end block differs only in four wall-clock lines (RINEX parse seconds, three
`[RT] obs backlog` wall-clock deltas).  Expected: the three readers are all
knob-gated (`phase_leveling_local_block_min_age_ep` 0, `candidacy_usable` off,
`oracle_relevel` off in the golden), so a resume leg with the age knobs unarmed
cannot see the sentinel.

**The bar is now TRANSPARENT, not BLIND — and that is the test.**  On the fixed
exe `min_age_ep: 60` is **byte-identical to `min_age_ep: 0`** (`products.txt`
`903DFAAC0E7A5896…` both, plus all six traces; the skip partition identical at
0/3/7/0) — the restored-entry contract delivered: on a leg whose candidates all
predate the snapshot, the maturity test correctly refuses nobody, so the LEDGER
half decides.  Before the fix those two bars were completely different runs
(§15.3's third row: 3 rank changes, `handover_cold` 3 → 0 only at bar 0).  The
bar's BITE is pinned by `RestoredArcAge.AGenuinelyYoungCandidateIsStillRefused`
and not by this leg: with the fix the sweep finds a usable candidate at rank 0–2
every time, so it never walks down to an in-leg-born arc, and `skip_young` stays
0 for a reason a longer or slipped leg would change.

### 15.5 G2 — the census proxy named the wrong families, and the production path answered anyway

**The four chosen families all hand over CONTINUOUSLY with the knob OFF.**  Each
`anchor_handover` directive at +10 retires the incumbent and lets the production
rules choose; on this bed the top-ranked candidate's carried value was usable
every time:

| leg | injected family | OFF successor (elev) | OFF branch | ON0 successor | ON0 branch |
|---|---|---|---|---|---|
| `coldfb_e09_*` | E09 L1C/L5Q/L7Q | S02000SIM (17.6°) | continuous | S02000SIM | continuous |
| `coldfb_e10_*` | E10 ×3 | S56000SIM (17.7°) | continuous | S56000SIM | continuous |
| `coldfb_g25_*` | G25 ×3 | S60000SIM (15.9°) | continuous | S60000SIM | continuous |
| `coldfb_g10_*` (§12.7 family) | G10 ×3 | S06000SIM (24.1°) | continuous | S06000SIM | continuous |
| `coldfb_g30_*` (replacement, see below) | G30 ×3 | S02000SIM (68.5°) | continuous | S02000SIM | continuous |

So **arc age and elevation at the snapshot epoch do not predict carry
usability** on this bed — the §15 census's proxy is the wrong screen, and the
only oracle is the branch the election actually takes.  (The census remains the
right *shape* test: it found families whose top-ranked candidate is young or low
with a mature high one below, 70 of 102 assessable.)

**The cold population on this bed is NATURAL, not forced.**  Every leg that does
not inject on G30 carries the same event at +22 (tow 280140): the G30 family's
sticky incumbent S25000SIM loses the key and the election hands all three bands
over.  With the knob OFF it lands on the top-ranked candidate **S31000SIM at
12.59°**, whose ledger value is refused by the 0.01 m² ceiling, and all three
families are pinned at 0 — a COLD re-realization.  With the knob ON (`min_age_ep`
0) the pass skips exactly that one candidate and elects **S12000SIM at 39.81°**,
whose value is usable, and all three go CONTINUOUS at targets −13.65 / −11.86 /
+3.97 m.  The trace says so in one row: `cand_skipped=1;cand_first_unusable=S31000SIM`.

**Six independent pairs, one result.**  (`cand_*` from the run-end clause;
consumer = the closed-loop 3D error at +40 and at the leg's last epoch; ZWD =
pooled error against `truth_tropo.csv` over all 60 stations × 62 epochs, n = 3720.)

| pair | cand entered / usable-elected / rank-changed | skips (var / freed) | cold OFF → ON | consumer +40 (OFF → ON) | consumer end | ZWD rms (OFF → ON) | ZWD max abs |
|---|---|---|---|---|---|---|---|
| null | 24 / 24 / 3 | 3 / 0 | **3 → 0** | 1.711 → 1.665 (−46 mm) | 1.546 → 1.490 | 0.07211 → 0.07180 | 0.363 → 0.335 |
| e09 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 2.622 → 2.512 (−110 mm) | 2.538 → 2.413 | 0.07238 → 0.07208 | 0.363 → 0.335 |
| e10 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 1.707 → 1.661 (−46 mm) | 1.539 → 1.484 | 0.07213 → 0.07182 | 0.363 → 0.335 |
| g25 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 1.920 → 1.857 (−62 mm) | 1.711 → 1.633 | 0.07211 → 0.07180 | 0.364 → 0.335 |
| g10 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 1.767 → 1.756 (−10 mm) | 1.621 → 1.610 | 0.07212 → 0.07181 | 0.364 → 0.335 |
| slip (§12.7, 40 cyc L1C at S29000SIM/G10, `anchor_slip_handover` armed both sides) | 39 / 39 / **9** | 3 / **7** | **3 → 0** | 1.113 → 1.241 (**+128 mm**) | 1.061 → 1.172 | 0.07262 → 0.07184 | 0.363 → 0.335 |

T4 attribution rows: **0 in every leg, both arms** (a quiet bed; the injected slip
is taken by the MW drain, never by T4), so the T4 half of the gate is vacuous
here and is reported as such.

Three things in that table are worth naming:

1. **The slip leg is the only one where the `freed` clause fires** — 7 candidates
   refused because a slip response had freed their own arc that epoch — and it is
   the only leg where the pass moves more than the three G30 elections (9 rank
   changes).  Invariant #1 at candidacy time, on the production path, with a
   counter.
2. **The slip leg is also the only channel that went the wrong way**: its
   consumer is 11–13 cm worse with the knob ON while its ZWD is better (rms
   0.07262 → 0.07184) and its cold count still falls 3 → 0.  The consumer replays
   COLD from the leg start and is path-sensitive over 60 epochs (§14.4's standing
   caveat), and the six extra elections this leg moves are its own; it is
   recorded, not explained away.
3. The emitted G30 phase-bias level differs between the arms by 0.87 m (L1C) and
   8.73 m (L2W) from the handover epoch on — that is the *datum*, not an error
   (invariant #5): the cold arm re-realized the family on S31000SIM's ambiguity
   while the ON arm preserved the outgoing frame.  The satellite clock difference
   is 3–6 mm.

**G2 verdict: PASS on the mechanism, with the population replaced.**  In every
pair the cold handovers are exactly the avoidable ones and the pass removes all
of them (3 → 0, six times), the successor moves from a 12.6° unconverged arc to a
39.8° converged one, ZWD is equal-or-better everywhere and the consumer is better
in five pairs of six.  The pre-registered form of G2 ("OFF = cold on each chosen
family") cannot be met on this bed by injection at all, and that is a property of
the bed, not of the card.

### 15.6 G3 — the storm arm: every producer channel the card targets improves, and the USER'S PRODUCT does not

`config/test_suite/_golden_sim_campaign_zwdblk_coldfb.json` = the cbscope5 arm plus
`network.phase_datum_candidacy_usable` and the run path, nothing else (verified by a
flattened key-by-key diff).  Exe `gnss_processor_coldfb_storm.exe` = `c763c168…`, alone
on the box, 720/720 epochs, exit 0, **1226 s**.  `min_age_ep` at its 60 default — this
is a COLD run, so the bar is live from epoch 60 on.  A = scope OFF (`mwevid`), C4 =
cbscope4, **C5 = cbscope5 (the base this card starts from)**, B = this arm.

| channel | A (OFF) | C4 | C5 | **B (coldfb)** | bar | verdict |
|---|---|---|---|---|---|---|
| **PHASE_ANCHOR handovers / cold** | 305 / 68 | 291 / 51 | 315 / 78 | 309 / **46** | cold ≤ 30 | MISS by 16 |
| ...cold-start (first 30 min) / settled | — | — | 27 / 51 | **27 / 19** | (reported apart) | settled −63 % |
| ...cold by hour h0…h5 | 27 11 7 2 8 13 | — | 41 12 7 0 5 13 | **38 8 0 0 0 0** | | **zero from hour 2** |
| **PB-GAUGE elected / continuous / cold re-realizations** | — | 442/369/73 | 462/362/100 | 473/421/**52** | cold ≤ 40 | MISS by 12 (settled 67 → 19) |
| **continuous successor arc age p10 / p50 / p90 (ep)** | — | — | 91 / 240 / 459 | **99 / 231 / 475** | p10 ≥ 60 | **PASS** |
| candidacy pass: entered / usable-elected / **rank-changed** / fell through | — | — | — | 473 / 404 / **53** / 69 | | the pass moved 53 elections |
| candidacy skips: no_entry / var-refused / freed / young | — | — | — | 0 / 1074 / 2 / 161 | | |
| **consumer settled p50 (pooled) / rms** | .1741 / .365 | .1673 / .331 | **.1204 / .2215** | **.1794 / .2823** | ≤ .1204 | **FAIL** (+5.9 cm, worse than A) |
| W1 window-excluded p50 (4.6833–5.40 out) | .1663 | .1584 | .1028 | **.1456** | ≤ .1028 | **FAIL** (better than A) |
| consumer hourly p50 h0…h5 | .447 .247 .229 .174 .086 .280 | .469 .283 .268 .165 .099 .347 | .524 .113 .114 .101 .090 .236 | **.610 .263 .240 .186 .109 .204** | | h1–h3 roughly doubled; h5 best of every arm |
| consumer hourly rms | .62 .29 .23 .18 .47 .39 | .61 .27 .26 .16 .36 .42 | .65 .11 .12 .10 .24 .28 | **.69 .26 .236 .18 .327 .315** | | |
| pivot-recovery window 4.68–5.40 alone / last 36 min | .273 / .403 | .212 / .431 | .221 / .267 | **.314 / .160** | | window worse, tail **best of every arm** |
| **ZWD settled truth rms / span p50** | .0071 / .031 | .0064 / .020 | .0069 / .021 | **.0061 / .0199** | ≤ .0069 | **PASS — best of every arm** |
| **ref-free WL / EWL / L1C** | 98.9/100.0/87.1 | 99.2/99.9/90.1 | 99.2/100.0/89.9 | **99.3/100.0/90.9** | ≥ 99.2 / ≥ 89.9 | **PASS — best of every arm** |
| T4 events / ambient | 78 / .229 | 11 / .196 | 11 / .197 | **11 / .196** | ≤ 11 | PASS |
| W2 4.9917 h: T4 rows / anchor decouples / declared CLK breaks | 0/0/0 | 0/0/0 | 0/0/0 | **0/0/0** | absent | PASS |
| G4 `keep` exonerating an unanswered slip / spurious-implicate | — | 0 / 2 | 0 / 2 | **0 / 2** | 0 / (residual) | PASS / unchanged residual |
| **split-referenced satellites, mean / max of 52.8** | 3.5 / 7 | 7.6 / 13 | **0.0 / 0** | **0.9 / 3** | 0 | **FAIL** |
| anchor stations holding the datum, per epoch (GPS / Gal) | — | — | 17.26 / 11.42 | **17.76 / 11.63** | | +0.5 / +0.2 |
| common-band scope verdicts / narrowed / LLI-epoch (narrowed, co-located) | — | — | 4208 / 3147 / 1205 (1153, 52) | 4208 / 3148 / 1205 (1151, 54) | | unchanged |
| run time | 1130 s | 983 s | 1050 s | **1226 s** | | +17 % |

**The mechanism is proven and the cost has a name.**  The pass did exactly what §15
said it would: 53 of 473 elections went to a station today's sweep would not have
taken, the settled cold handovers fell 51 → 19 and are **zero from hour 2 onward**
(the 27 that remain are the cold-start 30 minutes, where every arc is young by
construction and no candidacy rule can help), the successors it picks are OLDER
(p10 91 → 99 epochs), and every producer-side channel it could plausibly move
improved — ZWD settled rms 0.0069 → **0.0061** and ref-free L1C 89.9 → **90.9**,
both the best of any arm on this bed, with WL, EWL and T4 equal-or-better.

**But the user's product is materially worse**, and worse than the scope-OFF
baseline: pooled settled p50 0.1204 → 0.1794 m (+49 %), hours 1–3 roughly doubled,
and the window-excluded median 0.1028 → 0.1456.  The named cause is visible in the
same run: **split-referenced satellites 0.0 → 0.9 mean / 3 max**, and the datum
spread over +0.5 GPS / +0.2 Galileo anchor stations per epoch.  The screen is
**per-FAMILY** — each (sat, signal) independently elects the best-carried host — so
when a satellite's three bands hand over in one epoch they can land on three
different stations, each correct for its own band and jointly an inter-frequency
inconsistency the per-band producer instruments cannot see and the user pays for.
That is exactly the shape §16.7/§16.8 measured for the LLI-epoch narrowing (producer
channels clean, consumer −3.5 to −7 cm/hour) and exactly the defect the **anchor
clause** cured there by moving a satellite's families together.

### 15.7 Verdict and what is owed

**`network.phase_datum_candidacy_usable` is KEEP_OFF (2026-09-11), merged default-OFF
and byte-identical OFF.**  Three of the pre-registered G3 bars fail — consumer pooled
p50, consumer window-excluded p50, and split-referenced = 0 — and two more miss
because the cold-start 30 minutes are irreducible (cold 46 vs ≤ 30, cold
re-realizations 52 vs ≤ 40, both with their settled halves down 63–72 %).  The rule
itself is not refuted: it is the ONLY arm on this bed with zero settled cold
handovers from hour 2, the best ZWD and the best L1C, and the invariant-#1 `freed`
clause fires on the production path (2 refusals on the storm arm, 7 on the harness
slip leg).  What is refuted is the **per-family scope**.

Pre-registered next iteration, before it exists: **the SATELLITE clause.**  When a
satellite's families are handed over in the same epoch, elect ONE successor for all
of them — the highest coverage-ranked station that is usable for the most of the
satellite's bands (ties to the coverage rank), with the per-family fall-through kept
for a family that hands over alone.  Gates: split-referenced back to 0.0 / 0, the
settled cold count no worse than this arm's 19, and consumer pooled p50 ≤ 0.1204
(cbscope5) — i.e. the producer wins of this arm kept while the inter-frequency
inconsistency that bought them is removed.  The §15.4 `birth_ep` resume defect is a
separate, smaller card and is independent of this one.

⚠ **§15.8 reports that iteration: it met its own primary gate (splits 0.0 / 0) and FALSIFIED this paragraph's mechanism — the consumer got 1.5 cm WORSE with every split removed.  Read §15.8 before quoting the split hypothesis from here.**

**Lessons for the memory file, both paid for here:**

1. **A candidacy screen that acts per-BAND on a multi-band satellite buys producer
   quality with a SPLIT REFERENCE, and the producer's own per-band instruments say
   only the good half.**  ZWD, ref-free integer rates and the cold-handover counters
   all improved while the user's median went up 49 %.  Any election-side rule must
   report the split-referenced census next to its own counters.
2. **Arc age and elevation do not predict carry usability.**  Five injected
   anchor-handover families chosen on that proxy all handed over CONTINUOUSLY with
   the knob off; the only cold population on the quiet bed was one unforced handover
   nobody had selected.  For a card about the ledger, screen on the LEDGER — or on
   the branch the election actually takes.

