#!/usr/bin/env python3
# false_live_cross.py - THE false-LIVE measurement (classifier-independent).
#
# Event: a real song is determinably airing (named by ANY independent channel in the
# wide-probe capture: raw in-band tag / ctm 200-with-track / fresh trackHistory) while
# RE-MOCT's committed display (deep log stState) reads Live at the same instant.
# Every such minute is a song shown as LIVE - the upstream bug; the lost scrobble is
# its shadow. The deep log alone CANNOT measure this when the classifier itself goes
# quiet (mfCls=None during a real song) - the probe's channels are the independent eye.
#
# Usage: python false_live_cross.py <probe.jsonl> <deep-log> [stationId=1469]
import json, sys
from datetime import datetime
from collections import defaultdict

probe_f, deep_f = sys.argv[1], sys.argv[2]
station = sys.argv[3] if len(sys.argv) > 3 else '1469'

def sq(s): return ''.join(c.lower() for c in (s or '') if c.isalnum())
def lh(ep): return int(((ep - 5*3600)//3600) % 24)

# ---- probe ticks: per ts, is a song NAMED by any channel, and by which ----
probe = []
for line in open(probe_f, encoding='utf-8', errors='replace'):
    line = line.strip()
    if not line: continue
    try: o = json.loads(line)
    except: continue
    if 'cells' not in o: continue
    for c in o['cells']:
        if c.get('station') != station: continue
        namers = {}
        if c.get('inband') == 'Music' and (c.get('inbandArtist') or c.get('inbandTitle')):
            namers['inband'] = (c.get('inbandArtist',''), c.get('inbandTitle',''))
        th = c.get('trackHistory', {})
        if th.get('status') == 200 and th.get('artist'):
            namers['trackHistory'] = (th.get('artist',''), th.get('title',''))
        for cc in c.get('ctm', []):
            if cc.get('status') == 200 and (cc.get('artist') or cc.get('title')):
                namers['ctm'] = (cc.get('artist',''), cc.get('title',''))
                break
        # STRICT airing evidence (anti-stale): the probe doesn't log th/ctm freshness, and a
        # stale trackHistory entry names the PREVIOUS song through a genuine ad break - which
        # would wrongly count as false-LIVE. So a tick is song-airing-evidenced only when the
        # RAW in-band tag names it (segment-level truth), or ctm AND trackHistory independently
        # agree on the same track (two feeds converging on one song = it's the current one).
        strict = {}
        if 'inband' in namers:
            strict = dict(namers)
        elif 'ctm' in namers and 'trackHistory' in namers:
            ka = sq(namers['ctm'][0]) + '|' + sq(namers['ctm'][1])
            kb = sq(namers['trackHistory'][0]) + '|' + sq(namers['trackHistory'][1])
            if ka == kb or (sq(namers['ctm'][1]) and sq(namers['ctm'][1]) == sq(namers['trackHistory'][1])):
                strict = {'ctm+th-agree': namers['ctm']}
        probe.append({'ts': o['ts'], 'namers': strict, 'loose': namers,
                      'inbandCls': c.get('inband')})
probe.sort(key=lambda p: p['ts'])
print(f'probe ticks for station {station}: {len(probe)}')

# ---- deep log: committed display timeline ----
deep = []
for line in open(deep_f, encoding='utf-8', errors='replace'):
    line = line.strip()
    if not line: continue
    try: o = json.loads(line)
    except: continue
    if o.get('_meta') or not o.get('ts'): continue
    ep = datetime.strptime(o['ts'][:19], '%Y-%m-%d %H:%M:%S').timestamp()
    deep.append({'ep': ep, 'stState': o.get('stState'), 'stDisp': o.get('stDisp'),
                 'mfCls': o.get('mfCls')})
deep.sort(key=lambda d: d['ep'])
print(f'deep-log records: {len(deep)}')
if not deep or not probe: sys.exit('missing data')

# committed display at time t = last deep record at or before t (dedup log => state
# holds between records; heartbeat<=30s bounds the staleness of this lookup)
import bisect
deps = [d['ep'] for d in deep]
def disp_at(ep):
    i = bisect.bisect_right(deps, ep) - 1
    if i < 0: return None
    if ep - deep[i]['ep'] > 120: return None   # outside deep-log coverage
    return deep[i]

# ---- the cross ----
false_live = 0.0; total = 0.0; runs = []; run = None
by_namer = defaultdict(float)
mfNone_share = 0.0   # false-LIVE minutes where deep log's OWN classifier said None (the blind spot)
examples = []
for i, p in enumerate(probe):
    ep = p['ts']
    w = min((probe[i+1]['ts'] - ep) if i+1 < len(probe) else 8, 60) / 60.0
    d = disp_at(ep)
    if d is None: continue
    total += w
    named = bool(p['namers'])
    if named and d['stState'] == 'Live':
        false_live += w
        for k in p['namers']: by_namer[k] += w
        if d['mfCls'] != 'Song': mfNone_share += w
        if run is None:
            a, t = next(iter(p['namers'].values()))
            run = {'start': ep, 'end': ep, 'song': f'{a} - {t}',
                   'namers': set(p['namers'])}
        else:
            run['end'] = ep; run['namers'] |= set(p['namers'])
    else:
        if run: runs.append(run); run = None
if run: runs.append(run)

print(f'\ncovered (probe x deep overlap): {total:.0f} min')
print(f'FALSE-LIVE (song named by ANY channel while display=Live): {false_live:.2f} song-min')
print(f'  of which deep-log mfCls was NOT Song (invisible to deep-log-only measure): {mfNone_share:.2f} min')
print(f'  named-by (a false-LIVE minute counts once per naming channel):')
for k, v in sorted(by_namer.items(), key=lambda x: -x[1]):
    print(f'     {k:14} {v:.2f} min')
runs.sort(key=lambda r: r['end'] - r['start'], reverse=True)
print(f'  episodes >=30s (start-CDT, dur, song, named-by):')
n = 0
for r in runs:
    dur = (r['end'] - r['start']) / 60.0
    if dur < 0.5: continue
    n += 1
    st = datetime.fromtimestamp(r['start']).strftime('%H:%M:%S')
    print(f"     {st}  {dur:5.1f} min  '{r['song']}'  via {sorted(r['namers'])}")
    if n >= 12: break
if n == 0: print('     (none >=30s - all false-LIVE is sub-30s onset slivers)')
