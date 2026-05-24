# CLAUDE.md — onboarding для будущих сессий

Этот файл подгружается в контекст Claude Code при работе в репе. Держим компактным.

## Что это за проект

**chowdy** (читается как «C-howdy» — Howdy, только на C++) — быстрая
face-аутентификация для Linux через IR-камеру. Замена оригинального
Howdy на одной машине: резидентный C++ daemon, минимальный PAM-модуль,
IR-only pipeline. Цель — холодный auth ≤ 400 мс (Howdy сейчас 2–4 с).

Канонический документ дизайна: **`DESIGN.md`** (Section 1–16 + Приложения A/B).
**Любое архитектурное решение сверять с ним.** При расхождении между дизайном
и реальностью — править DESIGN.md явно, не неявно.

## Целевая платформа (фиксированная)

- ASUS ZenBook 13 Flip, Arch Linux.
- IR-камера IMC Networks `13d3:56eb` на `/dev/video2`, by-path
  `/dev/v4l/by-path/pci-0000:00:14.0-usb-0:5:1.2-video-index0`.
- Pixel format **только `GREY`** (8-bit greyscale, 640×360, 15/30 fps).
  Никакого YUYV decode не нужно.

## Состояние работ

Идём строго по milestones из DESIGN.md Section 13. Текущий статус:

| M | Что | Статус |
|---|---|---|
| M1 | V4L2 capture (`tools/m1_capture_test.cpp`) | ✅ done — "M1 findings" в DESIGN.md Приложение A |
| M2 | ONNX Runtime + один кадр (`tools/m2_detect_test.cpp`) | ✅ done — "M2 findings" в DESIGN.md Приложение A. Total warm: ~40 мс. |
| M3 | Полный pipeline e2e (`tools/m3_pipeline_test.cpp`) | ✅ done — "M3 findings" в DESIGN.md. Live MATCH 503 мс cold, sim ~0.87, negative test чисто. |
| M4 | IPC + daemon skeleton + systemd units | ✅ done — common/, proto/, daemon/, cli/, systemd/ wired, test/auth-test stubs. |
| M5 | Real auth handler (camera+models+pipeline+store) | ✅ done — "M5 findings" в DESIGN.md. Warm auth ~35 мс через daemon. |
| M6 | `pam_chowdy.so` (only .so и README, install — твой) | ✅ done — модуль собирается, нужно установить руками по `pam/README.md`. |
| M7 | Полный `chowdy-cli` (enroll/list/remove/test) | ✅ done — interactive enroll с hint'ами на русском, list/remove работают. |
| M8 | config.toml + idle_keep policy + secure_buffer ready | ✅ done — `/etc/chowdy/config.toml`, camera освобождается через idle_keep_ms. |
| M9 | AUR PKGBUILD + install hooks | ✅ done — `packaging/arch/`, готово к `makepkg -si` (не делал сам). |

**Все M1-M9 закрыты.** Следующее — реальная установка пакета и настройка PAM на живой системе (только пользователь).
| M6 | PAM модуль (**особо осторожно**, см. Section 16 чеклист) | — |
| M7 | Enrollment CLI | — |
| M8 | Hardening + QoL | — |
| M9 | AUR packaging | — |

`tools/` — milestone-helpers, **не часть финального бинаря**. Финальная структура
описана в DESIGN.md Section 11 (`common/`, `daemon/`, `pam/`, `cli/`).

## Build / Run

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/chowdy-capture-test        # M1 утилита, пишет PGM в /tmp/
./build/chowdy-detect-test         # M2 утилита, detect+embed на PGM от M1
./build/chowdy-pipeline-test --enroll  # M3 утилита, сохраняет твой embedding
./build/chowdy-pipeline-test           # M3 utility, auth по живой камере
```

Модели для M2+ скачиваются локально по инструкции в `models/README.md`
(`*.onnx` в `.gitignore`, в репо не комитятся).

Компилятор: `g++` 16.1.1, C++20, флаги `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion -fstack-protector-strong`. Код должен
компилироваться без warnings.

## Зависимости (Arch package names)

Установлены: `opencv libsodium systemd cmake pam gtest pkgconf v4l-utils
onnxruntime nlohmann-json tomlplusplus`. Никаких vendor'енных third-party.

## Конвенции, которые легко забыть

- **PAM правило всегда `sufficient`, никогда `required`.** Падение daemon
  не должно лочить пользователя. Подробнее DESIGN.md Section 10, 15, 16.
- **`chowdyd` НИКОГДА не запускается под root.** uid=chowdy, без caps,
  только `SupplementaryGroups=video` для `/dev/video2`.
- **Embeddings — секретны.** Хранить через `secure_buffer` (mlock +
  `explicit_bzero` на destroy). Не логировать векторы даже на debug уровне
  — только метаданные.
- **Никогда не коммитить захваченные кадры, embeddings, модели.**
  `.gitignore` уже покрывает `*.enc`, `*.bin`, `*.onnx`,
  `tests/fixtures/sample_ir_frames/*.pgm`, `/tmp/chowdy-*`.
- **Anti-spoof откладываем в v2.** Embedder default — **MobileFaceNet
  (`w600k_mbf.onnx` из insightface buffalo_s)**. EdgeFace ONNX не существует
  в природе, только PyTorch checkpoints; см. M2 findings в DESIGN.md.
  Лицензия моделей: **InsightFace non-commercial research** — это влияет
  на пакетирование (M9), для личного использования ok.
- **Camera policy default — `idle_keep_ms=10000`.** Lazy реалистично невозможен
  — cold open съедает 308 мс из 400 мс бюджета (см. M1 findings).

## Язык

- Текстовые сообщения пользователю, коммит-сообщения для людей, CLI hint'ы,
  README, документация — **русский**.
- Идентификаторы, комментарии в коде, API-сообщения, типы reason в IPC —
  **английский**.

## Безопасность разработки (Section 16 чеклист)

Особенно важно с M6 (PAM):
- Перед любым `/etc/pam.d/*` редактом — открыт **второй TTY с залогиненным root**.
- Бэкап `/etc/pam.d/` (`sudo cp -r /etc/pam.d /etc/pam.d.bak`).
- Тестируем сначала на `sudo`, не на login screen.
- В новых PAM конфигах `auth sufficient pam_unix.so try_first_pass` ПЕРЕД
  строкой chowdy, пока всё не оттестировано.
