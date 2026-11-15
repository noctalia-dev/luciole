#!/usr/bin/env python3
"""Launch Luciole builds in a private headless Umbriel and measure startup, CPU and memory.

Each sample runs in its own systemd user scope with a private D-Bus session bus and private
XDG directories, so the user's desktop, settings and caches are never touched.
"""

import argparse
import json
import os
import random
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

BENCH_ROOT = Path("/var/tmp/luciole-bench")
FIXTURES = BENCH_ROOT / "fixtures"
REPO = Path(__file__).resolve().parents[2]
SAMPLE_INTERVAL = 0.010
PSS_INTERVAL = 0.100
QUIET_WINDOW_NS = 500_000_000
# CPU growth below this many microseconds during the quiet window counts as quiet.
QUIET_THRESHOLD_USEC = 1000
SETTLE_TIMEOUT = 60.0


# Built-in launch description for Luciole builds. A --profile file adds other kinds with the same keys:
#   command: argv with {binary} and {dir} placeholders
#   views:   view names the program supports (others are skipped)
#   files:   per view, files to write relative to the sample profile (home, config, data, state)
#   env:     extra environment; "{repo}" expands to this checkout
LUCIOLE = {
    "command": ["{binary}", "{dir}"],
    "views": ["list", "grid"],
    "files": {view: {"config/luciole/config.toml": f'[browser]\nview_mode = "{view}"\n'} for view in ("list", "grid")},
    "env": {"LUCIOLE_ASSETS_DIR": "{repo}/lib/assets", "LUCIOLE_STARTUP_TRACE": "1"},
}


@dataclass
class App:
    name: str
    binary: str
    kind: dict

    def views(self, requested: list[str]) -> list[str]:
        return [view for view in requested if view in self.kind.get("views", requested)]

    def command(self, folder: Path) -> list[str]:
        return [part.format(binary=self.binary, dir=folder) for part in self.kind["command"]]

    def configure(self, profile: dict[str, Path], view: str) -> dict[str, str]:
        for relative, contents in self.kind.get("files", {}).get(view, {}).items():
            root, _, rest = relative.partition("/")
            path = profile[root] / rest
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        return {key: value.replace("{repo}", str(REPO)) for key, value in self.kind.get("env", {}).items()}


@dataclass
class Compositor:
    runtime: Path
    process: subprocess.Popen | None = None
    subscriber: subprocess.Popen | None = None
    events: list[tuple[int, bool]] = field(default_factory=list)
    lock: threading.Lock = field(default_factory=threading.Lock)
    mode: str = ""

    def env(self) -> dict[str, str]:
        return {
            "PATH": os.environ["PATH"],
            "HOME": os.environ["HOME"],
            "XDG_RUNTIME_DIR": str(self.runtime),
            "UMBRIEL_SOCKET": str(self.runtime / "umbriel-wayland-0.sock"),
            "WLR_BACKENDS": "headless",
        }

    def start(self) -> None:
        if self.runtime.exists():
            shutil.rmtree(self.runtime)
        self.runtime.mkdir(parents=True)
        self.runtime.chmod(0o700)
        config = self.runtime / "umbriel.conf"
        # No startup cheatsheet overlay and no Xwayland: nothing but the measured client draws or wakes.
        config.write_text("[general]\nshow_cheatsheet = false\nxwayland = false\n")
        log = open(self.runtime / "umbriel.log", "wb")
        self.process = subprocess.Popen(["umbriel", "-c", str(config)], env=self.env(), stdout=log, stderr=log)
        deadline = time.monotonic() + 15
        socket = self.runtime / "umbriel-wayland-0.sock"
        while not (socket.exists() and (self.runtime / "wayland-0").exists()):
            if time.monotonic() > deadline or self.process.poll() is not None:
                raise RuntimeError(f"umbriel did not start; see {self.runtime / 'umbriel.log'}")
            time.sleep(0.05)
        time.sleep(0.5)
        outputs = subprocess.run(["umbriel", "outputs"], env=self.env(), capture_output=True, text=True, check=True)
        lines = [line.strip() for line in outputs.stdout.splitlines()]
        self.mode = " ".join(line for index, line in enumerate(lines)
                             if line.startswith("HEADLESS") or (index > 0 and lines[index - 1].startswith("Modes")))
        self.subscriber = subprocess.Popen(
            ["umbriel", "subscribe", "windows"], env=self.env(), stdout=subprocess.PIPE, text=True
        )
        threading.Thread(target=self._read_events, daemon=True).start()

    def _read_events(self) -> None:
        assert self.subscriber is not None and self.subscriber.stdout is not None
        for line in self.subscriber.stdout:
            now = time.monotonic_ns()
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("event") == "windows":
                with self.lock:
                    self.events.append((now, bool(event.get("data"))))

    def first_window_after(self, start: int) -> int | None:
        with self.lock:
            for stamp, nonempty in self.events:
                if stamp >= start and nonempty:
                    return stamp
        return None

    def wait_no_windows(self, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = subprocess.run(["umbriel", "windows"], env=self.env(), capture_output=True, text=True)
            if not result.stdout.strip():
                return
            time.sleep(0.05)

    def screenshot(self, path: Path) -> None:
        env = self.env() | {"WAYLAND_DISPLAY": "wayland-0"}
        subprocess.run(["grim", str(path)], env=env, check=False)

    def stop(self) -> None:
        for process in (self.subscriber, self.process):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()


def cgroup_dir(pid: int) -> Path | None:
    try:
        text = Path(f"/proc/{pid}/cgroup").read_text()
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith("0::"):
            return Path("/sys/fs/cgroup" + line[3:])
    return None


def cpu_usec(cgroup: Path) -> int | None:
    try:
        for line in (cgroup / "cpu.stat").read_text().splitlines():
            if line.startswith("usage_usec"):
                return int(line.split()[1])
    except OSError:
        return None
    return None


def procs(cgroup: Path) -> list[int]:
    try:
        return [int(value) for value in (cgroup / "cgroup.procs").read_text().split()]
    except OSError:
        return []


def pss_kib(pid: int) -> int | None:
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1])
    except OSError:
        return None
    return None


