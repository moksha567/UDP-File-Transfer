# File-copy demo

Do steps 1–3 on each computer. In the folder that contains `ft.c`:

**Step 1. Compile the program**

```sh
cc -O2 -Wall -Wextra -o ft ft.c
mkdir -p "$HOME/bin"
cp ft "$HOME/bin/ft"
```

**Step 2. Point FT_REMOTE at that program**

```sh
export FT_REMOTE="$HOME/bin/ft"
```

**Step 3. Save that line for the next login**

Mac, add this to `~/.zshenv`:

```sh
export FT_REMOTE="$HOME/bin/ft"
```

Linux, add this at the top of `~/.bashrc`:

```sh
export FT_REMOTE="$HOME/bin/ft"
```

**Step 4. Send or receive a file**

```text
~/bin/ft <source> <destination>
```

One path is local. The other is `user@host:folder`.

## One computer

`localhost` means this same computer.

```sh
mkdir -p "$HOME/ft-lab/src" "$HOME/ft-lab/dst" "$HOME/ft-lab/back"
head -c 5000 /dev/urandom > "$HOME/ft-lab/src/sample.bin"
"$HOME/bin/ft" "$HOME/ft-lab/src/sample.bin" "localhost:$HOME/ft-lab/dst/"
cmp "$HOME/ft-lab/src/sample.bin" "$HOME/ft-lab/dst/sample.bin" && echo IDENTICAL
"$HOME/bin/ft" "localhost:$HOME/ft-lab/dst/sample.bin" "$HOME/ft-lab/back/"
cmp "$HOME/ft-lab/src/sample.bin" "$HOME/ft-lab/back/sample.bin" && echo IDENTICAL
```

You should see `sent 5000 bytes` and `IDENTICAL` twice.

## Two computers

Replace `user@host` with the other computer's account and address. Same commands as above.

```sh
mkdir -p "$HOME/ft-lab/from-other"
"$HOME/bin/ft" "$HOME/ft-lab/src/sample.bin" "user@host:~/ft-lab/dst/"
"$HOME/bin/ft" "user@host:~/ft-lab/dst/sample.bin" "$HOME/ft-lab/from-other/"
cmp "$HOME/ft-lab/src/sample.bin" "$HOME/ft-lab/from-other/sample.bin" && echo IDENTICAL
```

You should see `IDENTICAL`.

## This one should fail

The program copies one file, not a folder. Same command.

```sh
mkdir -p "$HOME/ft-lab/src/not-a-file"
"$HOME/bin/ft" "$HOME/ft-lab/src/not-a-file" "localhost:$HOME/ft-lab/dst/not-a-file"
echo "exit code: $?"
```

You should see `not a regular file` and `exit code: 1`.
