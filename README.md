# Weetany.

Gospel, a litany plugin for WeeChat.

Litany is an end-to-end encrypted and peer-to-peer chat protocol
using the sanctum as its transport layer.

For details on how the underlying tunnels works see
<a href="https://github.com/jorisvink/sanctum/blob/master/docs/crypto.md">docs/crypto.md</a> in the sanctum repository.

## Features

Litany supports having one-to-one or group conversations. The litany
establishes a sanctum tunnel for each peer in a conversation, meaning
group conversations have multiple active tunnels.

# Building

You need to have weechat and libkyrka installed on your system.

The CFLAGS must contain the correct linking libs for libkyrka.

On MacOS set LIBKYRKA to the dist-build path for your libkyrka build.

```
$ export LDFLAGS="-lkyrka -lsodium"
$ make
```

# Loading

## First time

Make sure you update $HOME/.config/weechat/plugins.conf and set
at least the following two configurations:

```
gospel.cathedral.ip = "x.x.x.x"
gospel.cathedral.port = "xxxx"
```

## Load the plugin

```
/plugin load /path/to/gospel.so
```

# Configuration

```
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
