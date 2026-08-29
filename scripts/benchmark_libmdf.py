#!/usr/bin/env python3
import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path


def run_checked(cmd, root, env=None):
    proc = subprocess.run(
        cmd,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    if proc.returncode != 0:
        if proc.stdout:
            sys.stderr.write(proc.stdout)
        if proc.stderr:
            sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)


def time_command(cmd, root, env=None):
    started = time.perf_counter()
    proc = subprocess.run(
        cmd,
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
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


def print_table(results):
    print("| impl | mode | median_s | mean_s | min_s | max_s | rounds |")
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |")
    for result in results:
        stats = result["stats"]
        print(
            "| {impl} | {mode} | {median:.6f} | {mean:.6f} | {min:.6f} | {max:.6f} | {rounds} |".format(
                impl=result["impl"],
                mode=result["mode"],
                median=stats["median"],
                mean=stats["mean"],
                min=stats["min"],
                max=stats["max"],
                rounds=result["rounds"],
            )
        )


def print_ratios(results):
    by_key = {(result["impl"], result["mode"]): result for result in results}
    print()
    print("| ratio | value |")
    print("| --- | ---: |")
    for mode in ("ansi", "html"):
        c = by_key.get(("C API", mode))
        lua = by_key.get(("Lua API", mode))
        if c and lua and c["stats"]["median"] > 0:
            print(f"| Lua API/C API {mode} median | {lua['stats']['median'] / c['stats']['median']:.2f}x |")


def benchmark_case(case, root, rounds, warmups, repeats):
    for _ in range(warmups):
        time_command(case["cmd"], root, case.get("env"))
    samples = [time_command(case["cmd"], root, case.get("env")) / repeats for _ in range(rounds)]
    return {
        "impl": case["impl"],
        "mode": case["mode"],
        "cmd": case["cmd"],
        "rounds": rounds,
        "warmups": warmups,
        "samples": samples,
        "stats": summarize(samples),
    }


def lua_env(root):
    env = os.environ.copy()
    tree = root / "build/luarocks/tree"
    prefix = root / "build/luarocks/libmdf-prefix"
    debug_lib = root / "build/debug"
    env["LUA_PATH"] = f"{tree}/share/lua/5.5/?.lua;{tree}/share/lua/5.5/?/init.lua;;"
    env["LUA_CPATH"] = f"{tree}/lib/lua/5.5/?.so;;"
    existing = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = f"{debug_lib}:{prefix}/lib" + (f":{existing}" if existing else "")
    return env


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Benchmark libmdf C API and Lua binding render paths.")
    parser.add_argument("--input", default="testdata/code-review-is-a-dead-end.md")
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--repeat", type=int, default=1, help="real renders per timed sample")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-lua", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if args.rounds < 1:
        raise SystemExit("--rounds must be >= 1")
    if args.warmups < 0:
        raise SystemExit("--warmups must be >= 0")
    if args.repeat < 1:
        raise SystemExit("--repeat must be >= 1")

    fixture = (root / args.input).resolve()
    if not fixture.is_file():
        raise SystemExit(f"benchmark input not found: {fixture}")

    if not args.no_build:
        run_checked([str(root / "scripts/build.sh"), "debug"], root)
        if not args.no_lua:
            run_checked([str(root / "scripts/build_lua_rock.sh")], root)

    render = root / "build/debug/golden_render"
    if not render.is_file():
        raise SystemExit(f"libmdf benchmark renderer not found: {render}")

    cases = [
        {"impl": "C API", "mode": "ansi", "cmd": [str(render), "--ansi", "-w", "80", "--repeat", str(args.repeat), str(fixture)]},
        {"impl": "C API", "mode": "html", "cmd": [str(render), "--html", "--repeat", str(args.repeat), str(fixture)]},
    ]

    if not args.no_lua:
        env = lua_env(root)
        lua_render = root / "scripts/benchmark_lua_render.lua"
        cases.extend(
            [
                {"impl": "Lua API", "mode": "ansi", "cmd": ["lua", str(lua_render), "--ansi", "-w", "80", "--repeat", str(args.repeat), str(fixture)], "env": env},
                {"impl": "Lua API", "mode": "html", "cmd": ["lua", str(lua_render), "--html", "--repeat", str(args.repeat), str(fixture)], "env": env},
            ]
        )

    results = []
    for case in cases:
        print(f"benchmarking {case['impl']} {case['mode']}...", file=sys.stderr)
        results.append(benchmark_case(case, root, args.rounds, args.warmups, args.repeat))

    payload = {"input": str(fixture), "rounds": args.rounds, "warmups": args.warmups, "repeat": args.repeat, "results": results}
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"input: {fixture}")
        print_table(results)
        print_ratios(results)


if __name__ == "__main__":
    main()
