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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gospel.h"

static struct chat	*chat_create_new(const char *,
			    u_int64_t, u_int16_t, int);

static int	chat_prune(const void *, void *, int);
static int	chat_buffer_close(const void *, void *, struct t_gui_buffer *);
static int	chat_buffer_input(const void *, void *,
		    struct t_gui_buffer *, const char *);

/* Our active chats. */
static LIST_HEAD(, chat)	chats;

/* Our system chat window, should only exist once. */
static struct chat		*syschat;

/* Periodic pruning of chats that do not need to exist. */
static struct t_hook		*prune;

/*
 * Initialise the chat subsystem.
 */
int
gospel_chat_init(void)
{
	struct t_gui_buffer	*buf;

	PRECOND(syschat == NULL);

	LIST_INIT(&chats);

	if ((prune = weechat_hook_timer(1000, 0, 0,
	    chat_prune, NULL, NULL)) == NULL) {
		gospel_log("failed to create new timer hook");
		return (-1);
	}

	if ((syschat = chat_create_new("system", 0, 0, 0)) == NULL)
		return (-1);

	/* system shouldn't be part of the chat list. */
	LIST_REMOVE(syschat, list);

	if (gospel_liturgy_new(syschat, 0, 1) == -1) {
		gospel_chat_free(syschat);
		syschat = NULL;
		gospel_log("failed to create signaling liturgy");
		return (-1);
	}

	if ((buf = weechat_buffer_search("gospel", "system")) == NULL)
		gospel_fatal("failed to find newly created system buffer");

	weechat_buffer_set(buf, "notify", "0");
	weechat_buffer_set(buf, "title", "Litany system messages");

	return (0);
}

/*
 * Cleanup all active chats.
 */
void
gospel_chat_cleanup(void)
{
	struct chat	*chat;

	if (syschat != NULL)
		gospel_chat_free(syschat);

	while ((chat = LIST_FIRST(&chats)) != NULL) {
		LIST_REMOVE(chat, list);
		gospel_chat_free(chat);
	}

	weechat_unhook(prune);

	syschat = NULL;
	LIST_INIT(&chats);
}

/*
 * Unlink this chat and free all its resources. We leave the
 * underlying buffer so the user can do whatever it wants with it.
 */
void
gospel_chat_free(struct chat *chat)
{
	struct tunnel		*tun;
	struct t_gui_buffer	*buf;

	PRECOND(chat != NULL);

	LIST_REMOVE(chat, list);
	gospel_log("[chat] removed '%s'", chat->name);

	if ((buf = weechat_buffer_search("gospel", chat->name)) != NULL)
		weechat_buffer_set_pointer(buf, "input_callback", NULL);

	if (chat->liturgy != NULL)
		gospel_liturgy_free(chat->liturgy);

	while ((tun = LIST_FIRST(&chat->tunnels)) != NULL)
		gospel_tunnel_free(tun);

	free(chat->name);
	free(chat);
}

/*
 * Find a chat by its given name.
 */
struct chat *
gospel_chat_find_name(const char *name)
{
	struct chat		*chat;

	PRECOND(name != NULL);

	LIST_FOREACH(chat, &chats, list) {
		if (!strcmp(chat->name, name))
			return (chat);
	}

	return (NULL);
}

/*
 * Find a chat by its flock+id combination.
 */
struct chat *
gospel_chat_find(u_int64_t flock, u_int16_t id)
{
	struct chat		*chat;

	LIST_FOREACH(chat, &chats, list) {
		if (chat->flock == flock && chat->id == id)
			return (chat);
	}

	return (NULL);
}

/*
 * List all active chats and tunnels under them.
 */
void
gospel_chat_list(struct t_gui_buffer *buf)
{
	struct in_addr		in;
	struct tunnel		*tun;
	struct chat		*chat;

	PRECOND(buf != NULL);

	if (LIST_EMPTY(&chats)) {
		weechat_printf(buf, "no active gospels");
		return;
	}

	weechat_printf(buf, "active gospels");

	LIST_FOREACH(chat, &chats, list) {
		weechat_printf(buf, "  chat %s", chat->name);

		if (!LIST_EMPTY(&chat->tunnels))
			weechat_printf(buf, "    \\");

		LIST_FOREACH(tun, &chat->tunnels, list) {
			weechat_printf(buf, "     | -> %s", tun->name);

			in.s_addr = tun->cathedral.addr.sin_addr.s_addr;
			weechat_printf(buf, "          cathedral: %s:%u",
			    inet_ntoa(in),
			    be16toh(tun->cathedral.addr.sin_port));

			in.s_addr = tun->peer.sin_addr.s_addr;
			weechat_printf(buf, "          peer     : %s:%u",
			    inet_ntoa(in), be16toh(tun->peer.sin_port));
		}
	}
}

