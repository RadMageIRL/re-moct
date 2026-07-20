#!/usr/bin/env python3
# music_live_cross.py - MUSIC-airing-shown-as-LIVE (the user-experience event).
#
# Distinct from false_live_cross.py: that one counted "a channel NAMED a song while
# display=Live". This counts "the in-band segment says MUSIC IS AIRING (song_spot=M,
# title or no title) while the committed display = Live" - broken out by whether any
# channel had a title (nameable) vs none did (music playing, zero titles anywhere,
# screen says LIVE - the pure form of the bug). "No channel named a song" is NOT
# "no song was playing"; every prior number collapsed those two.
#
# Usage: python music_live_cross.py <probe.jsonl> <deep-log> [stationId=1469]
import json, sys, bisect
from datetime import datetime
from collections import defaultdict

probe_f, deep_f = sys.argv[1], sys.argv[2]
station = sys.argv[3] if len(sys.argv) > 3 else '1469'

# ---- probe ticks ----
probe = []
for line in open(probe_f, encoding='utf-8', errors='replace'):
    line = line.strip()
    if not line: continue
    try: o = json.loads(line)
    except: continue
    if 'cells' not in o: continue
    for c in o['cells']:
        if c.get('station') != station: continue
        th = c.get('trackHistory', {})
        ctm = next((cc for cc in c.get('ctm', []) if cc.get('status') == 200
                    and (cc.get('artist') or cc.get('title'))), None)
        probe.append({
            'ts': o['ts'],
            'music': c.get('inband') == 'Music',
            'spot': c.get('inbandSpot', ''),
            'ibArtist': c.get('inbandArtist', ''), 'ibTitle': c.get('inbandTitle', ''),
            'thTitle': (th.get('artist') or '') and f"{th.get('artist','')} - {th.get('title','')}",
            'ctmTitle': ctm and f"{ctm.get('artist','')} - {ctm.get('title','')}" or '',
        })
probe.sort(key=lambda p: p['ts'])

# ---- deep log committed display ----
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
deps = [d['ep'] for d in deep]
def disp_at(ep):
    i = bisect.bisect_right(deps, ep) - 1
    if i < 0 or ep - deep[i]['ep'] > 120: return None
    return deep[i]

total = 0.0
music_live = 0.0          # music airing + display Live (the event)
ml_named = 0.0            # ...and some channel had a title
ml_unnamed = 0.0          # ...and NO channel had a title (pure form)
mfcls_at = defaultdict(float)
runs = []; run = None
for i, p in enumerate(probe):
    w = min((probe[i+1]['ts'] - p['ts']) if i+1 < len(probe) else 8, 60) / 60.0
    d = disp_at(p['ts'])
    if d is None: continue
    total += w
    hit = p['music'] and d['stState'] == 'Live'
    if hit:
        music_live += w
        named = bool(p['ibArtist'] or p['ibTitle'] or p['thTitle'] or p['ctmTitle'])
        if named: ml_named += w
        else: ml_unnamed += w
        mfcls_at[d['mfCls']] += w
        tagnow = f"spot={p['spot'] or '?'} artist='{p['ibArtist']}' title='{p['ibTitle']}'"
        if run is None:
            run = {'start': p['ts'], 'end': p['ts'], 'ticks': 1, 'named_ticks': 1 if named else 0,
                   'tags': [tagnow], 'th': p['thTitle'], 'ctm': p['ctmTitle']}
        else:
            run['end'] = p['ts']; run['ticks'] += 1; run['named_ticks'] += 1 if named else 0
            if len(run['tags']) < 4 and tagnow != run['tags'][-1]: run['tags'].append(tagnow)
            run['th'] = run['th'] or p['thTitle']; run['ctm'] = run['ctm'] or p['ctmTitle']
    else:
        if run: runs.append(run); run = None
if run: runs.append(run)

print(f'covered: {total:.0f} min   (station {station})')
print(f'MUSIC-AIRING-SHOWN-AS-LIVE: {music_live:.2f} song-min')
print(f'   named by some channel : {ml_named:.2f} min  (a title existed somewhere)')
print(f'   NAMED BY NONE         : {ml_unnamed:.2f} min  <- pure form: music playing, zero titles, screen LIVE')
print(f'   deep-log mfCls during these minutes: ' +
      ', '.join(f'{k}:{v:.2f}m' for k, v in sorted(mfcls_at.items(), key=lambda x: -x[1])))
runs.sort(key=lambda r: r['end'] - r['start'], reverse=True)
print(f'\nepisodes >=30s (start, dur, raw segment tag(s) at those instants, th/ctm if any):')
n = 0
for r in runs:
    dur = (r['end'] - r['start']) / 60.0 + 8/60.0
    if dur < 0.5: continue
    n += 1
    st = datetime.fromtimestamp(r['start']).strftime('%H:%M:%S')
    nm = 'named' if r['named_ticks'] else 'UN-NAMED'
    print(f"  {st}  {dur:5.1f} min  [{nm} {r['named_ticks']}/{r['ticks']} ticks]")
    for t in r['tags'][:3]: print(f"      tag: {t}")
    if r['th']:  print(f"      trackHistory had: '{r['th']}'")
    if r['ctm']: print(f"      ctm had:          '{r['ctm']}'")
    if n >= 15: break
if n == 0: print('  (none >=30s)')
