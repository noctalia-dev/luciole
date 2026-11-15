#!/usr/bin/env python3
"""Build or remove the benchmark fixtures used by gui_bench.py and transfer_bench.py.

Every fixture directory carries a marker file; `clean` removes only marked directories.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

MARKER = ".luciole-bench-fixture"
DEFAULT_ROOT = Path("/var/tmp/luciole-bench/fixtures")
COUNTS = {"files-1": 1, "files-1k": 1_000, "files-10k": 10_000, "files-100k": 100_000}


def filesystem_type(path: Path) -> str:
    result = subprocess.run(["findmnt", "-no", "FSTYPE", "-T", str(path)], capture_output=True, text=True, check=True)
    return result.stdout.strip()


def prepare(root: Path, name: str) -> Path | None:
    target = root / name
    if (target / MARKER).exists():
        return None
    if target.exists():
        sys.exit(f"fixtures: {target} exists without {MARKER}; refusing to touch it")
    target.mkdir(parents=True)
    return target


def finish(target: Path) -> None:
    (target / MARKER).write_text("luciole benchmark fixture\n")


def build_empty_files(root: Path) -> None:
    if (target := prepare(root, "empty")) is not None:
        finish(target)
    for name, count in COUNTS.items():
        target = prepare(root, name)
        if target is None:
            continue
        for index in range(count):
            (target / f"file-{index:06d}.txt").touch()
        finish(target)


def run(*command: str) -> None:
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def copy(source: Path, destination: Path) -> None:
    run("cp", "--reflink=never", str(source), str(destination))


def build_mixed(root: Path) -> None:
    target = prepare(root, "mixed-2k")
    if target is None:
        return
    seeds = target / ".seeds"
    seeds.mkdir()
    run("magick", "-size", "3000x2000", "plasma:fractal", "-quality", "90", str(seeds / "seed.jpg"))
    run("magick", "-size", "1024x768", "gradient:red-blue", str(seeds / "seed.png"))
    run("magick", "-size", "1600x1200", "plasma:green-yellow", str(seeds / "seed.webp"))
    run("ffmpeg", "-y", "-f", "lavfi", "-i", "testsrc=duration=5:size=1280x720:rate=25", "-pix_fmt", "yuv420p",
        str(seeds / "seed.mp4"))
    for index in range(500):
        copy(seeds / "seed.jpg", target / f"photo-{index:04d}.jpg")
        copy(seeds / "seed.png", target / f"image-{index:04d}.png")
    for index in range(400):
        copy(seeds / "seed.webp", target / f"picture-{index:04d}.webp")
    for index in range(200):
        copy(seeds / "seed.mp4", target / f"clip-{index:04d}.mp4")
    for index in range(100):
        (target / f"note-{index:04d}.txt").write_text(f"Note {index}\n" * 20)
        (target / f"übersicht-日本語-{index:04d}.txt").write_text(f"Unicode note {index}\n")
        long_name = f"very-long-file-name-that-keeps-going-for-quite-a-while-to-test-wrapping-{index:04d}.md"
        (target / long_name).write_text(f"# Long {index}\n")
    for index in range(50):
        (target / f"README-{index:04d}").write_text("Plain text without an extension.\n" * 5)
    for index in range(40):
        (target / f"link-{index:04d}").symlink_to(f"note-{index:04d}.txt")
    for index in range(10):
        (target / f"folder-{index:02d}").mkdir()
    shutil.rmtree(seeds)
    finish(target)


def random_file(path: Path, size: int) -> None:
    with open("/dev/urandom", "rb") as source, open(path, "wb") as output:
        remaining = size
        while remaining > 0:
            chunk = source.read(min(remaining, 8 << 20))
            output.write(chunk)
            remaining -= len(chunk)


def build_transfer(root: Path) -> None:
    if (target := prepare(root, "copy-large")) is not None:
        random_file(target / "large.bin", 2 << 30)
        finish(target)
    if (target := prepare(root, "copy-small")) is not None:
        for folder in range(100):
            directory = target / "tree" / f"dir-{folder:03d}"
            directory.mkdir(parents=True)
            for index in range(100):
                (directory / f"small-{index:03d}.bin").write_bytes(os.urandom(4096))
        finish(target)
    if (target := prepare(root, "move-many")) is not None:
        payload = os.urandom(1 << 20)
        directory = target / "items"
        directory.mkdir()
        for index in range(1000):
            (directory / f"item-{index:04d}.bin").write_bytes(payload)
        finish(target)


def clean(root: Path) -> None:
    if not root.exists():
        return
    for child in root.iterdir():
        if (child / MARKER).exists():
            shutil.rmtree(child)
        else:
            print(f"fixtures: leaving unmarked {child}", file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["build", "clean"])
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()
    root: Path = args.root.resolve()
    if args.command == "clean":
        clean(root)
        return
    root.mkdir(parents=True, exist_ok=True)
    if filesystem_type(root) == "tmpfs":
        sys.exit("fixtures: refusing a tmpfs root; page-cache state there is not representative")
    build_empty_files(root)
    build_mixed(root)
    build_transfer(root)


if __name__ == "__main__":
    main()
