# chowdy

Быстрая face-аутентификация для Linux через IR-камеру. Читается как
**«C-howdy»** — Howdy, только переписанный на C++ как резидентный
daemon. Цель — холодный auth ≤ 400 мс, ради сравнения у оригинального
Howdy 2–4 с.

> ⚠️ chowdy — это **удобство**, а не более сильная аутентификация чем
> пароль. В PAM правило всегда `sufficient`, никогда `required`. Не
> используйте как единственный фактор. См. `DESIGN.md` §3 (Threat
> model) и §16 (security checklist).

---

## Установка

### Из AUR

```sh
yay -S chowdy
# или
paru -S chowdy
```

Пакет ставит:

- `/usr/bin/chowdyd`, `/usr/bin/chowdy-cli`
- `/usr/lib/security/pam_chowdy.so` (не подключается к PAM stack автоматически)
- `/usr/lib/systemd/system/chowdyd.{service,socket}` (не активируются автоматически)
- `/usr/lib/tmpfiles.d/chowdy.conf`
- `/etc/chowdy/config.toml` (помечен `backup=`, переживает upgrade)
- Документация в `/usr/share/doc/chowdy/`

Пост-install hook (`chowdy.install`) автоматически:

- создаёт системного пользователя `chowdy` (group `video` для `/dev/video2`)
- создаёт `/var/lib/chowdy/{models,users}` с правильными правами (`0750`/`0700`)
- выполняет `systemd-tmpfiles --create` и `systemctl daemon-reload`
- печатает quick-start в Russian

Сценарий **полностью идемпотентный** — если пользователь уже создан
(например от Howdy с тем же именем — крайне маловероятно), ничего не
ломается.

### После установки пакета

#### 1. Скачать ONNX модели

Лицензия InsightFace research запрещает редистрибуцию моделей в
пакете, поэтому скачивай руками. Команды из
`/usr/share/doc/chowdy/MODELS.md`:

```sh
# Один buffalo_s.zip содержит обе модели (~16 МБ нужных из ~122 МБ архива)
curl -L -o /tmp/buffalo_s.zip \
  https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip
unzip -j -o /tmp/buffalo_s.zip det_500m.onnx w600k_mbf.onnx -d /tmp/
sudo install -m 0640 -o root -g chowdy /tmp/det_500m.onnx \
                                       /var/lib/chowdy/models/detector.onnx
sudo install -m 0640 -o root -g chowdy /tmp/w600k_mbf.onnx \
                                       /var/lib/chowdy/models/embedder.onnx
rm /tmp/buffalo_s.zip /tmp/det_500m.onnx /tmp/w600k_mbf.onnx

# Проверка целостности (см. MODELS.md для актуальных sha256)
sudo sha256sum /var/lib/chowdy/models/detector.onnx \
               /var/lib/chowdy/models/embedder.onnx
```

#### 2. Поднять demon

```sh
sudo systemctl enable --now chowdyd.socket
systemctl status chowdyd.socket
# при первом запросе daemon стартует автоматически через socket activation
```

#### 3. Проверь что камера видна

В config по умолчанию `/dev/v4l/by-path/pci-…-video-index0`
(оптимально для ASUS ZenBook 13 Flip с IR-камерой IMC Networks
`13d3:56eb`). Если у тебя другое железо — поправь
`device` в `/etc/chowdy/config.toml`. Для определения путей:

```sh
v4l2-ctl --list-devices
ls /dev/v4l/by-path/
```

Камера должна поддерживать pixel format `GREY` (8-bit greyscale) —
типично для IR UVC.

#### 4. Энроллмент

```sh
sudo chowdy-cli enroll --label main -n 12
```

Энроллмент **требует sudo** — добавление нового лица означает добавление
нового учётного фактора, поэтому сначала система должна убедиться что
это делаешь действительно ты (через пароль sudo). Daemon отвергает
enroll/remove от непривилегированных процессов (`SO_PEERCRED != root`).
Лицо при этом привязывается к пользователю, вызвавшему sudo
(`SUDO_UID`), а не к root.

CLI попросит нажать Enter и подержать позу. **Полезно** в процессе:

- первые 3 кадра — прямо в камеру
- следующие 3 — чуть поверни голову влево
- ещё 3 — вправо
- последние 3 — лёгкая улыбка / другое выражение

Чем разнообразнее набор — тем ниже автоматически подобранный threshold
и выше шансы на match в реальных условиях.

#### 5. Проверь auth

```sh
chowdy-cli auth-test
# {"reason":"matched","confidence":0.87,"elapsed_ms":371,"success":true,...}
```

