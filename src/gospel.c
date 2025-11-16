/*
 * Copyright (c) 2025 Joris Vink <joris@sanctorum.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gospel.h"

static int	gospel_cmd_info(const void *, void *, struct t_gui_buffer *,
		    int, char **, char **);
static int	gospel_cmd_group(const void *, void *, struct t_gui_buffer *,
		    int, char **, char **);
static int	gospel_cmd_chat(const void *, void *, struct t_gui_buffer *,
		    int, char **, char **);
static int	gospel_cmd_nick(const void *, void *, struct t_gui_buffer *,
		    int, char **, char **);
static int	gospel_typing_hook(const void *, void *, const char *,
		    const char *, void *);

/* The /chat command. */
static struct t_hook		*chat_cmd;

/* The /nick command. */
static struct t_hook		*nick_cmd;

/* The /group command. */
static struct t_hook		*group_cmd;

/* The /info command. */
static struct t_hook		*info_cmd;

/* The hook to receive typing information. */
static struct t_hook		*typing_hook;

/* Our current nick. */
static char			nick[LITANY_NICK_MAX_SIZE + 1];

/* The typing status. */
static struct {
	u_int16_t	id;
	u_int64_t	flock;
	int		active;
} typing;

/*
 * Initialise the gospel by creating all required weechat related things.
 */
int
gospel_init(void)
{
	PRECOND(chat_cmd == NULL);
	PRECOND(nick_cmd == NULL);
	PRECOND(group_cmd == NULL);
	PRECOND(info_cmd == NULL);
	PRECOND(typing_hook == NULL);

	if (gospel_chat_init() == -1)
		return (-1);

	if ((chat_cmd = weechat_hook_command("chat",
	    "start new 1:1 chat", "flock peer", NULL, NULL,
	    gospel_cmd_chat, NULL, NULL)) == NULL)
		return (-1);

	if ((nick_cmd = weechat_hook_command("nick",
	    "Sets your nick", "nick", NULL, NULL,
	    gospel_cmd_nick, NULL, NULL)) == NULL)
		return (-1);

	if ((chat_cmd = weechat_hook_command("group",
	    "start new group chat", "flock group", NULL, NULL,
	    gospel_cmd_group, NULL, NULL)) == NULL)
		return (-1);

	if ((info_cmd = weechat_hook_command("info",
	    "show a information", NULL, NULL, NULL,
	    gospel_cmd_info, NULL, NULL)) == NULL)
		return (-1);

	if ((typing_hook = weechat_hook_signal("typing_self_*",
	    gospel_typing_hook, NULL, NULL)) == NULL)
		return (-1);

	gospel_log("gospel plugin loaded");

	return (0);
}

/*
 * Bad juju happened.
 */
void
gospel_fatal(const char *fmt, ...)
{
	va_list		args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");

	exit(1);
}

/*
 * Cleanup any resource allocated by us.
 */
void
gospel_cleanup(void)
{
	gospel_chat_cleanup();

	if (chat_cmd)
		weechat_unhook(chat_cmd);

	if (group_cmd)
		weechat_unhook(group_cmd);

	if (nick_cmd)
		weechat_unhook(nick_cmd);

	if (info_cmd)
		weechat_unhook(info_cmd);

	if (typing_hook)
		weechat_unhook(typing_hook);
}

/*
 * Log a message into our system buffer.
 */
void
gospel_log(const char *fmt, ...)
{
	va_list		args;

	PRECOND(fmt != NULL);

	va_start(args, fmt);
	gospel_logv(fmt, args);
	va_end(args);
}

/*
 * Log a message into our system buffer, the v variant.
 */
void
gospel_logv(const char *fmt, va_list args)
{
	int			len;
	struct t_gui_buffer	*pbuf;
	char			buf[128];

	PRECOND(fmt != NULL);

	/*
	 * If the system buffer is gone, pbuf is NULL and our calls
	 * to weechat_printf() will end up in the core window.
	 */
	pbuf = weechat_buffer_search("gospel", "system");

	len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (len == -1 || (size_t)len >= sizeof(buf)) {
		weechat_printf(pbuf, "log message too long for (%s)", fmt);
		return;
	}

	weechat_printf(pbuf, "%s", buf);
}

/*
 * Set our current nick name to the given name.
 */
