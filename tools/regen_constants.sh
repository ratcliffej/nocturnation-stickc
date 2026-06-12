#!/usr/bin/env bash
# Regenerate include/transport/espnow/protocol_constants_generated.h
# from the protocol-constants SOT in the sibling nocturnation-docs repo.
#
# Run from the firmware repo root:
#   ./tools/regen_constants.sh
#
# The accompanying check script (tools/check_protocol_constants.py)
# re-runs the generator and fails on any drift between the checked-in
# header and the SOT, so committing without running this script first
# is caught before push.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
DOCS_ROOT="$(cd "$REPO_ROOT/../Docs" && pwd)"

python3 "$DOCS_ROOT/tools/gen_protocol_constants.py" --cpp \
  > "$REPO_ROOT/include/transport/espnow/protocol_constants_generated.h"

echo "Regenerated include/transport/espnow/protocol_constants_generated.h from $DOCS_ROOT/protocol/constants.yaml"
