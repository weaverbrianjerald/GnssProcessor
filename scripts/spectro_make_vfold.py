#!/usr/bin/env python3
"""Generate the S2 virtual-fold legs of the lane-spectrometer campaign.

docs/SPEC_lane_spectrometer.md §4: from the S1 run's snapshot, resume paired legs
that are identical except ``network.sd_ewl.fix_enable``, and read the between-leg
state delta, which is the single-epoch constrained-update redistribution, exact,
per constraint, with no dynamics and no QC feedback.

Three legs, not two, and why
----------------------------
``ctl``  sd_ewl entirely off        : the plain control resume.
``off``  sd_ewl.enable on, fix off  : the increment-1 float row layer.
``on``   sd_ewl.enable on, fix on   : the increment-2 fixed fold.

§4.2 asks for "the off leg must be byte-identical to a plain control resume". Taken
literally that cannot hold for a leg with the float layer armed: the float rows are
measured to move ev1 by +0.51 %. The applicable version of the same validity check is
the three-leg ladder: ``ctl`` vs ``off`` isolates the float layer (a known, measured
quantity) and ``off`` vs ``on`` isolates the integer, which is the question S2 exists
to answer. Both differences are then single-treatment.

A fourth leg, ``inert``, arms fix_enable with an impossible FFR gate. It must be
byte-identical to ``off``: that is the only test that the new branch does nothing when
it does not fire, as opposed to being accidentally load-bearing.

Why the legs are about 6 epochs and not 1
------------------------------------
The MW arc accumulator is not in the state snapshot, so a resumed leg starts with zero
MW samples and needs ``sd_ewl.min_arc_epochs`` lagged samples before any row can build.
A literal one-epoch leg would build zero rows and the pair would come out
byte-identical, precisely the Card-1e trap ("a byte-identical A/B means the feature
never ran"). So the legs run a handful of epochs at ``min_arc_epochs = 2`` and the
graded epoch is the first one with ``rows_fixed > 0``. That epoch is still an exact
single-epoch read: every earlier epoch is bit-identical across the legs (no rows exist
yet), so the two legs enter it from the same state and the delta is the constrained
update alone. Those identical leading epochs are also the pair's own validity check.

Every key written here is verified absent from the warm-resume fingerprint, so a leg may
arm them against a seed that did not: ``sd_ewl.*`` (grep-verified absent from
NetworkRunContext.cpp's fingerprint chain) and the trace knobs. ``start_offset_hours``
carries the +15 s half-epoch back-off for the same reason chain_make_leg.py does: epoch
elapsed times carry ~2e-5 s of double-precision residue and the fast-forward gate is a
strict ``<``.

Usage
-----
    python scripts/spectro_make_vfold.py --snapshot runs/spectro_3h/snap.txt \\
        [--outdir runs/spectro_vfold] [--epochs 6] [--min-arc 2]
"""
import argparse
import json
import os
import shutil

# First synced epoch of DOY-129 = 2024-05-08 00:00:00 GPS (week 2313, tow 259200) in
# continuous GPS seconds. The epoch loop latches this from the file start; it is not
# restored from the snapshot.
DAY_ANCHOR_S = 1399161600.0
EPOCH_INTERVAL_S = 30.0
HALF_EPOCH_BACKOFF_S = 15.0

BASE = 'C:/GnssProcessor/config/test_suite/_spectro_50sta_3h.json'
CFG_DIR = 'C:/GnssProcessor/config/test_suite'

# (leg name, sd_ewl overrides, one-line purpose for the config comment)
LEGS = [
    ('ctl', None,
     'CONTROL: sd_ewl entirely off — the plain resume this pair is measured against.'),
    ('off', {'enable': True, 'fix_enable': False},
     'FLOAT: increment-1 rows only.  ctl->off isolates the float layer.'),
    ('on', {'enable': True, 'fix_enable': True},
     'FIXED: increment-2 integers folded.  off->on isolates THE INTEGER.'),
    ('inert', {'enable': True, 'fix_enable': True, 'fix_max_frac_cyc': 1e-9},
     'INERT ARM: fix armed but the |frac| gate is unreachable, so no MEASURED integer '
     'is usable and every datum-only group is demoted.  Must be byte-identical to `off` '
     'on the SOLVE - the only test that the new branch is inert when it does not fire.  '
     'NB the gate has to be the FRACTIONAL one: an FFR gate of 0.99999999 still admits '
     'very mature arcs (measured - 26 integers and 6 fixed rows got through it), because '
     'a long arc variance-of-the-mean makes the rounding arbitrarily confident.'),
]


