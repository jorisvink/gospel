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
#include <sys/socket.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gospel.h"

static int	liturgy_weechat_manage(const void *, void *, int);
static int	liturgy_weechat_socket(const void *, void *, int);

static void	liturgy_event(KYRKA *, union kyrka_event *, void *);
static void	liturgy_cathedral(const void *, size_t, u_int64_t, void *);
static int	liturgy_configure(struct liturgy *,
		    struct kyrka_cathedral_cfg *, u_int16_t);

static void	liturgy_tunnel_discovery(struct liturgy *, u_int8_t, int);
static void	liturgy_tunnel_signaling(struct liturgy *, u_int8_t, int);

/*
 * Create a new liturgy towards the configured cathedral.
 */
int
gospel_liturgy_new(struct chat *chat, u_int16_t group, int sig)
{
	struct kyrka_cathedral_cfg	cfg;
	struct liturgy			*lit;

	PRECOND(chat != NULL);
	PRECOND(chat->liturgy == NULL);
	PRECOND(sig == 0 || sig == 1);

	if ((lit = calloc(1, sizeof(*lit))) == NULL)
		return (-1);

	lit->fd = -1;
	lit->sig = sig;
	lit->group = group;

	if (liturgy_configure(lit, &cfg, group) == -1) {
		gospel_liturgy_free(lit);
		return (-1);
	}

	if ((lit->fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		gospel_log("socket failed: %s", strerror(errno));
		gospel_liturgy_free(lit);
		return (-1);
	}

	if ((lit->timer = weechat_hook_timer(2000, 0, 0,
	    liturgy_weechat_manage, lit, NULL)) == NULL) {
		gospel_log("failed to create new timer hook");
		gospel_liturgy_free(lit);
		return (-1);
	}

	if ((lit->events = weechat_hook_fd(lit->fd, 1, 0, 0,
	    liturgy_weechat_socket, lit, NULL)) == NULL) {
		gospel_log("failed to create new fd hook");
		gospel_liturgy_free(lit);
		return (-1);
	}

	if ((lit->ctx = kyrka_ctx_alloc(liturgy_event, lit)) == NULL) {
		gospel_liturgy_free(lit);
		return (-1);
	}

	if (kyrka_cathedral_config(lit->ctx, &cfg) == -1) {
		weechat_printf(NULL, "failed to configure cathedral: %d",
		    kyrka_last_error(lit->ctx));
		gospel_liturgy_free(lit);
		return (-1);
	}

	chat->liturgy = lit;

	gospel_log("[liturgy] %" PRIx64 ":%04x (%d) created (cathedral:%p)",
	    cfg.flock_src, group, sig, (void *)&lit->cathedral);

	return (0);
}

/*
 * Free all resources allocated to a liturgy.
 */
void
gospel_liturgy_free(struct liturgy *lit)
{
	PRECOND(lit != NULL);

	gospel_log("[liturgy] %" PRIx64 ":%04x (%d) removed",
	    lit->flock, lit->group, lit->sig);

	kyrka_ctx_free(lit->ctx);

	/* XXX move into gospel_event. */
	if (lit->fd != -1)
		(void)close(lit->fd);

	if (lit->timer != NULL)
		weechat_unhook(lit->timer);

	if (lit->events != NULL)
		weechat_unhook(lit->events);

	free(lit);
}

/*
 * Clear the online indicator for the given peer.
 */
void
gospel_liturgy_peer_offline(struct liturgy *lit, u_int8_t peer)
{
	PRECOND(lit != NULL);

	lit->peers[peer] = 0;
}

/*
 * Populate a cathedral configuration from our saved weechat settings.
 */
static int
liturgy_configure(struct liturgy *lit, struct kyrka_cathedral_cfg *cfg,
    u_int16_t group)
{
	u_int16_t	id;

	PRECOND(lit != NULL);
	PRECOND(cfg != NULL);

	memset(cfg, 0, sizeof(*cfg));

	if (gospel_config_cathedral(&lit->cathedral) == -1)
		return (-1);

	if (gospel_config_uint16("kek-id", &id, 16) == -1) {
		gospel_log("[cfg] plugins.gospel.kek-id missing or invalid");
		return (-1);
	}

	if (gospel_config_uint64("flock", &cfg->flock_src, 16) == -1) {
		gospel_log("[cfg] plugins.gospel.flock missing or invalid");
		return (-1);
	}

	if (cfg->flock_src & 0xff) {
		gospel_log("[cfg] plugins.gospel.flock has domain bits");
		return (-1);
	}

	if (gospel_config_uint32("identity", &cfg->identity, 16) == -1) {
		gospel_log("[cfg] plugins.gospel.identity missing or invalid");
		return (-1);
	}

	if ((cfg->secret = gospel_config_string("cs-path")) == NULL) {
		gospel_log("[cfg] plugins.gospel.cs-path missing or invalid");
		return (-1);
	}

	if ((cfg->cosk = gospel_config_string("cosk-path")) == NULL) {
		gospel_log("[cfg] plugins.gospel.cosk-path missing or invalid");
		return (-1);
	}

	cfg->tunnel = id;
	cfg->group = group;
	cfg->remembrance = 1;
	cfg->flock_src |= LITANY_FLOCK_GROUP_DOMAIN;

	cfg->udata = lit;
	cfg->send = liturgy_cathedral;

	lit->flock = cfg->flock_src;

	return (0);
}

/*
 * A libkyrka event occurred on a liturgy, we handle it.
 */
static void
liturgy_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	int			idx;
	struct liturgy		*lit;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	lit = udata;

	gospel_remembrance_cathedral_alive(&lit->cathedral);

	switch (evt->type) {
	case KYRKA_EVENT_LITURGY_RECEIVED:
		if (lit->sig) {
			for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
				liturgy_tunnel_signaling(lit,
				    idx, evt->liturgy.peers[idx]);
			}
		} else {
			for (idx = 1; idx < KYRKA_PEERS_PER_FLOCK; idx++) {
				liturgy_tunnel_discovery(lit,
				    idx, evt->liturgy.peers[idx]);
			}
		}
		break;
	case KYRKA_EVENT_REMEMBRANCE_RECEIVED:
		if (lit->sig)
			gospel_remembrance_save(&evt->remembrance);
		break;
	default:
		gospel_log("received event %d for a liturgy", evt->type);
		break;
	}
}

