# Arch packaging

PKGBUILD и `.install` хуки для `chowdy` на AUR.

## Текущая конфигурация

PKGBUILD здесь — **AUR-ready**: тянет release tarball с GitHub
(`https://github.com/q-artem/chowdy/archive/v$pkgver.tar.gz`), `sha256`
зашит явно. Это позволяет:

- любому юзеру Arch'а: `yay -S chowdy` / `paru -S chowdy` / `makepkg -si`
- сборщику AUR: тот же PKGBUILD без модификаций

Для **dev-итерации** (правишь код, хочешь сразу запустить — без push
и release) **не используй** `makepkg`. Возьми `tools/install-dev.sh`
из корня репо.

## Что внутри `makepkg -si`

1. **fetch.** Скачивает `v$pkgver.tar.gz` с GitHub, проверяет `sha256`.
2. **build().** `cmake -S . -B build` с `-DCHOWDY_BUILD_TOOLS=OFF`
   (тестбеды `tools/m{1,2,3}_*` в production не нужны),
   `cmake --build build`.
3. **package().** Раскладывает файлы в `pkg/`:

       /usr/bin/{chowdyd,chowdy-cli}
       /usr/lib/security/pam_chowdy.so
       /usr/lib/systemd/system/chowdyd.{service,socket}
       /usr/lib/tmpfiles.d/chowdy.conf
       /etc/chowdy/config.toml          (помечен backup=)
       /usr/share/doc/chowdy/*.md
       /usr/share/licenses/chowdy/LICENSE

4. **`pacman -U`** (с sudo) ставит пакет. `chowdy.install` post-hook:
   - `groupadd --system chowdy`, `useradd --system --groups video chowdy`
   - `/var/lib/chowdy/{models,users}` с правильными правами
   - `systemd-tmpfiles --create`, `systemctl daemon-reload`
   - печатает quick-start

Сценарий идемпотентный: повторная установка / upgrade не ломает
существующего пользователя или содержимое `/var/lib/chowdy/`.

## Что НЕ устанавливается автоматически

- **ONNX модели.** InsightFace non-commercial research license
  запрещает редистрибуцию в пакете. Юзер скачивает по инструкции
  в `/usr/share/doc/chowdy/MODELS.md`.
- **PAM конфигурация.** Правка `/etc/pam.d/*` слишком рискованная для
  автоматизации в `.install`. Юзер делает руками по чек-листу в
  `/usr/share/doc/chowdy/PAM-INSTALL.md` (с открытым root TTY).

---

## Публикация в AUR

PKGBUILD + `.SRCINFO` в репо актуальные для текущей версии. Дальше:

1. **Регистрация на AUR.**
   - Создай аккаунт https://aur.archlinux.org/register
   - Залей SSH public key в профиль (Settings → SSH Public Key)

2. **Создай пустой AUR-репозиторий пакета.** Он отдельный от GitHub
   и хранится на серверах AUR:
   ```sh
   git clone ssh://aur@aur.archlinux.org/chowdy.git ~/aur-chowdy
   # директория будет пустая, это нормально
   ```

3. **Скопируй три файла из репо в AUR-репо:**
   ```sh
   cp packaging/arch/PKGBUILD       ~/aur-chowdy/
   cp packaging/arch/chowdy.install ~/aur-chowdy/
   cp packaging/arch/.SRCINFO       ~/aur-chowdy/
   ```

4. **Push:**
   ```sh
   cd ~/aur-chowdy
   git add PKGBUILD chowdy.install .SRCINFO
   git commit -m "initial import: chowdy 0.1.0"
   git push origin master
   ```

5. **Проверь страницу пакета** — https://aur.archlinux.org/packages/chowdy
   должна показать pkgver, deps, твоего maintainer'а.

После этого любой юзер Arch'а ставит командой:
```sh
yay -S chowdy
# или
paru -S chowdy
```

---

## Обновление версии

Когда выпускаешь новый релиз:

1. Сделай новый tag в GitHub-репо:
   ```sh
   # в корне chowdy/
   git tag -a v0.2.0 -m "release 0.2.0"
   git push origin v0.2.0
   ```

2. Обнови `PKGBUILD` в этой папке:
   - `pkgver=0.2.0`
   - `pkgrel=1` (сбрасывается на 1 при новой `pkgver`)
   - Обнови `sha256sums` (старая сумма уже не подходит):
     ```sh
     curl -sL https://github.com/q-artem/chowdy/archive/v0.2.0.tar.gz | sha256sum
     # подставь полученный хэш в sha256sums=(...)
     ```

3. Регенерируй `.SRCINFO`:
   ```sh
   cd packaging/arch
   makepkg --printsrcinfo > .SRCINFO
   ```

4. Закоммить оба файла в GitHub-репо chowdy:
   ```sh
   git add PKGBUILD .SRCINFO
   git commit -m "packaging: bump to 0.2.0"
   git push
   ```

5. Скопируй и push в AUR:
   ```sh
   cp packaging/arch/{PKGBUILD,chowdy.install,.SRCINFO} ~/aur-chowdy/
   cd ~/aur-chowdy
   git add -A && git commit -m "0.2.0" && git push
   ```

**Packaging-only fix** (без изменения upstream версии — например исправил
PKGBUILD): bump `pkgrel` (1 → 2), регенерируй `.SRCINFO`, push в AUR.

---

## Проверка пакета до публикации

```sh
# Линт PKGBUILD
namcap PKGBUILD

# Локальная сборка (потребует доступ в интернет — fetch'ит tarball)
makepkg -s              # собрать без install
namcap chowdy-0.1.0-1-x86_64.pkg.tar.zst   # линт собранного пакета

# Что внутри пакета
tar tf chowdy-0.1.0-1-x86_64.pkg.tar.zst | head -20

# Полный цикл
makepkg -si              # установить
sudo systemctl enable --now chowdyd.socket
chowdy-cli enroll --label main
chowdy-cli auth-test     # должен success
sudo pacman -R chowdy    # удалить (помни про pam_chowdy строки!)
```
