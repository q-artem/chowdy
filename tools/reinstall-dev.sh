#!/usr/bin/env bash
# reinstall-dev.sh — uninstall + install подряд. Удобно при изменении
# дефолтов в etc/chowdy/config.toml.example или после правок в коде.
#
# /var/lib/chowdy/ (модели + энроллменты) сохраняется — uninstall
# идёт без --purge.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "нужен sudo: sudo bash tools/reinstall-dev.sh"
    exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"

echo "=== uninstall ==="
bash "$HERE/uninstall-dev.sh"

echo
echo "=== install ==="
bash "$HERE/install-dev.sh"

echo
echo "=== restart демона ==="
systemctl restart chowdyd.socket 2>/dev/null || true
systemctl restart chowdyd        2>/dev/null || true

echo
echo "готово. проверь:"
echo "    grep camera_policy /etc/chowdy/config.toml"
echo "    chowdy-cli auth-test"
