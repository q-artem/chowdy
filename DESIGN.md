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
- `lazy` (**default**): открываем `/dev/video2` при первом запросе, закрываем после ответа. Минус: cold open ~300 мс на каждый auth (на 13d3:56eb из-за VIDIOC_S_FMT + warmup-кадров). Плюс: камера свободна, лампочка гаснет сразу после auth — лучший UX по приватности.
- `warm`: открываем при старте daemon, держим открытой. Плюс: warm auth ~35 мс. Минус: камера занята всегда, лампочка горит постоянно.
- `idle_keep_ms=N`: гибрид. Закрываем через N мс после auth. Помогает только если ожидается retry внутри окна (несколько разных PAM-вызовов подряд). Серия `sudo` сюда **НЕ** относится — sudo кэширует креды на 15 минут и PAM повторно не звонит.

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

**Следствие для camera_policy:** `lazy` съедает 77% бюджета `< 400 мс` на cold open и не помещается в цель Section 1, **но** укладывается в пороговое `< 1500 мс p99` и даёт лучший UX (лампочка камеры не горит). Изначально дефолт планировался `idle_keep_ms=10000`, но это обоснование («серия sudo не платит auth дважды») оказалось неверным — sudo кэширует креды и второй раз PAM не зовёт. Реальные сценарии для `idle_keep` это только retry после failed auth — а 2–3 секунд для этого достаточно. **Default переключён на `lazy`** (см. также `etc/fastauth/config.toml.example`). Хочешь warm-перформанс — `camera_policy = "warm"` явно.

### M2 findings (измерено на `tools/m2_detect_test.cpp`, 2026-05-24)

Pipeline detect → embed на одном кадре от M1:

| Этап | Время (Release, 2 intra-op threads) |
|---|---|
| Загрузка SCRFD-500MF | ~22 мс |
| Загрузка MobileFaceNet (w600k_mbf) | ~32 мс |
| SCRFD preproc (letterbox + normalize) | ~2 мс |
| SCRFD inference | ~20 мс |
| SCRFD decode + NMS | < 0.1 мс |
| Embedder preproc (5-point similarity align) | ~1.5 мс |
| Embedder inference | ~13–17 мс |
| **Total recognize() warm** | **~37–41 мс** |

С учётом Section 6 шага `b` (skip dark frames, ~67 мс на кадр на 15 fps), холодный auth на 2-м кадре получается ≈ `308 (cold open) + 67 (dark skip) + 67 (capture) + 41 (pipeline) = 483 мс`. Это превышает «< 400 мс» от Section 1 на ~80 мс — но при `camera_policy=warm` или `idle_keep` всё попадает в ~110 мс (capture + pipeline без open).

Артефакты:
- **EdgeFace ONNX недоступен.** Ни в `otroshi/edgeface`, ни в `Idiap/EdgeFace-*` коллекции на HF — везде только `.pt`. Лицензия HF-зеркал — CC-BY-NC-SA-4.0, не Apache. Конвертацию PT→ONNX в проекте не делаем (нет PyTorch). Поэтому embedder — **MobileFaceNet на WebFace600K** (`w600k_mbf.onnx`, 13.6 МБ) из `buffalo_s`. **Лицензия моделей: InsightFace non-commercial research.** Это сужает потенциал коммерческой дистрибуции — для личного использования OK, для AUR public package — уточнить в M9.
- **SCRFD ONNX layout sanity-check.** Выходы — rank-2 `[N, K]` (без batch-dim), три stride'а {8, 16, 32}, два anchor'а на cell, distance2bbox decoding с anchor-центром `((x+0.5)*stride, (y+0.5)*stride)`.
- **IR-эмиттер реально помогает.** На «тёмных» кадрах с включённым эмиттером (brightness ~19) SCRFD даёт score ~0.75, на «светлых» без эмиттера (brightness ~41) — < 0.05. То есть «тёмный» по среднему — это **лицо с подсветкой на чёрном фоне**, что для face detection идеально. Перевернуть интерпретацию `dark_threshold` в Section 6: не «отбрасываем темнее N», а возможно «отбрасываем где лицо не подсвечено» — точная эвристика требует ещё измерений на M3. **ОПРОВЕРГНУТО НА M3** — см. ниже.

