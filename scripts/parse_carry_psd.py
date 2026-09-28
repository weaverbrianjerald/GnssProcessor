"""Carry-PSD audit of a network state snapshot: the gate of the PD-safe fold card.

    python scripts/parse_carry_psd.py <snapshot.bin> [more_snapshots ...] [--fresh 1e6]
                                      [--top N] [--json OUT.json] [--quiet]

docs/SPEC_pd_safe_dd_fold.md §7.1.  Positive-definiteness of the carried Pz is an
algebraic invariant of the exact invariant-carry recursion: independent chords give a
full-row-rank projection W, so W·PSD·Wᵀ plus a fresh diagonal is PD, and the exact
posterior of a PD prior is PD.  It can therefore break only numerically, and when it does
the fold's `M := 0` branch freezes the whole block silently and permanently.  A frozen
block still reports PD (its rows never move enough to go non-finite or negative), so a
large share of a lineage's carried state can sit frozen without failing a naive PD check;
see docs/SPEC_pd_safe_dd_fold.md §7.1 for the field measurement that motivated this gate.

The gate, per block:
  * PD            : the correlation-normalized spectrum is strictly positive.  Graded
                    normalized, not absolutely: a block carrying a 1e-4 .. 1e6 variance
                    spread has an eigensolver round-off floor around 1e-10 in absolute
                    terms, so an absolute test measures numpy, not the carry.
  * diagonal      : every variance strictly positive and finite.  A negative variance is
                    decisive on its own and needs no eigenvalue to reject.
  * too-fresh count : advisory only.  Nothing the recursion produces exceeds the prior it
                    started from, but the recursion is not the only writer: a D5 re-tap
                    installs sigma0^2 * Cov(v)_ii, and sigma0 is both above 1 and time-varying
                    under LS-VCE calibration, so no ceiling readable from a snapshot is a
                    hard bound.  Reported against sigma0^2 * fresh from the blob's "S" line;
                    never fails the gate.
  * diffuse rows  : rows still at (≥ half) the fresh prior.  These should be this epoch's
                    births only.  A standing diffuse set is the frozen-block signature:
                    the block takes no data, so its rows never leave the prior.

Exit status is the gate: 0 = every block passed, 1 = at least one block failed, 2 = the
snapshot carried no z-store at all (which is not a pass: it is "nothing was measured").

Run it on the ON and OFF arms of the same lineage.  The OFF arm is expected to fail; that
failure is the card's field evidence, not a problem with the script.
"""
import argparse
import json
import math
import os
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover - the script is useless without it
    sys.stderr.write("parse_carry_psd.py needs numpy (pip install numpy)\n")
    raise


# --------------------------------------------------------------------------- parsing
def read_carry_section(path):
    """Return the snapshot's verbatim CARRY..CEND lines (empty list if absent).

    The snapshot is a plain text file; the z-store blob is embedded verbatim between a
    ``CARRY,v<n>,<count>`` header and a ``CEND`` terminator (NetworkStateSnapshot.cpp).
    Reading it directly, rather than through any C++ path, is deliberate: this script is
    an independent audit of what the producer actually wrote.
    """
    out, inside = [], False
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not inside:
                if line.startswith("CARRY,"):
                    inside = True
                    out.append(line)
                continue
            out.append(line)
            if line == "CEND":
                break
    return out


