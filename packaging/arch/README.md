# Arch packaging

PKGBUILD и `.install` хуки для сборки пакета `chowdy` под Arch / AUR.

## Сборка пакета локально

`source=` в PKGBUILD по умолчанию указывает на родительскую директорию репо
как plain tarball (`file://${PWD}/../../`). Это удобно для тестов до публикации
git tag.

```sh
cd packaging/arch
makepkg -si        # собрать и установить (нужен sudo для install шага)
```

`makepkg` выполнит:
1. `cmake` + `cmake --build` (build/ внутри `$srcdir`).
2. `package()` — установит файлы в `$pkgdir` по правильным путям.
3. `chowdy.install` post-install hook:
   - `useradd chowdy` (system user, supplementary group `video`).
   - Создаст `/var/lib/chowdy/{models,users}` с правильными правами.
   - Напомнит про скачивание моделей, enroll, и PAM конфигурацию.

## Что НЕ устанавливается автоматически

- **ONNX модели.** Лицензия InsightFace non-commercial research запрещает
  их редистрибуцию в пакете. Пользователь скачивает сам по инструкции
  `/usr/share/doc/chowdy/MODELS.md`.
- **PAM конфигурация.** Любая правка `/etc/pam.d/*` слишком рискованная для
  автоматизации в `.install`. Пользователь делает руками по чек-листу
  `/usr/share/doc/chowdy/PAM-INSTALL.md`.

## Для публикации на AUR

Перед публикацией:

1. Замени `source=` на git tag:
   ```
   source=("$pkgname-$pkgver.tar.gz::https://github.com/USER/chowdy/archive/v$pkgver.tar.gz")
   sha256sums=('реальная_сумма_тут')
   ```
2. Обнови `pkgver`/`pkgrel`.
3. Сгенерируй `.SRCINFO`:
   ```sh
   makepkg --printsrcinfo > .SRCINFO
   ```
4. Запушь в `aur@aur.archlinux.org:chowdy.git`.

## Проверка до публикации

```sh
namcap PKGBUILD                  # lint PKGBUILD
namcap chowdy-0.1.0-1-x86_64.pkg.tar.zst   # lint собранный пакет
```

## Что должно сделать ваше тестирование пакета

- Установка без багов: `makepkg -si` проходит, демон стартует через
  `systemctl enable --now chowdyd.socket`.
- `chowdy-cli enroll` и `auth-test` работают как от обычного юзера.
- Удаление: `pacman -R chowdy` чисто убирает бинари, не трогает
  `/var/lib/chowdy/users/` (это работа `post_remove` напоминания).
- Backup behaviour: после правки `/etc/chowdy/config.toml` upgrade
  пакета не должен затирать ваши изменения (это контролируется
  `backup=('etc/chowdy/config.toml')` в PKGBUILD).
