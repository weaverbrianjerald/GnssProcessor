#!/usr/bin/env python3
"""Splice a CONTINUOUS seed wire with a warm-resume LEG's wire at the seam.

    python scripts/splice_wire.py <seed_products> <leg_products> <out_products>

Writes one product file that is the seed's epochs up to (and excluding) the leg's
FIRST emitted epoch, followed by the leg's epochs verbatim — i.e. the wire a
consumer that has been tracking the producer for hours actually sees when the
producer bounces and warm-resumes.

## Why this exists

A COLD consumer replayed on a leg's wire alone starts every key at its own first
sighting, so a per-key LEVEL offset across the seam is structurally invisible to
it (SPEC_event_harness §21.10 / RS-4: a 17.666 m p50 per-(sat,signal) emitted step
cost a cold consumer nothing).  The consumer a seam card is actually about is the
STATEFUL one, and the only way to put the replay driver in that state is to feed
it a spliced wire.  Named in docs/SPEC_synthetic_minimum_network.md §18.14 and
docs/SPEC_event_harness.md §21.10; the original lived in a session scratchpad and
was lost, so this is a re-implementation with its verification recorded below.

## The §18.14 replay-arm offset trap

The splice is only half the job — the replay CONFIG has to be built to match, or
the consumer silently grades the wrong window.  Derive each arm from the LEG's own
`leg.json` and change exactly these fields (the shape of `runs/_rs3_rp_*` /
`runs/_rs4_rp_*`, verified again for `runs/_rs5_rp_*`):

  network.closed_loop.replay_products_path = <the spliced wire>
  network.product_output_path              = <the arm's own dir>/products.txt
  network.start_offset_hours               = 0.0     # NOT the leg's own offset
  network.state_snapshot_path              = ""
  network.state_snapshot_require           = false
  processing.max_duration_s                = 30 * (seed epochs + leg epochs)

`start_offset_hours` is the trap proper: `runNetwork` short-circuits to the
consumer-only driver, which takes its start from the PRODUCT FILE's first tow, so
a leg's own offset (5.029166668918398 h on the DOY-129 ep0604 legs) both means
nothing here and reads as if it did.  `max_duration_s` is the second half: a leg's
own value (1830 s = its 61 epochs) truncates a 665-epoch spliced wire to its first
61 epochs — which lands entirely BEFORE the seam, so the arm looks clean and has
measured nothing.

## The three arms an RS-5-shaped seam card needs

Two spliced wires and the seed's own, replayed by the SAME consumer over the SAME
window, so the only thing differing is the leg:

    python scripts/splice_wire.py runs/evseed_field6h_v4/products.txt \\
        runs/evleg_rs5_null_restore/products.txt   runs/_rs5_splice/splice_restore.txt
    python scripts/splice_wire.py runs/evseed_field6h_v4/products.txt \\
        runs/evleg_rs5_null_norestore/products.txt runs/_rs5_splice/splice_control.txt
    # then three replay arms on the fields listed above:
    #   _rs5_rp_splice_restore -> runs/_rs5_splice/splice_restore.txt
    #   _rs5_rp_splice_control -> runs/_rs5_splice/splice_control.txt
    #   _rs5_rp_seed           -> runs/evseed_field6h_v4/products.txt  (no seam at all)

The seed arm is the control that says what the consumer does with no bounce; a
spliced arm is only readable against it.

## Verification (RS-5, 2026-09-12)

Run on `runs/evseed_field6h_v4/products.txt` (720 epochs) + each of
`runs/evleg_rs5_null_{restore,norestore}/products.txt` (61 epochs from tow 277320):

  * the spliced file's bytes are IDENTICAL to the seed's over the whole prefix
    (all 8 118 693 bytes of it, ending at the `H` row that opens the seam epoch),
    then continue with the leg's epochs — checked by direct string compare, not
    by record count;
  * 665 `E` records out = 604 seed (tow < 277320) + 61 leg, no duplicated and no
    dropped epoch at the join;
  * the three arms built from it ran clean and the consumer's own reset census at
    the seam separated the legs exactly (control 19 phase-bias resets, restore 0 =
    the continuous wire's 0).

## Conventions

The file header (the `#` comment block + the `S` contract) is taken from the SEED,
which is what a consumer running since the seed's first epoch already holds; the
leg's own header lines are dropped.  `H` heartbeat rows belong to the epoch they
PRECEDE, so they travel with it.  Everything else is copied byte-for-byte, newlines
included, so a splice is a pure concatenation of whole epoch blocks.

⚠ If the leg's `S` contract differs from the seed's (a different datum receiver,
pivot or threshold set) the consumer never learns it — the splice asserts that the
two wires share one contract, which is what a warm resume of the same lineage
gives.  Check the two `S` lines before reading a result from a splice across a
config change.
"""
import sys


def epoch_blocks(path):
    """Yield (tow_or_None, [lines]) — the header first (tow None), then one block per E."""
    head, block, tow = [], [], None
    started = False
    pend = []          # lines since the last block that precede the next E (the H row)
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for line in f:
            tag = line.split(",", 1)[0]
            if tag == "E":
                if started:
                    yield (tow, block)
                else:
                    yield (None, head)
                    started = True
                tow = int(round(float(line.rstrip().split(",")[2])))
                block = pend + [line]
                pend = []
                continue
            if not started:
                # An H row opens the epoch it precedes; it is not part of the header.
                if tag == "H":
                    pend.append(line)
                else:
                    head.extend(pend)
                    pend = []
                    head.append(line)
            else:
                if tag == "H":
                    pend.append(line)
                else:
                    block.extend(pend)
                    pend = []
                    block.append(line)
    if started:
        block.extend(pend)
        yield (tow, block)
    else:
        yield (None, head + pend)


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    seed_p, leg_p, out_p = argv[1:]
    leg = list(epoch_blocks(leg_p))
    leg_tows = [t for t, _ in leg if t is not None]
    if not leg_tows:
        print("FATAL: leg carries no E record")
        return 1
    cut = min(leg_tows)
    out, n_seed = [], 0
    for tow, lines in epoch_blocks(seed_p):
        if tow is None:
            out.extend(lines)            # the SEED's header + S contract
            continue
        if tow >= cut:
            break
        out.extend(lines)
        n_seed += 1
    if n_seed == 0:
        print("FATAL: the seed has no epoch before the leg's first (tow %d) — "
              "wrong seed, or the leg does not resume from inside it" % cut)
        return 1
    n_leg = 0
    for tow, lines in leg:
        if tow is None:
            continue                     # drop the leg's header
        out.extend(lines)
        n_leg += 1
    with open(out_p, "w", encoding="utf-8", newline="") as f:
        f.writelines(out)
    print("splice  seed=%s  leg=%s" % (seed_p, leg_p))
    print("  seam tow      %d  (the leg's first emitted epoch)" % cut)
    print("  seed epochs   %d  (< seam)" % n_seed)
    print("  leg epochs    %d" % n_leg)
    print("  out           %s  (%d lines, %d epochs)"
          % (out_p, len(out), n_seed + n_leg))
    print("  replay arm    max_duration_s = %.1f  (start_offset_hours 0.0)"
          % (30.0 * (n_seed + n_leg)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
