"""Repo-root conftest for pytest.

Adds the vendored rethinkdb python driver (``driver/python3``) to ``sys.path``
relative to this file, so ``python3 -m pytest test/<file>.py`` works from any
checkout location — no ``PYTHONPATH`` needed.

The driver is pure Python and lives in-tree (``driver/python3/``); it is
imported as a vendored module by the E2E probes in ``test/`` rather than being
pip-installed, so the path wiring has to be explicit.
"""

import os
import sys


_DRIVER_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "driver",
    "python3",
)

if _DRIVER_PATH not in sys.path:
    sys.path.insert(0, _DRIVER_PATH)