Несколько прогонов: если sim стабильно > пороговой (`chowdy-cli list`
покажет `Порог`), переходи к PAM. Если на грани (sim ~0.65, threshold
0.64) — повтори enroll с большим разнообразием:

```sh
sudo chowdy-cli remove --label main
sudo chowdy-cli enroll --label main -n 12   # с движением, см. шаг 4
```

---

## Настройка PAM

> 🚨 **Опасная зона.** Ошибка может залочить тебя из системы.
> Обязательно следуй чек-листу. Подробно — `/usr/share/doc/chowdy/PAM-INSTALL.md`.

**Перед редактированием `/etc/pam.d/*`:**

- [ ] Открой **второй TTY** (`Ctrl+Alt+F2`) и **залогинься под root**
      (пароль рута, не sudo). Не закрывай его до конца тестирования.
- [ ] Бэкап: `sudo cp -r /etc/pam.d /etc/pam.d.bak.$(date +%Y%m%d-%H%M)`
- [ ] Убедись что `chowdy-cli auth-test` уверенно возвращает success.

**Добавление в sudo** (начни именно с sudo, не с login):

```sh
# Вставить pam_chowdy ПЕРЕД существующей цепочкой
sudo sed -i '/^auth.*include.*system-auth/i\auth    sufficient    pam_chowdy.so timeout=2000' /etc/pam.d/sudo

# Проверь результат
cat /etc/pam.d/sudo
# должна быть строка:
#   auth    sufficient    pam_chowdy.so timeout=2000
#   auth        include        system-auth
#   ...

# Сбросить sudo cache (иначе PAM не будет вызвана)
sudo -k

# Тест
sudo whoami
# должно быть "root" без запроса пароля
```

Если что-то пошло не так — `sudo` спросит пароль (так и должно быть
при `sufficient`), либо в root TTY восстанови бэкап:

```sh
sudo cp /etc/pam.d.bak.YYYYMMDD-HHMM/sudo /etc/pam.d/sudo
```

**После того как sudo работает** можно осторожно добавлять в другие
stack'и — `kde` для GUI password prompts от polkit, `gdm-password`
для display manager и т.д. **Никогда не добавляй в `system-auth`
или `login`** пока не покрутишь хотя бы неделю на sudo.

### Параметры `pam_chowdy.so`

В строке PAM можно задать:

- `timeout=MS` — общий бюджет включая connect (default 2000)
- `socket=PATH` — переопределить путь к `auth.sock`
- `debug` — больше деталей в syslog (`journalctl -t pam_chowdy`)

---

## Конфигурация

`/etc/chowdy/config.toml` (помечен `backup=`, переживает pacman
upgrade). Основные секции:

```toml
[camera]
device         = "/dev/v4l/by-path/pci-0000:00:14.0-usb-0:5:1.2-video-index0"
camera_policy  = "lazy"        # lazy | warm | idle_keep
idle_keep_ms   = 10000          # для idle_keep
dark_threshold = 25             # отсечение off-фаз IR-эмиттера

[recognition]
detector_conf_threshold = 0.5
similarity_floor        = 0.40
enroll_quality_min      = 0.10  # на IR-сенсоре Laplacian низкий

[auth]
default_timeout_ms = 2000

[log]
level = "info"                  # debug | info | notice | warn | error
```

### camera_policy trade-off

| Policy | Холодный auth | Лампочка камеры | Камера в других приложениях |
|---|---|---|---|
| `lazy` (default) | ~300–700 мс | гаснет сразу после auth | свободна |
| `warm` | ~35 мс | горит постоянно | занята |
| `idle_keep` + `idle_keep_ms=N` | ~35 мс если в окне N мс | гаснет через ~1–1.5×N | занята всё это время |

После изменения config: `sudo systemctl restart chowdyd`.

---

## Что внутри

- **`chowdyd`** — резидентный C++ daemon, держит ONNX модели в RAM и
  V4L2-камеру. Работает под uid `chowdy`, без caps, доступ к
  `/dev/video2` через group `video`. Systemd unit с полным
  hardening (`NoNewPrivileges`, `ProtectSystem=strict`,
  `RestrictAddressFamilies=AF_UNIX`, пустой `CapabilityBoundingSet`).
- **`chowdy-cli`** — пользовательский CLI: `enroll`, `list`, `remove`,
  `test`, `auth-test`. Через unix-socket `/run/chowdy/mgmt.sock`
  (mode `0666`). Read-only команды (`list`, `test`, `auth-test`)
  доступны любому юзеру для своих данных (`SO_PEERCRED`); мутирующие
  (`enroll`, `remove`) daemon принимает **только от root** — запускай
  через sudo, целевой пользователь берётся из `SUDO_UID`.
