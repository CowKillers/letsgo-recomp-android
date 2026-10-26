"""Summarize LARECOMP_FRAMETIME_CSV game-present intervals (nearest-rank percentiles).

Usage: python tools/frametime_summary.py build/frametimes.csv --from-present 1000
The 60 fps menu's extra image is not a game present and is not included.
"""
import argparse
import csv
import math


def percentile(values, percent):
    return values[math.ceil(len(values) * percent / 100) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_path")
    parser.add_argument("--from-present", type=int, default=0)
    args = parser.parse_args()
    with open(args.csv_path, newline="") as stream:
        rows = csv.DictReader(stream)
        samples = [float(row["dt_ms"]) for row in rows if int(row["present"]) >= args.from_present]
    if not samples:
        parser.error("no samples in the selected present range")
    ordered = sorted(samples)
    mean = sum(samples) / len(samples)
    print(f"{len(samples)} presents; mean {mean:.3f} ms ({1000 / mean:.2f} fps); "
          f"P95 {percentile(ordered, 95):.3f} ms; P99 {percentile(ordered, 99):.3f} ms; "
          f"max {ordered[-1]:.3f} ms; >50 ms {sum(t > 50 for t in samples)}")


if __name__ == "__main__":
    main()
