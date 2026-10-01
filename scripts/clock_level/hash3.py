"""Windowed hashes of a run: the non-comment lines of products.txt before its 241st H record, and
the first 241 lines of closed_loop_trace.csv and network_epoch_status.csv (under logs/, else the
run directory).
    python scripts/clock_level/hash3.py <name under runs/> [<name> ...]"""
import hashlib, sys, os, io

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'runs').replace(os.sep, '/')

def products_blocks(path, n_blocks):
    """Non-comment lines of products.txt up to (not incl.) the start of block n_blocks+1."""
    out, blocks = [], 0
    with io.open(path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            if ln.startswith('#'):
                continue
            s = ln.rstrip('\n').rstrip('\r')
            if s.startswith('H,'):
                blocks += 1
                if blocks > n_blocks:
                    break
            out.append(s)
    return out, blocks

def csv_lines(path, n_rows):
    with io.open(path, encoding='utf-8', errors='replace') as f:
        return [l.rstrip('\n').rstrip('\r') for l in f][:n_rows]

def h(lines):
    m = hashlib.sha256()
    for l in lines:
        m.update(l.encode('utf-8')); m.update(b'\n')
    return m.hexdigest()[:16]

def report(tag, n_cons=241, n_prod=240):
    d = os.path.join(ROOT, tag)
    r = {}
    p = os.path.join(d, 'products.txt')
    L, b = products_blocks(p, n_prod)
    r['products'] = '%s/%d' % (h(L), len(L))
    for name, key in (('closed_loop_trace.csv','cl'), ('network_epoch_status.csv','es')):
        f = os.path.join(d, 'logs', name)
        if not os.path.exists(f): f = os.path.join(d, name)
        L = csv_lines(f, n_cons)
        r[key] = '%s/%d' % (h(L), len(L))
    return r

if __name__ == '__main__':
    for t in sys.argv[1:]:
        r = report(t)
        print('%-24s products=%s  cl=%s  es=%s' % (t, r['products'], r['cl'], r['es']))
