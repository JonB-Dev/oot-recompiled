#!/usr/bin/env python3
"""Summarize a contiguous window of one-second gameplay probe samples."""
import argparse
import json
import math
from pathlib import Path
from statistics import mean


def summarize(lines, first, last):
    if first < 0 or last < first:
        raise ValueError('invalid sample window')
    header = next((line[2:].split() for line in lines if line.startswith('# line ') or
                   (line.startswith('# ') and 'present_mean_us' in line)), None)
    required = ('line', 'presents', 'present_mean_us', 'present_max_us')
    if not header or any(key not in header for key in required):
        raise ValueError('missing probe columns')
    rows = []
    for line in lines:
        if not line.strip() or line.startswith('#'):
            continue
        cells = line.split()
        if len(cells) != len(header):
            raise ValueError('incomplete probe row')
        row = dict(zip(header, map(float, cells)))
        if any(not math.isfinite(row[key]) or row[key] < 0 for key in required):
            raise ValueError('invalid probe value')
        if first <= row['line'] <= last:
            rows.append(row)
    if [r['line'] for r in rows] != list(range(first, last + 1)):
        raise ValueError('sample window is missing, duplicated, or out of order')
    return {
        'first_sample': first, 'last_sample': last, 'samples': len(rows),
        'mean_presents_per_second': mean(r['presents'] for r in rows),
        'minimum_presents_per_second': min(r['presents'] for r in rows),
        'maximum_presents_per_second': max(r['presents'] for r in rows),
        'mean_present_ms': mean(r['present_mean_us'] for r in rows) / 1000,
        'worst_present_ms': max(r['present_max_us'] for r in rows) / 1000,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('--first', type=int, default=69)
    parser.add_argument('--last', type=int, default=92)
    args = parser.parse_args()
    try:
        result = summarize(args.probe.read_text().splitlines(), args.first, args.last)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
