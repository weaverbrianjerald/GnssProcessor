# SPEC — `cbpin=` S-contract fill: declare the product's internally-consistent signal pair

**Lineage:** `REPORT_day129_frame_guards_phase01.md` §1.5 → `HANDOFF_night305_day129.md` §7 → this. *(Neither
predecessor exists any more — both were deleted before the 2026-08-15 consolidation and were never atticed;
`docs/SPEC_day129_frame_guards.md` is their surviving record — see its header and Part F item 6.)* Written
2026-07-27 alongside the P-header schema fix. Self-contained. **Status: LANDED and in the wire.** Format layer
2026-07-27; producer fill 2026-08-12; **fill SOURCE corrected and `gfpin=` added 2026-08-13** (§6 — the
2026-08-12 fill was structurally inert on every FIELD posture, which is how the gap resurfaced). **Read §6
before treating §3 as the built design**: two deliberate divergences are recorded there.

---

## 1. Why (measured, not hypothetical)

Phase-0 replays of the DOY-129 reference stream (frame-guards report §1.2–§1.5) graded three consumer
signal-pair choices against the same product file:

| consumer IF pair | frame | hours 15–17 median 3D |
|---|---|---|
| (C1C,C2L) | **datum-pinned** (`selectIfPinPair`) | 7.0 – 15.0 m |
| (C1W,C2W) | RTKLIB convention | 41.5 – 46.4 m |
| **(C1C,C2W)** | **MIXED — one signal in each frame** | **63.4 → 113.7 m, monotone growth** |

The producer pins IF(C1C,C2L) per satellite plus per-(constellation,signal) sum-to-zero; combinations outside
the pinned pair ride data-determined directions and carry the full frame wander. A mixed pair is worst of all
(§1.5 mechanism [H]: the consumer's receiver code-bias state must absorb a between-channel differential while
the satellite OSB itself moves). Nothing in the emitted stream tells a consumer which pair is the consistent
one — the pin is chosen by a lexical rule the consumer cannot see. **The product must declare it; a consumer
must form its IF within one frame.**

This card DECLARES the realized pair. It deliberately does NOT re-point the pin at the convention pair — that
is the investigation report §7 "D3" datum re-reference (fingerprint-breaking, fresh convergence), a separate
decision.

## 2. Format layer — LANDED (2026-07-27, with the P-header schema fix)

- `SbasisContract::cb_pin_pair` — `std::map<std::string, std::string>`, constellation letter → `"<s1>-<s2>"`
  (`include/products/NetworkProductIO.hpp`).
- S-line grammar: repeated trailing `cbpin=<const>:<s1>-<s2>` key=value fields, written ONLY when non-empty →
  an undeclaring producer writes the S line byte-for-byte (`NetworkProductWriter::writeSLine_`). Parse
  dispatches BEFORE the `':'` realized-pivot fallback (the value contains a colon).
- `checkContract` compares per-constellation, only for constellations the consumer declares (the
  `clock_pivot_sat` semantics): file-extra entries are never a mismatch; a declared-pair mismatch reads
  `cb_pin_pair[G]: file 'C1C-C2L' vs expected 'C1W-C2W'`.
- `makeReplayEmitter` ignores it — interpretation hint, not a replay-emitter construction parameter (the
  `agek=` class, not the `discfrac=` class).
- Header legend declares the token (grammar-complete legend, same fix).
- Tests: `CbPinPairTravelsInContractAndChecks` (round-trip incl. RTA-5 re-broadcast, check semantics,
  byte-identity when absent) and the `HeaderLegendDeclaresEveryEmittedField` sweep,
  `test_network_product_io.cpp`.

## 3. Producer fill — TO BUILD (~15 lines, 3 sites)

The realized pair is per-SATELLITE and epoch-dependent; the contract declares a per-CONSTELLATION pair.
Reduction rule: **modal pair over the emitted satellites of the declaring epoch**, refreshed at each emit
exactly like the realized clock pivots (`pending_contract.decl.clock_pivot_sat`).

1. **Record** (`NetworkSbasis.cpp`, `buildCodeBiasDatumRows`, beside the `selectIfPinPair` call at ~`:354`
   where `sig1/sig2` are in hand): write the effective pair — POST-hold, i.e. after the
   `clock_signal_basis_hold` held/latch block resolves `sig1/sig2` — into a new carry map
   `ctx.carry.this_epoch_if_pin_real` (sat → {s1,s2}). Unconditional, unlike `this_epoch_if_pin_sel` which is
   gated on `!hold && emit_clock_disc` and serves the disc-signalling path; read only by the contract fill, so
   wire and solution stay byte-identical. Clear it with the other per-epoch scratch.
