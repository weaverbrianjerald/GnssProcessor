"""Card SFR: candidate sim fixtures for the coloured-phase calibration, and arms that read them.

gen : python scripts/clock_level/sfr_calib.py gen <tag> --sim-exe <gnss_simulator exe>
          [--hours 2.75] [--white <phase_sigma_zen_m>] [--tau <s>] [--sigma <m>]
          [--class tropical=<sigma_m>,<tau_s>] [--class mid=...] [--class high=...]
      Writes runs/_sfr_<tag>/recipe.json from config/sim/simnet60_quiet_24h.json with the run
      length cut to --hours, the white phase noise set to --white, a network-level phase-colour
      block (--sigma, --tau) and per-station overrides by latitude class (tropical |lat| < 23.5,
      mid 23.5 to 50, high above 50 degrees, the classes of phase_acf.py), then generates the
      fixture into runs/_sfr_<tag>/fix with `synth-network`.
genp: python scripts/clock_level/sfr_calib.py genp <tag> --sim-exe <exe> [--jobs 8] [--hours 2.75]
          [--white <m>] [--mp-phase <m>] [--sigma <m>] [--tau <s>] [--class <name>=<sigma_m>,<tau_s>]
          [--pc-floor <deg>]
      The calibration path: the 24 h fixture's own per-station child configs (station, seed salt,
      biases) with the span, the white phase noise, the multipath phase amplitude (its code
      amplitude and draw untouched) and the phase-colour block changed, synthesised per station
      with `synth-static` in parallel (synth-network is serial, about 25 s a station).
arm : python scripts/clock_level/sfr_calib.py arm <tag> <golden|program> [--hours 2]
      Makes runs/_rap_sfr_<tag>_<g|p> with scripts/rapid_arm.py from the sim golden
      (config/golden/golden_sim_refound.json) or the program posture's sim control
      (runs/_rap_wsx_ctl_s/arm.json, card REV's block-clone instrument), every RINEX path moved
      from the 24 h fixture to the candidate, starting at 00:30:15 (the fixture-start convention).
      Run it with `rapid_arm.py run`; grade with phase_acf.py (golden or program) and
      wsf_persist.py (program).
"""
import argparse, json, math, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
RECIPE = ROOT + '/config/sim/simnet60_quiet_24h.json'
OLD_FIX = 'C:/GnssProcessor/runs/simnet60_quiet_wu93_24h'
START_H = 0.504167


def klass(lat):
    a = abs(lat)
    return 'tropical' if a < 23.5 else ('mid' if a <= 50.0 else 'high')


def gen(a):
    r = json.load(open(RECIPE, encoding='utf-8'))
    out = f'{RUNS}/_sfr_{a.tag}'
    os.makedirs(out, exist_ok=True)
    r['_comment'] = [f'Card SFR calibration candidate {a.tag}, derived by scripts/clock_level/sfr_calib.py from '
                     f'config/sim/simnet60_quiet_24h.json: {" ".join(sys.argv[2:])}']
    dt = r['trajectory']['interval_s']
    r['trajectory']['n_epochs'] = int(round(a.hours * 3600.0 / dt))
    if a.white is not None:
        r['noise']['phase_sigma_zen_m'] = a.white
    if a.sigma is not None or a.tau is not None:
        pc = r.setdefault('phase_colour', {})
        if a.sigma is not None:
            pc['sigma_zen_m'] = a.sigma
        if a.tau is not None:
            pc['tau_s'] = a.tau
    cls = {}
    for c in a.cls or []:
        name, v = c.split('=', 1)
        s, t = v.split(',')
        cls[name] = (float(s), float(t))
    n_over = {}
    for st in r['network_stations']:
        k = klass(st['lat_deg'])
        if k in cls:
            st['phase_colour_sigma_zen_m'], st['phase_colour_tau_s'] = cls[k]
            n_over[k] = n_over.get(k, 0) + 1
    json.dump(r, open(f'{out}/recipe.json', 'w', encoding='utf-8'), indent=2)
    print(f'{a.tag}: {r["trajectory"]["n_epochs"]} epochs, white {r["noise"]["phase_sigma_zen_m"]}, '
          f'network colour {r.get("phase_colour")}, overrides {n_over}')
    with open(f'{out}/gen.log', 'w') as log:
        rc = subprocess.run([a.sim_exe, 'synth-network', f'{out}/fix', '--sim-config', f'{out}/recipe.json'],
                            stdout=log, stderr=subprocess.STDOUT).returncode
    print(f'{a.tag}: synth-network exit {rc} (log {out}/gen.log)')
    sys.exit(rc)


