Here's a plain version.

HOW TO USE ft

ft sends a file from one computer to another. The computer that receives is the server. The computer that sends is the client. You need a Mac or Linux computer with a C compiler.

STEP 1: BUILD IT

Open a terminal in the folder that has ft.c, then run:

```
cc -O2 -Wall -Wextra -o ft ft.c
```

If nothing prints and a file called ft appears, it worked. Build it on each computer you want to use. A build from a Mac won't run on Linux.

STEP 2: TRY IT ON ONE COMPUTER

Open two terminals in the project folder.

In terminal 1, start the receiver:

```
mkdir -p ~/ft-inbox
./ft recv ~/ft-inbox 9000
```

It asks for the peer's address. Just press Enter. When you see "receiver ready", leave it alone. It's waiting.

In terminal 2, send a file (use your own file path):

```
./ft send ~/Desktop/photo.png 127.0.0.1:9000
```

When it's done, the sender prints a line starting with "sent", and the receiver prints where it saved the file. The file will be in ~/ft-inbox, named received-0001, received-0002, and so on.

To stop the receiver, click its terminal and press Ctrl+C.

STEP 3: SEND TO ANOTHER COMPUTER

Build ft on both computers. Start the receiver first.

On the receiving computer:

```
mkdir -p ~/ft-inbox
./ft recv ~/ft-inbox 9000
```

Press Enter at the question. It prints a line like "Your public address: 203.0.113.5:9000". If you have a firewall, allow UDP port 9000.

On the sending computer, use the address it printed:

```
./ft send myfile.bin 203.0.113.5:9000
```

If that doesn't work and both computers are on home networks, start both programs without the address and type each computer's address into the other when asked. Do it within a few seconds of each other.

```
./ft recv ~/ft-inbox
./ft send myfile.bin
```

If you're on the same home network, you can also just use the other computer's local address, like 192.168.1.20:9000.

IF SOMETHING GOES WRONG

"no answer from peer": The sender waited about 60 seconds and heard nothing. Start the receiver first, and double-check the address and port.

"peer stopped answering": The receiver closed or the network dropped.

"Address already in use": Something else is using that port. Close it with Ctrl+C or pick a different port number.

"bad address": You typed something wrong at the address question. On the receiver, just press Enter.

THINGS TO KNOW

It sends one file at a time, to one receiver. It only works with IPv4 addresses. The file is not encrypted. If you want to be sure the copy is perfect, compare the two files with shasum.

Want me to add a short section on the tests, or leave it out like this?