# Gospel

Gospel, a litany plugin for WeeChat.

Litany is an end-to-end encrypted and peer-to-peer chat protocol using
sanctum as its transport layer.

If you don't know what sanctum is, see the
<a href="https://github.com/jorisvink/sanctum/">sanctum</a> repository. The
gospel plugin uses the
<a href="https://github.com/jorisvink/libkyrka">libkyrka</a> library to
implement the sanctum protocol.

For details on how the underlying sanctum tunnels works see
<a href="https://github.com/jorisvink/sanctum/blob/master/docs/crypto.md">docs/crypto.md</a> in the sanctum repository.

<img src="gospel.png" alt="Gospel chat" width="512" />

## Features

The litany protocol supports having one-to-one or group conversations.

We establish establishes a sanctum tunnel for each peer in a conversation,
meaning group conversations will have multiple active tunnels.

## Building

The gospel plugin builds on Linux, OpenBSD and MacOS.

You need to have weechat and libkyrka installed on your system, the
build also depends on pkg-config.

On MacOS set LIBKYRKA to the dist-build path for your libkyrka build.

```
$ make
```

## Configuration

```
/set plugins.var.gospel.kek-id "kek_id"
/set plugins.var.gospel.identity "cs_id"
/set plugins.var.gospel.flock "flock_id"

/set plugins.var.gospel.cathedral.ip = "x.x.x.x"
/set plugins.var.gospel.cathedral.port = "xxxx"

/set plugins.var.gospel.kek-path "/path/to/read/kek"
/set plugins.var.gospel.cosk-path "/path/to/read/cosk-file"
/set plugins.var.gospel.cs-path "/path/to/read/cathedral-secret"
```

If you wish to use remembrances for your cathedral setup, enable
them by setting the **remembrance-path** configuration option.

```
/set plugins.var.gospel.remembrance-path "/path/to/write/remembrance.cfg"
```

If you want to disable p2p tunnels, you can set **p2p-enabled** to 0. This
is useful if you want to use cathedrals as a relay to hide your location
from the people you are talking with.

```
/set plugins.var.gospel.p2p-enabled 0
```

By default WeeChat logs all conversations, you probably don't want
that if you're using this plugin. To disable that either unload
the logger plugin or turn off autologging.

```
/set logger.file.auto_log off
```

Don't forget to /save after setting these.

## Load the plugin

```
/plugin load /path/to/gospel.so
```

## Chatting

```
/command gospel nick yourname
/command gospel chat <flock> <peer>
/command gospel group <flock> <group>
```

## Protocol limitations & traffic analysis.

Messages are limited 512 bytes.

This is because the litany protocol depends on sending send full-sized
message frames for each message that is sent regardless of the length
of the plaintext message.

This is done to prevent traffic analysis on what is being communicated.
