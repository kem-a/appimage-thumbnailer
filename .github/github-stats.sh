#!/usr/bin/env bash
set -euo pipefail

REPO="${1:-kem-a/appimage-thumbnailer}"
API_URL="https://api.github.com/repos/${REPO}/releases/latest"

# Optional token for higher rate limits or private repos
CURL_ARGS=("-fsSL" "-H" "Accept: application/vnd.github+json" "-H" "User-Agent: appimage-thumbnailer-download-check")
if [[ -n "${GITHUB_TOKEN:-}" ]]; then
  CURL_ARGS+=("-H" "Authorization: Bearer ${GITHUB_TOKEN}")
fi

PY_CODE=$'import sys, json\nobj = json.load(sys.stdin)\nassets = obj.get("assets", [])\nif not assets:\n    print("No assets found (release may have none).")\n    raise SystemExit(0)\n\ntotal = 0\nfor asset in assets:\n    name = asset.get("name") or asset.get("label") or "<unnamed>"\n    count = int(asset.get("download_count", 0) or 0)\n    total += count\n    print(f"{name}: {count}")\n\nprint("TOTAL:", total)'

curl "${CURL_ARGS[@]}" "$API_URL" | python3 -c "$PY_CODE"
