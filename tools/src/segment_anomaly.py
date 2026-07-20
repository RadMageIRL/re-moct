#!/usr/bin/env python3
# segment_anomaly.py - infer song-slopped-into-ad from SHAPE, not the tag (scope segment-anomaly).
#
# The residual bug shape: a real song airing inside segments iHeart tagged non-music.
# Metadata instruments score that "correctly LIVE" because they trust the tag. But a
# mislabeled song still has song-shaped duration and NO ad-pod structure; a real ad
# break is a churn of distinct spot ids (cartcutId/spotInstanceId) over ~2-3 min.
# Score every non-music stretch on: duration vs song-length, spot-id churn (or its
# absence), mfSeq continuity (audio flowing vs frozen), and adjacent naming (the
# song's identity usually appears immediately before/after the mislabeled window).
# FLAG = long-duration AND no churn = a song wearing an ad tag.
#
# Baselines are computed from the corpus itself and printed - thresholds are
# data-derived, not guessed.
#
# Corroborating axis (auto-loaded): the runtime log's `hlsPollMedia ... cls=` lines -
# RE-MOCT's own segment-level song_spot read every ~10s, independent of the title-based
# mfCls. A flagged stretch with cls=music under a non-music mfCls = titleless/mislabeled
# music (the bug); cls=other/spot = genuine talk/ads (benign). Looked up as
# remoct-<date>.log beside/above the deep log. The one residual NO tag-based axis can
# see: music inside segments whose song_spot itself lies - that needs actual audio
# (music-vs-speech classification), noted per flag as audio-unavailable.
#
# Usage: python segment_anomaly.py <deep-log> [<deep-log> ...]
import json, sys, os, re, glob, statistics as st
from datetime import datetime
from collections import Counter

seen = set(); files = []
for a in sys.argv[1:]:
    for f in glob.glob(a):
        b = os.path.basename(f)
        if b not in seen: seen.add(b); files.append(f)
if not files: sys.exit('usage: segment_anomaly.py <deep-log...>')

