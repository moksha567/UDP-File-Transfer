# How to build and use ft

`ft` copies a file between two computers using the same commands as `scp`:

```sh
ft myfile.bin  user@server:backup/     # send a file to the server
ft user@server:backup/myfile.bin  .    # get a file from the server
```

You do not have to start anything on the other computer first, and you do not have to open a firewall port or change a router. If `ssh` and `scp` work between the two computers, `ft` works too (see the limits in section 6).

You need a Mac or a Linux computer with a C compiler on both sides.

## 1. Build the program

Open a terminal in the project folder (the folder that contains `ft.c`) and check that a compiler is installed:

```sh
cc --version
```

Then build:

```sh
cc -O2 -Wall -Wextra -o ft ft.c
```

When it works the terminal stays quiet and a file named `ft` appears next to `ft.c`. A program built on a Mac does not run on Linux, so build on each kind of computer.

If `cc` is missing: on a Mac run `xcode-select --install`. On Ubuntu or Debian run `sudo apt update && sudo apt install build-essential`.

## 2. Check it works without a second computer

The test suite pretends to be ssh and runs the "remote" side on the same computer. It does not need ssh set up.

```sh
test/run.sh
```

It takes a few seconds and ends with a line like:

```text
26 passed, 0 failed
```

Anything other than `0 failed` is a problem. Each line says what was checked: different file sizes, uploads and downloads, destination rules, error cases, and running without STUN.

For the slow-network tests (30 MB through a pretend bad network, needs `python3`, about a minute):

```sh
test/run.sh --slow
```

Each network line also prints the speed, for example:

```text
  PASS  20 Mbit/s, 40 ms RTT, 1% loss, 60-pkt queue (upload)
        sent 30000000 bytes in 12.93 s (18.56 Mbit/s), 763 of 25000 chunks retransmitted
```

The pretend network is limited to 20 Mbit/s here, so about 18.6 is close to the best possible. The "retransmitted" count is how many pieces had to be sent again because the network dropped them.

## 3. Copy a file to or from another computer

### One-time setup: put ft on the other computer

`ft` has to exist on both computers, the same way `scp` has to. Copy the source over with scp and build it there. Replace `user@server` with your own login:

```sh
scp ft.c user@server:
ssh user@server 'mkdir -p "$HOME/bin" && cc -O2 -Wall -Wextra -o "$HOME/bin/ft" "$HOME/ft.c"'
```

Do the same on this computer: `mkdir -p "$HOME/bin" && cc -O2 -Wall -Wextra -o "$HOME/bin/ft" ft.c`. A plain ssh command often does not search your home folder, so each computer records where **its own** copy lives. The computer you type `ft` on does not choose the path on the other computer. The other computer's shell expands `FT_REMOTE` when the login starts.

Save this line in the file that a non-interactive ssh command actually reads. On a Mac that is `~/.zshenv`. On Linux, put it at the top of `~/.bashrc`, above any line that returns early for a non-interactive shell:

```sh
export FT_REMOTE="$HOME/bin/ft"
```

Use `"$HOME/bin/ft"`, not `'~/bin/ft'`. A tilde stored inside the variable is not expanded later. If `ft` is already on that computer's normal command path (for example `/usr/local/bin`), you do not need this line. Do this on every computer that will be logged into, including this one if you copy through `localhost`.

### Copy

Send a file to the server. It appears in the folder `backup` in your home folder there:

```sh
./ft myfile.bin user@server:backup/
```

Get a file from the server into the current folder:

```sh
./ft user@server:backup/myfile.bin .
```

What you will see: ssh may ask for your password or about the server's fingerprint, as usual. Then a progress line about once a second, then a line starting with `sent`. When the prompt comes back, the file is complete.

These work like `scp`:

| Command | What happens |
|---|---|
| `./ft a.bin user@server:/tmp/b.bin` | creates `/tmp/b.bin` on the server |
| `./ft a.bin user@server:/tmp` | `/tmp` is a folder, so creates `/tmp/a.bin` |
| `./ft a.bin user@server:` | creates `a.bin` in your home folder on the server |
| `./ft user@server:/tmp/a.bin .` | creates `./a.bin` here |
| `./ft user@server:/tmp/a.bin out/` | creates `out/a.bin` here (the folder `out` must exist) |

One file per command, and copying directly between two servers is not supported. Your usual `~/.ssh/config` settings (user name, port, keys, jump hosts) are used automatically. For a one-off ssh option, use `FT_SSH`:

```sh
FT_SSH="ssh -4 -p 2222" ./ft myfile.bin user@server:
```

The default ssh command is already `ssh -4`, because the file transfer is IPv4 only. Setting `FT_SSH` replaces that default, so include `-4` when you set it.

### Check the copy is exact

Compare fingerprints on both sides (`shasum` on a Mac, `sha1sum` also works on Linux):

```sh
shasum myfile.bin
ssh user@server shasum backup/myfile.bin
```

The long codes must match.

### Try the real thing on one computer

If your Mac has Remote Login turned on (System Settings, General, Sharing) or your Linux machine runs sshd, you can use ssh to your own computer:

```sh
./ft myfile.bin localhost:/tmp/copy.bin
```

