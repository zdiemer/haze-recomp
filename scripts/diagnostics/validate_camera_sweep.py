"""Validate actual local-player angles and wall-time trajectory for FPS windows."""
import csv
import math


def validate_camera_sweep(path, windows):
    with path.open(newline='') as stream:
        rows = [{k: float(v) for k, v in row.items()} for row in csv.DictReader(stream)]
    if not rows or any(b['qpc'] <= a['qpc'] for a, b in zip(rows, rows[1:])):
        raise ValueError('Camera audit is empty or not chronological')
    frequency = windows['qpc_frequency']
    if any(row['qpc_frequency'] != frequency for row in rows):
        raise ValueError('Camera QPC frequency differs')
    anchors, pitches, motors, reports = [], [], set(), []
    for block in windows['blocks']:
        start, end = block['start_qpc'], block['end_qpc']
        if rows[0]['qpc'] > start or rows[-1]['qpc'] < end:
            raise ValueError('Camera audit does not bracket the complete window')
        selected = [r for r in rows if start <= r['qpc'] <= end]
        if len(selected) < 20:
            raise ValueError('Insufficient camera samples')
        origin = selected[0]['qpc'] / frequency - selected[0]['phase_s']
        yaw_errors, pitch_errors, bins = [], [], set()
        for row in selected:
            if not all(math.isfinite(v) for v in row.values()):
                raise ValueError('Nonfinite camera audit')
            if row['token'] != block['camera_token']:
                raise ValueError('Camera trajectory token differs from the recorded block')
            phase = row['phase_s']
            if abs(row['qpc'] / frequency - phase - origin) > 1e-5:
                raise ValueError('Camera phase is not advancing with wall time')
            anchors.append(row['wanted_yaw'] - math.pi/6 * math.sin(phase*math.pi/2))
            pitches.append(row['wanted_pitch'])
            motors.add(row['motor'])
            yaw_errors.append(abs(math.remainder(row['actual_yaw']-row['wanted_yaw'], 2*math.pi)))
            pitch_errors.append(abs(row['actual_pitch']-row['wanted_pitch']))
            bins.add(int((phase % 4) * 4))
        if max(yaw_errors) > 1e-4 or max(pitch_errors) > 1e-4:
            raise ValueError('Actual game camera does not follow the requested trajectory')
        if len(bins) != 16:
            raise ValueError('Camera samples do not cover the complete repeating sweep')
        reports.append(dict(samples=len(selected), phase_bins=len(bins),
            max_yaw_error_radians=max(yaw_errors), max_pitch_error_radians=max(pitch_errors),
            max_sample_gap_s=max((b['qpc']-a['qpc'])/frequency for a,b in zip(selected,selected[1:]))))
    if len(motors) != 1 or max(anchors)-min(anchors) > 1e-5 or max(pitches)-min(pitches) > 1e-5:
        raise ValueError('Camera owner, center or pitch drifted between windows')
    return dict(source=str(path), blocks=reports, center_yaw_radians=sum(anchors)/len(anchors),
                note='Actual local-player angles follow the same wall-time sweep; world/NPC state remains live.')