### M3 findings (измерено на `tools/m3_pipeline_test.cpp`, 2026-05-24)

Первый живой end-to-end на камере. Pipeline: open camera → loop {capture → SCRFD → align → MobileFaceNet → cosine match against enrollment}. Я перед камерой → **MATCH**, я не перед камерой → **FAIL без ложных срабатываний**.

Замеры на ZenBook (cold processes, не daemon):

| Тест | Total | Attempts | Лучший sim | Verdict |
|---|---|---|---|---|
| Auth с лицом #1 (cold) | 503 мс | 5 (4 warmup + 1 capture) | 0.862 | MATCH |
| Auth с лицом #2 (cold) | 490 мс | 5 | 0.892 | MATCH |
| Auth с прикрытой камерой | 2016 мс | 28 | n/a | FAIL (0 faces) |

Что внутри positive 503 мс: models load 65 мс + camera open 108 мс + 3 warmup-кадра × ~67 мс + 1 noface-кадр + 1 detect-кадр × ~38 мс ≈ 500 мс. **При warm daemon (модели уже в RAM, камера уже открыта) останется ~110 мс** — это уверенно бьёт цель Section 12 «< 200 мс warm».

Enrollment: собрать 8 эмбеддингов с quality-фильтром = ~1.8 секунды. Picked threshold = `min_pairwise_cosine − 0.05`, в нашем тесте = 0.638. Реальные positive sim'ы 0.86–0.89 значительно выше — запас ~0.25 для устойчивости к освещению/повороту.

Поправки к дизайну (требуют изменений в Section 6 и 7 при реализации M5):

- **Brightness-гипотеза из M2 опровергнута.** На живом потоке, когда я перед камерой: `bright≈19 → no face`, `bright≈45 → score 0.73`. То есть «тёмные» кадры — это **off-фаза мигающего IR-эмиттера без активной подсветки**, а вовсе не «лицо на чёрном фоне». На M1 я был не перед камерой, и SCRFD случайно давал score 0.75 на шуме «тёмных» кадров — это false positive от детектора на сенсорном noise, ничего общего с реальным лицом. Эвристика Section 6 шаг 5b («skip dark frames») — **правильная в исходной формулировке**, нужно отбрасывать кадры с `mean_brightness < dark_threshold` (порог ~25–30).
- **Эмиттер пульсирует с фиксированной частотой ~15 Гц**, не «motion-reactive». Половина захваченных кадров (off-фаза) всегда без лица. На 15 fps effective = ~7.5 useful fps. Это укладывается в бюджет (нужен 1–2 successful frame для MATCH), но `default_timeout_ms=2000` оставлять.
- **`kEnrollQualityMin=0.10`, не 0.25 из Section 7.** На IR-сенсоре variance(Laplacian) на face crop стабильно низкий (20–50), что даёт quality множитель 0.10–0.25 даже для нормальных кадров. Формула в Section 7 рассчитана на RGB-сенсор с большей детализацией. Для IR `sharpness_laplacian / 100` нужно заменить на `/ 30` или ослабить порог.
- **SCRFD ловит даже слабую side-view** (например рука над камерой, частично закрывающая лицо) с score 0.7+ если есть структурные признаки. Это означает что **anti-spoof в v2 может быть важнее чем казалось** — IR + threshold защищает от полной подмены, но не от частичного closeup.

Лицензионная заметка: тестовые embeddings хранятся в `~/.cache/fastauth/test_embedding.bin` (формат FA01, см. §8). Это просто бинарный дамп 8 × 512 float, читаемый только локально и привязан к `embedder_id` (hash файла модели) — при апдейте модели daemon должен fail-with-message и просить переэнроллиться.

### M5 findings (резидентный daemon с реальным auth handler, 2026-05-24)

