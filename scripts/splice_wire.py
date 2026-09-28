#!/usr/bin/env python3
"""Splice a continuous seed wire with a warm-resume leg's wire at the seam.

    python scripts/splice_wire.py <seed_products> <leg_products> <out_products>

Writes one product file: the seed's epochs up to (and excluding) the leg's first
emitted epoch, followed by the leg's epochs verbatim. This is the wire a consumer
that has been tracking the producer for hours actually sees when the producer
bounces and warm-resumes.

## Why this exists

A cold consumer replayed on a leg's wire alone starts every key at its own first
sighting, so a per-key level offset across the seam is structurally invisible to
it (SPEC_event_harness §21.10, RS-4). The consumer a seam card is about
is the stateful one, and the only way to put the replay driver in that state is
to feed it a spliced wire. Named in docs/SPEC_synthetic_minimum_network.md §18.14
and docs/SPEC_event_harness.md §21.10.

## The §18.14 replay-arm offset trap

The splice is only half the job: the replay config also has to be built to
match, or the consumer silently grades the wrong window. Derive each arm from
the leg's own `leg.json` and change exactly these fields:

  network.closed_loop.replay_products_path = <the spliced wire>
  network.product_output_path              = <the arm's own dir>/products.txt
  network.start_offset_hours               = 0.0     # not the leg's own offset
  network.state_snapshot_path              = ""
  network.state_snapshot_require           = false
  processing.max_duration_s                = 30 * (seed epochs + leg epochs)

`start_offset_hours` is the trap: `runNetwork` short-circuits to the
consumer-only driver, which takes its start from the product file's first tow,
so a leg's own offset means nothing here even though it looks like it should
apply. `max_duration_s` is the second half: a leg's own value truncates the
spliced wire at the leg's own epoch count, which lands entirely before the
seam, so the arm looks clean while measuring nothing.

## The three arms a seam card needs

Two spliced wires and the seed's own, replayed by the same consumer over the
same window, so the only thing differing is the leg:

    python scripts/splice_wire.py <seed_products> <leg_a_products> <out_a>
    python scripts/splice_wire.py <seed_products> <leg_b_products> <out_b>
    # then three replay arms: one per spliced wire, plus one on the seed's own
    # products file directly (no seam at all).

The seed arm is the control that shows what the consumer does with no bounce; a
spliced arm is only readable against it.

## Verification

Checked directly, not just by output record count: the spliced file's bytes
match the seed's byte-for-byte over the whole prefix, up to the row that opens
the seam epoch; the output epoch count is exactly the seed's epochs before the
seam plus the leg's epochs, with none duplicated or dropped at the join; and a
consumer replayed on the spliced wire reproduces the reset behavior of the
continuous wire it approximates, while a control splice that omits the leg's
restore step does not.

## Conventions

The file header (the `#` comment block and the `S` contract line) comes from
the seed, since that is what a consumer running since the seed's first epoch
already holds; the leg's own header lines are dropped. `H` heartbeat rows
belong to the epoch they precede, so they travel with it. Everything else is
copied byte-for-byte, newlines included, so a splice is a pure concatenation
of whole epoch blocks.

If the leg's `S` contract differs from the seed's (a different datum receiver,
pivot or threshold set), the consumer never learns it: the splice assumes the
two wires share one contract, which is what a warm resume of the same lineage
gives. Check the two `S` lines before reading a result from a splice across a
config change.
"""
import sys


def epoch_blocks(path):
    """Yield (tow_or_None, [lines]): the header first (tow None), then one block per E."""
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
            out.extend(lines)            # the seed's header and S contract
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