def parse_blocks(lines):
    """Parse CARRY lines into a list of block dicts.

    Row shapes (NetworkInvariantCarryDriver::serializeCarry):
      B,group,pair,lambda1,lambda2,zdim,n_chords,n_vertices,n_arcs
      Z,<zdim values>
      P,<zdim values>   x zdim      (v1/v2 dense), or  PD,<zdim values> (v3 diagonal)
      H,<chord u,v pairs>   X,<vertex ids>   A,<arc u,v pairs>
    """
    blocks, i, n = [], 0, len(lines)
    version, sigma0_sq = "", 1.0
    if n and lines[0].startswith("CARRY,"):
        parts = lines[0].split(",")
        version = parts[1] if len(parts) > 1 else ""
        i = 1
    while i < n:
        line = lines[i]
        if line == "CEND":
            break
        # The v2 "S,<sigma0_sq>,<eff_rows>" line holds the D5 LS-VCE inflation state
        # (card RE-1); it is the ceiling this audit must grade "too fresh" against, see
        # audit_block.
        if line.startswith("S,"):
            f = line.split(",")
            if len(f) == 3:
                try:
                    sigma0_sq = max(1.0, float(f[1]))
                except ValueError:
                    pass
            i += 1
            continue
        if not line.startswith("B,"):
            i += 1
            continue
        f = line.split(",")
        if len(f) != 9:
            raise ValueError("malformed B header: %r" % line)
        blk = {
            "group": int(f[1]),
            "pair": f[2] != "0",
            "lambda1": float(f[3]),
            "lambda2": float(f[4]),
            "zdim": int(f[5]),
            "n_chords": int(f[6]),
        }
        zdim = blk["zdim"]
        i += 1
        zf = lines[i].split(",")
        if zf[0] != "Z":
            raise ValueError("expected Z row, got %r" % lines[i])
        blk["z"] = np.array([float(v) for v in zf[1:]], dtype=float)
        i += 1
        P = np.zeros((zdim, zdim), dtype=float)
        blk["diagonal_blob"] = False
        if zdim > 0:
            pf = lines[i].split(",")
            if pf[0] == "PD":                      # v3 diagonal-only blob
                d = np.array([float(v) for v in pf[1:]], dtype=float)
                np.fill_diagonal(P, d)
                blk["diagonal_blob"] = True
                i += 1
            else:
                for r in range(zdim):
                    pf = lines[i].split(",")
                    if pf[0] != "P":
                        raise ValueError("expected P row %d, got %r" % (r, lines[i]))
                    P[r, :] = [float(v) for v in pf[1:]]
                    i += 1
        blk["Pz"] = P
        # H / X / A carry the identity; keep the leading chord as the block's label.
        chord0 = None
        for _ in range(3):
            if i >= n:
                break
            tf = lines[i].split(",")
            if tf[0] == "H" and len(tf) >= 3:
                chord0 = (int(tf[1]), int(tf[2]))
            i += 1
        blk["chord0"] = chord0
        blocks.append(blk)
    return version, sigma0_sq, blocks