def process_detail(pid: int) -> dict:
    try:
        name = Path(f"/proc/{pid}/comm").read_text().strip()
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])
    except (OSError, IndexError, ValueError):
        return {"pid": pid, "name": "?", "cpu_s": None, "pss_kib": None}
    return {"pid": pid, "name": name, "cpu_s": ticks / os.sysconf("SC_CLK_TCK"), "pss_kib": pss_kib(pid)}


# Desktop services a private session bus starts on demand; a running desktop already has them, shared.
SERVICE_PREFIXES = ("dbus-", "xdg-", "gvfs", "at-spi", "fusermount")


def own_process(pid: int) -> bool:
    try:
        name = Path(f"/proc/{pid}/comm").read_text().strip()
    except OSError:
        return False
    return not name.startswith(SERVICE_PREFIXES)


def wait_for_idle_machine(limit: float) -> None:
    deadline = time.monotonic() + 600
    while os.getloadavg()[0] >= limit and time.monotonic() < deadline:
        print(f"waiting for load average below {limit} (now {os.getloadavg()[0]:.2f})", file=sys.stderr)
        time.sleep(10)


def make_profile(root: Path, cache: Path | None) -> dict[str, Path]:
    if root.exists():
        shutil.rmtree(root)
    profile = {name: root / name for name in ("home", "config", "data", "state")}
    profile["cache"] = cache if cache is not None else root / "cache"
    for path in profile.values():
        path.mkdir(parents=True, exist_ok=True)
    return profile


