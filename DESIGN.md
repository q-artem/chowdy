# fastauth — быстрая face-аутентификация для Linux

> Дизайн-документ для замены Howdy на одной машине: резидентный C++ daemon, минимальный PAM-модуль, IR-only pipeline. Целевая среда — Arch Linux на ASUS ZenBook 13 Flip с IR-камерой `13d3:56eb` (IMC Networks).

---

## 1. Цели и не-цели

### Цели

- **Холодная аутентификация ≤ 400 мс** (от вызова PAM до ответа). Howdy сейчас даёт 2–4 секунды.
- **IR-only pipeline.** Используем только `/dev/video2`, RGB не трогаем. IR сам по себе хорошая защита от фото/экрана.
- **Лучше Howdy по безопасности.** Privilege separation, daemon под отдельным uid, минимум кода под root.
- **Простая сборка.** CMake + системные пакеты Arch, никаких exotic зависимостей.
- **Не залочиться при ошибке.** PAM всегда `sufficient`, никогда `required`. Падение daemon → fallback на пароль.

### Не-цели (v1)

- Multi-modal (отпечатки, голос, PIN).
- GUI. Энроллмент через CLI.
- Multi-user сервер. Дизайн под персональный лаптоп, 1–2 юзера.
- Cross-distro пакетирование. Сначала Arch, потом видно будет.
- Замена пароля. Только дополнительный/удобный способ.

---

## 2. Архитектура

Три бинаря, три роли:

```
        вход пользователя
              │
              ▼
┌──────────────────────────┐
│    sudo / login / etc    │
└────────────┬─────────────┘
             │ PAM stack
             ▼
┌──────────────────────────┐      unix socket      ┌──────────────────────────┐
│   pam_fastauth.so        │ ────────────────────▶ │       fastauthd          │
│   (tiny C, runs as root) │  /run/fastauth/auth   │   (C++, uid=fastauth)    │
│                          │ ◀──────────────────── │                          │
│  • getpwnam(user) → uid  │   AuthResp{ok|fail}   │  • модели в RAM          │
│  • connect, send uid     │                       │  • V4L2 fd lazy-open     │
│  • timeout 2s            │                       │  • SCM_CREDENTIALS check │
│  • PAM_SUCCESS|AUTH_ERR  │                       │  • zeroize buffers       │
└──────────────────────────┘                       └─────────────┬────────────┘
                                                                 │
                                                                 ▼
                                                   ┌──────────────────────────┐
                                                   │  /dev/video2 (IR)        │
                                                   │  /var/lib/fastauth/...   │
                                                   └──────────────────────────┘
                                                                 ▲
                                                                 │ unix socket
                                                                 │ /run/fastauth/mgmt
                                                                 │
┌──────────────────────────┐                                     │
│      fastauth-cli        │ ────────────────────────────────────┘
│      (C++, user uid)     │   enroll | list | remove | test
└──────────────────────────┘
```

Два сокета умышленно разные:
- `auth.sock` — для PAM, права `0660 root:fastauth`. Только аутентификация, минимальный API.
- `mgmt.sock` — для CLI, права `0666` или через PolicyKit. Энроллмент, список, удаление. Daemon проверяет uid вызывающего через `SO_PEERCRED` и работает только с энроллментами этого uid.

---

## 3. Threat model

### В скоупе

| Угроза | Защита |
|---|---|
| Спуф фоткой/экраном | IR + опциональная liveness-модель. Бумага и экраны иначе отражают IR. |
| Locally логиненный атакующий читает чужие эмбеддинги | `/var/lib/fastauth/users/<uid>/` mode 0700, owned by `fastauth:fastauth`. PAM-модуль и CLI ходят только через daemon. |
| Атакующий притворяется другим uid'ом в IPC | `SO_PEERCRED` на сокете, daemon верифицирует uid вызывающего и работает только с его данными. |
| Подмена PAM-модуля | Стандартная угроза Linux. Mitigated owner=root mode=0755 на `.so`. |
| Replay фрейма из утечки | Daemon никогда не принимает фрейм извне, всегда сам захватывает с `/dev/video2`. |
| DoS камеры (другой процесс держит её) | Если `/dev/video2` занят — внятный fail, password fallback. |
| Утечка эмбеддинга из памяти после auth | `mlock` буферов с embedding'ами, `explicit_bzero` после использования. |

### Вне скоупа

