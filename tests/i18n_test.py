"""Run against the shipped catalogs and private catalogs with distinct plural rules."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="luciole-i18n-") as temporary:
    catalogs = Path(temporary) / "translations"
    shutil.copytree(Path(os.environ["LUCIOLE_APP_ASSETS_DIR"]) / "translations", catalogs)
    custom = {
        "pluralRules": [
            {"range": [0, 0], "suffix": "-zero"},
            {"mod": 10, "range": [1, 1], "suffix": ""},
            {"suffix": "-rest"},
        ],
        "plural": {"byte-zero": "empty", "byte": "single", "byte-rest": "multiple"},
        "rename": {"items-zero": "Nothing to rename"},
    }
    (catalogs / "zz.json").write_text(json.dumps(custom))
    invalid = {"pluralRules": [{"mod": 0, "range": [0, 1], "suffix": ""}, {"suffix": "-rest"}]}
    (catalogs / "zz-BB.json").write_text(json.dumps(invalid))
    (catalogs / "xx.json").write_text(json.dumps({"plural": {"file": "entry", "file-plural": "entries"}}))
    result = subprocess.run(sys.argv[1:], env=dict(os.environ, LUCIOLE_APP_ASSETS_DIR=temporary))
    sys.exit(result.returncode)
