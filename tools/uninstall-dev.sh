#!/usr/bin/env bash
# uninstall-dev.sh — снять то что положил install-dev.sh.
# /var/lib/fastauth (модели + энроллменты) НЕ удаляется по умолчанию —
# передай --purge чтобы стереть.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "нужен sudo: sudo bash tools/uninstall-dev.sh [--purge]"
    exit 1
fi

PURGE=0
[[ "${1:-}" == "--purge" ]] && PURGE=1

systemctl --no-reload disable --now fastauthd.socket  2>/dev/null || true
systemctl --no-reload disable --now fastauthd.service 2>/dev/null || true

rm -f /usr/bin/fastauthd
rm -f /usr/bin/fastauth-cli
rm -f /usr/lib/security/pam_fastauth.so
rm -f /usr/lib/systemd/system/fastauthd.service
rm -f /usr/lib/systemd/system/fastauthd.socket
rm -f /usr/lib/tmpfiles.d/fastauth.conf
rm -f /etc/fastauth/config.toml
rmdir /etc/fastauth 2>/dev/null || true

systemctl daemon-reload

if (( PURGE )); then
    rm -rf /var/lib/fastauth
    if getent passwd fastauth >/dev/null; then userdel fastauth || true; fi
    if getent group  fastauth >/dev/null; then groupdel fastauth || true; fi
    echo "удалил пользователя fastauth и /var/lib/fastauth"
else
    echo "/var/lib/fastauth/ оставлен (модели + энроллменты)."
    echo "для полной очистки: sudo bash tools/uninstall-dev.sh --purge"
fi

echo "напоминание: проверь /etc/pam.d/* — если ты руками добавлял"
echo "pam_fastauth.so в какой-нибудь PAM stack, эти строки нужно убрать ВРУЧНУЮ."
