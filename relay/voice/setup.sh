#!/usr/bin/env bash
# Voice input setup (optional): a separate venv with faster-whisper and the
# Whisper models, inside the repo (relay/voice/), no sudo. The relay runs
# relay/voice/worker.py with this venv; without it voice input is simply off.
#
#   relay/voice/setup.sh              venv + packages + models (asks first)
#   relay/voice/setup.sh --packages   only the venv + packages   (134.4 MB)
#   relay/voice/setup.sh --models     only the models            (634.1 MB)
#   relay/voice/setup.sh --yes ...    don't ask (e.g. on another network)
#
# Downloads (exact, checked 2026-11-13):
#   Python packages (requirements.txt, 23 wheels)       134.4 MB
#   Systran/faster-whisper-base  (model.bin 145.2 MB)   147.9 MB
#   Systran/faster-whisper-small (model.bin 483.5 MB)   486.2 MB
#   total                                               768.5 MB
# Installed under relay/voice/.venv and relay/voice/models (both gitignored).
# Nothing else is downloaded later: the receiver loads models with
# local_files_only. Remove everything with: rm -rf relay/voice/.venv relay/voice/models
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
VENV=$HERE/.venv
MODELS=$HERE/models
BASE_REV=ebe41f70d5b6dfa9166e2c581c45c9c0cfc57b66
SMALL_REV=536b0662742c02347bc0e980a01041f333bce120

DO_PKG=1 DO_MOD=1 YES=0
for a in "$@"; do
    case $a in
        --packages) DO_MOD=0 ;;
        --models) DO_PKG=0 ;;
        --yes) YES=1 ;;
        *) echo "usage: $0 [--packages|--models] [--yes]" >&2; exit 2 ;;
    esac
done
size=0
(( DO_PKG )) && size=$((size + 1344))
(( DO_MOD )) && size=$((size + 6341))
echo "This downloads about $((size / 10)).$((size % 10)) MB:"
(( DO_PKG )) && echo "  Python packages (faster-whisper + 22 deps)  134.4 MB  -> $VENV"
(( DO_MOD )) && echo "  Whisper base 147.9 MB + small 486.2 MB      634.1 MB  -> $MODELS"
if (( ! YES )); then
    read -r -p "Continue? [y/N] " ok
    [[ $ok == y || $ok == Y ]] || { echo "nothing downloaded"; exit 1; }
fi

if (( DO_PKG )); then
    [[ -x $VENV/bin/python ]] || python3 -m venv "$VENV"
    "$VENV/bin/pip" install --no-input --disable-pip-version-check -r "$HERE/requirements.txt"
fi
if (( DO_MOD )); then
    [[ -x $VENV/bin/python ]] || { echo "install the packages first (--packages)" >&2; exit 1; }
    "$VENV/bin/python" - "$MODELS" "$BASE_REV" "$SMALL_REV" <<'PY'
import sys
from huggingface_hub import snapshot_download
models, base_rev, small_rev = sys.argv[1:]
for name, rev in (("base", base_rev), ("small", small_rev)):
    path = snapshot_download(f"Systran/faster-whisper-{name}", revision=rev,
                             local_dir=f"{models}/faster-whisper-{name}",
                             allow_patterns=["config.json", "model.bin", "tokenizer.json",
                                             "vocabulary.txt"])
    print("ok", name, "->", path)
PY
fi
echo "done. Restart the relay to turn voice input on: ./dschat restart-relay"