2. **Reduce + declare** (`NetworkSolPrep.cpp`, the realized-pivot merge at ~`:813`): per constellation, count
   `this_epoch_if_pin_real` pairs over the satellites actually in this emit; set
   `pending_contract.cb_pin_pair[cst] = modal "<s1>-<s2>"`. Deterministic tie-break: lexically smallest pair
   string. Skip constellations with no dual-frequency pin this epoch.
3. **Nothing else.** The v2 header construction (`NetworkSolPrep.cpp:823`) and the RTA-5 re-broadcast
   (`NetworkEmitStore.cpp:321`) both serialize `pending_contract` and pick the field up with zero change. A
   mid-run modal change (channel-set shift) reaches consumers at the next re-broadcast, the same latency
   realized-pivot changes have today.

**Fingerprint:** none. `cb_pin_pair` is S-line CONTENT (a declaration of what the run already does), not a
datum rule — same class as the realized pivots, which are not fingerprinted. Warm resumes are unaffected.

## 4. Declared semantics (write into the .hpp doc when the fill lands)

- The declared pair is the MODAL realized pin pair of the emitted satellites; individual satellites lacking a
  declared signal pin the lexically-first available pair by the same `selectIfPinPair` rule, and their
  consistency frame differs from the declaration. (On DOY-129 with full signal sets the GPS pair is uniformly
  (C1C,C2L); heterogeneity is the degenerate case, not the norm.)
- Under `clock_signal_basis_hold` the per-sat pairs can drift apart after re-latches; the modal-of-emitted
  rule still applies and the declaration refreshes per emit.
- Consumer guidance (the actual §1.5 consequence): form IF within ONE frame — either the declared pair or a
  pair wholly outside it (the convention pair's ~3× penalty vs the pinned pair is the producer's frame wander,
  unavoidable without D3); NEVER one-in-one-out. A consumer enforces this by declaring its own pair as the
  `checkContract` expectation and treating a `cb_pin_pair` mismatch as a warning that it is off-frame.

## 5. Follow-ups, explicitly out of scope here

- Consumer-side expectation knob (`network.closed_loop` / `realtime.contract_expect_*`) so
  `checkConsumerContract` (`ApplicationRealtime.cpp:483-502`) and the replay driver can assert the pair from
  config. Blocked on the same session lock (`GnssConfigLoadNetwork.cpp`).
- Auto-derivation of the consumer's OWN realized pair (from `decideConstellationCanonicals`) for the
  comparison — closes the loop without any user-declared expectation.
- D3: re-pointing `selectIfPinPair` at the consumer-convention pair (fingerprint-breaking datum re-reference;
  deferred by decision in the investigation report §7).

## 6. What actually landed, and where it diverges from §3–§4 (2026-08-13)

### 6.1 The fill was built on the wrong source, and was inert on every FIELD run

§3 step 1 called for a NEW carry map `this_epoch_if_pin_real`, explicitly "unconditional (unlike
`this_epoch_if_pin_sel`, which is gated on `!hold && emit_clock_disc`)". The fill that landed 2026-08-12 read
`this_epoch_if_pin_sel` instead and inherited that gate.

`network.emit_clock_disc` defaults **false** (`GnssConfigLoadNetwork.cpp:4324`) and appears **nowhere** in the
eight-config `extends` closure of the posture-C field arms (`_pc305_ggq_tri_slant_20m` → … →
`_ft63_24h_conv`), while every sim posture-C config arms it. Result: sim products declared `cbpin=` correctly
and **every field product declared nothing** — silently, because the "no cbpin= is declared" info line lives
INSIDE the per-constellation loop, which an empty source map never enters. The producer's own logs are the
record: `runs/simnet300_tri_slant/console.log` carries the declaring line; the field console logs carry
neither that line nor the split-diagnosis line. This blocked truth-grading of field products —
`simnet_grade.py` refuses to apply the §4.4 S-transform without a declared pair rather than assume one.

**The defect class is worth naming: a product-contract DECLARATION accidentally gated on a QC-channel knob.**