/*
 * Return our system chat window to the caller.
 */
struct chat *
gospel_system_chat(void)
{
	return (syschat);
}

/*
 * Signal a peer we would like to chat with them.
 */
void
gospel_chat_signal(u_int8_t peer, u_int8_t onoff)
{
	PRECOND(syschat != NULL);
	PRECOND(onoff == 0 || onoff == 1);

	syschat->liturgy->signaling[peer] = onoff;
	gospel_chat_log(syschat, "signaling for %02x is now %u", peer, onoff);
}

/*
 * Open a new 1:1 chat by setting up a new buffer and initialising the
 * tunnel we want to establish.
 */
int
gospel_chat_direct(u_int64_t flock, u_int8_t peer)
{
	int			len;
	struct chat		*chat;
	char			name[32];

	len = snprintf(name, sizeof(name),
	    "%" PRIx64 ":%02x [priv]", flock, peer);

	if (len == -1 || (size_t)len >= sizeof(name)) {
		gospel_log("failed to create buffer name for new chat");
		return (-1);
	}

	if ((chat = gospel_chat_find_name(name)) != NULL) {
		gospel_log("chat '%s' already exists", chat->name);
		return (0);
	}

	if ((chat = chat_create_new(name,
	    flock, peer, CHAT_MODE_DIRECT)) == NULL)
		return (-1);

	if (gospel_tunnel_new(chat, flock, peer, 0) == -1) {
		gospel_chat_free(chat);
		return (-1);
	}

	return (0);
}

/*
 * Open up a group chat and start the liturgy attached to it.
 */
int
gospel_chat_group(u_int64_t flock, u_int16_t group)
{
	int			len;
	struct chat		*chat;
	char			name[32];

	len = snprintf(name, sizeof(name),
	    "%" PRIx64 ":%04x [group]", flock, group);
	if (len == -1 || (size_t)len >= sizeof(name)) {
		gospel_log("failed to create buffer name for new chat");
		return (-1);
	}

	if ((chat = gospel_chat_find_name(name)) != NULL)
		return (0);

	if ((chat = chat_create_new(name,
	    flock, group, CHAT_MODE_GROUP)) == NULL)
		return (-1);

	if (gospel_liturgy_new(chat, group, 0) == -1) {
		gospel_chat_free(chat);
		return (-1);
	}

	return (0);
}

/*
 * Log a message to our chat buffer.
 */
void
gospel_chat_log(struct chat *chat, const char *fmt, ...)
{
	int			len;
	va_list			args;
	struct t_gui_buffer	*cbuf;
	char			buf[1024];

	PRECOND(chat != NULL);
	PRECOND(fmt != NULL);

	cbuf = weechat_buffer_search("gospel", chat->name);

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len == -1 || (size_t)len >= sizeof(buf)) {
		weechat_printf(cbuf, "snprintf failure on '%s'", fmt);
		return;
	}

	if (chat->mode == CHAT_MODE_DIRECT) {
		weechat_printf(cbuf, "[%" PRIx64 ":%02x] %s",
		    chat->flock, chat->id, buf);
	} else {
		weechat_printf(cbuf, "[%" PRIx64 ":%04x] %s",
		    chat->flock, chat->id, buf);
	}
}

/*
 * Insert the given message to our chat buffer, prefixed by who sent it.
 */
void
gospel_chat_msg(struct chat *chat, struct tunnel *src, const char *input)
{
	struct t_gui_buffer		*buf;
	size_t				len, namelen;
	const char			*name, *color;

	PRECOND(chat != NULL);
	PRECOND(src != NULL);
	PRECOND(input != NULL);

	len = strlen(input);
	if (len == 0)
		return;

	name = gospel_nick_get();
	namelen = strlen(name);

	if (len > namelen &&
	    memcmp(input, name, namelen) == 0 && input[namelen] == ':') {
		color = weechat_color("*red");
	} else {
		color = weechat_color("chat_nick");
	}

	buf = weechat_buffer_search("gospel", chat->name);
	weechat_printf(buf, "%s%s %s<%02x>\t%s", color, src->name,
	    weechat_color(".gray"), src->peerid, input);
}

/*
 * Update all information required on a chat after changes, for now
 * this only happens after a nick change.
 */
void
gospel_chat_update_all(void)
{
	struct chat		*chat;

	LIST_FOREACH(chat, &chats, list)
		gospel_chat_update_input_prompt(chat);
}

/*
 * Update the input_prompt for the given chat.
 */
