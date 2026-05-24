#!/usr/bin/env bash
# uninstall-dev.sh — снять то что положил install-dev.sh.
# /var/lib/chowdy (модели + энроллменты) НЕ удаляется по умолчанию —
# передай --purge чтобы стереть.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "нужен sudo: sudo bash tools/uninstall-dev.sh [--purge]"
    exit 1
fi

PURGE=0
[[ "${1:-}" == "--purge" ]] && PURGE=1

systemctl --no-reload disable --now chowdyd.socket  2>/dev/null || true
systemctl --no-reload disable --now chowdyd.service 2>/dev/null || true

rm -f /usr/bin/chowdyd
rm -f /usr/bin/chowdy-cli
rm -f /usr/lib/security/pam_chowdy.so
rm -f /usr/lib/systemd/system/chowdyd.service
rm -f /usr/lib/systemd/system/chowdyd.socket
rm -f /usr/lib/tmpfiles.d/chowdy.conf
rm -f /etc/chowdy/config.toml
rmdir /etc/chowdy 2>/dev/null || true

systemctl daemon-reload

if (( PURGE )); then
    rm -rf /var/lib/chowdy
    if getent passwd chowdy >/dev/null; then userdel chowdy || true; fi
    if getent group  chowdy >/dev/null; then groupdel chowdy || true; fi
    echo "удалил пользователя chowdy и /var/lib/chowdy"
else
    echo "/var/lib/chowdy/ оставлен (модели + энроллменты)."
    echo "для полной очистки: sudo bash tools/uninstall-dev.sh --purge"
fi

echo "напоминание: проверь /etc/pam.d/* — если ты руками добавлял"
echo "pam_chowdy.so в какой-нибудь PAM stack, эти строки нужно убрать ВРУЧНУЮ."