Fixed by doing what §3 said: a dedicated `this_epoch_if_pin_real` (`NetworkRunContext.hpp`), written in
`buildCodeBiasDatumRows` under the sole guard `f1 > 0 && f2 > 0`, on BOTH the held and the re-selected path —
the hold branch previously recorded no selection at all, so merely ungating the disc scratch would still have
declared nothing under `clock_signal_basis_hold`. `this_epoch_if_pin_sel` keeps its gate and its meaning (it
drives per-satellite discontinuity bumps; widening it would change wire bytes). Guarded by
`NetworkSbasisTest.BuildCodeBiasDatumRowsDeclarationRecordIsUngated`.

### 6.2 UNANIMITY, not the modal rule of §3 step 2

The implementation declares **only on unanimity** and otherwise declares nothing, logging why. **Read this as
superseding §3/§4**: a modal declaration publishes a pair that is FALSE for the minority satellites and a
consumer has no way to learn which ones — the mixed-frame damage the token exists to prevent (§1's 63.4 →
113.7 m regime). Silence is recoverable; a confidently wrong declaration is not. On the field roster the point
is currently moot — all 32 GPS satellites resolve to (C1C,C2W) and all 21 Galileo to (C1C,C5Q), perfectly
unanimous — but it matters on any heterogeneous constellation, which is exactly the Galileo Q/X family case
(`docs/SPEC_family_gf_datum.md`).

### 6.3 Declared at EVERY emit, not stamped once

§3 step 3 assumed the RTA-5 re-broadcast would pick up a mid-run change. It does not:
`NetworkEmitStore.cpp:404–410` refreshes only `clock_pivot_sat` and `eq34_datum_receiver`. A once-stamped
declaration would go stale on the wire after a channel-set shift moved the pin — the same silent mis-framing
the token exists to prevent. The declaration is now rebuilt each emit and assigned wholesale (so a
constellation that stops qualifying DROPS its token at the next broadcast), logged only on change.

### 6.4 `gfpin=` — the companion token for the slant posture

Added the same day, same grammar, same unanimity rule: `gfpin=<const>:<s1>-<s2>`, written only when
`network.code_bias_gf_datum` folded a geometry-free row for every dual-frequency emitted satellite of that
constellation. Rationale and semantics (including precisely what its ABSENCE does and does not prove) are in
`docs/SPEC_sbasis_realization_contract.md` §A row 2b/3b, §B row 4 and §D; the per-family value form is
`docs/SPEC_family_gf_datum.md` §D.3, which today's parser round-trips and today's producer never writes.

### 6.5 Still open

- `_pc305_tri_slant_10ep.json` does not set `network.serialize_sbasis_contract` anywhere in its chain, so
  that smoke arm writes a v1 header with no S line at all and remains ungradeable regardless of this fix. The
  20 m / 1 h / 4 h arms inherit `true` from `_pc305_gg_tri_slant_20m.json:96` and are unaffected.
- The consumer-side expectation knob and the auto-derived consumer pair (§5) are not built.

### 6.6 `lvlauth=` / `rxdcb=` — the LEVEL half of the frame, NOT BUILT (requirement recorded 2026-08-05)

Absorbed 2026-08-15 from `docs/CONTRACT_level_authority.md` §5 (now `docs/attic/CONTRACT_level_authority.md`;
the rest of it is `docs/SPEC_level_information_budget.md` §30). Recorded **here**, in the doc that owns the
S-line grammar (§2), so the next product-format card need not re-derive it.

Today the emitted stream declares its satellite-bias signal pair (`cbpin=`, §2) but says **nothing** about the
level authority. A consumer therefore **cannot tell an a₀₀-anchored product from a sum-to-zero-gauged one, and
the two differ by the receiver-bias mean (~15 TECU on this network)**. The natural extension, when a card next
opens the S-line grammar:

- **`lvlauth=`** — one of `satzero` (the W6 per-(constellation,signal) satellite code-bias sum-to-zero
  gauge), `a00anchor:<mean>±<sigma>` (the per-epoch mean-reverting a₀₀ climatology/GIM anchor), or `none`.
- **`rxdcb=`** — `zeromean:<n_stations>` when `network.rx_code_bias_datum` is armed, **since the receiver SET
  is part of that frame: a different station list realizes a different zero.**

Both are **interpretation hints of the `cbpin=`/`agek=` class — NEVER replay-emitter construction parameters**
(the classification §2 records for `cbpin=`, and the reason `makeReplayEmitter` ignores it). Not built.
Semantics a value would carry: `docs/SPEC_level_information_budget.md` §30.
