"""Request consecutive native readbacks for visual diagnostics (not timing).

The run must already have LD_FRAME_DUMP_TRIGGER configured by run.ps1.
Readbacks add overhead; exclude these frames from performance comparisons.
"""
import argparse
import json
from pathlib import Path
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('run', type=Path)
p.add_argument('--frames', type=int, default=40)
p.add_argument('--label', required=True)
a = p.parse_args()
if not 2 <= a.frames <= 240:
    p.error('frames must be between 2 and 240')
if not a.label.replace('-', '').replace('_', '').isalnum():
    p.error('label must contain letters, numbers, dashes or underscores')
trigger = a.run / 'capture.request'
directory = a.run / 'frames'
seen = set(directory.glob('requested_*.ppm'))
captured = []
for n in range(a.frames):
    trigger.write_text('1\n')
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        found = set(directory.glob('requested_*.ppm')) - seen
        if found and not trigger.exists():
            # The next present cannot begin until the current readback writer
            # finishes; its next trigger can safely be queued now.
            captured.extend(sorted(str(f.resolve()) for f in found))
            seen.update(found)
            break
        time.sleep(.005)
    else:
        raise TimeoutError(f'No native capture for request {n}')
manifest = a.run / f'sequence-{a.label}.json'
manifest.write_text(json.dumps(dict(label=a.label, files=captured,
    note='GPU readbacks perturb timing; exclude this interval.'),indent=2))
print(manifest)
