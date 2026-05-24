# chowdy

Быстрая face-аутентификация для Linux через IR-камеру. Читается как
**«C-howdy»** — Howdy, только переписанный на C++ как резидентный
daemon. Цель — холодный auth ≤ 400 мс, ради сравнения у Howdy 2–4 с.

> ⚠️ chowdy — это **удобство**, а не более сильная аутентификация чем
> пароль. В PAM правило всегда `sufficient`, никогда `required`. Не
> используйте как единственный фактор. См. DESIGN.md §3 (Threat model)
> и §16 (security checklist).

## Что внутри

- **`chowdyd`** — резидентный C++ daemon, держит ONNX модели в RAM и
  V4L2-камеру (`lazy`/`warm`/`idle_keep` policy). Работает под
  непривилегированным uid `chowdy`, доступ к `/dev/video2` через
  supplementary group `video`.
- **`chowdy-cli`** — пользовательский CLI: enroll / list / remove / test
  / auth-test. Использует unix-socket `/run/chowdy/mgmt.sock`.
- **`pam_chowdy.so`** — минимальный PAM модуль на C, дёргает daemon
  через `/run/chowdy/auth.sock`. Подключается к PAM stack как
  `sufficient`.
- **systemd socket activation** через `chowdyd.socket` + `chowdyd.service`.

## Целевая платформа

ASUS ZenBook 13 Flip / Arch Linux, IR-камера IMC Networks `13d3:56eb`
на `/dev/video2`, pixel format GREY 640×360 @ 15/30 fps. Другие IR
UVC камеры с похожими параметрами скорее всего тоже заработают —
конфигурация в `/etc/chowdy/config.toml`.

## Установка

### Локальная сборка через makepkg

```sh
cd packaging/arch
makepkg -si
```

`makepkg` собирает из локального git checkout, ставит через `pacman`
(оно само спросит пароль на install шаге), и `chowdy.install` пост-хук
создаёт системного пользователя `chowdy`, директории
`/var/lib/chowdy/{models,users}` с правильными правами,
перезагружает systemd. Подробности — `packaging/arch/README.md`.

После установки:

```sh
# 1. Скачать ONNX модели (~16 МБ, лицензия InsightFace
#    non-commercial research — см. models/README.md)
sudo install -m 0640 -o root -g chowdy path/to/scrfd_500m_bnkps.onnx \
                    /var/lib/chowdy/models/detector.onnx
sudo install -m 0640 -o root -g chowdy path/to/w600k_mbf.onnx \
                    /var/lib/chowdy/models/embedder.onnx

# 2. Поднять демон (через socket activation)
sudo systemctl enable --now chowdyd.socket

# 3. Энроллмент
chowdy-cli enroll --label main

# 4. Проверка
chowdy-cli auth-test
```

Если `auth-test` уверенно возвращает `success: true` — можно
настраивать PAM по `pam/README.md`. **Обязательно** прочитай
чек-лист из DESIGN.md §16 перед редактированием `/etc/pam.d/*`.

### Dev-режим (без makepkg)

`tools/install-dev.sh` делает то же что и пакет, но из `build/`
вместо собранного `.pkg.tar.zst`. Удобно когда правишь код и не
хочешь каждый раз пересобирать pacman-пакет.

```sh
cmake -S . -B build -G Ninja && cmake --build build
sudo bash tools/install-dev.sh
sudo systemctl enable --now chowdyd.socket
```

Снять: `sudo bash tools/uninstall-dev.sh` (без `--purge` оставляет
`/var/lib/chowdy/` с моделями и enrollment'ами).

## Документация

- `DESIGN.md` — полный дизайн-документ, threat model, milestone'ы,
  замеры на железе.
- `pam/README.md` — чек-лист установки PAM модуля.
- `models/README.md` — где скачать ONNX модели и SHA-256.
- `systemd/README.md` — содержимое юнитов и пути установки.
- `packaging/arch/README.md` — как собрать пакет и опубликовать в AUR.

## Лицензия

MIT (см. `LICENSE`). ONNX модели — InsightFace non-commercial
research license, скачиваются отдельно по инструкции из
`models/README.md`, в репозиторий и в пакет НЕ включены.