int
gospel_nick_set(const char *name)
{
	size_t		idx, len;

	PRECOND(name != NULL);

	len = strlen(name);

	if (len < 2 || len > LITANY_NICK_MAX_SIZE)
		return (-1);

	for (idx = 0; idx < len; idx++) {
		if (!isalnum((unsigned char)name[idx]))
			return (-1);
	}

	memset(nick, 0, sizeof(nick));
	memcpy(nick, name, len);

	return (0);
}

/*
 * Returns the current nick to the caller.
 */
const char *
gospel_nick_get(void)
{
	return (nick);
}

/*
 * Returns the current nick prefixed with its KEK id to the caller.
 */
const char *
gospel_nick_prefixed(void)
{
	int		len;
	u_int16_t	kek;
	static char	buf[32];

	if (gospel_config_uint16("kek-id", &kek, 16) == -1)
		gospel_fatal("kek-id was not found as a configuration");

	len = snprintf(buf, sizeof(buf), "%s%02x+%s%s",
	    weechat_color(".gray"), kek, weechat_color("*white"), nick);
	if (len == -1 || (size_t)len >= sizeof(buf))
		gospel_fatal("prefixed nick length didn't work out");

	return (buf);
}

/*
 * Returns the typing status if chat matches the flock/id.
 */
int
gospel_typing_active(struct chat *chat)
{
	PRECOND(chat != NULL);

	if (chat->flock == typing.flock && chat->id == typing.id)
		return (typing.active);

	return (0);
}

/*
 * Load the cathedral ip:port from our configuration.
 */
int
gospel_config_cathedral(struct sockaddr_in *sin)
{
	const char		*addr;

	PRECOND(sin != NULL);

	if (gospel_config_uint16("cathedral.port", &sin->sin_port, 10) == -1) {
		gospel_log("no cathedral.port configuration option found");
		return (-1);
	}

	sin->sin_family = AF_INET;
	sin->sin_port = htobe16(sin->sin_port);

	if ((addr = gospel_config_string("cathedral.ip")) == NULL) {
		gospel_log("no cathedral.ip configuration option found");
		return (-1);
	}

	if (inet_pton(AF_INET, addr, &sin->sin_addr) == -1) {
		gospel_log("cathedral.ip contains an invalid address");
		return (-1);
	}

	return (0);
}

/*
 * Send the given signal into weechat.
 */
void
gospel_weechat_signal(const char *signal, const char *fmt, ...)
{
	int		len;
	va_list		args;
	char		data[128];

	va_start(args, fmt);
	len = vsnprintf(data, sizeof(data), fmt, args);
	if (len == -1 || (size_t)len >= sizeof(data))
		gospel_fatal("failed to create data for signal *%s'", signal);

	weechat_hook_signal_send(signal, WEECHAT_HOOK_SIGNAL_STRING, data);
}

/*
 * Called when typing status changes.
 */
static int
gospel_typing_hook(const void *ptr, void *udata, const char *signal,
    const char *type_data, void *signal_data)
{
	struct t_gui_buffer	*buf;
	struct chat		*chat;
	const char		*title;

	PRECOND(ptr == NULL);
	PRECOND(udata == NULL);

	buf = weechat_current_buffer();

	if ((title = weechat_buffer_get_string(buf, "title")) == NULL)
		return (WEECHAT_RC_OK);

	if ((chat = gospel_chat_find_name(title)) == NULL)
		return (WEECHAT_RC_OK);

	typing.id = chat->id;
	typing.flock = chat->flock;

	if (!strcmp(signal, "typing_self_typing"))
		typing.active = 1;
	else
		typing.active = 0;

	return (WEECHAT_RC_OK);
}

/*
 * The /chat command entry point.
 */
static int
gospel_cmd_chat(const void *ptr, void *udata, struct t_gui_buffer *buf,
    int argc, char **argv, char **argv_eol)
{
	u_int8_t		peer;
	u_int64_t		flock;

	PRECOND(ptr == NULL);
	PRECOND(udata == NULL);
	PRECOND(buf != NULL);
	PRECOND(argc >= 0);
	PRECOND(argv != NULL);
	PRECOND(argv_eol != NULL);

	if (nick[0] == '\0') {
		weechat_printf(buf, "No nick set, set first with /nick");
		return (WEECHAT_RC_ERROR);
	}

	if (argc != 3) {
		weechat_printf(buf, "Usage: /chat flock peer");
		return (WEECHAT_RC_ERROR);
	}

	if (strlen(argv[1]) > 16 || sscanf(argv[1], "%" PRIx64, &flock) != 1) {
		weechat_printf(buf, "flock is incorrect");
		return (WEECHAT_RC_ERROR);
	}

	if (strlen(argv[2]) > 2 || sscanf(argv[2], "%hhx", &peer) != 1) {
		weechat_printf(buf, "peer is incorrect");
		return (WEECHAT_RC_ERROR);
	}

	if (flock & 0xff) {
		weechat_printf(buf, "given flock has domain bits set, invalid");
		return (WEECHAT_RC_ERROR);
	}

	if (gospel_chat_direct(flock, peer) == -1) {
		weechat_printf(buf, "failed to create new direct chat");
		return (WEECHAT_RC_ERROR);
	}

	return (WEECHAT_RC_OK);
}