def run_sample(app: App, compositor: Compositor, fixture: str, view: str, args, out: Path, label: str) -> dict:
    wait_for_idle_machine(args.load_limit)
    sample_id = uuid.uuid4().hex[:12]
    persistent = BENCH_ROOT / "cache" / app.name
    cold_thumbnails = fixture.startswith("mixed")
    profile = make_profile(BENCH_ROOT / "profiles" / sample_id, None if cold_thumbnails else persistent / "xdg-cache")
    shader_cache = persistent / "mesa-shader-cache"
    shader_cache.mkdir(parents=True, exist_ok=True)
    env = compositor.env() | {
        "WAYLAND_DISPLAY": "wayland-0",
        "HOME": str(profile["home"]),
        "XDG_CONFIG_HOME": str(profile["config"]),
        "XDG_DATA_HOME": str(profile["data"]),
        "XDG_STATE_HOME": str(profile["state"]),
        "XDG_CACHE_HOME": str(profile["cache"]),
        "MESA_SHADER_CACHE_DIR": str(shader_cache),
        "LANG": os.environ.get("LANG", "C.UTF-8"),
    }
    env.pop("UMBRIEL_SOCKET")
    env.pop("WLR_BACKENDS")
    env |= app.configure(profile, view)
    command = app.command(FIXTURES / fixture)
    if args.thp_off:
        command = [sys.executable, str(Path(__file__).with_name("thp_off.py")), *command]
    unit = f"lbench-{sample_id}"
    launch = ["systemd-run", "--user", "--scope", "--quiet", "--collect", f"--unit={unit}",
              "env", "-i", *[f"{key}={value}" for key, value in env.items()],
              "dbus-run-session", "--", *command]
    log_path = out / "logs" / f"{label}-{sample_id}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = open(log_path, "wb")
    start = time.monotonic_ns()
    process = subprocess.Popen(launch, stdout=log, stderr=log)
    cgroup = None
    while cgroup is None or not cgroup.name.startswith(unit):
        if process.poll() is not None:
            break
        cgroup = cgroup_dir(process.pid)
        time.sleep(0.001)
    result: dict = {"app": app.name, "fixture": fixture, "view": view, "sample": sample_id,
                    "label": label, "thp_off": args.thp_off}
    if cgroup is None or process.poll() is not None:
        result["error"] = "process exited before its scope was observed"
        log.close()
        return result
    anchor_time, anchor_cpu = start, 0
    peak_pss = 0
    peak_own_pss = 0
    pss_lower_bound = False
    last_pss = 0.0
    settled_at = None
    while True:
        now = time.monotonic_ns()
        usage = cpu_usec(cgroup)
        if usage is None or process.poll() is not None:
            result["error"] = f"process exited early with {process.returncode}"
            break
        if usage - anchor_cpu >= QUIET_THRESHOLD_USEC:
            anchor_time, anchor_cpu = now, usage
        if time.monotonic() - last_pss >= PSS_INTERVAL:
            last_pss = time.monotonic()
            total = 0
            own = 0
            for pid in procs(cgroup):
                value = pss_kib(pid)
                if value is None:
                    pss_lower_bound = True
                else:
                    total += value
                    if own_process(pid):
                        own += value
            peak_pss = max(peak_pss, total)
            peak_own_pss = max(peak_own_pss, own)
        if now - anchor_time >= QUIET_WINDOW_NS and compositor.first_window_after(start) is not None:
            settled_at = anchor_time + QUIET_WINDOW_NS
            break
        if (now - start) / 1e9 > SETTLE_TIMEOUT:
            result["error"] = "did not settle"
            break
        time.sleep(SAMPLE_INTERVAL)
    result["cpu_s"] = (cpu_usec(cgroup) or 0) / 1e6
    result["peak_pss_mib"] = peak_pss / 1024
    result["peak_own_pss_mib"] = peak_own_pss / 1024
    result["pss_lower_bound"] = pss_lower_bound
    window = compositor.first_window_after(start)
    result["first_window_ms"] = (window - start) / 1e6 if window else None
    if settled_at is not None:
        result["settled_s"] = (settled_at - start) / 1e9
        result["last_activity_s"] = (settled_at - QUIET_WINDOW_NS - start) / 1e9
        result["processes"] = [process_detail(pid) for pid in procs(cgroup)]
        result["own_cpu_s"] = sum(detail["cpu_s"] or 0 for detail in result["processes"]
                                  if not detail["name"].startswith(SERVICE_PREFIXES))
        if args.screenshot_dir:
            directory = Path(args.screenshot_dir)
            directory.mkdir(parents=True, exist_ok=True)
            compositor.screenshot(directory / f"{label}-{fixture}-{view}-{sample_id}.png")
        if args.idle > 0 and not label.endswith("-warmup"):
            before = cpu_usec(cgroup) or 0
            time.sleep(args.idle)
            result["idle_cpu_ms"] = ((cpu_usec(cgroup) or 0) - before) / 1000
    for pid in procs(cgroup):
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 2
    while procs(cgroup) and time.monotonic() < deadline:
        time.sleep(0.02)
    if procs(cgroup):
        try:
            (cgroup / "cgroup.kill").write_text("1")
        except OSError:
            pass
    try:
        process.wait(5)
    except subprocess.TimeoutExpired:
        process.kill()
    log.close()
    result["trace"] = parse_trace(log_path, start)
    compositor.wait_no_windows()
    shutil.rmtree(BENCH_ROOT / "profiles" / sample_id, ignore_errors=True)
    return result


def parse_trace(path: Path, start: int) -> dict[str, float]:
    events: dict[str, float] = {}
    for line in path.read_text(errors="replace").splitlines():
        if not line.startswith("luciole-trace "):
            continue
        parts = line.split(" ", 2)
        name = parts[2].split(" ", 1)[0]
        events.setdefault(name, (int(parts[1]) - start) / 1e6)
    return events