def genp(a):
    """The calibration path: the existing fixture's per-station child configs (station, seed salt
    and biases as generated) with the run length, the white phase noise and the phase-colour block
    changed, each station synthesised by `synth-static` in parallel.  Every other realization stays
    the 24 h fixture's (the simulator draws the white phase noise unconditionally, and the colour
    has its own stream)."""
    import concurrent.futures as cf, glob
    out = f'{RUNS}/_sfr_{a.tag}'
    os.makedirs(f'{out}/children', exist_ok=True)
    cls = {}
    for c in a.cls or []:
        name, v = c.split('=', 1)
        s, t = v.split(',')
        cls[name] = (float(s), float(t))
    jobs = []
    for p in sorted(glob.glob(OLD_FIX + '/_child_*.json')):
        c = json.load(open(p, encoding='utf-8'))
        sta = c['station']['name']
        c['_comment'] = [f'Card SFR calibration candidate {a.tag} (scripts/clock_level/sfr_calib.py genp): '
                         f'{" ".join(sys.argv[2:])}; from {p}']
        dt = c['trajectory']['interval_s']
        c['trajectory']['n_epochs'] = int(round(a.hours * 3600.0 / dt))
        if a.white is not None:
            c['noise']['phase_sigma_zen_m'] = a.white
        if a.mp_phase is not None:
            c.setdefault('multipath', {})['phase_amp_zen_m'] = a.mp_phase
        sig, tau = a.sigma, a.tau
        k = klass(c['trajectory']['lat0_deg'])
        if k in cls:
            sig, tau = cls[k]
        if a.pc_floor is not None and sig:
            c.setdefault('phase_colour', {})['elev_factor_floor_deg'] = a.pc_floor
        if sig is not None or tau is not None:
            pc = c.setdefault('phase_colour', {})
            if sig is not None:
                pc['sigma_zen_m'] = sig
            if tau is not None:
                pc['tau_s'] = tau
        cp = f'{out}/children/_child_{sta}.json'
        json.dump(c, open(cp, 'w', encoding='utf-8'), indent=2)
        jobs.append((sta, cp))

    def one(job):
        sta, cp = job
        os.makedirs(f'{out}/fix/{sta}', exist_ok=True)
        with open(f'{out}/fix/{sta}.gen.log', 'w') as log:
            return sta, subprocess.run([a.sim_exe, 'synth-static', f'{out}/fix/{sta}', '--sim-config', cp],
                                       stdout=log, stderr=subprocess.STDOUT).returncode
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        rcs = list(ex.map(one, jobs))
    bad = [s for s, rc in rcs if rc != 0]
    print(f'{a.tag}: {len(rcs)} stations synthesised, {len(bad)} failed {bad}; classes {cls}, white {a.white}')
    sys.exit(1 if bad else 0)


def arm(a):
    fix = f'{RUNS}/_sfr_{a.tag}/fix'
    assert os.path.isdir(fix), fix
    if a.posture == 'golden':
        base = json.load(open(ROOT + '/config/golden/golden_sim_refound.json', encoding='utf-8'))
    else:
        base = json.load(open(RUNS + '/_rap_wsx_ctl_s/arm.json', encoding='utf-8'))
    n = 0
    net = base['network']
    for st in net['stations'] + ([net['closed_loop']['station']] if net.get('closed_loop', {}).get('station') else []):
        p = st.get('rinex_path', '')
        if p.startswith(OLD_FIX):
            # The file name carries the span (01D for the 24 h fixture), so the candidate's
            # RINEX is found in the station's directory rather than by name.
            sdir = os.path.dirname(fix + p[len(OLD_FIX):])
            rnx = sorted(f for f in os.listdir(sdir) if f.endswith('.rnx'))
            assert len(rnx) == 1, (sdir, rnx)
            st['rinex_path'] = sdir + '/' + rnx[0]
            n += 1
    tmp = f'{RUNS}/_sfr_{a.tag}/base_{a.posture}.json'
    json.dump(base, open(tmp, 'w', encoding='utf-8'), indent=1)
    tag = f'sfr_{a.tag}_{a.posture[0]}'
    cmd = [sys.executable, ROOT + '/scripts/rapid_arm.py', 'make', '--base', tmp, '--tag', tag,
           '--hours', str(a.hours), '--set', f'network.start_offset_hours={START_H}']
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(f'{tag}: {n} RINEX paths moved; ' + (r.stdout.strip().splitlines() or [''])[-1] + r.stderr[-300:])


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    sp = p.add_subparsers(dest='cmd', required=True)
    g = sp.add_parser('gen')
    g.add_argument('tag'); g.add_argument('--sim-exe', required=True)
    g.add_argument('--hours', type=float, default=2.75)
    g.add_argument('--white', type=float); g.add_argument('--sigma', type=float); g.add_argument('--tau', type=float)
    g.add_argument('--class', dest='cls', action='append')
    gp = sp.add_parser('genp')
    gp.add_argument('tag'); gp.add_argument('--sim-exe', required=True)
    gp.add_argument('--hours', type=float, default=2.75); gp.add_argument('--jobs', type=int, default=8)
    gp.add_argument('--white', type=float); gp.add_argument('--sigma', type=float); gp.add_argument('--tau', type=float)
    gp.add_argument('--class', dest='cls', action='append')
    gp.add_argument('--mp-phase', dest='mp_phase', type=float)
    gp.add_argument('--pc-floor', dest='pc_floor', type=float)
    m = sp.add_parser('arm')
    m.add_argument('tag'); m.add_argument('posture', choices=['golden', 'program'])
    m.add_argument('--hours', type=float, default=2.0)
    a = p.parse_args()
    {'gen': gen, 'genp': genp, 'arm': arm}[a.cmd](a)