`localhost` logs into this same computer, so `FT_REMOTE` must already be saved in this computer's startup file (section 3). The ssh connection is forced to IPv4.

## 4. What happens behind the scenes

1. `ft` uses ssh (port 22) to start a copy of itself on the other computer. It stops by itself when the transfer is done. That is why nothing needs to be running first.
2. The two copies tell each other, over ssh, the network address where each can receive UDP packets.
3. Both start sending small UDP packets to each other at the same moment. Each firewall or router sees the other side's packets as replies to its own, and lets them in. No port is opened.
4. The file travels over UDP. ssh carries only the setup messages.

## 5. What the messages mean

The sending side prints one line about once a second:

```text
 85.3%  goodput   15.86 Mbit/s  btl_bw   19.90 Mbit/s  min_rtt   47.0 ms  probe_bw
```

| What you see | What it means |
|---|---|
| `85.3%` | How much of the file the receiver has confirmed |
| `goodput` | How fast useful data arrived in the last second |
| `btl_bw` | The sender's guess of the network's top speed |
| `min_rtt` | The shortest round trip it has measured, in milliseconds |
| last word | What the sender is doing: `startup` (speeding up), `drain` (easing off), `probe_bw` (steady sending), or `probe_rtt` (measuring delay) |

When it finishes:

```text
sent 1642812 bytes in 0.05 s (242.75 Mbit/s), 0 of 1370 chunks retransmitted
```

That is the size, the time, the average speed, and how many pieces (1200 bytes each) had to be sent again. On a download you see this line from the server's side, and `received ...` from yours.

## 6. If something goes wrong

| What you see | What it means and what to do |
|---|---|
| `ft: could not start the transfer on HOST (is ft installed there?...)` | ssh worked but `ft` did not start on the other computer. Install it there (section 3) and set `FT_REMOTE` on **that** computer, for example `export FT_REMOTE="$HOME/bin/ft"` in `~/.zshenv` (Mac) or at the top of `~/.bashrc` (Linux). The line above this message from the other computer usually says why, such as `ft: command not found`. |
| `send: no answer from peer (UDP blocked, or a symmetric NAT?)` | ssh worked, but UDP packets could not get through for 60 seconds. Either a firewall blocks UDP (both outgoing and replies), or one network uses a strict kind of NAT that cannot be punched through. Try from another network. |
| `send: peer stopped answering` | The other side went silent for 20 seconds in the middle of the transfer: it was stopped, or the network dropped. |
| `recv: timed out` | The receiver got nothing for 30 seconds in the middle of a transfer. |
| `recv: the sender never showed up` | Same as "no answer from peer", seen from the receiving side. |
| `ft: copying between two remote hosts is not supported` | Both paths had a `host:` part. Copy to your own computer first, then on to the second server. |
| `usage: ./ft SRC DST ...` | Neither path (or both) looked remote. Remote paths look like `user@host:path`. A local file whose name contains a colon needs `./` in front, for example `./a:b`. |
| `FILE: No such file or directory` | The source file does not exist, or the destination folder does not exist. |
| ssh asks for a password every time, or fails | That is ssh itself. Set up key login as you would for `scp`. |

Things to know:

- Both sides must be able to send UDP out and receive the replies. Ordinary home routers and typical stateful company firewalls allow this. A network that blocks all UDP does not work, and there is no TCP fallback.
- The file is **not encrypted** on its way. Do not send secrets this way. Only the setup messages go through ssh.
- It only works with IPv4. One file per command.
- If a computer cannot reach the public STUN server, `ft` still tries the other address it learned from ssh. That is enough on a shared network and for computers with public addresses. To skip the lookup (or use your own STUN server), set `FT_STUN` to nothing or to `host:port`.

## 7. More tests, by hand

`test/netem.py` is a UDP path emulator. `test/run.sh --slow` uses it for you. Its arguments are:

```text
python3 test/netem.py PORT_A PORT_B SPEED DELAY LOSS QUEUE [REORDER]
```

| Number | Meaning |
|---|---|
| `SPEED` | Top speed, in megabits per second |
| `DELAY` | Wait in each direction, in milliseconds (a round trip is twice this) |
| `LOSS` | Percent of packets to drop |
| `QUEUE` | How many packets can wait in line; extras are dropped |
| `REORDER` | Optional: percent of packets delayed a little so they arrive out of order |

Python can fake a link up to about 100 Mbit/s. On Linux you can also use `tc netem` for the real kernel (needs root), for example `sudo tc qdisc add dev lo root netem delay 20ms loss 1% rate 20mbit`, and remove it afterwards with `sudo tc qdisc del dev lo root`.

To see how it fails when the other side goes away, start a large transfer and press Ctrl+C on the receiving side or stop the other computer's `ft` process. The sending side stops with `peer stopped answering` after 20 seconds.

## 8. What has and has not been tested

Tested, all on one computer with a stand-in for ssh: file sizes from 0 bytes to 30 MB, uploads and downloads, every destination rule in the table above, awkward file names, the error messages, running with STUN disabled, and transfers through emulated bad networks (loss, delay, small and bloated queues, reordering).

**Not tested:** a real sshd, real home routers or company firewalls, or a real internet path. The address exchange over ssh and the hole punching are the parts that depend on those. If you try it on real machines and it fails, the message in section 6 tells you which part.
