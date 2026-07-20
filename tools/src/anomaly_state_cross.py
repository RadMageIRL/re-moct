#!/usr/bin/env python3
# anomaly_state_cross.py - repin/logic-state cross over long confirmed-talk LIVE
# stretches (scope anomaly-state-cross). segment_anomaly found the felt-bug-size
# stretches are ALL genuine talk; this asks: does the LIVE-floor stall / repin logic
# FIRE through them (thrashing - jumping the stream to escape an "ad pod" that is
# actually the program), or ride them out (QUIET, correct)?
#
# Joins, per long non-music stretch (>=140s, low churn - same enumeration as
# segment_anomaly): the runtime log's fire lines ("LIVE-floor stall Ns -> requesting
# live-edge re-pin", elapsed ~35s = on-mode floor, ~150s+ = smart), plus the deep
# log's connectSeq increments and audioSec rebases inside the stretch (the repin
# actually executing). THRASHING = any fire inside; QUIET = rode it out.
#
# Usage: python anomaly_state_cross.py <deep-log> [<deep-log> ...]
import json, sys, os, re, glob
from datetime import datetime
from collections import Counter

seen = set(); files = []
for a in sys.argv[1:]:
    for f in glob.glob(a):
        b = os.path.basename(f)
        if b not in seen: seen.add(b); files.append(f)
if not files: sys.exit('usage: anomaly_state_cross.py <deep-log...>')

def load(f):
    recs = []
    for line in open(f, encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line: continue
        try: o = json.loads(line)
        except: continue
        if o.get('_meta') or not o.get('ts'): continue
        o['_ep'] = datetime.strptime(o['ts'][:23], '%Y-%m-%d %H:%M:%S.%f').timestamp()
        recs.append(o)
    recs.sort(key=lambda r: r['_ep'])
    return recs

def load_fires(near, date):
    # (epoch, elapsedSec) per "LIVE-floor stall Ns -> requesting live-edge re-pin"
    out = []
    d = os.path.dirname(os.path.abspath(near))
    for cand in [os.path.join(d, f'remoct-{date}.log'),
                 os.path.join(os.path.dirname(d), f'remoct-{date}.log')]:
        if not os.path.exists(cand): continue
        for line in open(cand, encoding='utf-8', errors='replace'):
            m = re.match(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ .*LIVE-floor stall (\d+)s -> requesting', line)
            if m:
                out.append((datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S').timestamp(),
                            int(m.group(2))))
        break
    return out

quiet = thrash = 0; spurious = 0; rows = []
for f in files:
    recs = load(f)
    if len(recs) < 30: continue
    fires = load_fires(f, recs[0]['ts'][:10])
    i = 0
    while i < len(recs):
        if recs[i].get('mfCls') == 'Song': i += 1; continue
        j = i
        while j + 1 < len(recs) and recs[j+1].get('mfCls') != 'Song': j += 1
        seg = recs[i:j+1]; dur = seg[-1]['_ep'] - seg[0]['_ep']
        if dur >= 140:
            ids = set(); cuts = set()
            for o in seg:
                s = o.get('spotInstanceId') or ''
                if s and s != '-1': ids.add(s)
                if o.get('cartcutId'): cuts.add(o.get('cartcutId'))
            if max(len(ids), len(cuts)) <= 1:
                a, b = seg[0]['_ep'], seg[-1]['_ep']
                inside = [(t, e) for (t, e) in fires if a <= t <= b]
                # repin EXECUTING: connectSeq increment / audioSec rebase inside
                cseqs = [o.get('connectSeq') for o in seg if isinstance(o.get('connectSeq'), int)]
                creconn = (cseqs[-1] - cseqs[0]) if len(cseqs) >= 2 else 0
                arebase = sum(1 for k in range(1, len(seg))
                              if isinstance(seg[k].get('audioSec'), (int, float)) and
                                 isinstance(seg[k-1].get('audioSec'), (int, float)) and
                                 seg[k]['audioSec'] < seg[k-1]['audioSec'] - 5)
                gaps = [inside[k][0] - inside[k-1][0] for k in range(1, len(inside))]
                mode = ('smart' if inside and min(e for _, e in inside) >= 100
                        else ('on' if inside else '-'))
                verdict = 'THRASHING' if inside else 'QUIET'
                if inside: thrash += 1; spurious += len(inside)
                else: quiet += 1
                rows.append(dict(f=os.path.basename(f), ts=seg[0]['ts'][11:19], dur=dur,
                                 fires=len(inside), gaps=gaps, mode=mode,
                                 creconn=creconn, arebase=arebase, verdict=verdict))
        i = j + 1

rows.sort(key=lambda r: -r['dur'])
print('long confirmed-talk/non-music LIVE stretches - repin behavior inside each:')
for r in rows[:20]:
    g = (f" fire-gaps={['%.0fs' % x for x in r['gaps']]}" if r['gaps'] else '')
    print(f"  {r['ts']}  {r['dur']/60:5.1f}min  [{r['f'][:34]}]  {r['verdict']:9} "
          f"fires={r['fires']} mode={r['mode']} connectSeq+={r['creconn']} audioRebase={r['arebase']}{g}")
print(f"\nTALLY: stretches QUIET={quiet}  THRASHING={thrash}  total spurious repins inside talk={spurious}")
if thrash == 0:
    print('VERDICT: repin/floor logic RODE OUT every long talk stretch - exonerated on this corpus.')
else:
    print('VERDICT: repin fires during confirmed talk - the floor logic mistakes long talk for an ad pod.')
