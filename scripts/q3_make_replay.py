#!/usr/bin/env python3
"""Generate consumer-REPLAY configs for Card Q3 ladder arms (SPEC §8.4 gate 3).

The live closed-loop consumer runs inside the producer arm and is one
instrument; the replay is the SECOND, and it differs in a way that matters here:
it re-reads the emitted product stream from disk, so it sees exactly the
discontinuity marks the wire actually carries.  A guard whose marks do not land
on the wire looks fine to the live consumer (which is fed in-process) and shows
up in the replay — which is precisely the failure mode Q3's own trap list opens
with (`markSatDiscontinuity` alone is replay-invisible).

One knob per config: `network.closed_loop.replay_products_path`.  The producer
never runs (`runNetwork` short-circuits before setup), so a replay cannot
perturb the producer artifact it reads.

Usage: python scripts/q3_make_replay.py cta4_q3e6 cta4_q3e7 ...
"""
import io
import json
import os
import sys

BASE = "C:/GnssProcessor/config/test_suite/_golden63_gps_1h_laneoff.json"
CFG = r"C:\GnssProcessor\config\test_suite"
DUR = 10800.0


def main():
    arms = sys.argv[1:]
    if not arms:
        print(__doc__)
        return 1
    for a in arms:
        src = f"C:/GnssProcessor/runs/{a}/products.txt"
        if not os.path.exists(src.replace("/", os.sep)):
            print(f"  SKIP {a}: no products.txt yet")
            continue
        name = f"{a}_replay"
        d = {
            "_comment": [
                f"Card Q3 §8 consumer REPLAY of runs/{a}/products.txt — the second",
                "consumer instrument.  Reads the emitted stream from disk, so it sees the",
                "wire's own discontinuity marks rather than the in-process counters; a mark",
                "that never reached the wire is invisible to the live consumer and visible",
                "here.  enable_rt_engine makes the consumer's resets countable and is",
                "solve-neutral.  Producer never runs.",
            ],
            "extends": [BASE],
            "processing": {"max_duration_s": DUR},
            "logging": {"enable_rt_engine": True},
            "network": {
                "max_duration_s": DUR,
                "closed_loop": {"replay_products_path": src},
                "product_output_path": f"C:/GnssProcessor/runs/{name}/products.txt",
                "state_snapshot_path": f"C:/GnssProcessor/runs/{name}/state_snapshot.bin",
            },
        }
        p = os.path.join(CFG, f"_{name}.json")
        with io.open(p, "w", encoding="utf-8", newline="\n") as f:
            json.dump(d, f, indent=1)
            f.write("\n")
        print("wrote", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