/*
 * Check if we need to attach a new tunnel to our group chat or if we
 * should remove a specific tunnel.
 */
static void
liturgy_tunnel_discovery(struct liturgy *lit, u_int8_t peer, int state)
{
	struct tunnel		*tun;
	struct chat		*chat;
	u_int64_t		flock;

	PRECOND(lit != NULL);

	if (state != 0 && state != 1) {
		gospel_log("[liturgy] invalid discovery state for %02x (%d)",
		    peer, state);
		return;
	}

	flock = lit->flock & ~(0xff);

	if ((chat = gospel_chat_find(flock, lit->group)) == NULL)
		return;

	tun = gospel_tunnel_find(&chat->tunnels, flock, peer);

	if (state == 1 && tun == NULL) {
		if (gospel_tunnel_new(chat, flock, peer, lit->group) == -1) {
			gospel_log("[tunnel] failed to add %" PRIx64
			    ":%02x to group %04x", flock, peer, lit->group);
		} else {
			gospel_log("[tunnel] added %" PRIx64
			    ":%02x to group %04x", flock, peer, lit->group);
		}
	} else if (state == 0 && tun != NULL) {
		gospel_tunnel_free(tun);
		gospel_log("[tunnel] removed %" PRIx64
		    ":%02x from group %04x", flock, peer, lit->group);
	}
}

/*
 * Check to see if we need to start a tunnel based on its signaling
 * state that the cathedral reported to us.
 */
static void
liturgy_tunnel_signaling(struct liturgy *lit, u_int8_t peer, int state)
{
	struct chat	*chat;
	u_int64_t	flock;

	PRECOND(lit != NULL);

	if (state != 0 && state != 1) {
		gospel_log("[liturgy] invalid signaling state for %02x (%d)",
		    peer, state);
		return;
	}

	flock = lit->flock & ~(0xff);

	if (lit->peers[peer] == 0 && state == 1) {
		if (gospel_chat_direct(flock, peer) == -1) {
			gospel_log("failed to start tunnel %" PRIx64 ":%02x",
			    lit->flock, peer);
		} else {
			gospel_log("[tunnel] %" PRIx64 ":%02x started",
			    lit->flock, peer);
		}
	}

	if (lit->peers[peer] == 1 && state == 0) {
		if ((chat = gospel_chat_find(flock, peer)) != NULL)
			gospel_chat_free(chat);
	}

	lit->peers[peer] = state;
}

/*
 * Callback from libkyrka when we have data to be written to
 * the cathedral.
 */
static void
liturgy_cathedral(const void *data, size_t len, u_int64_t magic, void *udata)
{
	struct liturgy		*lit;

	PRECOND(data != NULL);
	PRECOND(len > 0);
	PRECOND(udata != NULL);

	lit = udata;

	if (sendto(lit->fd, data, len, 0,
	    (struct sockaddr *)&lit->cathedral.addr,
	    sizeof(lit->cathedral.addr)) == -1)
		gospel_log("sendto: %s", strerror(errno));
}

/*
 * Callback called once every second, we do liturgy announcements here.
 */
static int
liturgy_weechat_manage(const void *ptr, void *udata, int calls)
{
	union deconst		p;
	size_t			len;
	struct liturgy		*lit;
	void			*peers;

	PRECOND(ptr != NULL);
	PRECOND(udata == NULL);
	PRECOND(calls == -1);

	p.cp = ptr;
	lit = p.p;

	if (lit->sig) {
		peers = lit->signaling;
		len = sizeof(lit->signaling);
	} else {
		len = 0;
		peers = NULL;
	}

	if (kyrka_cathedral_liturgy(lit->ctx, peers, len) == -1) {
		gospel_log("kyrka_cathedral_liturgy: %d",
		    kyrka_last_error(lit->ctx));
	}

	gospel_remembrance_cathedral_check(&lit->cathedral);

	return (WEECHAT_RC_OK);
}

/*
 * Callback from weechat when there is data to be read on our socket.
 */
static int
liturgy_weechat_socket(const void *ptr, void *udata, int fd)
{
	union deconst		p;
	ssize_t			ret;
	struct liturgy		*lit;
	char			pkt[1500];

	PRECOND(ptr != NULL);
	PRECOND(udata == NULL);
	PRECOND(fd >= 0);

	p.cp = ptr;
	lit = p.p;

	ret = recv(lit->fd, pkt, sizeof(pkt), MSG_DONTWAIT);
	if (ret == -1 && errno != EAGAIN) {
		gospel_log("liturgy recv: %s", strerror(errno));
		return (WEECHAT_RC_OK);
	}

	if (kyrka_purgatory_input(lit->ctx, pkt, ret) == -1) {
		gospel_log("kyrka_purgatory_input: %d",
		    kyrka_last_error(lit->ctx));
	}

	return (WEECHAT_RC_OK);
}
