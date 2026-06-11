#!/usr/bin/env bash
# install-dev.sh — поставить chowdy в систему БЕЗ настройки PAM.
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
    "$REPO/build/daemon/chowdyd" \
    "$REPO/build/cli/chowdy-cli" \
    "$REPO/build/pam/pam_chowdy.so" \
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
if ! getent group chowdy >/dev/null; then
    echo "создаю группу chowdy"
    groupadd --system chowdy
fi
if ! getent passwd chowdy >/dev/null; then
    echo "создаю пользователя chowdy"
    useradd --system --gid chowdy --groups video \
            --home-dir /var/lib/chowdy --no-create-home \
            --shell /usr/bin/nologin chowdy
fi
# Гарантируем что chowdy в group video (для /dev/video2).
if ! id -nG chowdy | grep -qw video; then
    usermod -aG video chowdy
fi

# --- 3. каталоги -----------------------------------------------------------
install -d -o root     -g chowdy -m 0750 /var/lib/chowdy
install -d -o root     -g chowdy -m 0750 /var/lib/chowdy/models
install -d -o chowdy -g chowdy -m 0700 /var/lib/chowdy/users
install -d -o root     -g root     -m 0755 /etc/chowdy

# --- 4. бинарники ----------------------------------------------------------
install -m 0755 "$REPO/build/daemon/chowdyd"    /usr/bin/chowdyd
install -m 0755 "$REPO/build/cli/chowdy-cli"    /usr/bin/chowdy-cli

# --- 5. PAM-модуль (только установка, в PAM stack не подключаем) -----------
install -m 0755 "$REPO/build/pam/pam_chowdy.so" /usr/lib/security/pam_chowdy.so

# --- 6. systemd units ------------------------------------------------------
install -m 0644 "$REPO/systemd/chowdyd.service"            /usr/lib/systemd/system/chowdyd.service
install -m 0644 "$REPO/systemd/chowdyd.socket"             /usr/lib/systemd/system/chowdyd.socket
install -m 0644 "$REPO/systemd/tmpfiles.d/chowdy.conf"     /usr/lib/tmpfiles.d/chowdy.conf

# --- 7. config -------------------------------------------------------------
if [[ ! -f /etc/chowdy/config.toml ]]; then
    install -o root -g root -m 0644 "$REPO/etc/chowdy/config.toml.example" /etc/chowdy/config.toml
    echo "положил /etc/chowdy/config.toml (можешь править — backup не настроен)"
else
    echo "/etc/chowdy/config.toml уже есть — не трогаю"
fi

# --- 8. модели -------------------------------------------------------------
# Они уже в repo от M2 — копируем под именами которые ждёт daemon.
install -m 0640 -o root -g chowdy \
    "$REPO/models/scrfd_500m_bnkps.onnx" \
    /var/lib/chowdy/models/detector.onnx
install -m 0640 -o root -g chowdy \
    "$REPO/models/w600k_mbf.onnx" \
    /var/lib/chowdy/models/embedder.onnx

# --- 9. systemd reload + tmpfiles ------------------------------------------
systemd-tmpfiles --create chowdy.conf
systemctl daemon-reload

# --- 10. опционально перенести существующий ~/.cache enrollment ------------
SUDO_USER="${SUDO_USER:-}"
if [[ -n "$SUDO_USER" ]]; then
    SUDO_UID="$(id -u "$SUDO_USER")"
    SUDO_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    SRC="$SUDO_HOME/.cache/chowdy/test_embedding.bin"
    DST_DIR="/var/lib/chowdy/users/$SUDO_UID"
    DST="$DST_DIR/main.enc"
    if [[ -f "$SRC" && ! -f "$DST" ]]; then
        install -d -o chowdy -g chowdy -m 0700 "$DST_DIR"
        install -o chowdy -g chowdy -m 0600 "$SRC" "$DST"
        echo "переcнёс $SRC → $DST"
        echo "(можешь убрать через chowdy-cli remove --label main)"
    fi
fi

cat <<MSG

  установка завершена. PAM НЕ тронут.

  следующее:
    1. поднять демон (через socket activation):
         sudo systemctl enable --now chowdyd.socket
         systemctl status chowdyd

    2. если ещё нет энроллмента (sudo обязателен):
         sudo chowdy-cli enroll --label main

    3. проверка:
         chowdy-cli list
         chowdy-cli auth-test

  логи демона:
         journalctl -u chowdyd -f

  снять всё:
         sudo bash tools/uninstall-dev.sh

MSG
