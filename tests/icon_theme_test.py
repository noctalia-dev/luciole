"""Verify theme notifications with private settings, schemas and artwork."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_case(binary, mode):
    with tempfile.TemporaryDirectory(prefix="luciole-icon-theme-") as directory:
        root = Path(directory)
        for name in ("home", "config/gtk-3.0", "data", "cache", "schemas", "empty"):
            (root / name).mkdir(parents=True)
        (root / "config/gtk-3.0/settings.ini").write_text("[Settings]\ngtk-icon-theme-name=Blue\n")
        for name, color in (("Red", "#ff0000"), ("Blue", "#0000ff")):
            theme = root / "data/icons" / name
            (theme / "scalable/places").mkdir(parents=True)
            (theme / "index.theme").write_text(
                f"[Icon Theme]\nName={name}\nDirectories=scalable/places,scalable/places\n"
                "[scalable/places]\nSize=48\nType=Scalable\n"
            )
            (theme / "scalable/places/folder.svg").write_text(
                "<svg xmlns='http://www.w3.org/2000/svg' width='48' height='48'>"
                f"<path fill='{color}' d='M0 0h48v48H0z'/></svg>"
            )
        # Keep artwork behind a linked root while preserving its logical theme path.
        (root / "artwork").mkdir()
        (root / "data/icons/Blue").rename(root / "artwork/Blue")
        (root / "data/icons/Blue").symlink_to(root / "artwork/Blue", target_is_directory=True)
        if mode == "schema":
            (root / "schemas/desktop.gschema.xml").write_text(
                "<schemalist><schema id='org.gnome.desktop.interface' path='/org/gnome/desktop/interface/'>"
                "<key name='icon-theme' type='s'><default>'Red'</default></key></schema></schemalist>"
            )
            subprocess.run(["glib-compile-schemas", "--strict", root / "schemas"], check=True)
        env = dict(os.environ)
        env.update(
            HOME=str(root / "home"),
            XDG_CONFIG_HOME=str(root / "config"),
            XDG_DATA_HOME=str(root / "data"),
            XDG_DATA_DIRS=str(root / "empty"),
            XDG_CACHE_HOME=str(root / "cache"),
            GSETTINGS_SCHEMA_DIR=str(root / "schemas"),
            GSETTINGS_BACKEND="memory",
            GIO_USE_VFS="local",
            DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-bus"),
            G_DEBUG="fatal-warnings",
        )
        subprocess.run([binary, str(root), mode], env=env, check=True, timeout=15)


for case in ("schema", "fallback"):
    run_case(sys.argv[1], case)
