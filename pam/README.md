# pam_chowdy — установка и тестирование

> **Это PAM модуль.** Любая ошибка в конфигурации может оставить тебя без
> возможности войти в систему. Прочитай Section 16 DESIGN.md и **выполни
> чек-лист безопасности целиком** перед тем как трогать `/etc/pam.d/*`.

## Чек-лист перед установкой

- [ ] Запущен **второй TTY с залогиненным root** (`Ctrl+Alt+F2`, логин под
      root через пароль). Не закрывай этот TTY до конца тестирования.
- [ ] Сделан бэкап:
      `sudo cp -r /etc/pam.d /etc/pam.d.bak.$(date +%Y%m%d-%H%M)`.
- [ ] `chowdyd.service` уже работает: `systemctl status chowdyd`,
      `ls -l /run/chowdy/auth.sock` (должно быть `srw-rw---- root chowdy`).
- [ ] Минимум один enrollment для твоего uid:
      `ls /var/lib/chowdy/users/$(id -u)/`.
- [ ] CLI smoke-test: `chowdy-cli auth-test` уходит в SUCCESS.

Только после этого ставим `.so`.

## Установка модуля

```sh
sudo install -m 0755 build/pam/pam_chowdy.so /usr/lib/security/pam_chowdy.so
```

(`/usr/lib/security/` — стандартный путь для PAM модулей в Arch.
В дистрибутивах с `/lib64/security` смотри `pam.d(5)`.)

Файл `.so` принадлежит `root:root`, права `0755`. Никаких setuid битов.

## Тестирование

Сначала **на `sudo`**, никогда сразу на login. Открой `/etc/pam.d/sudo`
в редакторе из второго TTY (root) и добавь chowdy **перед** существующей
строкой `auth`:

```pam
# chowdy — НЕ required, fallback на пароль если модуль не уверен
auth   sufficient   pam_chowdy.so timeout=2000
auth   sufficient   pam_unix.so try_first_pass
auth   include      system-auth
```

Параметры модуля:
- `timeout=MS` — общий бюджет (включая connect + auth pipeline). По умолчанию 2000.
- `socket=PATH` — переопределить путь к `auth.sock`. По умолчанию `/run/chowdy/auth.sock`.
- `debug` — больше деталей в syslog.

В новом терминале (НЕ закрывая root TTY) попробуй:
```sh
sudo whoami
```

Сядь перед камерой → должен пройти без пароля. Прикрой камеру → должен
свалиться на пароль `pam_unix`.

Если что-то пошло не так — из root TTY верни бэкап:
```sh
sudo cp /etc/pam.d.bak.YYYYMMDD-HHMM/sudo /etc/pam.d/sudo
```

## После того как sudo работает

Только тогда можно добавлять chowdy в другие PAM stack'и:
- `polkit-1` (графические запросы пароля),
- `login` (text login, **с особой осторожностью**),
- `gdm-password` или другой display manager.

**Никогда не добавляй в `system-auth` или `login` до того как сценарий
"sudo по лицу + fallback на пароль" работает стабильно несколько дней.**

## Отладка

- Логи демона: `journalctl -u chowdyd -f`
- Логи модуля: `journalctl -t pam_chowdy -f` (после `debug` опции).
- Прямой CLI тест: `chowdy-cli auth-test`.
- Если `chowdy-cli` работает, а `sudo` — нет, причина почти всегда в
  правах на сокет (PAM модуль работает под root, должен быть в группе с
  `auth.sock`, у нас `0660 root:chowdy` так что root имеет доступ через
  ownership).

## Откат

Просто удали строку `auth ... pam_chowdy.so` из PAM конфига, или
восстанови бэкап `/etc/pam.d/`. Демон при этом можно оставить — он
просто никто не будет звать.