METRICS = [
    ("first_window_ms", "First window (ms)", "{:.0f}"),
    ("content_presented_ms", "Content presented (ms)", "{:.0f}"),
    ("settled_s", "Settled (s)", "{:.3f}"),
    ("last_activity_s", "Last activity (s)", "{:.3f}"),
    ("cpu_s", "CPU (s)", "{:.3f}"),
    ("own_cpu_s", "Own CPU (s)", "{:.2f}"),
    ("peak_pss_mib", "Peak PSS (MiB)", "{:.1f}"),
    ("peak_own_pss_mib", "Own peak PSS (MiB)", "{:.1f}"),
    ("idle_cpu_ms", "Idle CPU (ms)", "{:.1f}"),
]


def summarize(samples: list[dict], mode: str) -> str:
    groups: dict[tuple[str, str, str], list[dict]] = {}
    for sample in samples:
        if sample.get("warmup"):
            continue
        sample["content_presented_ms"] = sample.get("trace", {}).get("content-presented")
        groups.setdefault((sample["fixture"], sample["view"], sample["app"]), []).append(sample)
    lines = [f"Compositor output: {mode}", "",
             "| Fixture | View | App | n | " + " | ".join(title for _, title, _ in METRICS) + " | Flags |",
             "| --- | --- | --- | ---: | " + " | ".join("---:" for _ in METRICS) + " | --- |"]
    for (fixture, view, app), group in sorted(groups.items()):
        cells = []
        for key, _, fmt in METRICS:
            values = [sample[key] for sample in group if sample.get(key) is not None]
            if not values:
                cells.append("-")
                continue
            cells.append(f"{fmt.format(statistics.median(values))} ({fmt.format(min(values))}-{fmt.format(max(values))})")
        flags = []
        if any(sample.get("pss_lower_bound") for sample in group):
            flags.append("PSS lower bound")
        errors = [sample["error"] for sample in group if sample.get("error")]
        if errors:
            flags.append(f"{len(errors)} errors: {errors[0]}")
        lines.append(f"| {fixture} | {view} | {app} | {len(group)} | " + " | ".join(cells) + f" | {', '.join(flags)} |")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", action="append", required=True,
                        help="NAME=BINARY for a Luciole build, or NAME=KIND:BINARY for a kind from --profile")
    parser.add_argument("--profile", type=Path, help="JSON file mapping extra kind names to launch descriptions")
    parser.add_argument("--fixture", action="append", required=True)
    parser.add_argument("--view", default="list,grid")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--idle", type=float, default=0)
    parser.add_argument("--thp-off", action="store_true")
    parser.add_argument("--screenshot-dir")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--load-limit", type=float, default=3.0, help="wait while the 1-minute load average is higher")
    args = parser.parse_args()
    kinds = {"luciole": LUCIOLE}
    if args.profile:
        kinds |= json.loads(args.profile.read_text())
    apps = []
    for spec in args.app:
        name, binary = spec.split("=", 1)
        kind = "luciole"
        if ":" in binary and binary.split(":", 1)[0] in kinds:
            kind, binary = binary.split(":", 1)
        apps.append(App(name, binary, kinds[kind]))
    views = args.view.split(",")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out: Path = args.out or BENCH_ROOT / "results" / stamp
    out.mkdir(parents=True, exist_ok=True)
    compositor = Compositor(BENCH_ROOT / "run" / stamp)
    samples: list[dict] = []

    def interrupted(*_):
        compositor.stop()
        sys.exit(130)

    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        compositor.start()
        for app in apps:
            warm = run_sample(app, compositor, args.fixture[0], app.views(views)[0], args, out, f"{app.name}-warmup")
            warm["warmup"] = True
            samples.append(warm)
        for fixture in args.fixture:
            for run in range(args.runs):
                combos = [(app, view) for app in apps for view in app.views(views)]
                random.shuffle(combos)
                for app, view in combos:
                    sample = run_sample(app, compositor, fixture, view, args, out, app.name)
                    sample["run"] = run
                    samples.append(sample)
                    print(json.dumps({key: sample.get(key) for key in
                                      ("app", "fixture", "view", "first_window_ms", "settled_s", "cpu_s",
                                       "peak_pss_mib", "error")}), flush=True)
                (out / "samples.json").write_text(json.dumps(samples, indent=1))
    finally:
        compositor.stop()
    (out / "samples.json").write_text(json.dumps(samples, indent=1))
    summary = summarize(samples, compositor.mode)
    (out / "summary.md").write_text(summary)
    print(summary)
    print(f"results: {out}")


if __name__ == "__main__":
    main()
