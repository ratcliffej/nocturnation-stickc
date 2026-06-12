#!/usr/bin/env python3
"""CI guard for include/transport/espnow/protocol_constants_generated.h.

Re-runs the protocol-constants generator (in the sibling Docs repo)
against its YAML SOT and asserts the checked-in C++ header is
byte-identical. Exits 0 on match, 1 on drift.

Run from the firmware repo root:
    python3 tools/check_protocol_constants.py

Catches:
  * Hand-edits to protocol_constants_generated.h (auto-output; touch
    the YAML and run tools/regen_constants.sh).
  * SOT edits that were not followed by a regen.
  * Generator changes that bump output formatting (regen + commit).

Cross-repo path assumption: nocturnation-docs sits at ../Docs relative
to this firmware repo. Matches the user's local layout
(NocturNation/{Tildagon, StickC, Docs}/) and is documented in
tools/regen_constants.sh.
"""

import pathlib
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DOCS_ROOT = REPO_ROOT.parent / "Docs"
GENERATOR = DOCS_ROOT / "tools" / "gen_protocol_constants.py"
GENERATED = REPO_ROOT / "include" / "transport" / "espnow" / "protocol_constants_generated.h"


def main():
    if not GENERATOR.is_file():
        sys.stderr.write(
            "warning: sibling Docs repo not found at {}; skipping check\n".format(DOCS_ROOT)
        )
        return 0
    result = subprocess.run(
        [sys.executable, str(GENERATOR), "--cpp"],
        capture_output=True, text=True, check=True,
    )
    expected = result.stdout
    actual = GENERATED.read_text()
    if actual != expected:
        sys.stderr.write(
            "error: {} is out of sync with Docs/protocol/constants.yaml.\n"
            "Run tools/regen_constants.sh and commit the result.\n".format(
                GENERATED.relative_to(REPO_ROOT)
            )
        )
        return 1
    print("ok: {} matches the SOT".format(GENERATED.relative_to(REPO_ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
