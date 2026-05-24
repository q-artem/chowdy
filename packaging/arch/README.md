# Arch packaging

PKGBUILD и `.install` хуки для сборки пакета `chowdy` под Arch / AUR.

## Локальная сборка

`source=` в PKGBUILD по умолчанию указывает на локальный git checkout
(`git+file://$startdir/../..` — то есть корень нашего репо). Это удобно
для тестов до публикации: правишь код, коммитишь, и `makepkg -si` соберёт
ровно то что в HEAD.

```sh
cd packaging/arch
makepkg -si
```

`makepkg` выполнит:

1. **fetch.** Клонирует наш git репо в `src/chowdy/`.
2. **build().** `cmake -S . -B build` с `-DCHOWDY_BUILD_TOOLS=OFF`
   (тестбеды из `tools/` для production пакета не нужны), `cmake --build build`.
3. **package().** Раскладывает файлы в `pkg/`:

       /usr/bin/{chowdyd,chowdy-cli}
       /usr/lib/security/pam_chowdy.so
       /usr/lib/systemd/system/chowdyd.{service,socket}
       /usr/lib/tmpfiles.d/chowdy.conf
       /etc/chowdy/config.toml          (помечен backup=)
       /usr/share/doc/chowdy/*.md
       /usr/share/licenses/chowdy/LICENSE

4. **`pacman -U` (sudo)** — кладёт пакет в систему. `chowdy.install`
   post-install hook:
   - `groupadd --system chowdy`, `useradd --system --groups video --shell /usr/bin/nologin chowdy`
   - создаёт `/var/lib/chowdy/{models,users}` с правильными правами
   - `systemd-tmpfiles --create`, `systemctl daemon-reload`
   - печатает «что делать дальше»

## Что НЕ устанавливается автоматически

- **ONNX модели.** Лицензия InsightFace non-commercial research
  запрещает их редистрибуцию в пакете. Пользователь скачивает сам по
  инструкции `/usr/share/doc/chowdy/MODELS.md`.
- **PAM конфигурация.** Любая правка `/etc/pam.d/*` слишком рискованная
  для автоматизации в `.install`. Пользователь делает руками по
  чек-листу `/usr/share/doc/chowdy/PAM-INSTALL.md`.

## Проверка пакета до публикации

```sh
# Линт PKGBUILD до сборки
namcap PKGBUILD

# Линт собранного .pkg.tar.zst
namcap chowdy-0.1.0-1-x86_64.pkg.tar.zst

# Что внутри пакета (без install)
tar tf chowdy-0.1.0-1-x86_64.pkg.tar.zst | head -20
```

Также имеет смысл прогнать full cycle:

```sh
makepkg -si              # установить
sudo systemctl enable --now chowdyd.socket
chowdy-cli enroll --label main
chowdy-cli auth-test     # должен success
sudo pacman -R chowdy    # удалить
```

После `pacman -R` проверь что `/var/lib/chowdy/` остался (это
напоминание для пользователя — модели и enrollment'ы не теряются при
удалении пакета). Полная очистка — `post_remove` подскажет команды.

## Публикация в AUR

Когда локальная сборка стабильна:

1. **Запушь репо публично** (GitHub/GitLab/Gitea — что удобно).
   Создай git tag для версии:
   ```sh
   git tag -a v0.1.0 -m "release 0.1.0"
   git push origin v0.1.0
   ```

2. **Поправь PKGBUILD** под release tarball:
   ```bash
   url='https://github.com/<твой-юзер>/chowdy'
   source=("$pkgname-$pkgver.tar.gz::https://github.com/<твой-юзер>/chowdy/archive/v$pkgver.tar.gz")
   sha256sums=('реальная_сумма')   # makepkg -g подскажет
   ```
   В `build()`/`package()` поменяй `cd "$srcdir/$pkgname"` на
   `cd "$srcdir/$pkgname-$pkgver"` (так распакует release tarball).

3. **Сгенерируй `.SRCINFO`** — AUR обязательно требует его в репо:
   ```sh
   makepkg --printsrcinfo > .SRCINFO
   ```

4. **Зарегистрируй AUR account** на https://aur.archlinux.org, загрузи
   SSH-ключ.

5. **Создай AUR-репо** (он отдельный, не GitHub):
   ```sh
   git clone ssh://aur@aur.archlinux.org/chowdy.git aur-chowdy
   cp PKGBUILD chowdy.install .SRCINFO aur-chowdy/
   cd aur-chowdy
   git add -A
   git commit -m "initial import"
   git push origin master
   ```

6. После этого `yay -S chowdy` (или `paru -S chowdy`) у любого юзера
   Arch'а сработает.

## Bumping версии

При новых релизах в `PKGBUILD`:
- `pkgver=` — новая upstream версия
- `pkgrel=1` — сбрасывается на 1 при новой `pkgver`
- Внутри той же `pkgver` каждое packaging-исправление bump'ает
  `pkgrel` (1 → 2 → ...)

После любого изменения PKGBUILD — пересобери `.SRCINFO` и закоммить
оба файла вместе в AUR репо.
