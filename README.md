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
/set plugins.var.gospel.cathedral.ip "ip"
/set plugins.var.gospel.cathedral.port "port"

/set plugins.var.gospel.kek-id "kek_id"
/set plugins.var.gospel.identity "cs_id"
/set plugins.var.gospel.flock "flock_id"

/set plugins.var.gospel.kek-path "/path/to/kek"
/set plugins.var.gospel.cosk-path "/path/to/cosk-file"
/set plugins.var.gospel.cs-path "/path/to/cathedral-secret"
```

Don't forget to /save after setting these.

# Chatting

```
/command gospel nick yourname
/command gospel chat <flock> <peer>
/command gospel group <flock> <group>
```