void
gospel_chat_update_input_prompt(struct chat *chat)
{
	int			len;
	struct t_gui_buffer	*buf;
	char			input[64];

	PRECOND(chat != NULL);

	len = snprintf(input, sizeof(input), "%s%s",
	    weechat_color("*yellow"), gospel_nick_get());
	if (len == -1 || (size_t)len >= sizeof(input)) {
		gospel_log("failed to create input_prompt");
		return;
	}

	if ((buf = weechat_buffer_search("gospel", chat->name)) != NULL)
		weechat_buffer_set(buf, "input_prompt", input);
}

/*
 * Our chat buffer has closed, mark chat for cleanup.
 */
static int
chat_buffer_close(const void *ptr, void *udata, struct t_gui_buffer *buf)
{
	union deconst		p;
	struct chat		*chat;
	const char		*name;

	PRECOND(ptr != NULL);
	PRECOND(udata == NULL);
	PRECOND(buf != NULL);

	p.cp = ptr;
	name = ptr;

	if ((chat = gospel_chat_find_name(name)) != NULL)
		chat->release = 1;

	free(p.p);

	return (WEECHAT_RC_OK);
}

/*
 * There is input to be received from the underlying buffer. We take
 * said input and send it as a protocol message on all connected tunnels.
 */
static int
chat_buffer_input(const void *ptr, void *udata, struct t_gui_buffer *buf,
    const char *input)
{
	struct tunnel		*tun;
	struct chat		*chat;
	const char		*name;

	PRECOND(ptr != NULL);
	PRECOND(udata == NULL);
	PRECOND(buf != NULL);
	PRECOND(input != NULL);

	name = ptr;

	if ((chat = gospel_chat_find_name(name)) == NULL)
		return (WEECHAT_RC_ERROR);

	if (strlen(input) > LITANY_MESSAGE_MAX_SIZE)
		return (WEECHAT_RC_ERROR);

	LIST_FOREACH(tun, &chat->tunnels, list)
		gospel_tunnel_send(tun, input);

	weechat_printf(buf, "%s\t%s", gospel_nick_get(), input);

	return (WEECHAT_RC_OK);
}

/*
 * Allocate a new initial chat that is to be used for 1:1 or group.
 * We attach it to the existing buffer for this chat, or create a new one.
 */
static struct chat *
chat_create_new(const char *name, u_int64_t flock, u_int16_t id, int mode)
{
	void			*cb;
	struct t_gui_buffer	*buf;
	char			*bufname;
	struct chat		*chat, *ret;

	PRECOND(name != NULL);

	ret = NULL;
	chat = NULL;

	if ((chat = calloc(1, sizeof(*chat))) == NULL)
		return (NULL);

	if ((chat->name = strdup(name)) == NULL) {
		free(chat);
		return (NULL);
	}

	LIST_INSERT_HEAD(&chats, chat, list);

	chat->id = id;
	chat->mode = mode;
	chat->flock = flock;
	LIST_INIT(&chat->tunnels);

	if (!strcmp(name, "system"))
		cb = NULL;
	else
		cb = chat_buffer_input;

	if ((bufname = strdup(chat->name)) == NULL)
		goto cleanup;

	if ((buf = weechat_buffer_search("gospel", name)) == NULL) {
		if ((buf = weechat_buffer_new(name, cb, bufname, NULL,
		    chat_buffer_close, bufname, NULL)) == NULL) {
			free(bufname);
			goto cleanup;
		}

		if (strcmp(name, "system")) {
			if ((chat->nicks = weechat_nicklist_add_group(buf,
			    NULL, "PRIESTS", "yellow", 1)) == NULL)
				goto cleanup;

			(void)weechat_nicklist_add_nick(buf, chat->nicks,
			    gospel_nick_prefixed(), NULL, NULL, NULL, 1);
		}
	} else {
		free(bufname);
		weechat_buffer_set_pointer(buf, "input_callback", cb);
	}

	gospel_chat_update_input_prompt(chat);

	weechat_buffer_set(buf, "title", name);
	weechat_buffer_set(buf, "nicklist", "1");

	ret = chat;
	chat = NULL;

cleanup:
	if (chat != NULL)
		gospel_chat_free(chat);

	return (ret);
}

/*
 * Perform chat pruning, called by weechat timers.
 */
static int
chat_prune(const void *ptr, void *udata, int calls)
{
	struct chat		*chat, *next;

	PRECOND(ptr == NULL);
	PRECOND(udata == NULL);
	PRECOND(calls == -1);

	for (chat = LIST_FIRST(&chats); chat != NULL; chat = next) {
		next = LIST_NEXT(chat, list);

		if (chat->release)
			gospel_chat_free(chat);
	}

	return (WEECHAT_RC_OK);
}
