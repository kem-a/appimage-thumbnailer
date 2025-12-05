#!/usr/bin/env bash
set -euo pipefail

echo "Fetching latest release for ${REPO:-unknown/repo}"
release_json=$(curl -s -H "Authorization: token ${GITHUB_TOKEN}" -H "Accept: application/vnd.github+json" "https://api.github.com/repos/${REPO}/releases/latest")

total=$(printf '%s' "$release_json" | python3 - <<'PY'
import sys, json
obj=json.load(sys.stdin)
assets=obj.get('assets', [])
print(sum(int(a.get('download_count', 0) or 0) for a in assets))
PY
)

echo "Total downloads: $total"

badge_file="badge-downloads.json"
cat > "$badge_file" <<EOF
{"schemaVersion":1,"label":"downloads","message":"$total","color":"blue"}
EOF

git config user.name "github-actions[bot]"
git config user.email "github-actions[bot]@users.noreply.github.com"
git add "$badge_file"
if git commit -m "Update downloads badge: $total"; then
  git push origin HEAD:main
else
  echo "No changes to commit"
fi
