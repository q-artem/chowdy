# systemd integration

These unit files are **not** installed by `cmake --build .` — installation
is handled by the AUR package (M9) or by manual `cp` for development.

## Files

| File | Destination | Mode |
|---|---|---|
| `fastauthd.service` | `/usr/lib/systemd/system/fastauthd.service` | 0644 root:root |
| `fastauthd.socket`  | `/usr/lib/systemd/system/fastauthd.socket`  | 0644 root:root |
| `tmpfiles.d/fastauth.conf` | `/usr/lib/tmpfiles.d/fastauth.conf` | 0644 root:root |

## Prerequisites

A system user `fastauth` with the `video` supplementary group:

```bash
sudo groupadd -r fastauth
sudo useradd  -r -g fastauth -G video -d /var/lib/fastauth -s /sbin/nologin fastauth
sudo install -d -o fastauth -g fastauth -m 0700 /var/lib/fastauth
sudo install -d -o root     -g fastauth -m 0750 /var/lib/fastauth/models
```

## Enable

```bash
sudo systemctl daemon-reload
sudo systemd-tmpfiles --create
sudo systemctl enable --now fastauthd.socket
# fastauthd.service starts on demand when something connects to auth.sock.
```

## Verify

```bash
sudo systemctl status fastauthd.socket
systemctl list-sockets | grep fastauth
ls -la /run/fastauth/
# expected:
#   srw-rw---- 1 root     fastauth … auth.sock
#   srw-rw-rw- 1 fastauth fastauth … mgmt.sock
```

Then poke the daemon via the CLI:

```bash
fastauth-cli test
# → {"type":"test_result","reason":"ok",…}
```
