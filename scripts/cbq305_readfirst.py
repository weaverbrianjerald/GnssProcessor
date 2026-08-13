"""Card CB-Q leg C — the invariant-#4 read-first sheet for a 2-signal arm.

Reads the ON-PATH counters BEFORE any quality number, because the failure mode
this arm risks (Odijk two-frequency OSB non-estimability) looks like a clean run:
every tier prints ARMED, ingest is perfect, every epoch walks, and nothing is
produced.  Order here is the order of the questions.

Usage: python scripts/cbq305_readfirst.py <run_dir> [run_dir ...]
"""
import collections
import os
import re
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main():
    for run in sys.argv[1:]:
        con = os.path.join(run, "console.txt")
        prod = os.path.join(run, "products.txt")
        s = open(con, "rb").read().decode("utf-8", "replace") if os.path.exists(con) else ""
        print(f"\n================ {run} ================")

        # 1. Did it solve at all?  D0 section timers print per SOLVED epoch.
        d0 = len(re.findall(r"Network \(D0\) TOW", s))
        eps = len(re.findall(r"^E,", open(prod, errors="replace").read(), re.M)) \
            if os.path.exists(prod) else 0
        print(f"[1] SOLVED     : {eps} emitted epoch(s); {d0} D0 section-timer line(s)")
        if eps == 0:
            print("    ^^ ZERO EPOCHS — check for 'the shared datum is under-specified' below")
        for pat in (r"shared datum is under-specified[^\n]*",
                    r"DD-prior fold ran on ZERO epochs[^\n]*",
                    r"drop-and-heal[^\n]{0,160}"):
            for m in re.findall(pat, s)[:2]:
                print(f"    ! {m.strip()}")

        # 2. Which signals actually became canonical?
        can = re.findall(r"Canonical signals locked for (\w+) \((\d+) sats[^:]*: ([^\n]+)", s)
        seen = collections.Counter()
        for _, _, body in can:
            for sig in re.findall(r"\b([CL]\d[A-Z])\(", body):
                seen[sig] += 1
        print(f"[2] CANONICALS : {len(can)} station lock line(s); distinct signals = "
              f"{dict(sorted(seen.items()))}")

        # 3. The emitted signal set — the wire is the authority, not the log.
        if os.path.exists(prod):
            bs = collections.Counter()
            for line in open(prod, errors="replace"):
                if line.startswith("B,"):
                    f = line.split(",")
                    if len(f) >= 3:
                        bs[f[2]] += 1
            print(f"[3] EMITTED B  : {dict(sorted(bs.items()))}")

        # 4. Code-bias column count + gauge rows (the rank question).
        for pat in (r"n_cb[^\n]{0,140}", r"code-bias[^\n]{0,60}gauge[^\n]{0,100}",
                    r"declared_dimension[^\n]{0,120}"):
            for m in re.findall(pat, s)[:2]:
                print(f"[4] RANK       : {m.strip()}")

        # 5. Stations admitted.
        for pat in (r"(\d+) station\(s\) (?:admitted|active|in the network)",
                    r"Network: (\d+) station[^\n]{0,80}"):
            m = re.findall(pat, s)[:1]
            if m:
                print(f"[5] STATIONS   : {m[0]}")
                break

        # 6. Variance factors + post-fit sigma0 + timing.
        for m in re.findall(r"family '(\w+)':[^\n]*variance factor: ([0-9.eE+-]+)", s):
            print(f"[6] vf {m[0]:>5}   : {m[1]}")
        t = re.findall(r"epoch-loop wall (\d+) ms", s)
        if t:
            print(f"[7] WALL       : epoch loop {int(t[-1])/1000.0:.1f} s"
                  + (f"  ({int(t[-1])/1000.0/eps:.2f} s/epoch)" if eps else ""))
        for m in re.findall(r"VERDICT +: ([^\n]+)", s)[-1:]:
            print(f"[8] CONSUMER   : {m.strip()}")
        for m in re.findall(r"position \(last \d+ ep\) *: ([^\n]+)", s)[-1:]:
            print(f"    {m.strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
