# SPEC — `cbpin=` S-contract fill: declare the product's internally-consistent signal pair

**Lineage:** `docs/REPORT_day129_frame_guards_phase01.md` §1.5 →
`docs/HANDOFF_night305_day129.md` §7 → this.  Written 2026-07-27 alongside the
P-header schema fix.  Self-contained.

**Status: format layer LANDED + tested; producer FILL specified here, not
built** (the fill sites — `NetworkSolPrep.cpp`, `NetworkRunContext.hpp` — were
under a concurrent session's edit lock when this was written; the remaining
work is ~15 lines against a frozen grammar).

---

## 1. Why (measured, not hypothetical)

Phase-0 replays of the DOY-129 reference stream (frame-guards report §1.2–§1.5)
graded three consumer signal-pair choices against the same product file:

| consumer IF pair | frame | hours 15–17 median 3D |
|---|---|---|
| (C1C,C2L) | **datum-pinned** (`selectIfPinPair`) | 7.0 – 15.0 m |
| (C1W,C2W) | RTKLIB convention | 41.5 – 46.4 m |
| **(C1C,C2W)** | **MIXED — one signal in each frame** | **63.4 → 113.7 m, monotone growth** |

The producer pins IF(C1C,C2L) per satellite plus per-(constellation,signal)
sum-to-zero; combinations outside the pinned pair ride data-determined
directions and carry the full frame wander.  A mixed pair is worst of all
(§1.5 mechanism [H]: the consumer's receiver code-bias state must absorb a
between-channel differential while the satellite OSB itself moves).  Nothing
in the emitted stream tells a consumer which pair is the consistent one — the
pin is chosen by a lexical rule the consumer cannot see.  **The product must
declare it; a consumer must form its IF within one frame.**

This card DECLARES the realized pair.  It deliberately does NOT re-point the
pin at the convention pair — that is the investigation report §7 "D3" datum
re-reference (fingerprint-breaking, fresh convergence), a separate decision.

## 2. Format layer — LANDED (2026-07-27, with the P-header schema fix)

- `SbasisContract::cb_pin_pair` — `std::map<std::string, std::string>`,
  constellation letter → `"<s1>-<s2>"` (`include/products/NetworkProductIO.hpp`).
- S-line grammar: repeated trailing `cbpin=<const>:<s1>-<s2>` key=value
  fields, written ONLY when non-empty → an undeclaring producer writes the S
  line byte-for-byte (`NetworkProductWriter::writeSLine_`).  Parse dispatches
  BEFORE the `':'` realized-pivot fallback (the value contains a colon).
- `checkContract` compares per-constellation, only for constellations the
  consumer declares (the `clock_pivot_sat` semantics): file-extra entries are
  never a mismatch; a declared-pair mismatch reads
  `cb_pin_pair[G]: file 'C1C-C2L' vs expected 'C1W-C2W'`.
- `makeReplayEmitter` ignores it (interpretation hint, not a replay-emitter
  construction parameter — the `agek=` class, not the `discfrac=` class).
- Header legend declares the token (grammar-complete legend, same fix).
- Tests: `CbPinPairTravelsInContractAndChecks` (round-trip incl. RTA-5
  re-broadcast, check semantics, byte-identity when absent) and the
  `HeaderLegendDeclaresEveryEmittedField` sweep, `test_network_product_io.cpp`.

## 3. Producer fill — TO BUILD (~15 lines, 3 sites)

The realized pair is per-SATELLITE and epoch-dependent; the contract declares
a per-CONSTELLATION pair.  Reduction rule: **modal pair over the emitted
satellites of the declaring epoch**, refreshed at each emit exactly like the
realized clock pivots (`pending_contract.decl.clock_pivot_sat`).

1. **Record** (`NetworkSbasis.cpp`, `buildCodeBiasDatumRows`, beside the
   `selectIfPinPair` call at ~`:354` where `sig1/sig2` are in hand): write the
   effective pair — POST-hold, i.e. after the `clock_signal_basis_hold`
   held/latch block resolves `sig1/sig2` — into a new carry map
   `ctx.carry.this_epoch_if_pin_real` (sat → {s1,s2}).  Unconditional
   (unlike `this_epoch_if_pin_sel`, which is gated on
   `!hold && emit_clock_disc` and serves the disc-signalling path); it is
   read only by the contract fill, so the wire and solution stay
   byte-identical.  Clear it with the other per-epoch scratch.
2. **Reduce + declare** (`NetworkSolPrep.cpp`, the realized-pivot merge at
   ~`:813`): per constellation, count `this_epoch_if_pin_real` pairs over the
   satellites actually in this emit; set
   `pending_contract.cb_pin_pair[cst] = modal "<s1>-<s2>"`.  Deterministic
   tie-break: lexically smallest pair string.  Skip constellations with no
   dual-frequency pin this epoch (single-frequency sats pin nothing).
3. **Nothing else.**  The v2 header construction (`NetworkSolPrep.cpp:823`)
   and the RTA-5 re-broadcast (`NetworkEmitStore.cpp:321`) both serialize
   `pending_contract` — they pick the field up with zero change.  A mid-run
   modal change (channel-set shift) reaches consumers at the next re-broadcast,
   the same latency realized-pivot changes have today.

**Fingerprint:** none.  `cb_pin_pair` is S-line CONTENT (a declaration of what
the run already does), not a datum rule — same class as the realized pivots,
which are not fingerprinted.  Warm resumes are unaffected.

## 4. Declared semantics (write into the .hpp doc when the fill lands)

- The declared pair is the MODAL realized pin pair of the emitted satellites;
  individual satellites lacking a declared signal pin the lexically-first
  available pair by the same `selectIfPinPair` rule, and their consistency
  frame differs from the declaration.  (On DOY-129 with full signal sets the
  GPS pair is uniformly (C1C,C2L); heterogeneity is the degenerate case, not
  the norm.)
- Under `clock_signal_basis_hold` the per-sat pairs can drift apart after
  re-latches; the modal-of-emitted rule still applies and the declaration
  refreshes per emit.
- Consumer guidance (the actual §1.5 consequence): form IF within ONE frame —
  either the declared pair or a pair wholly outside it (the convention pair's
  ~3× penalty vs the pinned pair is the producer's frame wander, unavoidable
  without D3); NEVER one-in-one-out.  A consumer can enforce this by declaring
  its own pair as the `checkContract` expectation and treating a
  `cb_pin_pair` mismatch as a warning that it is off-frame.

## 5. Follow-ups, explicitly out of scope here

- Consumer-side expectation knob (`network.closed_loop` /
  `realtime.contract_expect_*`) so `checkConsumerContract`
  (`ApplicationRealtime.cpp:483-502`) and the replay driver can assert the
  pair from config.  Blocked on the same session lock
  (`GnssConfigLoadNetwork.cpp`).
- Auto-derivation of the consumer's OWN realized pair (from
  `decideConstellationCanonicals`) for the comparison — closes the loop
  without any user-declared expectation.
- D3: re-pointing `selectIfPinPair` at the consumer-convention pair
  (fingerprint-breaking datum re-reference; deferred by decision in the
  investigation report §7).