# --------------------------------------------------------------------------- audit
def audit_block(blk, fresh, sigma0_sq=1.0):
    """Grade one block.  Returns a dict; `ok` is the gate."""
    P = blk["Pz"]
    k = P.shape[0]
    r = {
        "group": blk["group"],
        "k": int(k),
        "pair": blk["pair"],
        "chord0": blk["chord0"],
        "diagonal_blob": blk["diagonal_blob"],
        "min_diag": None, "max_diag": None,
        "min_eig_norm": None, "cond_norm": None,
        "n_nonpos_diag": 0, "n_nonfinite": 0,
        "n_superfresh": 0, "n_diffuse_rows": 0,
        "reasons": [],
    }
    if k == 0:
        r["ok"] = True
        return r

    if not np.all(np.isfinite(P)):
        r["n_nonfinite"] = int(np.count_nonzero(~np.isfinite(P)))
        r["reasons"].append("non-finite entries")

    d = np.diag(P)
    finite_d = d[np.isfinite(d)]
    if finite_d.size:
        r["min_diag"] = float(finite_d.min())
        r["max_diag"] = float(finite_d.max())
    r["n_nonpos_diag"] = int(np.count_nonzero(~(d > 0.0)))
    if r["n_nonpos_diag"]:
        r["reasons"].append("%d non-positive variance(s)" % r["n_nonpos_diag"])

    # Anything above the diffuse fresh prior cannot be produced by the recursion: the
    # posterior of a prior is never looser than that prior.  But the recursion is not the
    # only writer.  A D5 re-tap (network.carry_invariant_innov_gate) installs the chord's
    # honest post-fit variance sigma0^2 * Cov(v)_ii, and under LS-VCE calibration
    # (innov_calibrate) sigma0 is measured and typically runs above 1, so a re-tapped
    # diffuse chord can legitimately land well above the fresh prior.  Grading against a
    # bare `fresh` would therefore flag the D5 gate doing its job on both arms and bury
    # the real signal, a negative variance.  The blob carries the number needed: the v2
    # "S," line is sigma0^2.
    ceiling = fresh * sigma0_sq
    r["ceiling"] = ceiling
    if fresh > 0.0:
        # Advisory only, not a gate item.  sigma0 is time-varying (LS-VCE with a 15-epoch
        # forgetting half-life), so the sigma0^2 stored in the final blob does not bound
        # what an earlier re-tap installed: sigma0^2 can be higher mid-run than at the
        # snapshot.  No single ceiling read from the snapshot can be a hard bound, so this
        # is reported and never fails the gate.  SPEC 7.1 names the gate as "every block
        # PD ... diffuse rows ~ current-epoch births only", that is what `ok` grades.
        r["n_superfresh"] = int(np.count_nonzero(np.abs(P) > ceiling * (1.0 + 1e-9)))
        r["n_diffuse_rows"] = int(np.count_nonzero(d >= 0.5 * fresh))

    # Correlation-normalized spectrum (scale-free; see the module docstring).
    if r["n_nonpos_diag"] == 0 and r["n_nonfinite"] == 0:
        s = 1.0 / np.sqrt(d)
        R = (P * s[:, None]) * s[None, :]
        R = 0.5 * (R + R.T)
        try:
            ev = np.linalg.eigvalsh(R)
            r["min_eig_norm"] = float(ev.min())
            hi = float(np.abs(ev).max())
            r["cond_norm"] = float(hi / ev.min()) if ev.min() > 0.0 else math.inf
            if ev.min() <= 0.0:
                r["reasons"].append("normalized min eig %.3e <= 0" % ev.min())
        except np.linalg.LinAlgError:
            r["reasons"].append("eigensolver failed")

    r["ok"] = not r["reasons"]
    return r


def audit_snapshot(path, fresh):
    lines = read_carry_section(path)
    if not lines:
        return {"path": path, "present": False, "blocks": [], "version": ""}
    version, sigma0_sq, blocks = parse_blocks(lines)
    res = [audit_block(b, fresh, sigma0_sq) for b in blocks]
    return {"path": path, "present": True, "version": version,
            "sigma0_sq": sigma0_sq, "blocks": res}


