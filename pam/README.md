# pam_fastauth — установка и тестирование

> **Это PAM модуль.** Любая ошибка в конфигурации может оставить тебя без
> возможности войти в систему. Прочитай Section 16 DESIGN.md и **выполни
> чек-лист безопасности целиком** перед тем как трогать `/etc/pam.d/*`.

## Чек-лист перед установкой

- [ ] Запущен **второй TTY с залогиненным root** (`Ctrl+Alt+F2`, логин под
      root через пароль). Не закрывай этот TTY до конца тестирования.
- [ ] Сделан бэкап:
      `sudo cp -r /etc/pam.d /etc/pam.d.bak.$(date +%Y%m%d-%H%M)`.
- [ ] `fastauthd.service` уже работает: `systemctl status fastauthd`,
      `ls -l /run/fastauth/auth.sock` (должно быть `srw-rw---- root fastauth`).
- [ ] Минимум один enrollment для твоего uid:
      `ls /var/lib/fastauth/users/$(id -u)/`.
- [ ] CLI smoke-test: `fastauth-cli auth-test` уходит в SUCCESS.

Только после этого ставим `.so`.

## Установка модуля

```sh
sudo install -m 0755 build/pam/pam_fastauth.so /usr/lib/security/pam_fastauth.so
```

(`/usr/lib/security/` — стандартный путь для PAM модулей в Arch.
В дистрибутивах с `/lib64/security` смотри `pam.d(5)`.)

Файл `.so` принадлежит `root:root`, права `0755`. Никаких setuid битов.

## Тестирование

Сначала **на `sudo`**, никогда сразу на login. Открой `/etc/pam.d/sudo`
в редакторе из второго TTY (root) и добавь fastauth **перед** существующей
строкой `auth`:

```pam
# fastauth — НЕ required, fallback на пароль если модуль не уверен
auth   sufficient   pam_fastauth.so timeout=2000
auth   sufficient   pam_unix.so try_first_pass
auth   include      system-auth
```

Параметры модуля:
- `timeout=MS` — общий бюджет (включая connect + auth pipeline). По умолчанию 2000.
- `socket=PATH` — переопределить путь к `auth.sock`. По умолчанию `/run/fastauth/auth.sock`.
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

Только тогда можно добавлять fastauth в другие PAM stack'и:
- `polkit-1` (графические запросы пароля),
- `login` (text login, **с особой осторожностью**),
- `gdm-password` или другой display manager.

**Никогда не добавляй в `system-auth` или `login` до того как сценарий
"sudo по лицу + fallback на пароль" работает стабильно несколько дней.**

## Отладка

- Логи демона: `journalctl -u fastauthd -f`
- Логи модуля: `journalctl -t pam_fastauth -f` (после `debug` опции).
- Прямой CLI тест: `fastauth-cli auth-test`.
- Если `fastauth-cli` работает, а `sudo` — нет, причина почти всегда в
  правах на сокет (PAM модуль работает под root, должен быть в группе с
  `auth.sock`, у нас `0660 root:fastauth` так что root имеет доступ через
  ownership).

## Откат

Просто удали строку `auth ... pam_fastauth.so` из PAM конфига, или
восстанови бэкап `/etc/pam.d/`. Демон при этом можно оставить — он
просто никто не будет звать.
