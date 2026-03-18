# P25Time

`P25Time` is a single-binary radio clock built on top of DSD-Neo. It listens to a P25 Phase 1 control channel, decodes `Synchronization Broadcast` messages, and serves the recovered radio time as an NTP source.

## What it does

- Tunes a P25 Phase 1 control channel from an SDR input source.
- Decodes `Synchronization Broadcast` TSBKs (`opcode 0x30`).
- Reconstructs UTC from the control channel time payload.
- Serves that time over embedded NTP on `UDP/123`.
- Prints concise sync lines instead of the full DSD-Neo decode stream.

Example sync output:

```text
SYNC 2026-03-18 00:38:52.50 UTC LTO=-5:00
```

## Status and behavior

- `P25Time` is unsynchronized until it sees a valid radio time broadcast.
- Once synchronized, it answers NTP as a stratum-1 source with refid `P25T`.
- By default it binds to `0.0.0.0:123`, so run it as `root` or grant `CAP_NET_BIND_SERVICE`.

## Usage

```bash
sudo ./P25Time -i rtl:0:771.18125M
```

Supported input examples:

```bash
sudo ./P25Time -i rtl:0:771.18125M
sudo ./P25Time -i rtl:0:771.18125M:22:0:24:0:2
sudo ./P25Time -i rtltcp:127.0.0.1:1234:771.18125M
```

## Querying the server

You can verify the server replies to NTP with `nc`:

```bash
(printf '\x23'; dd if=/dev/zero bs=1 count=47 2>/dev/null) | nc -u -w1 127.0.0.1 123 | wc -c
```

A successful reply returns `48`.

To inspect the raw packet:

```bash
(printf '\x23'; dd if=/dev/zero bs=1 count=47 2>/dev/null) | nc -u -w1 127.0.0.1 123 | xxd -g1
```

## Using it with Linux time clients

### chrony

Add this to `/etc/chrony/chrony.conf`:

```conf
server 127.0.0.1 minpoll 4 maxpoll 4 iburst prefer
```

Then restart chrony:

```bash
sudo systemctl restart chronyd  # or chrony depending on distro
chronyc sources -v
chronyc tracking
```

### systemd-timesyncd

`systemd-timesyncd` is an SNTP client, not a full NTP daemon, but you can still point it at `P25Time`:

```ini
[Time]
NTP=127.0.0.1
```

Then restart it:

```bash
sudo systemctl restart systemd-timesyncd
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

The binary is produced at:

```text
build/apps/dsd-cli/P25Time
```

## Notes

- Built with DSD-Neo components.
- Current focus is P25 Phase 1 control-channel time recovery and embedded NTP service.
- Full validation passed in this tree: `251/251` tests.

## License

This repository inherits the licensing and notices from the underlying DSD-Neo codebase. See the existing license and copyright files in this repository.