Рефактор `tools/m3_pipeline_test.cpp` в `daemon/{camera,models,pipeline,enrollment_store}.cpp` + integration в `handlers/auth.cpp` через `Pipeline*`/`EnrollmentStore*` в `Context`. Daemon под текущим юзером, сокеты в `/tmp/fastauth-smoke/`, enrollment из M3 переименован в `users/1000/main.enc`.

| Сценарий | success | confidence | elapsed |
|---|---|---|---|
| Cold (первый auth-test после старта daemon) | true | 0.774 | 1813 мс |
| Warm #1 | true | 0.753 | **38.1 мс** |
| Warm #2 | true | 0.725 | **32.3 мс** |
| Warm #3 | true | 0.862 | **36.7 мс** |
| Negative (камера прикрыта) | false | 0.000 | 1999 мс (`reason: timeout`) |

Warm auth попадает в ~30–40 мс — **в 4× быстрее цели Section 12 «< 200 мс warm»** и в 13× быстрее cold M3 testbed (503 мс). Cold (1813 мс) — это разовая стоимость загрузки моделей + первый VIDIOC_S_FMT + 3 warmup-кадра + один capture с лицом. После cold камера и сессии ORT держатся в RAM (`warm` policy на M5 hardcoded; `idle_keep_ms` уйдёт в M8). Negative test возвращается за полный timeout без false positive.

Архитектурные решения, реализованные на M5:

- **Camera policy = warm.** Pipeline::ensure_camera_open() идемпотентна, после первого вызова камера не закрывается до завершения daemon. `idle_keep_ms` из DESIGN §6 — задача M8.
- **Один `std::mutex` на Pipeline.** Сериализует capture, чтобы параллельные auth-запросы от PAM не дрались за `/dev/video2`. Connection threads короткоживущие, конкуренции на mutex почти нет.
- **`EnrollmentStore` с mtime-кэшем.** Загруженные FA01 файлы кэшируются в RAM, инвалидация по последнему `mtime` файлов в `<users_dir>/<uid>/`. CLI enroll (M7) перезаписывает .enc через rename — mtime меняется, кэш дропается.
- **`embedder_id` mismatch handling.** На auth daemon фильтрует enrollments по `embedder_model_id()` Pipeline. Mismatch → response `reason: embedder_mismatch`. Это сценарий "пересобрал модель" — фейл с понятным сообщением вместо случайных результатов.
- **`SO_PEERCRED` gate в `handle_auth`**: `peer.uid==0` (PAM от root) ИЛИ `peer.uid==req.uid` (self-auth для CLI). Иначе `reason: peer_denied`. Это §3 threat model в действии.

Что НЕ делается на M5 (планы):
- `idle_keep_ms` (камера закрывается через N мс после auth) — M8.
- Конфиг из `/etc/fastauth/config.toml` — M8; пока всё через `--detector / --embedder / --device / --users-dir` CLI флаги.
- Quality scoring в production (на M5 он считается, но порог `enroll_quality_min` ещё не применяется в auth path — нужен только для enroll'а в M7).
- sd-journal structured logging — M8; пока stderr fallback.

Команды smoke-test (для повторения):
```
$ mkdir -p /tmp/fastauth-smoke/users/$(id -u)
$ cp ~/.cache/fastauth/test_embedding.bin \
     /tmp/fastauth-smoke/users/$(id -u)/main.enc
$ ./build/daemon/fastauthd --foreground --log-level info \
    --auth-socket /tmp/fastauth-smoke/auth.sock \
    --mgmt-socket /tmp/fastauth-smoke/mgmt.sock \
    --users-dir /tmp/fastauth-smoke/users \
    --detector models/scrfd_500m_bnkps.onnx \
    --embedder models/w600k_mbf.onnx &
$ ./build/cli/fastauth-cli \
    --mgmt-socket /tmp/fastauth-smoke/mgmt.sock \
    --auth-socket /tmp/fastauth-smoke/auth.sock auth-test
```

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
