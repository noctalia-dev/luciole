"""Exercise the public CLI without a graphical session."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

BINARY = str(Path(sys.argv.pop(1)).resolve())
VERSION = sys.argv.pop(1)


class CliTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="luciole-cli-test-")
        self.addCleanup(temporary.cleanup)
        self.config_dir = Path(temporary.name)
        self.runtime_dir = self.config_dir / "runtime"
        self.runtime_dir.mkdir(mode=0o700)

    def invoke(self, *args, config_home=None):
        env = dict(os.environ)
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(name, None)
        env["WAYLAND_DISPLAY"] = "/luciole-test-missing-wayland-socket"
        env["XDG_RUNTIME_DIR"] = str(self.runtime_dir)
        env["XDG_CONFIG_HOME"] = str(self.config_dir if config_home is None else config_home)
        return subprocess.run([BINARY, *args], check=False, capture_output=True, text=True, env=env, timeout=5)

    def test_headless_information(self):
        version = self.invoke("--version")
        self.assertEqual(version.returncode, 0, version.stderr)
        self.assertTrue(version.stdout.startswith(f"luciole {VERSION} ("))
        self.assertEqual(version.stderr, "")
        help_result = self.invoke("--help")
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("Usage: luciole", help_result.stdout)
        self.assertIn("Open Luciole in Home", help_result.stdout)
        self.assertEqual(help_result.stderr, "")

    def test_reject_invalid_argument_combinations(self):
        for args in (
            ("--unknown",),
            ("--help", "--version"),
            ("--version", "extra"),
            ("completions",),
            ("completions", "invalid"),
            ("completions", "bash", "extra"),
            ("config",),
            ("config", "validate"),
            ("config", "unknown", "file"),
            ("config", "validate", "file", "extra"),
        ):
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr.startswith("luciole:"))

    def test_completions_are_shell_specific(self):
        for shell, marker in (
            ("bash", "complete -F"),
            ("zsh", "#compdef luciole"),
            ("fish", "complete -c luciole"),
        ):
            with self.subTest(shell=shell):
                result = self.invoke("completions", shell)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(marker, result.stdout)
                self.assertEqual(result.stderr, "")

    def test_config_validation_is_headless_and_single_file(self):
        path = self.config_dir / "settings with spaces.toml"
        path.write_text("schema_version = 1\n[browser]\nshow_hidden = true\nsort_by = 'size'\n")
        (self.config_dir / "settings.toml").write_text("invalid adjacent override")
        result = self.invoke("config", "validate", str(path), config_home="invalid-relative-directory")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Configuration is valid", result.stdout)
        self.assertEqual(result.stderr, "")
        path.write_text("[browser]\nshow_hidden = 'wrong'\nunknown = true\n")
        result = self.invoke("config", "validate", str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn(f"{path}:2:", result.stderr)
        self.assertIn("browser.show_hidden", result.stderr)
        self.assertIn("browser.unknown", result.stderr)
        result = self.invoke("config", "validate", str(self.config_dir / "missing.toml"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing.toml", result.stderr)

    def test_bash_config_completion_preserves_spaces(self):
        available = subprocess.run(
            ["bash", "-c", "type compgen complete"], capture_output=True, text=True, check=False, timeout=5,
        )
        if available.returncode != 0:
            self.skipTest("this Bash build has no programmable completion")
        path = self.config_dir / "settings with spaces.toml"
        path.write_text("")
        script = r'''
source /dev/stdin
compopt() { :; }
COMP_WORDS=(luciole config validate "$1")
COMP_CWORD=3
_luciole
printf '%s\n' "${COMPREPLY[@]}"
'''
        result = subprocess.run(
            ["bash", "-c", script, "--", str(self.config_dir / "settings")],
            input=self.invoke("completions", "bash").stdout,
            capture_output=True, text=True, check=False, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout.splitlines(), [str(path)], result.stderr)

    def test_invalid_startup_configuration_fails_before_wayland(self):
        directory = self.config_dir / "luciole"
        directory.mkdir()
        (directory / "config.toml").write_text("[browser]\nshow_hidden = 'wrong'\n")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("browser.show_hidden", result.stderr)
        self.assertNotIn("connect to Wayland", result.stderr)
        self.assertEqual(self.invoke("--help", config_home="invalid-relative-directory").returncode, 0)
        self.assertEqual(self.invoke("--version", config_home="invalid-relative-directory").returncode, 0)

    def test_unknown_startup_settings_warn_without_blocking_wayland(self):
        directory = self.config_dir / "luciole"
        directory.mkdir()
        for name in ("config.toml", "settings.toml"):
            with self.subTest(name=name):
                path = directory / name
                original = "[browser]\nshow_tab_bar_with_single_tab = true\nview_mode = 'list'\n"
                path.write_text(original)
                result = self.invoke()
                self.assertIn("browser.show_tab_bar_with_single_tab: unknown setting; ignored", result.stderr)
                self.assertIn("failed to connect to Wayland", result.stderr)
                self.assertEqual(path.read_text(), original)
                strict = self.invoke("config", "validate", str(path))
                self.assertNotEqual(strict.returncode, 0)
                self.assertIn("browser.show_tab_bar_with_single_tab: unknown setting", strict.stderr)
                path.unlink()

    def test_startup_reports_connection_failure(self):
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("connect to Wayland", result.stderr)
        for args in ((str(self.config_dir),), ("--", str(self.config_dir))):
            result = self.invoke(*args)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("connect to Wayland", result.stderr)


if __name__ == "__main__":
    unittest.main()