- **Близнецы / очень похожие лица** — фундаментальное ограничение face recognition. Документируем.
- **3D-маски, сложные printed-on-curved-surface атаки** — нужен depth sensor, у нас его нет.
- **Скомпрометированный root** — game over для всей системы, не наша задача.
- **Cold boot / шина / side-channel** — слишком экзотично для домашнего use case.
- **Сетевые атаки** — daemon слушает только unix socket, не TCP.

### Honest disclaimer для README

> fastauth — это удобство, а не более сильная аутентификация чем пароль. Принципиально слабее пароля или хардварного ключа. Не используйте как единственный фактор. PAM правило должно быть `sufficient`, не `required`.

---

## 4. Технологический стек

| Компонент | Выбор | Альтернативы | Почему |
|---|---|---|---|
| Язык | C++20 | Rust, C | Запрошено явно. C++20 даёт concepts, `<bit>`, `jthread`, ranges. |
| Build | CMake 3.25+ | Meson | Универсально, ONNX Runtime даёт CMake-find. |
| Камера | V4L2 напрямую | libcamera | Одна камера, простой случай. libcamera тяжелее и pipeline-ориентирована. Pixel format = `V4L2_PIX_FMT_GREY` (8-bit greyscale, проверено на `13d3:56eb`) — никакого YUYV/MJPEG decode не нужно. |
| Image ops | OpenCV 4 | stb_image + ручной resize | Resize, crop, geometric ops для align лица. Color conversion не нужен — камера сразу отдаёт GREY. В Arch один пакет, проверено годами. |
| ML runtime | ONNX Runtime C++ | LibTorch, TensorFlow Lite, ncnn | Меньше LibTorch (~20 МБ vs ~500), один формат моделей, CPU EP достаточно. |
| Crypto | libsodium | OpenSSL | Argon2id и XChaCha20-Poly1305 из коробки, нельзя misuse. |
| PAM | libpam (system) | — | Стандарт. |
| IPC сериализация | JSON через nlohmann/json, length-prefixed | protobuf, msgpack | Один запрос на auth, perf не важен. JSON легко дебажить. |
| Logging | sd-journal (libsystemd) | spdlog | systemd everywhere в Arch, structured logs бесплатно. |
| Tests | GoogleTest + bash integration | doctest | Стандарт. |

### Модели

Все в формате ONNX, кладутся в `/var/lib/fastauth/models/`:

