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

#ifndef __H_GOSPEL_H
#define __H_GOSPEL_H

#include <sys/queue.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <libkyrka/libkyrka.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif

#include <weechat-plugin.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "litany.h"

/* We didn't obey. */
#define PRECOND(x)							\
	do {								\
		if (!(x)) {						\
			gospel_fatal("precondition fail @ %s:%s:%d\n",	\
			    __FILE__, __func__, __LINE__);		\
		}							\
	} while (0)

#define VERIFY(x)							\
	do {								\
		if (!(x)) {						\
			gospel_fatal("verify fail @ %s:%s:%d\n",	\
			    __FILE__, __func__, __LINE__);		\
		}							\
	} while (0)

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define htobe16(x)		OSSwapHostToBigInt16(x)
#define htobe32(x)		OSSwapHostToBigInt32(x)
#define htobe64(x)		OSSwapHostToBigInt64(x)
#define be16toh(x)		OSSwapBigToHostInt16(x)
#define be32toh(x)		OSSwapBigToHostInt32(x)
#define be64toh(x)		OSSwapBigToHostInt64(x)
#endif

/*
 * Time in seconds before we consider a cathedral dead
 *
 * This timeout is used for *all* communication with a cathedral, be
 * it active tunnels or liturgies and thus must take into the account
 * the least amount of traffic we can see (3x remembrances).
 */
#define GOSPEL_CATHEDRAL_TIMEOUT	45

/*
 * WeeChat its callbacks are quite terrible, so hack around it.
 */
union deconst {
	void		*p;
	const void	*cp;
};

/*
 * A cathedral together with its last received data timestamp.
 */
struct cathedral {
	struct sockaddr_in	addr;
	time_t			last;
};

/*
 * A single liturgy either in discovery or signaling mode.
 */
struct liturgy {
	int			fd;
	time_t			age;
	u_int16_t		group;

	int			sig;
	KYRKA			*ctx;
	u_int64_t		flock;

	struct t_hook		*timer;
	struct t_hook		*events;

	struct cathedral	cathedral;

	u_int8_t		peers[KYRKA_PEERS_PER_FLOCK];
	u_int8_t		signaling[KYRKA_PEERS_PER_FLOCK];
};

/*
 * A single tunnel connected to a peer somewhere.
 */
struct tunnel {
	int			fd;
	u_int16_t		tid;
	u_int16_t		group;

	time_t			age;
	u_int64_t		local_uid;
	u_int64_t		remote_uid;

	u_int64_t		flock;
	u_int8_t		peerid;
	int			online;
	int			p2p_allowed;

	KYRKA			*ctx;
	struct sockaddr_in	peer;
	struct cathedral	cathedral;

	u_int64_t		msgid;

	struct t_hook		*timer;
	struct t_hook		*events;

	struct chat		*chat;
	struct litany_msg_list	pending;

	char			name[LITANY_NICK_MAX_SIZE + 1];

	LIST_ENTRY(tunnel)	list;
};

LIST_HEAD(tunnel_list, tunnel);

/*
 * A chat that we currently have open, could be 1:1 or group chat.
 */
#define CHAT_MODE_DIRECT	1
#define CHAT_MODE_GROUP		2

struct chat {
	u_int16_t			id;
	u_int64_t			flock;
	char				*name;

	int				mode;
	int				release;

	struct liturgy			*liturgy;

	struct t_gui_nick_group		*nicks;
	struct tunnel_list		tunnels;

	LIST_ENTRY(chat)		list;
};

/* All code calling weechat crap needs access to this. */
extern struct t_weechat_plugin 	*weechat_plugin;

/* src/gospel.c */
int	gospel_init(void);
void	gospel_cleanup(void);
int	gospel_nick_set(const char *);
int	gospel_nick_valid(const char *);
void	gospel_logv(const char *, va_list);
int	gospel_typing_active(struct chat *);
int	gospel_config_cathedral(struct cathedral *);
void	gospel_weechat_signal(const char *, const char *, ...);
void	gospel_log(const char *, ...) __attribute__((format (printf, 1, 2)));
void	gospel_fatal(const char *, ...)
	    __attribute__((format (printf, 1, 2))) __attribute__((noreturn));

const char	*gospel_nick_get(void);
const char	*gospel_nick_prefixed(void);

/* src/chat.c */
int	gospel_chat_init(void);
void	gospel_chat_cleanup(void);
void	gospel_chat_update_all(void);
void	gospel_chat_free(struct chat *);
void	gospel_chat_signal(u_int8_t, u_int8_t);
void	gospel_chat_list(struct t_gui_buffer *);
int	gospel_chat_direct(u_int64_t, u_int8_t);
int	gospel_chat_group(u_int64_t, u_int16_t);
void	gospel_chat_update_input_prompt(struct chat *);
void	gospel_chat_log(struct chat *, const char *, ...);
void	gospel_chat_msg(struct chat *, struct tunnel *, const char *);

struct chat	*gospel_system_chat(void);
struct chat	*gospel_chat_find_name(const char *);
struct chat	*gospel_chat_find(u_int64_t flock, u_int16_t);

/* src/config.c */
int	gospel_config_uint16(const char *, u_int16_t *, int);
int	gospel_config_uint32(const char *, u_int32_t *, int);
int	gospel_config_uint64(const char *, u_int64_t *, int);

const char	*gospel_config_string(const char *);

/* src/liturgy.c */
void	gospel_liturgy_free(struct liturgy *);
int	gospel_liturgy_new(struct chat *, u_int16_t, int);
void	gospel_liturgy_peer_offline(struct liturgy *, u_int8_t);

/* src/remembrance.c */
void	gospel_remembrance_init(void);
int	gospel_remembrance_active(void);
void	gospel_remembrance_cathedral_alive(struct cathedral *);
void	gospel_remembrance_cathedral_check(struct cathedral *);
int	gospel_remembrance_cathedral_select(struct cathedral *);
void	gospel_remembrance_save(struct kyrka_event_remembrance *);

/* src/tunnel.c */
void	gospel_tunnel_init(void);
void	gospel_tunnel_free(struct tunnel *);
void	gospel_tunnel_offline(u_int64_t, u_int8_t);
void	gospel_tunnel_send(struct tunnel *, const char *);
void	gospel_tunnel_log(struct tunnel *, const char *, ...);
int	gospel_tunnel_new(struct chat *, u_int64_t, u_int8_t, u_int16_t);

const char	*gospel_tunnel_prefixed_nick(struct tunnel *);
struct tunnel	*gospel_tunnel_find(struct tunnel_list *, u_int64_t, u_int8_t);

/*
 * XXX - these are included in libkyrka but not prototype'd
 */
void	nyfe_random_init(void);
void	nyfe_random_bytes(void *, size_t);

#endif