- **`pam_chowdy.so`** — минимальный PAM модуль на C, дёргает daemon
  через `/run/chowdy/auth.sock` (mode `0660 root:chowdy`). При любой
  ошибке IPC возвращает `PAM_AUTHINFO_UNAVAIL` — стек идёт дальше,
  никаких локов.
- **Pipeline:** SCRFD-500MF detector → 5-point similarity alignment
  → MobileFaceNet (WebFace600K) embedder → L2-нормированный 512-d
  → cosine similarity против сохранённых embeddings.

Подробный дизайн и threat model — `/usr/share/doc/chowdy/DESIGN.md`.

---

## Совместимость с оригинальным Howdy

Можно жить параллельно — chowdy и Howdy используют тот же
`/dev/video2`, но V4L2 streaming эксклюзивный per-device.
chowdy умеет ждать (retry-on-EBUSY backoff в Camera) если Howdy
сейчас держит камеру. Если оба настроены в PAM как `sufficient` —
работает тот кто успевает первым.

В долгую: chowdy → одна race-prone зависимость меньше. Когда уверенно
работает — убирай Howdy из PAM:

```sh
sudo sed -i '/pam_howdy/d' /etc/pam.d/sudo /etc/pam.d/kde
```

---

## Удаление

```sh
sudo pacman -R chowdy
```

`pacman -R` снимет бинари, units, конфиг (если не правлен — твои
правки удерживаются как `.pacsave`). НЕ удалится:

- `/var/lib/chowdy/` (модели + энроллменты — pacman оставляет
  user data; `post_remove` hook напоминает)
- системный пользователь `chowdy` и группа
- строки `pam_chowdy.so` в `/etc/pam.d/*` (это твои правки)

**Перед `pacman -R` обязательно**:

```sh
# Убери chowdy из PAM, иначе после удаления .so там будет ссылка на
# несуществующий модуль и стек упадёт в "не знаю что делать"
sudo sed -i '/pam_chowdy/d' /etc/pam.d/sudo /etc/pam.d/kde
sudo systemctl disable --now chowdyd.socket chowdyd.service
```

Полная очистка после удаления пакета:

```sh
sudo rm -rf /var/lib/chowdy
sudo userdel chowdy
sudo groupdel chowdy
```

---

## Целевая платформа

Тестировалось на:

- **ASUS ZenBook 13 Flip** / Arch Linux, kernel 7.0.x
- IR-камера **IMC Networks `13d3:56eb`** (`/dev/video2`)
- Pixel format `V4L2_PIX_FMT_GREY` 640×360 @ 15–30 fps

Другие IR UVC камеры с похожими параметрами скорее всего тоже
заработают — поменяй `device`, `width`, `height`, `fps` в
`/etc/chowdy/config.toml`. RGB камеры не поддерживаются (концептуально:
chowdy = IR-only, см. `DESIGN.md` §1).

Зависимости (Arch): `opencv libsodium systemd-libs pam onnxruntime
nlohmann-json tomlplusplus`. Все из `extra` репозитория.

---

## Сборка из исходников (dev)

Если правишь код или хочешь свежую сборку без AUR:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Полные build-deps: `cmake ninja pkgconf git` + всё из runtime deps.

Опции:

- `-DCHOWDY_BUILD_TOOLS=OFF` — пропустить тестбеды `tools/m{1,2,3}_*`
- `-DCHOWDY_BUILD_PAM=OFF` — пропустить PAM модуль
- `-DCHOWDY_BUILD_TESTS=ON` — собрать unit-тесты (GoogleTest)

### Установка без pacman

Если не хочешь возиться с pacman'ом — `tools/install-dev.sh` делает
ровно то же что AUR пакет, но из `build/`:

```sh
sudo bash tools/install-dev.sh
```

Снять: `sudo bash tools/uninstall-dev.sh` (без `--purge` оставляет
`/var/lib/chowdy/` с моделями и энроллментами).

Re-install после правок: `sudo bash tools/reinstall-dev.sh`.

---

## Документация

В установленном пакете лежит в `/usr/share/doc/chowdy/`:

- `README.md` — этот файл
- `DESIGN.md` — полный дизайн-документ, threat model, milestone'ы,
  замеры на железе
- `PAM-INSTALL.md` — расширенный чек-лист настройки PAM
- `MODELS.md` — где скачать ONNX модели и SHA-256
- `SYSTEMD.md` — содержимое юнитов и пути

---

## Лицензия

Код — MIT (см. `LICENSE`).

ONNX модели — **InsightFace non-commercial research license**,
скачиваются отдельно (не входят в пакет). Для личного использования
ok; коммерческое распространение требует отдельной договорённости с
InsightFace.
