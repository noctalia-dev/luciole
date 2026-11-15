#!/usr/bin/env python3
"""Compare Luciole's TransferService with cp/mv on the copy and move fixtures.

Page cache stays warm (no root access); every scenario resets its destination outside the timed region.
"""

import argparse
import json
import shutil
import statistics
import subprocess
import time
from pathlib import Path

BENCH_ROOT = Path("/var/tmp/luciole-bench")
FIXTURES = BENCH_ROOT / "fixtures"
SAME_FS = BENCH_ROOT / "dest"
CROSS_FS = Path("/dev/shm/luciole-bench-dest")


def reset(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def timed(command: list[str]) -> float:
    start = time.monotonic()
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    return (time.monotonic() - start) * 1000


def luciole(bench: str, action: str, sources: list[Path], destination: Path) -> float:
    result = subprocess.run([bench, "--action", action, "--destination", str(destination), *map(str, sources)],
                            check=True, capture_output=True, text=True)
    report = json.loads(result.stdout)
    if list(report["outcomes"].values()) != [len(sources)]:
        raise RuntimeError(f"unexpected outcomes {report}")
    return report["wall_ms"]


def copy_scenarios(bench: str, runs: int) -> dict[str, list[float]]:
    results: dict[str, list[float]] = {}
    for fixture, source in (("copy-large", FIXTURES / "copy-large" / "large.bin"),
                            ("copy-small", FIXTURES / "copy-small" / "tree")):
        for place, destination in (("same-fs", SAME_FS), ("cross-fs", CROSS_FS)):
            for tool in ("luciole", "cp-reflink-auto", "cp-reflink-never"):
                key = f"copy {fixture} {place} {tool}"
                for _ in range(runs):
                    reset(destination)
                    subprocess.run(["sync"], check=True)
                    if tool == "luciole":
                        value = luciole(bench, "copy", [source], destination)
                    else:
                        reflink = "auto" if tool == "cp-reflink-auto" else "never"
                        value = timed(["cp", "-a", f"--reflink={reflink}", str(source), str(destination)])
                    results.setdefault(key, []).append(value)
    return results


def move_scenarios(bench: str, runs: int) -> dict[str, list[float]]:
    results: dict[str, list[float]] = {}
    origin = FIXTURES / "move-many" / "items"
    items = sorted(origin.iterdir())
    for place, staging, destination in (("same-fs", BENCH_ROOT / "move-source", SAME_FS),
                                         ("cross-fs", CROSS_FS.parent / "luciole-bench-move-source", SAME_FS)):
        for tool in ("luciole", "mv"):
            key = f"move move-many {place} {tool}"
            for _ in range(runs):
                reset(staging)
                reset(destination)
                for item in items:
                    shutil.copyfile(item, staging / item.name)
                subprocess.run(["sync"], check=True)
                sources = sorted(staging.iterdir())
                if tool == "luciole":
                    value = luciole(bench, "move", sources, destination)
                else:
                    value = timed(["mv", "-t", str(destination), *map(str, sources)])
                results.setdefault(key, []).append(value)
            shutil.rmtree(staging, ignore_errors=True)
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", required=True, help="luciole-transfer-bench executable")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scenarios", default="copy,move", help="comma-separated: copy, move")
    args = parser.parse_args()
    selected = args.scenarios.split(",")
    results = (copy_scenarios(args.bench, args.runs) if "copy" in selected else {}) | (
        move_scenarios(args.bench, args.runs) if "move" in selected else {})
    for path in (SAME_FS, CROSS_FS):
        shutil.rmtree(path, ignore_errors=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(results, indent=1))
    print("| Scenario | Median ms | Range ms |")
    print("| --- | ---: | ---: |")
    for key, values in results.items():
        print(f"| {key} | {statistics.median(values):.0f} | {min(values):.0f}-{max(values):.0f} |")
    print("Page cache warm; sources stay cached between runs.")


if __name__ == "__main__":
    main()
