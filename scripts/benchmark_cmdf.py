#!/usr/bin/env python3
import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path


def run_checked(cmd, root):
    proc = subprocess.run(
        cmd,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        if proc.stdout:
            sys.stderr.write(proc.stdout)
        if proc.stderr:
            sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)


def time_command(cmd, root):
    started = time.perf_counter()
    proc = subprocess.run(
        cmd,
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        if proc.stderr:
            sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)
    return elapsed


def summarize(samples):
    return {
        "min": min(samples),
        "median": statistics.median(samples),
        "mean": statistics.fmean(samples),
        "max": max(samples),
    }


def fmt_seconds(value):
    return f"{value:.6f}"


def print_table(results):
    print("| impl | mode | median_s | mean_s | min_s | max_s | rounds |")
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |")
    for result in results:
        stats = result["stats"]
        print(
            "| {impl} | {mode} | {median} | {mean} | {min_} | {max_} | {rounds} |".format(
                impl=result["impl"],
                mode=result["mode"],
                median=fmt_seconds(stats["median"]),
                mean=fmt_seconds(stats["mean"]),
                min_=fmt_seconds(stats["min"]),
                max_=fmt_seconds(stats["max"]),
                rounds=result["rounds"],
            )
        )


def print_ratios(results):
    by_key = {(result["impl"], result["mode"]): result for result in results}
    print()
    print("| ratio | value |")
    print("| --- | ---: |")
    for impl in ("C", "Lua"):
        html = by_key.get((impl, "html"))
        deck = by_key.get((impl, "deck"))
        if html and deck and html["stats"]["median"] > 0:
            value = deck["stats"]["median"] / html["stats"]["median"]
            print(f"| {impl} deck/html median | {value:.2f}x |")
    for mode in ("ansi", "html", "deck"):
        c = by_key.get(("C", mode))
        lua = by_key.get(("Lua", mode))
        if c and lua and c["stats"]["median"] > 0:
            value = lua["stats"]["median"] / c["stats"]["median"]
            print(f"| Lua/C {mode} median | {value:.2f}x |")


def benchmark_case(case, root, rounds, warmups):
    cmd = case["cmd"]
    for _ in range(warmups):
        time_command(cmd, root)
    samples = [time_command(cmd, root) for _ in range(rounds)]
    return {
        "impl": case["impl"],
        "mode": case["mode"],
        "cmd": cmd,
        "rounds": rounds,
        "warmups": warmups,
        "samples": samples,
        "stats": summarize(samples),
    }


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Benchmark cmdf and cmdf.lua UX paths on a Markdown fixture."
    )
    parser.add_argument(
        "--input",
        default="testdata/deck-corpus/comprehensive.md",
        help="Markdown fixture to render",
    )
    parser.add_argument("--rounds", type=int, default=10, help="timed rounds per case")
    parser.add_argument("--warmups", type=int, default=2, help="warmup rounds per case")
    parser.add_argument("--no-build", action="store_true", help="skip build/prep steps")
    parser.add_argument("--no-lua", action="store_true", help="skip cmdf.lua benchmarks")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of tables")
    args = parser.parse_args()

    if args.rounds < 1:
        raise SystemExit("--rounds must be >= 1")
    if args.warmups < 0:
        raise SystemExit("--warmups must be >= 0")

    fixture = (root / args.input).resolve()
    if not fixture.is_file():
        raise SystemExit(f"benchmark input not found: {fixture}")

    if not args.no_build:
        run_checked([str(root / "scripts/build.sh"), "debug"], root)
        if not args.no_lua:
            run_checked([str(root / "scripts/build_lua_rock.sh")], root)
            run_checked([str(root / "scripts/generate_cmdf_lua.sh"), str(root / "build/luarocks/cmdf.lua")], root)
            run_checked([str(root / "scripts/generate_cmdf_dev_sh.sh"), str(root / "build/cmdf.sh")], root)

    cmdf = root / "build/debug/cmdf"
    if not cmdf.is_file():
        raise SystemExit(f"cmdf not found: {cmdf}")

    cases = [
        {"impl": "C", "mode": "ansi", "cmd": [str(cmdf), "-b", str(fixture)]},
        {"impl": "C", "mode": "html", "cmd": [str(cmdf), "--html", str(fixture)]},
        {"impl": "C", "mode": "deck", "cmd": [str(cmdf), "--deck", str(fixture)]},
    ]

    if not args.no_lua:
        cmdf_lua = root / "build/cmdf.sh"
        if not cmdf_lua.is_file():
            raise SystemExit(f"cmdf.lua wrapper not found: {cmdf_lua}")
        cases.extend(
            [
                {"impl": "Lua", "mode": "ansi", "cmd": [str(cmdf_lua), "-b", str(fixture)]},
                {"impl": "Lua", "mode": "html", "cmd": [str(cmdf_lua), "--html", str(fixture)]},
                {"impl": "Lua", "mode": "deck", "cmd": [str(cmdf_lua), "--deck", str(fixture)]},
            ]
        )

    results = []
    for case in cases:
        print(f"benchmarking {case['impl']} {case['mode']}...", file=sys.stderr)
        results.append(benchmark_case(case, root, args.rounds, args.warmups))

    payload = {
        "input": str(fixture),
        "rounds": args.rounds,
        "warmups": args.warmups,
        "results": results,
    }
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"input: {fixture}")
        print_table(results)
        print_ratios(results)


if __name__ == "__main__":
    main()