/*
 * The /group command entry point.
 */
static int
gospel_cmd_group(const void *ptr, void *udata, struct t_gui_buffer *buf,
    int argc, char **argv, char **argv_eol)
{
	u_int16_t		group;
	u_int64_t		flock, cfg_flock;

	PRECOND(ptr == NULL);
	PRECOND(udata == NULL);
	PRECOND(buf != NULL);
	PRECOND(argc >= 0);
	PRECOND(argv != NULL);
	PRECOND(argv_eol != NULL);

	if (nick[0] == '\0') {
		weechat_printf(buf, "No nick set, set first with /nick");
		return (WEECHAT_RC_ERROR);
	}

	if (argc != 3) {
		weechat_printf(buf, "Usage: /group flock group");
		return (WEECHAT_RC_ERROR);
	}

	if (strlen(argv[1]) > 16 || sscanf(argv[1], "%" PRIx64, &flock) != 1) {
		weechat_printf(buf, "flock is incorrect");
		return (WEECHAT_RC_ERROR);
	}

	if (strlen(argv[2]) > 4 || sscanf(argv[2], "%hx", &group) != 1) {
		weechat_printf(buf, "group is incorrect");
		return (WEECHAT_RC_ERROR);
	}

	if (group == 0) {
		weechat_printf(buf, "group 0 is invalid");
		return (WEECHAT_RC_ERROR);
	}

	if (flock & 0xff) {
		weechat_printf(buf, "given flock has domain bits set, invalid");
		return (WEECHAT_RC_ERROR);
	}

	if (gospel_config_uint64("flock", &cfg_flock, 16) == -1) {
		weechat_printf(buf, "no flock configuration option found");
		return (WEECHAT_RC_ERROR);
	}

	if (flock != cfg_flock) {
		weechat_printf(buf,
		    "cross-flock group chats not yet supported");
		return (WEECHAT_RC_ERROR);
	}

	if (gospel_chat_group(flock, group) == -1) {
		weechat_printf(buf, "failed to create new group chat");
		return (WEECHAT_RC_ERROR);
	}

	return (WEECHAT_RC_OK);
}

/*
 * The /nick command entry point.
 */
static int
gospel_cmd_nick(const void *ptr, void *udata, struct t_gui_buffer *buf,
    int argc, char **argv, char **argv_eol)
{
	PRECOND(ptr == NULL);
	PRECOND(udata == NULL);
	PRECOND(buf != NULL);
	PRECOND(argc >= 0);
	PRECOND(argv != NULL);
	PRECOND(argv_eol != NULL);

	if (argc == 1) {
		if (nick[0] == '\0')
			weechat_printf(buf, "No nick set yet");
		else
			weechat_printf(buf, "Your nick: %s", nick);

		return (WEECHAT_RC_OK);
	}

	if (argc != 2) {
		weechat_printf(buf, "Usage: /nick nick");
		return (WEECHAT_RC_ERROR);
	}

	if (gospel_nick_set(argv[1]) == -1) {
		weechat_printf(buf, "nick is too short or too long");
		return (WEECHAT_RC_ERROR);
	}

	gospel_chat_update_all();
	weechat_printf(buf, "Your nick is now: %s", nick);

	return (WEECHAT_RC_OK);
}

/*
 * The /info command entry point.
 */
static int
gospel_cmd_info(const void *ptr, void *udata, struct t_gui_buffer *buf,
    int argc, char **argv, char **argv_eol)
{
	PRECOND(ptr == NULL);
	PRECOND(udata == NULL);
	PRECOND(buf != NULL);
	PRECOND(argc >= 0);
	PRECOND(argv != NULL);
	PRECOND(argv_eol != NULL);

	gospel_chat_list(buf);

	return (WEECHAT_RC_OK);
}
