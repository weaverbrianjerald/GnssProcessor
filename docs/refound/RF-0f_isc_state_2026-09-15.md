# RF-0f — C2X in the solve, its ISC a per-satellite state, out of the pin (2026-09-15)

**STATUS:** closed — **the mechanism is right and the network cannot use it: the C2W and C2X station sets are DISJOINT (25 vs 2 stations, no
receiver tracks both), so the ISC is unobservable for every satellite, C2X leaves the wire, and the solve is e2 to the digit.  The roster's
verdict: C2X cannot be emitted on this network; and the two C2X-only stations, not the channel, are what e3 showed — RF-0g below.**  Worker
branch `worktree-agent-a0aafda782ef5e7bd` (`cb04921`), merged.  Suite 2194 / 290 (0 failed); `--verify` PASS; exe `0DC43C60…`.  The OFF smoke
header hash moves again to **`9add1d3a…`** (the new B-record token `isc=obs` in the legend, 12 bytes; the record stream `333dcb08…` byte-identical
— the canonical OFF hash from this merge on).

**The build (`network.sat_isc_state`, default OFF, byte-identical):** no new state was needed — `b(C2X)` is already a shared per-(satellite,
signal) column (`ApplicationNetworkAssembly.cpp:2126 / :2150`, `est.registerCodeBias`), so `ISC^s ≡ b(C2X) − b(C2W)` and the whole change is NOT
folding the RF-0d row that asserted it zero where some present station carries BOTH signals of that satellite in its own bias-state map (each
station's receiver bias absorbs its half — the twin of RF-0d's band-1 companion rule); where no station bridges, the column keeps the pin at 0,
is counted, and its bias is ERASED from the record in `NetworkSolPrep` (a pinned ISC is an assertion no value distinguishes from an estimate);
the emitted ones carry `isc=obs` on the B record (a new token; the reader's `'='` guard also protects the positional disc-counter parse).  With
the companion unpinned its channel is no longer fully pinned, so RF-0d's level row correctly re-enables.  `refound_grade.py` gains
`if_dispersion` (reproduces the RF-0e read to the digit).  e2 armed, e1 not (RF-0e's field data favours e2).

## The arms (6 h, `broadcast_tgd`, GIM off, prior off, e2, the cut OFF)

| arm | emitted / withheld | ISC free / pinned |
|---|---|---|
| sim | 720 / 0, declared = observed | — (no X channels on that bed) |
| field | 715 / 5 | **0 free / 1 923 pinned — 100 %** |

**The field consumer is identical to e2 in every hour to the digit** (21.75 / 18.17 / 9.40 / 5.43 / 4.74 / 3.94): the mechanism fired and the roster
refused it — the only change is on the wire, where **0 C2X B records are emitted** (the withholding half working as designed).  Bars: both
C2X-only stations present all day PASS (INEG 715 / 715, OAFA 526 / 715 — it sets); IF-correction dispersion p90 **1.02 m** (bar 0.55; RF-0d 1.53,
RF-0c 0.55, RF-0 0.30) FAIL; the consumer bars FAIL; sim PASS.  M4 holds.

**The roster read (the third frozen reading, and it fires):** 27 stations carry any band-2 code — 25 track C2W, **2 track C2X (INEG00MEX,
OAFA00ARG), 0 track both** on any satellite at any epoch; C2X is seen on 5 satellites (G18, G24, G28, G29, G32) at 1–2 stations each.  The
inter-signal correction C2W → C2X is unobservable through every receiver in this network, so every value the producer could publish for C2X
is an assertion.  And the worst IF-dispersion satellites, **G12 and G08, carry no C2X at all** — the C2X assertion was never the whole term.

## Grade and reading

RF-0f is not the baseline; the mechanism stays built (default OFF) for a network that has a bridging receiver.  What the three cards RF-0d /
RF-0e / RF-0f now say together: (1) the C2X columns pinned at ISC = 0 enter the solve through exactly two stations, whose second frequency is
then wrong — and those two stations' clocks / ionos / ZWDs contaminate the shared clocks of every satellite they see (G12 and G08 included):
the "wire inherits the station split" mechanism of §24.9, from two stations with a mis-pinned band 2; (2) e3 removed the channel from the
solve and got 0.82 m at hour 2 — and starved the same two stations, whose withholds then collapsed it.  **The obvious arm has not been run:
e3 with those two stations OUT of the roster.**

## RF-0g (pre-registered): the coverage cut done right — C2X off the canonical set AND the two C2X-only stations off the roster

Config-only: e3's config (e2 + the L2X / C2X band-2 cut) with `network.stations` minus INEG00MEX and OAFA00ARG (59 stations; the WTZZ consumer
unaffected; the roster read recorded: which satellites lose a station), both beds (the sim unchanged), 6 h cold.  **Bars:** field ≥ 715 / 720
with 0 withholds after hour 1; the consumer per-hour p50 ≤ e3's in hours 1–3 (1.76 / 0.82 / 1.55) and ≤ 1.5 m in hours 4–6 (no collapse);
the IF-correction dispersion p90 ≤ 0.55 m; D_phase ≤ RF-0d's; sim 720 / 720.  **Frozen reading:** all ⇒ **RF-0g IS the baseline** — self-
contained, the GIM gone, 59 stations, C2X un-emitted; the RF-1 + RF-2b winner is re-run on it and the ledger rows are written on the author's
go; the consumer holds e3's hours 1–3 but drifts after ⇒ the drift's channel is read on a posture with no known gross term left (the clock
level, the tie-less iono, the anchor) — RF-2b's R-CL read applies; the consumer does not hold e3's hours 1–3 ⇒ e3's numbers came from the two
stations' absence in a way the roster cut does not reproduce — read the difference (which satellites' IF correction changed).
