# N3b: the readers on the roster, and the decision list

Card NOM step N3b (`docs/refound/NOM2_network_object_model_rows_2026-10-09.md` section 4; the
sites are card NOM section 18, groups A to G). Branch `nom/n3b` from `nom/wave2`. No decision is
taken by the build: a site whose counter is 0 on every arm reads the roster or the arc ledger,
and every other site keeps its rule and is listed below with its counts.

## 1. Measurement

**Census.** `app/SignalRosterSites.cpp` adds nine counters (`RosterN3bSite`) beside the eleven of
N3a. Six are RULE sites, evaluated at each audited epoch on that epoch's roster through the site's
own function where one exists (`networkPinBandsFor`, `broadcastGroupDelayDescribesPair`,
`channelFamilyOf`, `netDatumGraphFamily`), so a count is the rule's disagreement with the roster
whether or not the posture reaches the caller. Three are READ sites, counted at the site on every
read against the answer the old rule gives there. The census prints at run end in its own lines
(`Network (roster site census, N3b): ...`, trace class) and writes no CSV, product or manifest
record.

**Arms** (`D:/GnssProcessorSupport/gates/n3b/make_arms.sh`, runs `runs_nom/_rap_n3b_{base,new}_*`):
`f2` field golden (GPS+Galileo) 2 h, `s2` sim golden 2 h, `tri` NOM2's three-band GPS arm 1 h,
`gps` the retired GPS-only golden 1 h, and five family smokes of 0.25 h on the field golden:
`all` (`channel_mode all`), `sph` (`second_phase_channel`), `cnt` (`canonical_signal_rule count`),
`rdr` (`roster_tracker` + `ledger_reentry`), `lex` (`tracker_modeled_first` false), with `g25`
(the golden itself, 0.25 h) as their reference.

**Gate.** `gnss_processor_n3b_base.exe` (nom/wave2, SHA256 `8b579bbc...`) against
`gnss_processor_n3b_new1.exe` (this branch, `a29cae33...`), `nom_td_gate.py --whole` with
`NOM_GATE_DROP_POSTURE=1` and `NOM_GATE_DROP_RE` = `gates/n3b/drop_re.txt` (the census lines only):
IDENTICAL on all ten arms (products, every CSV in row order, the manifest, every other log line).

## 2. Switched (counter 0 on every arm)

| site | reads now | compared on f2 / s2 / tri / gps / all / sph | disagree |
| --- | --- | --- | --- |
| E10 (E9's latched pick): the tracker's band signal, `ApplicationNetworkAssembly.cpp` latch lookup | the roster's Modelled phase (`signalRosterModelledOr`); the latch only where the roster has no role yet (fallback 1 640 reads on f2, the first epoch of each constellation) | 394 813 / 423 374 / 115 114 / 120 468 / 49 482 / 49 467 | 0 |
| C2, E7: the second-phase census's band phase (same file) | the same read | 0 / 0 / 0 / 0 / 49 482 / 24 891 | 0 |
| Arc identity of the wind-up restart (`networkWindupRestartTargets`, carry branch) | the arc ledger: admitted, not-dead arcs of the (station, satellite) with a carry entry inside the horizon | 21 / 14 / 2 / 0 / 1 / 0 (and `cnt` 1) | 0 |

The gap re-entry response keys a band's arc by the tracker's signal, which is now the roster's
Modelled role (the ledger records the same role per arc); its population stays the raw tracker's.
Card GRK's local rule (the latched phase when reported with its code, else the lexical phase of the
band) is unchanged as the fallback. The ledger-driven population (`network.ledger_reentry`) on the
`rdr` smoke: `products.txt` and every CSV but `network_events.csv` identical to `g25`; the event
text differs (`src=ledger`, other fields), so it stays a reader knob (decision D7).

## 3. Decisions for the author (rule kept; counts over the run)

