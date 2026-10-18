# ft: scp-style file copy over UDP with BBR

`ft` copies a file between two machines with the same syntax as `scp`, but sends the
data over **UDP with BBR congestion control** instead of through the ssh TCP stream.
One C file (`ft.c`, about 580 lines), no dependencies.

```sh
ft file.bin   user@host:dir/        # upload
ft user@host:dir/file.bin   .       # download
```

What you do **not** have to do:

- start anything on the other machine first (no daemon; ssh starts it on demand);
- open a firewall port or configure a router. It only needs what `scp` needs:
  **sshd reachable on port 22**, plus `ft` installed on the other machine.

See [GUIDE.md](GUIDE.md) for step-by-step instructions, tests and troubleshooting.

## Usage

```
ft SRC DST        exactly one of SRC / DST is [user@]host:path, the other a local path
```

Rules are the same as scp:

| Command | Result |
|---|---|
| `ft a.bin host:/tmp/b.bin` | creates `/tmp/b.bin` on host |
| `ft a.bin host:/tmp` or `host:/tmp/` | `/tmp` is a directory, so creates `/tmp/a.bin` |
| `ft a.bin host:` | creates `a.bin` in your home directory on host |
| `ft host:/tmp/a.bin .` | creates `./a.bin` |
| `ft host:/tmp/a.bin out/` | `out` is a directory, so creates `out/a.bin` |

One file per command (no `-r`). Copying between two remote hosts is not supported.
Connection details (user, port, keys, jump hosts) come from `~/.ssh/config` as usual.

Environment variables:

| Variable | Meaning |
|---|---|
| `FT_SSH` | the ssh command on this machine, default `ssh -4`, e.g. `FT_SSH="ssh -4 -p 2222 -i key"` |
| `FT_REMOTE` | path to **this** machine's own `ft`. The shell on the machine `ssh` logs into expands it. Default: `ft` on that machine's PATH. Set `export FT_REMOTE="$HOME/bin/ft"` in `~/.zshenv` (Mac) or at the top of `~/.bashrc` (Linux). |
| `FT_STUN` | STUN server `host:port` (default Google's); empty (`FT_STUN=`) disables STUN |

## How it works

```
        ssh (port 22): control channel only                 ┌──────────────────────┐
 you ─────────────────────────────────────────────▶ sshd ─▶│ ft --serve (remote)  │
  │   "here are my UDP addresses" ◀────────▶ "and mine"      └──────────┬───────────┘
  │                                                                     │
  └───────────── UDP, both sides send to each other at once ◀──────────┘
                 (file data, ACKs, BBR pacing)
```

1. `ft` runs `ssh -4 host "${FT_REMOTE:-ft}" --serve ...`. The other computer's shell expands `FT_REMOTE` to its own copy of `ft`, which starts on demand and exits when done.
   That removes the "receiver must already be running" requirement. Each computer names its own binary.
2. Each side opens a UDP socket and works out how the other can reach it:
   its public address from a STUN server, and its LAN address as sshd sees it
   (`SSH_CONNECTION`). The two exchange these over the ssh session.
3. Both sides immediately send UDP packets to each other's addresses. An outbound
   packet makes a NAT or stateful firewall accept the matching reply, so each side's
   firewall sees the other's packets as replies to its own. This is UDP hole punching, so
   **no inbound port is ever opened**. All candidate addresses are tried and the first
   one that answers is used (this also covers two machines on the same LAN, where
   routers often cannot loop a packet back to their own public address).
4. The file goes over UDP. ssh carries no file data.

### The UDP protocol

16-byte header `{type, a, b, ts}` + up to 1200 bytes of data (1200 + 16 + 28 bytes of
IP/UDP fits under the 1280-byte IPv6 minimum MTU, so nothing is fragmented).

| Packet | `a` | `b` | `ts` |
|---|---|---|---|
| `DATA` | chunk number | total chunks | sender clock |
| `ACK` | chunk just received | cumulative ack (every chunk below it is held) | echo of the DATA's `ts` |
| `PUNCH` | | | opens the path and confirms it works |
| `FIN` | | | "all acknowledged, you can exit" |

**Reliability.** The receiver ACKs every packet and writes each chunk straight to its place
in the file, so out-of-order delivery is fine. ACKs carry a cumulative ack, so lost ACKs
cost nothing. The sender keeps a send-ordered log: a chunk is lost when a chunk sent
after it has been ACKed (RACK-style) or when its timeout expires, and lost chunks are
resent first. Echoed timestamps give exact RTT samples even for retransmissions. After the
last ACK the sender sends `FIN`; if every `FIN` is lost the receiver exits after 3 s anyway.

**BBR (v1).** Delivery-rate samples give a bottleneck-bandwidth estimate (maximum over 10
round trips); the minimum RTT is a 10 s window minimum. States: Startup, Drain, ProbeBW
(gain cycle 1.25, 0.75, 1 ×6) and ProbeRTT. Packets are paced at gain × bandwidth, with at
most 2 × the bandwidth-delay product in flight. Loss does **not** shrink the window.

## Requirements and limits

- Linux or macOS, IPv4, a C compiler. Build: `cc -O2 -Wall -Wextra -o ft ft.c`
- `ft` must be installed on **both** machines (like `scp`, which needs `scp` on the remote).
  Each machine sets `FT_REMOTE` to its own binary, in the file a non-interactive ssh command reads: `~/.zshenv` on a Mac, the top of `~/.bashrc` on Linux. If `FT_REMOTE` is unset there, `ft` must be on that machine's command path. A tilde stored inside the variable is not expanded; use `"$HOME/bin/ft"` so the path is absolute when the file is read.
- **UDP must be able to leave each machine and get replies back.** Typical stateful
  enterprise firewalls and home NATs allow that. A network that drops *all* UDP
  (or only allows UDP to a few ports) will fail with `no answer from peer`, and there
  is no TCP fallback.
- Hole punching does not work through **symmetric NATs** (some carrier-grade, mobile and
  strict corporate networks). The same error message appears.
- No encryption: file contents travel unencrypted over UDP (ssh protects only the
  address exchange). Integrity relies on the UDP checksum; there is no end-to-end hash.
- The sender's memory is fixed (~5 MB); the receiver needs 1 byte per 1200 bytes of file.

## Tests

`test/run.sh` runs 26 checks without needing an sshd: `test/fakessh` stands in for `ssh` and runs the
"remote" side on the same machine. `test/run.sh --slow` adds 30 MB transfers through
`test/netem.py`, a UDP path emulator (bandwidth limit, queue, delay, loss, reordering).

| Check | Result |
|---|---|
| sizes 0, 1200, 1201 bytes and 3 MB, upload and download | byte-identical |
| scp destination rules, names with spaces and quotes, `user@host:` forms | pass |
| errors (missing files, unwritable destination, ft not installed, bad arguments) | non-zero exit with a message |
| STUN disabled (LAN-address fallback) | byte-identical |
| 20 Mbit/s, 40 ms RTT, 1% loss, 60-packet queue: upload / download | 18.6 / 18.5 Mbit/s (ceiling about 19.7) |
| 20 Mbit/s, 40 ms RTT, 2000-packet queue | 18.8 Mbit/s, queue stays empty |
| 10 Mbit/s, 100 ms RTT, 8% loss, 5% reordering | byte-identical |

**Not tested:** a real sshd, real NATs, or real firewalls. Everything above ran on one machine.
[GUIDE.md](GUIDE.md) section 3 explains how to run the real thing.

## Files

| File | Purpose |
|---|---|
| `ft.c` | the program |
| `test/run.sh` | test suite |
| `test/fakessh` | stand-in for ssh used by the tests |
| `test/netem.py` | UDP path emulator used by `--slow` |
