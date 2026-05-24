# systemd integration

These unit files are **not** installed by `cmake --build .` — installation
is handled by the AUR package (M9) or by manual `cp` for development.

## Files

| File | Destination | Mode |
|---|---|---|
| `chowdyd.service` | `/usr/lib/systemd/system/chowdyd.service` | 0644 root:root |
| `chowdyd.socket`  | `/usr/lib/systemd/system/chowdyd.socket`  | 0644 root:root |
| `tmpfiles.d/chowdy.conf` | `/usr/lib/tmpfiles.d/chowdy.conf` | 0644 root:root |

## Prerequisites

A system user `chowdy` with the `video` supplementary group:

```bash
sudo groupadd -r chowdy
sudo useradd  -r -g chowdy -G video -d /var/lib/chowdy -s /sbin/nologin chowdy
sudo install -d -o chowdy -g chowdy -m 0700 /var/lib/chowdy
sudo install -d -o root     -g chowdy -m 0750 /var/lib/chowdy/models
```

## Enable

```bash
sudo systemctl daemon-reload
sudo systemd-tmpfiles --create
sudo systemctl enable --now chowdyd.socket
# chowdyd.service starts on demand when something connects to auth.sock.
```

## Verify

```bash
sudo systemctl status chowdyd.socket
systemctl list-sockets | grep chowdy
ls -la /run/chowdy/
# expected:
#   srw-rw---- 1 root     chowdy … auth.sock
#   srw-rw-rw- 1 chowdy chowdy … mgmt.sock
```

Then poke the daemon via the CLI:

```bash
chowdy-cli test
# → {"type":"test_result","reason":"ok",…}
```