def read_snapshot_header(path):
    """(week, tow, n_emitted, fingerprint) from the snapshot's M / N header lines."""
    week = tow = n_emitted = fingerprint = None
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if line.startswith('M,'):
                f = line.rstrip('\n').split(',')
                week, tow, fingerprint = int(f[1]), float(f[2]), f[5]
            elif line.startswith('N,'):
                n_emitted = int(line.split(',')[1])
                break
    if week is None or tow is None:
        raise SystemExit('FATAL: %s has no readable M/N header' % path)
    return week, tow, n_emitted, fingerprint


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', required=True)
    ap.add_argument('--outdir', default='C:/GnssProcessor/runs/spectro_vfold')
    ap.add_argument('--epochs', type=int, default=6,
                    help='epochs per leg (>= min_arc + 2 so a row can mature)')
    ap.add_argument('--min-arc', type=int, default=2)
    ap.add_argument('--base', default=BASE)
    args = ap.parse_args()

    week, tow, n_emitted, fp = read_snapshot_header(args.snapshot)
    resume_s = week * 604800.0 + tow
    offset_h = (resume_s - DAY_ANCHOR_S + HALF_EPOCH_BACKOFF_S) / 3600.0
    # max_duration_s is anchored on the first epoch that survives the fast-forward, not
    # on the first observation: ApplicationNetwork.cpp evaluates the start-offset
    # `continue` before the max-duration block, so `max_dur_anchor_s` is the resume
    # epoch. (The startup log line "epoch time relative to first obs" describes the
    # cold-start case.) Measured: a leg given (resume - anchor) + 6*30 ran 368 epochs
    # instead of 6. So the value is just the leg length, and the `>=` comparison makes
    # N*30 process exactly N epochs.
    max_dur = args.epochs * EPOCH_INTERVAL_S

    print('seed snapshot   %s' % args.snapshot)
    print('  week/tow      %d / %r   n_emitted %s   fingerprint %s'
          % (week, tow, n_emitted, fp))
    print('start_offset_h  %r   (first leg epoch tow %r)'
          % (offset_h, tow + EPOCH_INTERVAL_S))
    print('max_duration_s  %r   (%d epochs, measured FROM THE RESUME epoch)'
          % (max_dur, args.epochs))

    for name, sd, why in LEGS:
        legdir = os.path.join(args.outdir, name).replace('\\', '/')
        os.makedirs(os.path.join(legdir, 'logs'), exist_ok=True)
        # Each leg reads and writes its own snapshot copy, so a killed leg can never
        # destroy the state it resumed from.
        leg_snap = os.path.join(legdir, 'snap.txt').replace('\\', '/')
        shutil.copyfile(args.snapshot, leg_snap)

        cfg = {
            '_comment': [
                'S2 virtual-fold leg "%s" — %s' % (name, why),
                'GENERATED by scripts/spectro_make_vfold.py from %s' % args.snapshot,
                'seed week/tow %d/%r, fingerprint %s, n_emitted %s'
                % (week, tow, fp, n_emitted),
                'start_offset_hours carries the +15 s half-epoch back-off (the '
                'fast-forward gate is a strict <, and epoch elapsed times carry ~2e-5 s '
                'of double-precision residue).',
                'Every key set here is ABSENT from the warm-resume fingerprint, so it is '
                'legal to arm against a seed that did not carry it.',
                'Grade the FIRST epoch with rows_fixed > 0; every earlier epoch must be '
                'bit-identical across the legs (no rows exist yet), which is the pair\'s '
                'own validity check.',
            ],
            'extends': [args.base],
            'processing': {'max_duration_s': max_dur},
            'network': {
                'start_offset_hours': offset_h,
                'state_snapshot_path': leg_snap,
                'state_snapshot_require': True,   # never silently cold-start
                'state_snapshot_keep_history': False,
                'trace_dd_fractional': True,
                'trace_dd_fractional_max_pairs': 2000,
                'trace_dd_fractional_epoch_stride': 1,
                'trace_phase_anchor': True,
                'trace_postfit_keyed': True,
                'trace_estimability': True,
                'product_output_path': os.path.join(legdir, 'products.txt')
                                          .replace('\\', '/'),
            },
        }
        if sd is not None:
            se = dict(sd)
            se.setdefault('trace', True)
            se.setdefault('min_arc_epochs', args.min_arc)
            # The increment-1 measured ladder, identical in every leg so the only
            # difference between `off` and `on` is the integer itself.
            se.setdefault('sigma_floor_m', 1.0)
            se.setdefault('max_abs_z_m', 10.0)
            se.setdefault('fix_sigma_floor_m', 1.0)
            cfg['network']['sd_ewl'] = se
        else:
            cfg['network']['sd_ewl'] = {'enable': False, 'fix_enable': False}

        path = os.path.join(CFG_DIR, '_spectro_vfold_%s.json' % name)
        with open(path, 'w', encoding='utf-8') as fh:
            json.dump(cfg, fh, indent=2)
        print('  leg %-6s -> %s   (run with cwd %s)' % (name, path, legdir))


if __name__ == '__main__':
    main()
