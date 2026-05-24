#!/usr/bin/env bash
# install-dev.sh — поставить fastauth в систему БЕЗ настройки PAM.
#
# Делает всё что делает packaging/arch/PKGBUILD + post_install, кроме
# того что трогает /etc/pam.d/. PAM-модуль кладётся в
# /usr/lib/security/, но НЕ подключается к auth stack — это
# отдельная ручная операция по pam/README.md.
#
# Запускать из корня репо:
#   sudo bash tools/install-dev.sh
#
# Идемпотентен — можно перезапускать.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "нужен sudo: sudo bash tools/install-dev.sh"
    exit 1
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
echo "репозиторий: $REPO"

# --- 1. собранные артефакты ------------------------------------------------
for f in \
    "$REPO/build/daemon/fastauthd" \
    "$REPO/build/cli/fastauth-cli" \
    "$REPO/build/pam/pam_fastauth.so" \
    "$REPO/models/scrfd_500m_bnkps.onnx" \
    "$REPO/models/w600k_mbf.onnx"
do
    if [[ ! -f "$f" ]]; then
        echo "нет файла: $f"
        echo "сначала собери: cmake -S . -B build && cmake --build build"
        exit 1
    fi
done

# --- 2. system user / group ------------------------------------------------
if ! getent group fastauth >/dev/null; then
    echo "создаю группу fastauth"
    groupadd --system fastauth
fi
if ! getent passwd fastauth >/dev/null; then
    echo "создаю пользователя fastauth"
    useradd --system --gid fastauth --groups video \
            --home-dir /var/lib/fastauth --no-create-home \
            --shell /usr/bin/nologin fastauth
fi
# Гарантируем что fastauth в group video (для /dev/video2).
if ! id -nG fastauth | grep -qw video; then
    usermod -aG video fastauth
fi

# --- 3. каталоги -----------------------------------------------------------
install -d -o root     -g fastauth -m 0750 /var/lib/fastauth
install -d -o root     -g fastauth -m 0750 /var/lib/fastauth/models
install -d -o fastauth -g fastauth -m 0700 /var/lib/fastauth/users
install -d -o root     -g root     -m 0755 /etc/fastauth

# --- 4. бинарники ----------------------------------------------------------
install -m 0755 "$REPO/build/daemon/fastauthd"    /usr/bin/fastauthd
install -m 0755 "$REPO/build/cli/fastauth-cli"    /usr/bin/fastauth-cli

# --- 5. PAM-модуль (только установка, в PAM stack не подключаем) -----------
install -m 0755 "$REPO/build/pam/pam_fastauth.so" /usr/lib/security/pam_fastauth.so

# --- 6. systemd units ------------------------------------------------------
install -m 0644 "$REPO/systemd/fastauthd.service"            /usr/lib/systemd/system/fastauthd.service
install -m 0644 "$REPO/systemd/fastauthd.socket"             /usr/lib/systemd/system/fastauthd.socket
install -m 0644 "$REPO/systemd/tmpfiles.d/fastauth.conf"     /usr/lib/tmpfiles.d/fastauth.conf

# --- 7. config -------------------------------------------------------------
if [[ ! -f /etc/fastauth/config.toml ]]; then
    install -o root -g root -m 0644 "$REPO/etc/fastauth/config.toml.example" /etc/fastauth/config.toml
    echo "положил /etc/fastauth/config.toml (можешь править — backup не настроен)"
else
    echo "/etc/fastauth/config.toml уже есть — не трогаю"
fi

# --- 8. модели -------------------------------------------------------------
# Они уже в repo от M2 — копируем под именами которые ждёт daemon.
install -m 0640 -o root -g fastauth \
    "$REPO/models/scrfd_500m_bnkps.onnx" \
    /var/lib/fastauth/models/detector.onnx
install -m 0640 -o root -g fastauth \
    "$REPO/models/w600k_mbf.onnx" \
    /var/lib/fastauth/models/embedder.onnx

# --- 9. systemd reload + tmpfiles ------------------------------------------
systemd-tmpfiles --create fastauth.conf
systemctl daemon-reload

# --- 10. опционально перенести существующий ~/.cache enrollment ------------
SUDO_USER="${SUDO_USER:-}"
if [[ -n "$SUDO_USER" ]]; then
    SUDO_UID="$(id -u "$SUDO_USER")"
    SUDO_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    SRC="$SUDO_HOME/.cache/fastauth/test_embedding.bin"
    DST_DIR="/var/lib/fastauth/users/$SUDO_UID"
    DST="$DST_DIR/main.enc"
    if [[ -f "$SRC" && ! -f "$DST" ]]; then
        install -d -o fastauth -g fastauth -m 0700 "$DST_DIR"
        install -o fastauth -g fastauth -m 0600 "$SRC" "$DST"
        echo "переcнёс $SRC → $DST"
        echo "(можешь убрать через fastauth-cli remove --label main)"
    fi
fi

cat <<MSG

  установка завершена. PAM НЕ тронут.

  следующее:
    1. поднять демон (через socket activation):
         sudo systemctl enable --now fastauthd.socket
         systemctl status fastauthd

    2. если ещё нет энроллмента:
         fastauth-cli enroll --label main

    3. проверка:
         fastauth-cli list
         fastauth-cli auth-test

  логи демона:
         journalctl -u fastauthd -f

  снять всё:
         sudo bash tools/uninstall-dev.sh

MSG
