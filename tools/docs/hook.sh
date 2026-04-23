#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

python tools/docs/validate.py
python tools/docs/build_index.py
git add docs/data.json
