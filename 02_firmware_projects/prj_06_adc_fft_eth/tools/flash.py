"""
Shim. The implementation lives once in the workspace, at tools/flash.py.

Keeping a copy per project is what let flash.py and monitor.py drift: the
--timeout flag and the shared-driver staleness check existed in one tree and
not the others for weeks. This file exists so `py tools/flash.py` still works
from a project directory while there is only one script to fix.

Override the workspace location with STM32_WORKSPACE_TOOLS when a project is
built outside this repository.
"""

import os
import pathlib
import runpy
import sys

HERE = pathlib.Path(__file__).resolve()
PROJECT_ROOT = HERE.parent.parent

_override = os.environ.get("STM32_WORKSPACE_TOOLS")
TOOLS = pathlib.Path(_override).resolve() if _override else PROJECT_ROOT.parents[1] / "tools"
TARGET = TOOLS / HERE.name

if not TARGET.is_file():
    sys.exit(
        f"ERROR: workspace tools not found at {TOOLS}."
        " Check the workspace layout, or set STM32_WORKSPACE_TOOLS."
    )

# How the shared script knows which firmware tree it is acting on.
os.environ["STM32_PROJECT_ROOT"] = str(PROJECT_ROOT)
runpy.run_path(str(TARGET), run_name="__main__")
