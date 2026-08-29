#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
from pathlib import Path


def load_baseline(path):
    with path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    required = {}
    for result in data.get("results", []):
        key = (result.get("impl"), result.get("mode"))
        median = result.get("median_s")
        if key[0] is None or key[1] is None or median is None:
            raise SystemExit(f"invalid baseline entry in {path}")
        required[key] = float(median)
    if not required:
        raise SystemExit(f"empty benchmark baseline: {path}")
    return data, required


def run_benchmark(root, baseline, args, no_build):
    cmd = [
        str(root / "scripts/benchmark_libmdf.py"),
        "--input",
        baseline["input"],
        "--rounds",
        str(args.rounds),
        "--warmups",
        str(args.warmups),
        "--repeat",
        str(args.repeat),
        "--json",
    ]
    if no_build:
        cmd.append("--no-build")
    if args.no_lua:
        cmd.append("--no-lua")
    proc = subprocess.run(
        cmd,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        if proc.stdout:
            sys.stderr.write(proc.stdout)
        raise SystemExit(proc.returncode)
    return json.loads(proc.stdout)


def result_medians(payload):
    medians = {}
    for result in payload.get("results", []):
        medians[(result.get("impl"), result.get("mode"))] = float(result["stats"]["median"])
    return medians


def format_key(key):
    return f"{key[0]} {key[1]}"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Fail when libmdf benchmark medians regress beyond baseline allowance.")
    parser.add_argument("--baseline", default="testdata/benchmarks/libmdf-baseline.json")
    parser.add_argument("--allowance", type=float, default=None, help="allowed regression fraction; default comes from baseline")
    parser.add_argument("--rounds", type=int, default=None, help="benchmark rounds; default comes from baseline")
    parser.add_argument("--warmups", type=int, default=None, help="benchmark warmups; default comes from baseline")
    parser.add_argument("--repeat", type=int, default=None, help="real renders per sample; default comes from baseline")
    parser.add_argument("--attempts", type=int, default=3, help="number of serialized attempts; best median per path is compared")
    parser.add_argument("--no-lua", action="store_true", help="check only C API paths")
    args = parser.parse_args()

    baseline_path = (root / args.baseline).resolve()
    baseline, expected = load_baseline(baseline_path)
    allowance = baseline.get("allowance", 0.05) if args.allowance is None else args.allowance
    args.rounds = int(baseline.get("rounds", 200) if args.rounds is None else args.rounds)
    args.warmups = int(baseline.get("warmups", 20) if args.warmups is None else args.warmups)
    args.repeat = int(baseline.get("repeat", 1) if args.repeat is None else args.repeat)
    if allowance < 0:
        raise SystemExit("--allowance must be >= 0")
    if args.rounds < 1:
        raise SystemExit("--rounds must be >= 1")
    if args.warmups < 0:
        raise SystemExit("--warmups must be >= 0")
    if args.repeat < 1:
        raise SystemExit("--repeat must be >= 1")
    if args.attempts < 1:
        raise SystemExit("--attempts must be >= 1")
    if args.no_lua:
        expected = {key: value for key, value in expected.items() if key[0] != "Lua API"}

    print(
        f"benchmark baseline: {baseline_path.relative_to(root)}; "
        f"rounds={args.rounds}; warmups={args.warmups}; repeat={args.repeat}; attempts={args.attempts}; allowance={allowance:.1%}",
        file=sys.stderr,
    )
    best = {}
    for attempt in range(args.attempts):
        payload = run_benchmark(root, baseline, args, no_build=attempt > 0)
        medians = result_medians(payload)
        for key, median in medians.items():
            if key in expected and (key not in best or median < best[key]):
                best[key] = median

    missing = sorted(key for key in expected if key not in best)
    failures = []
    for key, baseline_median in sorted(expected.items()):
        if key in best:
            limit = baseline_median * (1.0 + allowance)
            if best[key] > limit:
                failures.append((key, baseline_median, limit, best[key]))

    print("| impl | mode | baseline_s | limit_s | best_s | status |")
    print("| --- | --- | ---: | ---: | ---: | --- |")
    for key, baseline_median in sorted(expected.items()):
        limit = baseline_median * (1.0 + allowance)
        actual = best.get(key)
        if actual is None:
            print(f"| {key[0]} | {key[1]} | {baseline_median:.6f} | {limit:.6f} | n/a | missing |")
        else:
            status = "ok" if actual <= limit else "regressed"
            print(f"| {key[0]} | {key[1]} | {baseline_median:.6f} | {limit:.6f} | {actual:.6f} | {status} |")

    if missing:
        for key in missing:
            print(f"missing benchmark result: {format_key(key)}", file=sys.stderr)
    if failures:
        for key, baseline_median, limit, actual in failures:
            print(
                f"benchmark regression: {format_key(key)} median {actual:.6f}s exceeds "
                f"{baseline_median:.6f}s baseline + {allowance:.1%} ({limit:.6f}s)",
                file=sys.stderr,
            )
    if missing or failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