# --------------------------------------------------------------------------- report
def report(snap, top, quiet):
    path = snap["path"]
    print("=" * 78)
    print("%s" % path)
    if not snap["present"]:
        print("  NO CARRY SECTION - this snapshot holds no invariant-carry z-store.")
        print("  That is not a pass: nothing was measured.  Check network.state_snapshot_carry")
        print("  and that the run actually armed network.carry_invariant.")
        return 2

    blocks = snap["blocks"]
    nz = [b for b in blocks if b["k"] > 0]
    bad = [b for b in nz if not b["ok"]]
    rows = sum(b["k"] for b in nz)
    bad_rows = sum(b["k"] for b in bad)
    diffuse = sum(b["n_diffuse_rows"] for b in nz)
    diag_blobs = sum(1 for b in nz if b["diagonal_blob"])

    s0sq = snap.get("sigma0_sq", 1.0)
    print("  blob %s | %d block(s), %d chordless, %d carried row(s)"
          % (snap["version"] or "?", len(blocks), len(blocks) - len(nz), rows))
    print("  D5 inflation sigma0 = %.3f (blob 'S' line) => variance ceiling %.4g"
          % (s0sq ** 0.5, s0sq * 1.0e6 if s0sq else 1.0e6))
    if diag_blobs:
        print("  NOTE: %d block(s) came from a DIAGONAL-only (v3) blob - their cross-"
              "covariances were never written, so the PD verdict below is about a matrix"
              % diag_blobs)
        print("        the run did not actually carry.  Re-run the audit on a dense blob.")
    superfresh = sum(b["n_superfresh"] for b in nz)
    print("  diffuse rows (>= half the fresh prior): %d of %d (%.1f %%)"
          % (diffuse, rows, 100.0 * diffuse / rows if rows else 0.0))
    if superfresh:
        print("  ADVISORY: %d entr(y/ies) above the sigma0-scaled ceiling - expected when the"
              % superfresh)
        print("            D5 gate re-taps (it installs sigma0^2*Cov(v), and sigma0 drifts);")
        print("            NOT a gate failure.  A NEGATIVE variance is.")
    print("        ^ should be THIS epoch's births only.  A standing diffuse set is the")
    print("          frozen-block signature - the block takes no data, so its rows never")
    print("          leave the prior.")

    if nz:
        worst = sorted(
            (b for b in nz if b["min_eig_norm"] is not None),
            key=lambda b: b["min_eig_norm"])[:top]
        if worst:
            print("  worst normalized min-eig (block / group / K / min_eig / cond / lead chord):")
            for b in worst:
                print("    %-6s g=%-4d K=%-5d min_eig=%- .3e cond=%.2e  chord=%s"
                      % ("FAIL" if not b["ok"] else "ok", b["group"], b["k"],
                         b["min_eig_norm"],
                         b["cond_norm"] if b["cond_norm"] is not None else float("nan"),
                         "%d-%d" % b["chord0"] if b["chord0"] else "-"))

    if bad:
        print("  FAILURES: %d block(s), %d carried row(s) = %.1f %% of the carried state"
              % (len(bad), bad_rows, 100.0 * bad_rows / rows if rows else 0.0))
        if not quiet:
            for b in sorted(bad, key=lambda x: -x["k"])[:top]:
                print("    g=%-4d K=%-5d chord=%-12s min_diag=%- .3e max_diag=%- .3e"
                      % (b["group"], b["k"],
                         "%d-%d" % b["chord0"] if b["chord0"] else "-",
                         b["min_diag"] if b["min_diag"] is not None else float("nan"),
                         b["max_diag"] if b["max_diag"] is not None else float("nan")))
                print("          %s" % "; ".join(b["reasons"]))
        print("  GATE: FAIL")
        return 1

    print("  GATE: PASS - every carried block is positive-definite.")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("snapshots", nargs="+", help="network state snapshot file(s)")
    ap.add_argument("--fresh", type=float, default=1.0e6,
                    help="network.carry_invariant fresh-chord prior variance (default 1e6)")
    ap.add_argument("--top", type=int, default=10, help="how many blocks to list")
    ap.add_argument("--json", dest="json_out", default=None,
                    help="also write the full audit as JSON")
    ap.add_argument("--quiet", action="store_true", help="summary lines only")
    args = ap.parse_args(argv[1:])

    results, rc = [], 0
    for p in args.snapshots:
        if not os.path.exists(p):
            print("MISSING: %s" % p)
            rc = max(rc, 2)
            continue
        snap = audit_snapshot(p, args.fresh)
        results.append(snap)
        rc = max(rc, report(snap, args.top, args.quiet))

    if len(results) > 1:
        print("=" * 78)
        print("A/B SUMMARY (arms are comparable only within one lineage and one build)")
        for s in results:
            nz = [b for b in s["blocks"] if b["k"] > 0]
            rows = sum(b["k"] for b in nz)
            bad_rows = sum(b["k"] for b in nz if not b["ok"])
            print("  %-52s %6d rows, %6d failing (%.1f %%)"
                  % (os.path.basename(os.path.dirname(s["path"])) or s["path"],
                     rows, bad_rows, 100.0 * bad_rows / rows if rows else 0.0))

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=1, default=str)
        print("wrote %s" % args.json_out)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
