# Weetany.

Gospel, a litany plugin for WeeChat.

Supports signaling, 1:1 chats and group chats.

# Building

```
$ export LDFLAGS="-L/path/to/libkyrka/lib -lkyrka -lsodium"
$ export CFLAGS="-I/path/to/weechat-plugin.h -I/path/to/libkyrka/include/"
$ make
```

# Configuration

```
/set plugins.var.litany.cathedral.ip "ip"
/set plugins.var.litany.cathedral.port "port"

/set plugins.var.litany.kek-id "kek_id"
/set plugins.var.litany.identity "cs_id"
/set plugins.var.litany.flock "flock_id"

/set plugins.var.litany.kek-path "/path/to/kek"
/set plugins.var.litany.cosk-path "/path/to/cosk-file"
/set plugins.var.litany.cs-path "/path/to/cathedral-secret"
```

Don't forget to /save after setting these.

# Chatting

```
/command litany nick yourname
/command litany chat <flock> <peer>
```

Opens up a new buffer with a tunnel to said peer if they are online.