| Роль | Модель | Размер | Источник |
|---|---|---|---|
| Face detection | SCRFD-500MF | ~2.5 МБ | [insightface model zoo](https://github.com/deepinsight/insightface/tree/master/model_zoo). Альтернатива: BlazeFace из MediaPipe (~1.5 МБ). |
| Face embedding | EdgeFace-XS-gamma-06 или MobileFaceNet | 5–8 МБ | [edgeface](https://github.com/otroshi/edgeface) (Apache-2.0), либо MobileFaceNet из insightface. Выдаёт 512-d вектор. |
| Anti-spoof (опц.) | MiniFASNet v2 | ~1.5 МБ | [silent-face-anti-spoofing](https://github.com/minivision-ai/Silent-Face-Anti-Spoofing). На IR может быть избыточен — тестировать. |

### IR-специфика для моделей

Все публичные модели тренированы на RGB. Подход для v1:
1. IR-кадр (уже GREY из V4L2, без декодирования) → реплицируем канал 3 раза → подаём как «RGB».
2. На энроллменте калибруем порог cosine similarity по 5–10 кадрам того же лица. Подбирается per-user.
3. Если accuracy недостаточная — в v2 fine-tune эмбеддера на небольшом IR-датасете либо отдельная head'а.

---

## 5. IPC протокол

Length-prefixed JSON. Заголовок — `uint32_t` big-endian с длиной payload в байтах. Payload — UTF-8 JSON.

### auth.sock (PAM → daemon)

**Request:**
```json
{
  "type": "auth",
  "uid": 1000,
  "timeout_ms": 2000,
  "request_id": "uuid-v4"
}
```

**Response:**
```json
{
  "type": "auth_result",
  "request_id": "uuid-v4",
  "success": true,
  "reason": "matched",
  "matched_label": "main",
  "confidence": 0.87,
  "elapsed_ms": 287
}
```

`reason` коды для логов и debug: `matched`, `no_face`, `low_confidence`, `liveness_failed`, `not_enrolled`, `camera_busy`, `timeout`, `internal_error`.

Daemon перед обработкой делает `getsockopt(SO_PEERCRED)` и проверяет, что вызывающий имеет uid 0 (то есть это PAM от root) ИЛИ uid из запроса (для самотестирования). PAM-модуль никогда не запрашивает auth для uid'а отличного от запрашивающего пользователя.

### mgmt.sock (CLI → daemon)

```json
// EnrollStart
{"type": "enroll_start", "label": "main", "min_frames": 8, "max_frames": 15}
// → {"type": "enroll_progress", "session": "...", "frames_collected": 0, "quality": 0.0}

// EnrollFrame (CLI просит daemon обработать ещё кадр)
{"type": "enroll_frame", "session": "..."}
// → {"type": "enroll_progress", "frames_collected": 1, "quality": 0.82, "done": false, "hint": "look_straight"}
// hint: "ok", "look_straight", "too_dark", "no_face", "too_close", "too_far", "blink_detected"

// EnrollFinish
{"type": "enroll_finish", "session": "..."}
// → {"type": "enroll_done", "label": "main", "embeddings_saved": 5}

// List
{"type": "list"}
// → {"type": "list_result", "enrollments": [{"label": "main", "created": "...", "embeddings": 5}]}

// Remove
{"type": "remove", "label": "main"}
// → {"type": "remove_result", "ok": true}

// Test (capture one frame, run pipeline, report what it sees)
{"type": "test", "timeout_ms": 3000}
// → {"type": "test_result", "face_detected": true, "best_match": "main", "confidence": 0.91, "would_auth": true}
```

CLI всегда подключается под uid пользователя, daemon работает только с энроллментами этого uid.

---

## 6. Pipeline аутентификации

Внутри daemon, при получении auth-запроса:

```
1. parse_request                                            ~0.1 ms
2. validate_uid (SO_PEERCRED + uid в запросе)               ~0.1 ms
3. load_enrollments_for_uid (lazy, cached, ~5 КБ файлы)     ~1 ms (cold) / 0 (cached)
4. ensure_camera_open(/dev/video2)                           ~50 ms (cold) / 0 (warm)
5. цикл до timeout_ms:
     a. capture_frame                                        ~33 ms (один кадр на 30 fps)
     b. mean_brightness < dark_threshold? → continue         ~0.5 ms
     c. detect_face                                          ~15-30 ms
     d. нет лица? → continue                                 -
     e. align_and_crop_to_112x112                            ~1 ms
     f. compute_embedding                                    ~20-40 ms
     g. cosine_similarity(emb, enrolled_emb) для каждого     ~0.01 ms
     h. max_sim > threshold? → success                       -
     i. (опц.) anti_spoof score                              ~10-20 ms
6. release_camera (если held_when_idle == false)
7. write_response, zero embedding buffers
```

**Бюджет:** успех на 2-3 кадре → ~150-250 мс при 30 fps. Если первый кадр темный/без лица — ещё ~33 мс на кадр (30 fps) или ~67 мс (15 fps). Камера `13d3:56eb` поддерживает оба режима; default запрашиваем 30 fps. Cap timeout = 2000 мс → максимум ~50 попыток на 30 fps.

**Camera open policy** (конфигурируемо):
- `lazy` (default): открываем `/dev/video2` при первом запросе, закрываем после ответа. Минус: 50 мс на каждый cold auth. Плюс: камера свободна для других приложений и для индикатора (лампочка не горит).
- `warm`: открываем при старте daemon, держим открытой. Минус: камера занята всегда. Плюс: -50 мс.
- `idle_keep_ms=N`: гибрид. Закрываем через N мс после auth. Хорошо для серии `sudo` команд.

Дефолт — `idle_keep_ms=10000`.

**Сравнение эмбеддингов.** Простая cosine similarity, threshold подбирается на энроллменте:
```cpp
float threshold = compute_threshold_from_enrollment(enrolled_embeddings);
// Берём min pair-wise cosine sim внутри энроллмента,
// вычитаем margin (например 0.05), но не ниже глобального floor (~0.4 для EdgeFace).
```

---

## 7. Pipeline энроллмента

CLI flow:

```
$ fastauth-cli enroll --label main
Подключение к fastauthd...
Готов? Смотрите прямо в камеру.
[Press Enter]
  Кадр 1/8: ok (quality 0.82)
  Кадр 2/8: look_straight
  Кадр 2/8: ok (quality 0.79)
  Кадр 3/8: too_dark
  ...
  Кадр 8/8: ok (quality 0.85)
Сохранено: 6 эмбеддингов высокого качества из 12 захваченных.
Порог сходства для этого энроллмента: 0.51
```

В daemon:
1. На `enroll_start` создаётся session с in-memory буфером эмбеддингов.
2. На каждый `enroll_frame` — захватываем кадр, прогоняем pipeline, считаем quality (детектор confidence × face size × sharpness).
3. На `enroll_finish` — фильтруем top-N по quality, считаем pairwise similarity, выбираем threshold, шифруем и сохраняем.

### Quality эвристика

```
quality = clamp(detector_conf, 0, 1)
        * clamp(face_height_px / frame_height_px / 0.3, 0, 1)
        * clamp(sharpness_laplacian / 100, 0, 1)
```

`sharpness_laplacian` = variance of Laplacian, классический blur detector из OpenCV. Размытые кадры выбрасываем.

### Hint'ы для пользователя

| Что обнаружено | hint |
|---|---|
| Лицо не найдено | `no_face` |
| Лицо < 15% высоты кадра | `too_far` |
| Лицо > 60% высоты кадра | `too_close` |
| Мean brightness < 30 | `too_dark` |
| Face landmarks показывают неровный yaw/pitch | `look_straight` |
| Sharpness < threshold | `blurry_hold_still` |
| Глаза закрыты (EAR < 0.2) | `blink_detected` |
| Всё ok | `ok` |

---

## 8. Хранение данных

```
/etc/fastauth/
  config.toml                      # системная конфигурация, root:root 0644

/var/lib/fastauth/
  models/                          # root:fastauth 0750
    detector.onnx
    embedder.onnx
    antispoof.onnx                 # опционально
  users/                           # fastauth:fastauth 0700
    1000/                          # fastauth:fastauth 0700, name = uid
      main.enc                     # fastauth:fastauth 0600
      glasses.enc

/run/fastauth/                     # tmpfiles.d создаёт, fastauth:fastauth 0755
  auth.sock                        # root:fastauth 0660
  mgmt.sock                        # fastauth:fastauth 0666

/usr/lib/security/
  pam_fastauth.so                  # root:root 0755

/usr/bin/
  fastauthd                        # root:root 0755
  fastauth-cli                     # root:root 0755

/usr/lib/systemd/system/
  fastauthd.service
  fastauthd.socket
```

### Формат файла энроллмента (`<label>.enc`)

В v1 — **без шифрования at-rest**. Защита — права 0700 на директорию uid'а + daemon под отдельным пользователем. Эмбеддинг не пароль, его нельзя «использовать» без `/dev/video2` доступа.

Бинарный формат, little-endian:
```
magic            "FA01"        4 bytes
created_unix     uint64        8 bytes
embedder_id      uint32        4 bytes      // hash of model file, для invalidate при апдейте моделей
threshold        float32       4 bytes
n_embeddings     uint32        4 bytes
embeddings       float32[n][512]            // нормализованные на L2
hint_metadata    json          remaining    // label, device, hostname, прочее для UX
```

**В v2** — опциональное шифрование через ключ из TPM 2.0 (sealed против PCR'ов). Или через keyring, ключ кладёт `pam_keyinit` после успешного password login. Документируем как future work.

### config.toml

```toml
[camera]
device = "/dev/v4l/by-path/pci-0000:00:14.0-usb-0:5:1.2-video-index0"
# fallback: "/dev/video2"
pixel_format = "GREY"          # единственный, что отдаёт 13d3:56eb на IR-интерфейсе
width = 640
height = 360
fps = 30                       # поддерживается 15 и 30, берём 30
dark_threshold = 30
camera_policy = "idle_keep"   # lazy | warm | idle_keep
idle_keep_ms = 10000

[recognition]
detector_model = "/var/lib/fastauth/models/detector.onnx"
embedder_model = "/var/lib/fastauth/models/embedder.onnx"
detector_conf_threshold = 0.5
similarity_floor = 0.40        # абсолютный минимум, ниже не аутентифицируем

[antispoof]
enabled = false                # включить когда модель оттюнена под IR
model = "/var/lib/fastauth/models/antispoof.onnx"
threshold = 0.7

[auth]
default_timeout_ms = 2000
max_concurrent_auths = 1

[log]
level = "info"                 # debug, info, warn, error
log_failed_attempts = true
log_successful_attempts = false
```

---

## 9. Privilege separation

| Процесс | uid | Capabilities | Доступ |
|---|---|---|---|
| `fastauthd` | `fastauth` | none | `/dev/video2` (через group `video`), `/var/lib/fastauth/**` (own), `/run/fastauth/*.sock` (own) |
| `pam_fastauth.so` | root (в контексте PAM) | none | только connect на `/run/fastauth/auth.sock`. Не читает `/var/lib/fastauth/`, не открывает камеру. |
| `fastauth-cli` | пользователь | none | только connect на `/run/fastauth/mgmt.sock`. |

Daemon **никогда не запускается под root**. systemd unit:
```ini
User=fastauth
Group=fastauth
SupplementaryGroups=video
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/fastauth /run/fastauth
PrivateTmp=true
PrivateDevices=false           # нам нужен /dev/video2
DeviceAllow=char-video4linux rw
RestrictAddressFamilies=AF_UNIX
RestrictNamespaces=true
LockPersonality=true
MemoryDenyWriteExecute=true    # ONNX Runtime может конфликтовать — проверить
SystemCallArchitectures=native
CapabilityBoundingSet=
AmbientCapabilities=
```

`MemoryDenyWriteExecute=true` — проверить совместимость с ONNX Runtime; если использует JIT, придётся отключить.

### Создание пользователя

`packaging/arch/PKGBUILD` post-install:
```bash
getent group fastauth >/dev/null || groupadd -r fastauth
getent passwd fastauth >/dev/null || useradd -r -g fastauth -G video -d /var/lib/fastauth -s /sbin/nologin fastauth
```

---

## 10. systemd integration

Socket activation: daemon стартует при первом подключении к `auth.sock`, остаётся живым.

**fastauthd.socket:**
```ini
[Unit]
Description=fastauth authentication socket

[Socket]
ListenStream=/run/fastauth/auth.sock
SocketUser=root
SocketGroup=fastauth
SocketMode=0660
Accept=false

ListenStream=/run/fastauth/mgmt.sock
SocketUser=fastauth
SocketGroup=fastauth
SocketMode=0666

[Install]
WantedBy=sockets.target
```

**fastauthd.service:**
```ini
[Unit]
Description=fastauth face authentication daemon
Requires=fastauthd.socket
After=fastauthd.socket

[Service]
Type=notify
ExecStart=/usr/bin/fastauthd
WatchdogSec=30s
Restart=on-failure
# ...все хардненинг из секции 9 выше
```

Daemon при старте делает `sd_notify(0, "READY=1")` после загрузки моделей.

**Tmpfiles** для `/run/fastauth/`:
```
d /run/fastauth 0755 fastauth fastauth -
```

### PAM конфигурация

В `/etc/pam.d/sudo` (или там где надо):
```
# fastauth ВСЕГДА sufficient, НИКОГДА required
auth   sufficient   pam_fastauth.so timeout=2000
auth   include      system-auth
```

Если daemon мёртв, сокет недоступен, или auth failed — PAM просто идёт на password. **Никогда не лочимся.**

---

## 11. Структура проекта

```
fastauth/
├── CMakeLists.txt
├── README.md
├── DESIGN.md                       # этот файл
├── LICENSE                         # MIT
├── .gitignore
│
├── cmake/
│   ├── FindONNXRuntime.cmake
│   └── CompileOptions.cmake        # -Wall -Wextra -Wpedantic -fstack-protector-strong
│
├── proto/
│   └── messages.hpp                # типы запросов/ответов, parse/serialize
│
├── common/
│   ├── ipc.{hpp,cpp}               # length-prefixed JSON over unix socket
│   ├── peer_cred.{hpp,cpp}         # SO_PEERCRED wrapper
│   ├── frame.{hpp,cpp}             # тип Frame, конверсии
│   ├── encoding.{hpp,cpp}          # Embedding, file format read/write
│   ├── config.{hpp,cpp}            # парсинг config.toml (toml++)
│   ├── logging.{hpp,cpp}           # обёртка над sd-journal
│   └── secure_buffer.{hpp,cpp}     # mlock + explicit_bzero RAII
│
├── daemon/
│   ├── main.cpp                    # entry point, parse args, drop privs, systemd notify
│   ├── camera.{hpp,cpp}            # V4L2: open, set format, MMAP buffers, capture
│   ├── camera_policy.{hpp,cpp}     # lazy / warm / idle_keep
│   ├── models.{hpp,cpp}            # ONNX Runtime wrappers (Detector, Embedder, AntiSpoof)
│   ├── pipeline.{hpp,cpp}          # capture → detect → align → embed → match
│   ├── enrollment.{hpp,cpp}        # session state, quality scoring, save
│   ├── matcher.{hpp,cpp}           # threshold logic, cosine sim
│   ├── server.{hpp,cpp}            # accept loop, dispatch по типу запроса
│   └── handlers/
│       ├── auth.{hpp,cpp}
│       ├── enroll.{hpp,cpp}
│       ├── list_remove.{hpp,cpp}
│       └── test.{hpp,cpp}
│
├── pam/
│   ├── pam_fastauth.c              # один файл, ~150 строк
│   └── ipc_client.c                # helper для подключения и обмена
│
├── cli/
│   └── main.cpp                    # argparse, команды: enroll/list/remove/test
│
├── systemd/
│   ├── fastauthd.service
│   ├── fastauthd.socket
│   └── tmpfiles.d/fastauth.conf
│
├── pam.d-examples/
│   ├── sudo.example
│   └── README.md                   # как добавлять в PAM безопасно
│
├── models/
│   └── README.md                   # ссылки на модели, инструкция как скачать
│
├── tests/
│   ├── unit/
│   │   ├── ipc_test.cpp
│   │   ├── encoding_test.cpp
│   │   ├── matcher_test.cpp
│   │   └── secure_buffer_test.cpp
│   ├── integration/
│   │   ├── auth_flow.sh            # запускает daemon в test mode, делает auth
│   │   └── enroll_flow.sh
│   └── fixtures/
│       └── sample_ir_frames/       # для regression тестов
│
└── packaging/
    └── arch/
        ├── PKGBUILD
        ├── fastauth.install         # pre/post hooks для создания пользователя
        └── README
```

---

## 12. Целевые показатели

| Метрика | Цель | Пороговое для v1 |
|---|---|---|
| Холодный auth (daemon живой, кадр не первый) | < 300 мс | < 500 мс |
| Тёплый auth (camera warm) | < 200 мс | < 300 мс |
| p99 latency auth | < 700 мс | < 1500 мс |
| Daemon RSS с моделями | < 120 МБ | < 200 МБ |
| Daemon idle CPU | ≈ 0% | < 0.5% |
| Daemon idle camera light | OFF (lazy/idle_keep после таймаута) | OFF |
| FAR (false accept rate) на личной калибровке | < 0.1% | < 1% |
| FRR (false reject rate) на нормальном освещении | < 5% | < 15% |

FAR/FRR измеряем на самописном test set из ~50 кадров «я» и ~50 «не я». Не публикационные цифры, но для одного пользователя в качестве sanity check работают.

---

## 13. Milestone'ы

**Каждый milestone — самостоятельная польза + проверяемая работоспособность.**

### M1: Скелет и захват кадра (1-2 дня)
- CMake скелет, базовая структура каталогов.
- Standalone утилита `fastauth-capture-test` — открывает `/dev/video2`, захватывает N кадров, пишет в `/tmp/`.
- **Цель:** убедиться что V4L2 mmap pipeline работает, разобраться с форматами (YUYV / GREY / MJPEG в зависимости от того что отдаёт камера).

### M2: ONNX Runtime + один кадр (1-2 дня)
- Скачать SCRFD-500MF + EdgeFace ONNX модели.
- Standalone утилита `fastauth-detect-test` — берёт JPEG из файла, прогоняет детектор, потом embedder, печатает confidence и embedding vector.
- **Цель:** убедиться что модели грузятся и inference работает на CPU за разумное время.

### M3: Полный pipeline end-to-end (без daemon ещё) (2-3 дня)
- Утилита `fastauth-pipeline-test`: open camera → capture loop → detect → embed → если есть `~/.cache/fastauth/test_embedding.bin`, сравнить cosine sim, иначе сохранить как «энроллмент».
- **Цель:** доказательство концепции — за сколько мс реально определяется лицо.

### M4: IPC и daemon (2-3 дня)
- `fastauthd` запускается, слушает сокет, отвечает на `test` запрос (без логики пока).
- `fastauth-cli test` подключается, получает ответ.
- systemd unit + socket activation.
- Drop privileges, hardening.
- **Цель:** живая инфраструктура без ML-логики.

### M5: Auth handler (1-2 дня)
- Соединяем M3 и M4: `auth` запрос реально гоняет pipeline, возвращает yes/no.
- Кэш загруженных энроллментов в памяти.
- **Цель:** `fastauth-cli test` показывает что ваше лицо распознаётся.

### M6: PAM модуль (2-3 дня)
- `pam_fastauth.c`, минимальный.
- **ОЧЕНЬ ОСТОРОЖНО:** держать TTY открытым, тестировать на `sudo -i`, не на login screen.
- **Цель:** `sudo whoami` работает по лицу.

### M7: Энроллмент CLI (2-3 дня)
- `fastauth-cli enroll`, `list`, `remove`.
- Quality scoring, hint'ы, прогресс.
- **Цель:** полный пользовательский flow без редактирования файлов руками.

### M8: Хардненинг и QoL (несколько дней)
- Опциональный anti-spoof.
- Конфиг файл.
- Структурированные логи в journal.
- Внятные сообщения об ошибках.
- README с troubleshooting.

### M9: Пакетирование (1-2 дня)
- AUR PKGBUILD.
- Хуки для создания пользователя.
- Примеры PAM конфигов в `/etc/fastauth/pam-examples/`.

Реалистичный срок — **3–5 недель вечерами** для одного человека с опытом C++.

---

## 14. Открытые вопросы

Решения, которые лучше отложить до начала имплементации:

1. **Anti-spoof обязателен в v1?** IR + reasonable threshold уже неплохо защищают. Возможно вынести в v2 после реальных тестов на спуф.
2. **TPM-sealed storage** — однозначно v2. Усложняет dev, не критично для одного юзера.
3. **Поддержка нескольких энроллментов на uid** — да, поддерживаем by design (с очками, без, разное освещение).
4. **Auto-update эмбеддингов** (если auth прошёл с высокой confidence, добавить эмбеддинг в энроллмент) — соблазнительно, но создаёт concept drift и attack vector. **Не делать в v1.**
5. **Web UI / GUI для энроллмента** — не v1.
6. **Кросс-дистрибутивные пакеты** — после стабилизации Arch версии.
7. **Какую модель выбрать дефолтной — EdgeFace или MobileFaceNet** — решить на M2 после тестов на IR кадрах.

---

## 15. Риски и подводные камни

### Технические

- **MemoryDenyWriteExecute vs ONNX Runtime.** ONNX может использовать JIT в некоторых providers. Проверить с CPU EP, скорее всего ок.
- **IR-камера и V4L2 controls.** Возможно понадобится принудительно ставить exposure / gain через `v4l2_ioctl(VIDIOC_S_CTRL)`. У IMC Networks `13d3:56eb` поведение надо протестировать на M1.
- **Первые кадры мусорные.** Многие UVC камеры дают 2-5 пустых/тёмных кадров пока exposure не стабилизируется. На lazy/cold open это значит +100-200 мс. Можно делать тёплый прогрев в фоне.
- **Модели тренированы на RGB.** IR может давать сниженную accuracy. План: измерить FAR/FRR на M5, если плохо — попробовать MobileFaceNet вместо EdgeFace, в крайнем случае fine-tune.
- **Threshold подбор.** Слишком низкий → false accepts (катастрофа), слишком высокий → пользователь страдает. Per-enrollment адаптивный threshold + глобальный floor.

### Безопасность разработки (отдельно, потому что важно)

- **PAM ошибка может залочить.** ВСЕГДА:
  - держать второй TTY открытым (`Ctrl+Alt+F2` в Arch, или второй SSH).
  - тестировать новые `.so` сначала на `sudo` (с открытым sudo session в одном tty, тестить в другом).
  - **никогда** не редактировать `/etc/pam.d/system-auth` или `/etc/pam.d/login` до тех пор пока всё не оттестировано на `sudo`.
  - всегда `auth sufficient pam_unix.so try_first_pass` ПЕРЕД строкой с fastauth в новых конфигах в первое время.
- **Daemon под root для отладки** — соблазнительно, но не делайте. Сразу настройте `User=fastauth` в systemd.
- **Не коммитить test embeddings в репо.** В gitignore: `*.enc`, `*.bin`, `~/.cache/fastauth/`.

### Продуктовые

- **Эта штука не для всех.** Если рядом есть похожий человек или близнец — система обманывается. Документировать честно.
- **Освещение.** Хороший IR-эмиттер делает систему устойчивой к темноте, но в полной темноте без эмиттера — мёртв. На некоторых ASUS эмиттер motion-reactive, в неподвижности может гаснуть. На `13d3:56eb` IMC Networks надо проверить поведение.
- **Update моделей сломает энроллмент.** В формате файла храним `embedder_id` (хэш файла модели). При несовпадении — fail с просьбой переэнроллиться.

---

## 16. Чеклист безопасности разработки

Распечатать и приклеить к монитору:

- [ ] Перед любым изменением `/etc/pam.d/*` — открыт второй TTY с залогиненным root.
- [ ] Перед первым тестом нового `pam_fastauth.so` — он тестируется на `sudo`, а не на login.
- [ ] В PAM конфиге fastauth — `sufficient`, никогда `required`.
- [ ] Бэкап `/etc/pam.d/` сделан до начала работы (`sudo cp -r /etc/pam.d /etc/pam.d.bak`).
- [ ] Daemon запускается под uid `fastauth`, никогда не под root в проде.
- [ ] Все секреты (embeddings) в `secure_buffer` с `mlock` + `explicit_bzero` на освобождении.
- [ ] Logging уровня `debug` НЕ логирует embedding векторы (только метаданные).
- [ ] Все ONNX модели проверены `sha256sum` против опубликованных значений перед использованием.

---

## Приложение A: Аппаратные заметки

**Целевая железка:** ASUS ZenBook 13 Flip с IR-камерой `IMC Networks 13d3:56eb` (USB2.0 HD UVC WebCam).

- IR это `/dev/video2`. RGB — `/dev/video0`. `video1`/`video3` — метаданные.
- `13d3:56eb` НЕ относится к проблемной серии Sonix `3277:0018`, поэтому `linux-enable-ir-emitter` НЕ нужен и НЕ должен использоваться.
- Подтверждено: оригинальный Howdy работает (медленно).

Стабильный путь к устройству (проверено через `ls -la /dev/v4l/by-path/`):
```
/dev/v4l/by-path/pci-0000:00:14.0-usb-0:5:1.2-video-index0
```

Это надёжнее чем `/dev/video2` — `videoN` могут переноминаться между бутами.

**Capabilities проверенные через `v4l2-ctl --device=/dev/video2 --list-formats-ext`:**
- Pixel format: единственный `GREY` (8-bit greyscale), без YUYV/MJPEG альтернатив.
- Frame size: единственный `640×360`.
- FPS: `15` или `30` (берём 30 по дефолту).
- Image size: 230400 байт на кадр.
- Driver: `uvcvideo`, стандартный V4L2 mmap pipeline.

### M1 findings (измерено на `tools/m1_capture_test.cpp`, 2026-05-24)

Реальные тайминги cold-open на ZenBook:

| Этап | Время |
|---|---|
| `open` + `VIDIOC_QUERYCAP` | ~0.05 мс |
| `VIDIOC_S_FMT` | **~106 мс** |
| `S_PARM` + `REQBUFS` + `mmap` + `STREAMON` | ~1 мс |
| `select()` до первого DQBUF | ~200 мс |
| **open → первый кадр (cold)** | **~308 мс** |
| Per-frame после warmup | ~64–68 мс |

Два сюрприза с прямым влиянием на дизайн:

1. **Effective fps ≈ 15, не 30.** Драйвер принимает `VIDIOC_S_PARM` с `1/30`, но реальный интервал между кадрами ~67 мс (то есть 15 fps). Похоже UVC auto-exposure ограничивает fps в условиях слабого освещения. **Бюджет в Section 6 пересчитан: один кадр ≈ 67 мс, в timeout 2000 мс умещается ~30 попыток, не 60.**

2. **IR-эмиттер мигает в неподвижности.** Mean brightness кадров строго чередуется: `41 / 19 / 41 / 19 / 41 / 19`. Каждый второй кадр — без подсветки. Об этом было предупреждение в Section 15, на практике подтвердилось: эмиттер motion-reactive с периодом 2 кадра. **Это делает шаг 5b pipeline (`mean_brightness < dark_threshold? → continue`) обязательным, не опциональным** — иначе половина попыток уйдёт на тёмные кадры.

**Следствие для camera_policy:** `lazy` практически нереалистичен — 308 мс cold-open съедает 77% бюджета `< 400 мс` ещё до начала pipeline. Дефолт `idle_keep_ms=10000` обоснован; для тяжёлых сценариев (серия `sudo`) можно подумать про `warm`.

---

## Приложение B: Что НЕ делаем

Список вещей, которые соблазняют, но в первой версии нет:

- Многофакторные сценарии («лицо + PIN»).
- Учёт времени суток / контекста («не аутентифицируй ночью»).
- Federated learning, шеринг моделей.
- Сетевая синхронизация энроллментов.
- Поддержка нескольких камер.
- Анализ эмоций, age/gender estimation.
- Что-либо в браузере.

Если хочется — отдельный проект.

---

*Последнее обновление: при создании. Версия документа: 0.1.*