def load_runtime_cls(near, date):
    # remoct-<date>.log beside the deep log or one level up; (epoch, cls) pairs
    out = []
    d = os.path.dirname(os.path.abspath(near))
    for cand in [os.path.join(d, f'remoct-{date}.log'),
                 os.path.join(os.path.dirname(d), f'remoct-{date}.log')]:
        if not os.path.exists(cand): continue
        for line in open(cand, encoding='utf-8', errors='replace'):
            m = re.match(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ \[stream\] hlsPollMedia:.* cls=(\w+)', line)
            if m:
                out.append((datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S').timestamp(), m.group(2)))
        break
    return out

def load(f):
    recs = []
    for line in open(f, encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line: continue
        try: o = json.loads(line)
        except: continue
        if o.get('_meta') or not o.get('ts'): continue
        o['_ep'] = datetime.strptime(o['ts'][:19], '%Y-%m-%d %H:%M:%S').timestamp()
        recs.append(o)
    recs.sort(key=lambda r: r['_ep'])
    return recs

def realSpot(o):
    s = o.get('spotInstanceId') or ''
    return s if (s and s != '-1') else None

# ---------- pass 1: corpus baselines ----------
song_durs, pod_durs, pod_churn, promo_durs = [], [], [], []
all_logs = {}
for f in files:
    recs = load(f)
    if len(recs) < 30: continue
    all_logs[f] = recs
    # contiguous mfCls runs
    i = 0
    while i < len(recs):
        cls = recs[i].get('mfCls')
        j = i
        while j + 1 < len(recs) and recs[j+1].get('mfCls') == cls: j += 1
        dur = recs[j]['_ep'] - recs[i]['_ep']
        seg = recs[i:j+1]
        if cls == 'Song' and dur >= 30:
            song_durs.append(dur)
        elif cls == 'Ad':
            ids = set(filter(None, (realSpot(o) for o in seg)))
            cuts = set(filter(None, (o.get('cartcutId') for o in seg)))
            pod_durs.append(dur); pod_churn.append(max(len(ids), len(cuts)))
        elif cls == 'None' and dur < 60:
            promo_durs.append(dur)
        i = j + 1

def dist(name, xs, unit='s'):
    if not xs: return f'{name}: n=0'
    xs = sorted(xs)
    p95 = xs[min(len(xs)-1, int(.95*len(xs)))]
    return (f'{name}: n={len(xs)} median={st.median(xs):.0f}{unit} '
            f'p95={p95:.0f}{unit} max={max(xs):.0f}{unit}')

print('=== CORPUS BASELINES (data-derived; thresholds follow from these) ===')
print(' ', dist('song span (mfCls=Song runs >=30s)', song_durs))
print(' ', dist('ad-pod duration (mfCls=Ad runs)', pod_durs))
print(' ', dist('ad-pod spot-id churn (distinct ids/run)', pod_churn, ''))
print(' ', dist('short None gaps (<60s, promo/boundary)', promo_durs))
pod_p95 = sorted(pod_durs)[min(len(pod_durs)-1, int(.95*len(pod_durs)))] if pod_durs else 180
LONG = max(90.0, pod_p95)   # longer than a real pod's p95 = duration anomaly
print(f'  -> LONG threshold = max(90s, pod p95) = {LONG:.0f}s; '
      f'churn threshold: real pods churn (see dist), flag needs <=1 distinct real id')

# ---------- pass 2: anomaly scan (non-music stretches, shape-scored) ----------
print('\n=== NON-MUSIC STRETCHES, shape-scored (sorted by duration desc) ===')
flags, benign = [], 0
for f, recs in all_logs.items():
    rt = load_runtime_cls(f, recs[0]['ts'][:10])
    i = 0
    while i < len(recs):
        if recs[i].get('mfCls') == 'Song': i += 1; continue
        j = i
        while j + 1 < len(recs) and recs[j+1].get('mfCls') != 'Song': j += 1
        seg = recs[i:j+1]
        dur = seg[-1]['_ep'] - seg[0]['_ep']
        if dur >= 60:
            ids  = set(filter(None, (realSpot(o) for o in seg)))
            cuts = set(filter(None, (o.get('cartcutId') for o in seg)))
            churn = max(len(ids), len(cuts))
            seqs = [o.get('mfSeq') for o in seg if isinstance(o.get('mfSeq'), (int, float)) and o.get('mfSeq', -1) >= 0]
            adv  = (seqs[-1] - seqs[0]) if len(seqs) >= 2 else 0
            expect = dur / 10.0                       # ~10s segments
            flow = adv / expect if expect > 0 else 0  # ~1.0 = audio flowing; ~0 = frozen manifest
            disp = Counter(o.get('stState') for o in seg)
            live_share = (disp.get('Live', 0) + disp.get('Ad', 0)) / max(sum(disp.values()), 1)
            before = recs[i-1] if i > 0 else None
            after  = recs[j+1] if j + 1 < len(recs) else None
            th_in  = next((o.get('th') for o in seg if o.get('th')), '')
            name = (after and after.get('mfSong')) or (before and before.get('mfSong')) or th_in or '(none adjacent)'
            anomalous = dur >= LONG and churn <= 1 and flow >= 0.5
            # corroborating axis: runtime segment-level cls= inside the stretch
            rc = Counter(c for (t, c) in rt if seg[0]['_ep'] <= t <= seg[-1]['_ep'])
            rn = sum(rc.values())
            musicpct = 100.0*rc.get('music', 0)/rn if rn else -1.0   # -1 = no runtime coverage
            entry = dict(f=os.path.basename(f), ts=seg[0]['ts'][11:19], dur=dur,
                         churn=churn, ids=sorted(cuts or ids)[:4], flow=flow,
                         live=live_share, name=name, cls=Counter(o.get('mfCls') for o in seg),
                         musicpct=musicpct, rtn=rn)
            if anomalous: flags.append(entry)
            else: benign += 1
        i = j + 1

flags.sort(key=lambda e: -e['dur'])
print(f'flagged (LONG + no-churn + audio-flowing): {len(flags)}   benign non-music stretches >=60s: {benign}')
bug_min = talk_min = uncov_min = 0.0
for e in flags:
    if e['musicpct'] >= 50: v = 'MUSIC-UNDER-NONMUSIC-TAG <- THE BUG'; bug_min += e['dur']/60
    elif e['musicpct'] >= 0: v = 'talk/ads by segment-level cls (benign)'; talk_min += e['dur']/60
    else: v = 'no runtime-cls coverage (unresolved by this axis)'; uncov_min += e['dur']/60
    e['verdict'] = v
for e in flags[:15]:
    print(f"\n  FLAG {e['ts']}  {e['dur']/60:5.1f} min  [{e['f']}]")
    print(f"       spot-churn={e['churn']} distinct ids {e['ids'] or '(none)'}  <- a real pod churns")
    print(f"       mfSeq flow={e['flow']:.2f} (~1.0 = audio flowing under the non-music tag)")
    print(f"       display Live/Ad share={e['live']*100:.0f}%   mfCls mix={dict(e['cls'])}")
    print(f"       runtime cls=music {e['musicpct']:.0f}% (n={e['rtn']})  -> {e['verdict']}")
    print(f"       adjacent naming: '{e['name']}'   [audio axis: unavailable in metadata captures]")
print(f"\nVERDICT: music-under-nonmusic-tag={bug_min:.1f} min   talk/benign={talk_min:.1f} min   "
      f"cls-uncovered={uncov_min:.1f} min")
if not flags:
    print('  (no stretch is simultaneously song-length, churn-free, and audio-flowing -')
    print('   within everything shape and structure can attest, this corpus is clean;')
    print('   the residual would be audio-only: music under a non-music tag at normal short duration)')
