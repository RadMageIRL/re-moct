#!/usr/bin/env python3
# np_ledger.py - song-minutes ledger for nowplaying_wide_probe output.
#
# The acceptance criterion is NOT the per-cell rate table (that is "the 27% in a new
# hat"). It is: for every song that actually aired (in-band = ground truth), did each
# now-playing channel show it correctly, and if not, WHY (cause-tagged). Plus the sharp
# test on the table: does a station's now-playing survive WITHOUT ctm (via trackHistory/
# in-band)? -> if Breeze is accurate with ctm adding ~0 marginal, ctm is not the source.
#
# Usage: python np_ledger.py np_run/*.jsonl
import json, glob, sys
from collections import defaultdict, Counter

files = []
for a in sys.argv[1:]:
    files += glob.glob(a)
if not files:
    files = glob.glob('np_run/*.jsonl')
if not files:
    print('no jsonl given'); sys.exit(1)

ticks = []
for fn in files:
    for line in open(fn, encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line: continue
        try: o = json.loads(line)
        except: continue
        if 'cells' in o: ticks.append(o)
ticks.sort(key=lambda t: t.get('ts', 0))
if not ticks:
    print('no ticks'); sys.exit(1)

def sq(s): return ''.join(c.lower() for c in (s or '') if c.isalnum())
def songkey(a, t): return sq(a) + '|' + sq(t)

# window classification (CDT = UTC-5)
lo, hi = ticks[0]['ts'], ticks[-1]['ts']
def lh(ep): return int(((ep - 5*3600)//3600) % 24)
hours = set(lh(t['ts']) for t in ticks)
drive = any(6 <= h < 10 for h in hours)
span_min = (hi - lo)/60.0
label = ('DRIVE-TIME EVIDENCE' if drive and span_min >= 180
         else 'INSTRUMENT SMOKE (not decisive evidence)')
print('='*78)
print(f'NOW-PLAYING SONG-MINUTES LEDGER   [{label}]')
print(f'window: {span_min:.0f} min, local hours {sorted(hours)}, {len(ticks)} ticks')
if 'SMOKE' in label:
    print('  NOTE: short/off-peak run -> proves the instrument over time; song-count too')
    print('        thin to settle the drive-time question. Decisive run = full 06:00-10:00.')
print('='*78)

# discover ctm sessions present
ctm_sessions = []
for t in ticks:
    for c in t['cells']:
        for cc in c.get('ctm', []):
            s = cc.get('session')
            if s and s not in ctm_sessions: ctm_sessions.append(s)

# per station: build in-band Music episodes (one aired song each)
stations = []
for t in ticks:
    for c in t['cells']:
        if c['station'] not in stations: stations.append(c['station'])

def cell_for(tick, station):
    for c in tick['cells']:
        if c['station'] == station: return c
    return None

for station in stations:
    slug = ''
    episodes = []          # list of {key,artist,title,ticks:[...],start,end}
    cur = None
    for t in ticks:
        c = cell_for(t, station)
        if not c: continue
        slug = c.get('slug', slug)
        if c.get('inband') == 'Music':
            k = songkey(c.get('inbandArtist'), c.get('inbandTitle'))
            if cur is None or cur['key'] != k:
                if cur: episodes.append(cur)
                cur = {'key': k, 'artist': c.get('inbandArtist'), 'title': c.get('inbandTitle'),
                       'ticks': [], 'start': t['ts']}
            cur['ticks'].append((t, c)); cur['end'] = t['ts']
        else:
            if cur: episodes.append(cur); cur = None
    if cur: episodes.append(cur)

    print(f'\n### station {station} ({slug}) - {len(episodes)} aired songs (in-band truth) ###')
    # channel columns: trackHistory, graphqlOnAir, ctm.<session>
    chans = ['trackHistory', 'graphqlOnAir'] + [f'ctm.{s}' for s in ctm_sessions]
    carried = defaultdict(float)     # chan -> song-minutes carried
    total_min = 0.0
    causes = defaultdict(Counter)    # chan -> Counter of miss causes
    hdr = f'{"song":42} {"min":>4}  ' + '  '.join(f'{c[:9]:>9}' for c in chans)
    print(hdr); print('-'*len(hdr))
    for ep in episodes:
        dur = max((ep['end'] - ep['start'])/60.0, 1/60.0)
        total_min += dur
        row = {}
        for ch in chans:
            hit = False; statuses = Counter(); thval = set()
            for (t, c) in ep['ticks']:
                if ch == 'trackHistory':
                    d = c.get('trackHistory', {});
                    if d.get('matchesInband'): hit = True
                    statuses[d.get('status')] += 1; thval.add((sq(d.get('artist')), sq(d.get('title'))))
                elif ch == 'graphqlOnAir':
                    d = c.get('graphqlOnAir', {})
                    if d.get('matchesInband'): hit = True
                    statuses[d.get('status')] += 1
                else:
                    sess = ch.split('.', 1)[1]
                    for cc in c.get('ctm', []):
                        if cc.get('session') == sess:
                            if cc.get('matchesInband'): hit = True
                            statuses[cc.get('status')] += 1
            row[ch] = hit
            if hit: carried[ch] += dur
            else:
                # cause tag
                if ch == 'trackHistory':
                    frozen = len(thval) == 1
                    causes[ch]['feed-frozen-stale' if frozen else 'th-mismatch'] += 1
                elif ch == 'graphqlOnAir':
                    causes[ch]['schedule-not-song'] += 1
                else:
                    if statuses.get(204): causes[ch]['ctm-204'] += 1
                    elif 'unavailable' in statuses: causes[ch]['unavailable'] += 1
                    elif statuses.get(200): causes[ch]['ctm-200-mismatch'] += 1
                    else: causes[ch][f'ctm-{list(statuses)[:1]}'] += 1
        song = (ep['artist'] or '?') + ' - ' + (ep['title'] or '?')
        mark = lambda b: ' OK ' if b else ' .. '
        print(f'{song[:42]:42} {dur:4.1f}  ' + '  '.join(f'{mark(row[c]):>9}' for c in chans))

    # rollups
    print(f'\n  -- {station} coverage (% of {total_min:.0f} aired song-minutes) --')
    for ch in chans:
        pct = 100*carried[ch]/total_min if total_min else 0
        cz = ', '.join(f'{k}:{v}' for k, v in causes[ch].most_common(3))
        print(f'     {ch:16} {pct:5.0f}%   miss-causes: {cz}')
    # THE Breeze-no-ctm TEST: does now-playing survive without ctm?
    th_pct = 100*carried['trackHistory']/total_min if total_min else 0
    ctm_best = max((carried[f'ctm.{s}'] for s in ctm_sessions), default=0)
    ctm_pct = 100*ctm_best/total_min if total_min else 0
    # marginal ctm value = songs ctm caught that trackHistory missed
    marginal = 0.0
    for ep in episodes:
        dur = max((ep['end'] - ep['start'])/60.0, 1/60.0)
        th_hit = any(c.get('trackHistory', {}).get('matchesInband') for (_, c) in ep['ticks'])
        ctm_hit = any(cc.get('matchesInband') for (_, c) in ep['ticks'] for cc in c.get('ctm', []))
        if ctm_hit and not th_hit: marginal += dur
    marg_pct = 100*marginal/total_min if total_min else 0
    print(f'  -- WITHOUT-ctm test: trackHistory alone carries {th_pct:.0f}% of song-minutes; '
          f'ctm best {ctm_pct:.0f}%; ctm MARGINAL over trackHistory = {marg_pct:.0f}%')
    if marg_pct < 5:
        print(f'     -> ctm adds ~0 beyond trackHistory here: now-playing does NOT need ctm (Breeze-no-ctm shape).')
    else:
        print(f'     -> ctm carried {marg_pct:.0f}% of song-minutes trackHistory missed: ctm has marginal value here.')

    # FREEZE-WINDOW MARGINAL (the decisive metric): in-band=Music ticks ONLY, split by
    # which server channel is frozen/stale, then ask if the OTHER rescues. A whole-window
    # average drowns this in healthy ticks; break it out by freeze-window only.
    st_ticks=[t for t in ticks if cell_for(t,station)]
    thF=0.0; thF_ctmOk=0.0; ctmF=0.0; ctmF_thOk=0.0; bothF=0.0; musicW=0.0
    for i,t in enumerate(st_ticks):
        c=cell_for(t,station)
        if c.get('inband')!='Music': continue
        w=min((st_ticks[i+1]['ts']-t['ts']) if i+1<len(st_ticks) else 8, 40)/60.0
        musicW+=w
        th_ok=c.get('trackHistory',{}).get('matchesInband',False)
        ctm_ok=any(cc.get('matchesInband') for cc in c.get('ctm',[]))
        if not th_ok:
            thF+=w
            if ctm_ok: thF_ctmOk+=w
        if not ctm_ok:
            ctmF+=w
            if th_ok: ctmF_thOk+=w
        if (not th_ok) and (not ctm_ok): bothF+=w
    pc=lambda a,b:(100*a/b if b else 0)
    print(f'  -- FREEZE-WINDOW MARGINAL (in-band=Music only, {musicW:.0f} song-min) --')
    print(f'     trackHistory frozen/stale {thF:.1f} song-min -> ctm RESCUES {pc(thF_ctmOk,thF):.0f}% of it')
    print(f'     ctm frozen/204          {ctmF:.1f} song-min -> trackHistory RESCUES {pc(ctmF_thOk,ctmF):.0f}% of it')
    print(f'     BOTH frozen (server-sourceless, in-band ONLY) = {bothF:.1f} song-min ({pc(bothF,musicW):.0f}% of song-min)')

print('\n' + '='*78)
print('Read: (a) does a channel carry ~100% of song-minutes -> that is the source;')
print('      (b) does ctm marginal>0 on Z100 during the show -> ctm worth reviving;')
print('      (c) is Z100 trackHistory frozen (miss-cause feed-frozen-stale) during show.')