| # | decision | site(s) | counts (disagree / compared) |
| --- | --- | --- | --- |
| D1 | **The pin pair.** BIA clock-reference pair or the lexically first pair on distinct frequencies; they agree on bands, disagree on signals | A1, A3, A5 (`bia_pin_signals`; N3a `if_pin_pair`) | f2 240/960, s2 241/964, tri 121/242, gps 121/242 (all GPS band 1: BIA C1W, roster C1C); `if_pin_pair` f2 18 639/55 444, tri 2 159/13 918, gps 3 599/14 638, s2 0/57 600 |
| D2 | **Channel family.** "X against the rest" (F1, `channelFamilyOf`) or the attribute letter (F2, `netDatumGraphFamily`) | F1, F2, A4 (the secondary family's pin pair follows F1) | f2 1 434/4 302, s2 960/960, tri 1 200/2 400, gps 1 200/3 600, all 840/2 040 (GPS C1C~C1W, C1C~C2W, L1C~L2W; Galileo C1C~C5Q, L1C~L5Q: one family by F1, two by F2) |
| D3 | **The canonical pair is digits 1 and 2**, so Galileo (1, 5) is never screened | A10 (`latchedPairs`) | f2 13 861/27 722, s2 14 400/28 800 (every Galileo cell); tri 0/6 959, gps 0/7 319 |
| D4 | **Which signal stands for a band where several rules choose** (smallest folded variance, smallest stashed variance, the latch, lexical order) | E1 `NetworkClockAnchor` and E2 `pbHoldClonePickPair` (uncounted: per-satellite in-flight variances, no roster role per satellite; E2 unreached, clone arms only); E9 the tracker when the Modelled phase is not reported with its code (N3a `slip_tracker_band`) | `slip_tracker_band` f2 84/394 813, tri 72/115 114, g25 14/49 467, s2 0/423 374; gps 34 124/120 468 (lexical tracker) |
| D5 | **The latch rule** (priority against count) | N3a `latch_rule` | stations f2 22/58, tri 22/58, gps 24/61, s2 0/60 |
| D6 | **The wire's families per modelled signal** (card GAL2's open question) | N3a `wire_phase_bias`, `wire_code_bias` | f2 38 031 and 36 210 of 80 412; s2 13 390 of 40 170 (both); tri 4 605 and 3 726 of 19 100; gps 6 819 and 7 416 of 22 920 |
| D7 | **Arc lifecycle owners still disagreeing** | N3a `anchor_election` (a return read as a first sighting), `coverage_gate` (drop while an arc is in a gap), `gap_reentry`, `ambiguity_states`; `ledger_reentry` as the default | anchor f2 7/194, tri 6/128, g25 1/176; coverage f2 11/11, tri 7/7; gap_reentry tri 2/42 (0 on f2, s2, gps); ambiguity_states gps 8/150 111 |
| D8 | **GPS as the inter-system-bias and time reference** | review additions (`StateInitializer` ISB seed, `NetworkMultiGnss`, `ApplicationNetworkSetup`) | no roster role: not measurable |
| D9 | **BeiDou digit 1** (B1C in `NetworkChainDatum` rounding) | G2 | unreached: no BeiDou arm |

## 4. Counter 0, rule kept (the switch is a change of owner, not a read)

- `bia_pin_bands` (A5 to A9, A12, B3, B5, B7): 0 on every arm (f2 480, s2 482, tri 121, gps 121
  constellation-epochs). The readers run in PASS A and in `StateInitializer`, upstream of the epoch's
  IF pin that writes the roster's datum role, and the answer comes from the BIA product before the
  first epoch. Reading the roster would make the previous epoch's realized pin the owner of the pin
  bands: part of D1.
- `gd_pair_bands` (A2): 0 (f2 13 458 pinned satellite-epochs). The group-delay pair is a property
  of the broadcast message, not a roster role; nothing to decide.
- `traits_pair_bands` (A13): 0 (f2 480). The site lies in `NetworkLocalCarryHarvest.cpp` (worker
  HV1): listed and left.

## 5. Not measurable against the roster (the roster holds no role for the question)

- A11 (tier-2 bias admission: two distinct bands among a satellite's candidates), B1 (extras on
  the reference band stay free), B2 and B6 (the band-2 companion and its wire verdict), B4 (ISC
  band-mate roster), B8 and B9 (the all-channel census's grouping), E14 (the digit as a withholding
  key), G3 (which Galileo health bits count), G4 (ionosphere referenced to the GPS L1 carrier),
  G5 (17 GLONASS-exclusion tests in the network files), `discThresholdM` in `NetworkProductEmitter`.
- D1 (`sdEwlBands`, SD-EWL AR): a band-pair role; a roster role per band pair is a decision of
  its own (no arm arms SD-EWL).
- C4 and A8, A9, E12 in `StateInitializer`: the roster is not reachable there (no run context);
  C4 builds the companion by the rule the roster copies.
- E8 (`resolve`, `cap_of`): moved into the harvest (worker HV1): listed and left.

## 6. Retired since section 18 (code deleted, card RKD and the lane layer)

C5 (`NetworkMwWlGlue`), D2 to D10 (`isUncarriedThirdBand`, the UWL tiers and the lane pair census
of `NetworkDdPriorBuild`, now 231 lines with no band logic, the run-end E5/E5b label), E3 and E4
(the preferred-channel census), E5 (`NetworkFastClock`), G4's `NetworkIonoDwRelease` copy. E16
(the tracker cell of the first constellation with the band) is fixed: the census looks up the
row's own constellation.

## 7. Deviations

- Measurement and switch in one build: a read site's counter counts exactly the reads at which the
  switched site answers differently, so 0 means the old answers; the gate proves it on every arm.
- Rule sites are measured at the audit, not at their callers (most callers are pure functions of
  the constellation or run before the roster's datum exists).
- Cost not measured on an idle machine (the arms shared it with builds and other workers); the
  census adds a walk of the epoch's roster cells and pins to the audit pass and one map lookup per
  tracker read.
- Reduced G-R verifier (`scripts/nom_n3b_verify.py`): no body moves; it proves every frozen line of
  the seven touched files lands once and in order, with a closed list of ten changed lines that only
  gain tokens (one declared substitution, the run-end table's count 105 to 106); brackets; no added
  log call or event write; the two grown signatures keep their parameters. Six negative controls.
- The N3a table's last line ("the other sites ... have no counter yet; step N3b") is left as
  printed (a printed-text move left for the card's landing); the N3b census prints after the
  roster's run end.
