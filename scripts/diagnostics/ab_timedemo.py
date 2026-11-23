"""Frame-locked ABBA comparison of two Haze executables on one benchmark scene.

Each run is a `diagnose.py timedemo-run` session (virtual clock, frame-scheduled
camera sweep). Per run: sweep mean and p99 producer frame time and draw-count
agreement with the pair's control run. A pair whose draw counts differ by more
than --max-draw-diff percent, or whose scene broke (for example the stationary
player died), is excluded. Verdict: exact sign-permutation test on per-pair
(candidate - control) differences, raw and adjusted to equal draw counts.
Runs wait for a quiet host first. See docs/benchmark-clock.md.
"""
import argparse, itertools, json, statistics, subprocess, sys, time
from pathlib import Path

from diagnostics.compare_timedemo import load_run, summarize, agreement, frame_print

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'local'/'timedemo'/'out'

def sweep_draws(run):
    lo, hi = run['settle'], run['settle']+run['sweep']
    draws = [frame_print(run, g)[1] for g in range(lo, hi) if frame_print(run, g)]
    return statistics.fmean(draws) if draws else 0.0

def scene_intact(run):
    """The stationary player can be killed late in a sweep; the death fade drops
    draws to single digits. Any 300-frame window under 60% of the run's median
    means a different scene, so the run is invalid."""
    lo, hi = run['settle'], run['settle']+run['sweep']
    means = []
    for begin in range(lo, hi, 300):
        d = [frame_print(run, g)[1] for g in range(begin, min(begin+300, hi)) if frame_print(run, g)]
        if d: means.append(statistics.fmean(d))
    return bool(means) and min(means) >= 0.6*statistics.median(means)

def draw_slope(pairs):
    """Pooled within-arm slope of run mean ms on run mean draws."""
    xs, ys = [], []
    for arm in ('control', 'candidate'):
        d = [q[arm]['draws'] for q in pairs]; m = [q[arm]['mean_ms'] for q in pairs]
        if len(d) > 1:
            xs += [x-statistics.fmean(d) for x in d]; ys += [y-statistics.fmean(m) for y in m]
    den = sum(x*x for x in xs)
    return sum(x*y for x, y in zip(xs, ys))/den if den else 0.0

def verdict(name, d, base):
    observed = statistics.fmean(d)
    signs = list(itertools.product((1, -1), repeat=len(d)))
    p_low = sum(statistics.fmean(s*x for s, x in zip(sg, d)) <= observed for sg in signs)/len(signs)
    print(f"{name}: candidate-control {observed:+.3f} ms ({100*observed/base:+.2f}%) over {len(d)} pairs; "
          f"one-sided p(candidate faster)={p_low:.3f}")

def host_load(seconds=3.0):
    """Non-Haze CPU in cores, sampled with Get-Process CPU deltas."""
    script = (f"$a=Get-Process|?{{$_.ProcessName -notmatch '^(Idle|HazeRecomp)$'}}|select Id,CPU;"
              f"Start-Sleep -Milliseconds {int(seconds*1000)};"
              "$b=Get-Process|select Id,CPU,ProcessName;$t=0;$top=@{};"
              "foreach($p in $a){$q=$b|?{$_.Id -eq $p.Id};if($q -and $p.CPU){$d=$q.CPU-$p.CPU;$t+=$d;$top[$q.ProcessName]+=$d}};"
              f"'{{0:N3}}' -f ($t/{seconds});($top.GetEnumerator()|sort Value -desc|select -first 3|%{{$_.Name+'='+('{{0:N2}}' -f ($_.Value/{seconds}))}}) -join ' '")
    out = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout.split('\n')
    return float(out[0].strip() or 0), out[1].strip() if len(out) > 1 else ''

def run(stem, exe, a):
    out = OUT/stem
    if not Path(f'{out}-timeline.csv.done').exists():
        load, top = host_load()
        while load > a.quiet_cores:
            print(f'  waiting for quiet host: {load:.2f} cores ({top})', flush=True)
            time.sleep(20); load, top = host_load()
        subprocess.run([sys.executable, str(ROOT/'scripts'/'diagnose.py'), 'timedemo-run', stem, '--exe', str(exe),
                        '--settle', str(a.settle), '--sweep', str(a.sweep), '--scene', a.scene,
                        '--shared-root', str(a.shared_root)], check=True)
    return load_run(out)

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('tag'); p.add_argument('--control', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--pairs', type=int, default=4); p.add_argument('--settle', type=int, default=300)
    p.add_argument('--sweep', type=int, default=1500); p.add_argument('--max-draw-diff', type=float, default=100.0)
    p.add_argument('--quiet-cores', type=float, default=1.5)
    p.add_argument('--scene', default='later', help='benchmark scene (see timedemo-run --help)')
    p.add_argument('--shared-root', type=Path, default=ROOT, help='prepared checkout (see timedemo-run)')
    a = p.parse_args()
    pairs = []
    for i in range(a.pairs):
        order = [('c', a.control), ('n', a.candidate)] if i % 2 == 0 else [('n', a.candidate), ('c', a.control)]
        runs = {arm: run(f'{a.tag}-{i}{arm}', exe, a) for arm, exe in order}
        sc, sn = summarize(runs['c']), summarize(runs['n'])
        sc['draws'], sn['draws'] = sweep_draws(runs['c']), sweep_draws(runs['n'])
        ag = agreement(runs['c'], runs['n'], a.sweep)
        draws = [w['draw_difference_percent'] for w in ag['windows'] if w['begin'] >= a.settle]
        diff = max((abs(d) for d in draws if d is not None), default=0)
        intact = scene_intact(runs['c']) and scene_intact(runs['n'])
        pairs.append(dict(pair=i, control=sc, candidate=sn, draw_diff=diff, intact=intact,
                          valid=intact and diff <= a.max_draw_diff))
        print(f"pair {i}: control {sc['mean_ms']:.3f} ms p99 {sc['p99_ms']:.2f} | candidate {sn['mean_ms']:.3f} ms "
              f"p99 {sn['p99_ms']:.2f} | draws {diff:+.2f}% {'ok' if pairs[-1]['valid'] else 'EXCLUDED (scene)'}", flush=True)
    valid = [q for q in pairs if q['valid']]
    if valid:
        base = statistics.fmean(q['control']['mean_ms'] for q in valid)
        for metric in ('mean_ms', 'p99_ms'):
            verdict(metric, [q['candidate'][metric]-q['control'][metric] for q in valid], base)
        # Scene drift changes the workload (draws) and frame time together;
        # compare at equal workload using the pooled within-arm slope.
        slope = draw_slope(valid)
        adjusted = [q['candidate']['mean_ms']-q['control']['mean_ms']-slope*(q['candidate']['draws']-q['control']['draws'])
                    for q in valid]
        print(f'draw-adjusted (slope {slope:.4f} ms/draw):', end=' ')
        verdict('mean_ms', adjusted, base)
    (OUT/f'{a.tag}-ab.json').write_text(json.dumps(pairs, indent=2))

if __name__ == '__main__':
    main()
