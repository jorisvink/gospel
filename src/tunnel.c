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
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gospel.h"

static int	tunnel_weechat_manage(const void *, void *, int);
static int	tunnel_weechat_socket(const void *, void *, int);

static void	tunnel_alive(struct tunnel *);
static void	tunnel_ack_send(struct tunnel *, u_int64_t);
static void	tunnel_ack_recv(struct tunnel *, u_int64_t);

static void	tunnel_hb_queue(struct tunnel *);
static void	tunnel_hb_recv(struct tunnel *, struct litany_msg_data *);

static void	tunnel_msg_send(struct tunnel *);
static void	tunnel_msg_requeue(struct tunnel *);

static void	tunnel_event(KYRKA *, union kyrka_event *, void *);
static void	tunnel_heaven(struct kyrka_packet *, u_int64_t, void *);
static void	tunnel_cathedral(struct kyrka_packet *, u_int64_t, void *);
static void	tunnel_purgatory(struct kyrka_packet *, u_int64_t, void *);
static int	tunnel_configure(struct tunnel *, struct kyrka_cathedral_cfg *,
		    u_int64_t, u_int8_t, u_int16_t);

/* Used to signal to other side if we restarted somehow. */
static time_t		tunnel_sequence;

/*
 * Initialise our tunnel_sequence with current timestamp.
 */
void
gospel_tunnel_init(void)
{
	time(&tunnel_sequence);
}

/*
 * Create a new tunnel towards the given flock and peer and attach
 * it to the given chat. If this was not a tunnel for a group chat
 * we start the signaling process.
 */
int
gospel_tunnel_new(struct chat *chat, u_int64_t flock, u_int8_t peer,
    u_int16_t group)
{
	struct kyrka_cathedral_cfg	cfg;
	struct tunnel			*tun;
	u_int64_t			shroud;

	PRECOND(chat != NULL);

	if ((tun = calloc(1, sizeof(*tun))) == NULL)
		return (-1);

	LIST_INSERT_HEAD(&chat->tunnels, tun, list);

	tun->fd = -1;
	tun->msgid = 1;
	tun->chat = chat;
	tun->peerid = peer;
	tun->flock = flock;
	tun->group = group;
	tun->local_uid = tunnel_sequence++;

	if (tunnel_configure(tun, &cfg, flock, peer, group) == -1) {
		gospel_tunnel_free(tun);
		return (-1);
	}

	if ((tun->fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		gospel_tunnel_log(tun, "socket failed: %s", strerror(errno));
		gospel_tunnel_free(tun);
		return (-1);
	}

	if ((tun->timer = weechat_hook_timer(500, 0, 0,
	    tunnel_weechat_manage, tun, NULL)) == NULL) {
		gospel_tunnel_log(tun, "failed to create new timer hook");
		gospel_tunnel_free(tun);
		return (-1);
	}

	if ((tun->events = weechat_hook_fd(tun->fd, 1, 0, 0,
	    tunnel_weechat_socket, tun, NULL)) == NULL) {
		gospel_tunnel_log(tun, "failed to create new fd hook");
		gospel_tunnel_free(tun);
		return (-1);
	}

	if ((tun->ctx = kyrka_ctx_alloc(tunnel_event, tun)) == NULL) {
		gospel_tunnel_log(tun, "kyrka_ctx_alloc: failed");
		gospel_tunnel_free(tun);
		return (-1);
	}

	if (kyrka_purgatory_ifc(tun->ctx, tunnel_purgatory, tun) == -1) {
		gospel_tunnel_log(tun, "kyrka_purgatory_ifc: %d",
		    kyrka_last_error(tun->ctx));
		gospel_tunnel_free(tun);
		return (-1);
	}

	if (kyrka_heaven_ifc(tun->ctx, tunnel_heaven, tun) == -1) {
		gospel_tunnel_log(tun, "kyrka_heaven_ifc: %d",
		    kyrka_last_error(tun->ctx));
		gospel_tunnel_free(tun);
		return (-1);
	}

	if (kyrka_cathedral_config(tun->ctx, &cfg) == -1) {
		gospel_tunnel_log(tun, "failed to configure cathedral: %d",
		    kyrka_last_error(tun->ctx));
		gospel_tunnel_free(tun);
		return (-1);
	}

	if (tun->group == 0)
		gospel_chat_signal(tun->peerid, 1);

	if (gospel_config_uint64("shroud", &shroud, 10) != -1 && shroud == 1) {
		if (kyrka_shroud_enable(tun->ctx) == -1) {
			weechat_printf(NULL, "failed to enable shroud: %d",
			    kyrka_last_error(tun->ctx));
			gospel_tunnel_free(tun);
			return (-1);
		}
	}

	if (kyrka_mtu_size(tun->ctx, 800) == -1) {
		weechat_printf(NULL, "failed to set mtu: %d",
		    kyrka_last_error(tun->ctx));
		gospel_tunnel_free(tun);
		return (-1);
	}

	tunnel_alive(tun);
	gospel_log("[tunnel] %" PRIx64 ":%04x created (cathedral:%p)",
	    tun->flock, tun->tid, &tun->cathedral);

	return (0);
}

/*
 * Unlink and free all resources allocated to a tunnel.
 */
void
gospel_tunnel_free(struct tunnel *tun)
{
	PRECOND(tun != NULL);

	LIST_REMOVE(tun, list);
	gospel_log("[tunnel] %" PRIx64 ":%04x removed", tun->flock, tun->tid);

	kyrka_ctx_free(tun->ctx);

	if (tun->group == 0)
		gospel_tunnel_offline(tun->flock, tun->peerid);

	if (tun->fd != -1)
		(void)close(tun->fd);

	if (tun->timer != NULL)
		weechat_unhook(tun->timer);

	if (tun->events != NULL)
		weechat_unhook(tun->events);

	free(tun);
}

/*
 * Find a specific tunnel in the given tunnel list.
 */
struct tunnel *
gospel_tunnel_find(struct tunnel_list *tlist, u_int64_t flock, u_int8_t peer)
{
	struct tunnel		*tun;

	PRECOND(tlist != NULL);

	LIST_FOREACH(tun, tlist, list) {
		if (tun->flock == flock && tun->peerid == peer)
			return (tun);
	}

	return (NULL);
}

/*
 * Mark the tunnel with the given peer_id as offline.
 * Affects only the signaling liturgy.
 */
void
gospel_tunnel_offline(u_int64_t flock, u_int8_t peer)
{
	struct chat		*sys;

	if ((sys = gospel_system_chat()) == NULL)
		return;

	gospel_chat_signal(peer, 0);
	gospel_liturgy_peer_offline(sys->liturgy, peer);
	gospel_log("[tunnel] %" PRIx64 ":%02x offline", flock, peer);
}

/*
 * Log a tunnel message to our system buffer.
 */
void
gospel_tunnel_log(struct tunnel *tun, const char *fmt, ...)
{
	int		len;
	va_list		args;
	char		buf[128];

	PRECOND(tun != NULL);
	PRECOND(fmt != NULL);

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len == -1 || (size_t)len >= sizeof(buf)) {
		gospel_log("[%" PRIx64 ":%04x] %s", tun->flock, tun->tid, fmt);
	} else {
		gospel_log("[%" PRIx64 ":%04x] %s", tun->flock, tun->tid, buf);
	}
}

/*
 * Queue up a message to be sent in the tunnel.
 */
void
gospel_tunnel_send(struct tunnel *tun, const char *line)
{
	size_t			len;
	struct litany_msg	*msg;

	PRECOND(tun != NULL);
	PRECOND(line != NULL);

	len = strlen(line);
	VERIFY(len <= LITANY_MESSAGE_MAX_SIZE);

	if ((msg = calloc(1, sizeof(*msg))) == NULL) {
		gospel_tunnel_log(tun,
		    "failed to allocate new message for sending");
		return;
	}

	memcpy(msg->data.data, line, len);

	msg->data.len = htobe16(len);
	msg->data.id = htobe64(tun->msgid);
	msg->data.type = LITANY_MESSAGE_TYPE_TEXT;

	msg->id = tun->msgid;
	msg->age = gospel_ms();

	msg->pending = 1;
	TAILQ_INSERT_TAIL(&tun->waitq, msg, wlist);
	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);

	tun->msgid++;
}

/*
 * Returns the nick for the tunnel prefixed with its peer id.
 */
const char *
gospel_tunnel_prefixed_nick(struct tunnel *tun)
{
	int		len;
	static char	buf[32];

	PRECOND(tun != NULL);

	len = snprintf(buf, sizeof(buf), "%s%02x+%s%s",
	    weechat_color(".gray"), tun->peerid,
	    weechat_color("*white"), tun->name);
	if (len == -1 || (size_t)len >= sizeof(buf))
		gospel_fatal("prefixed nick length didn't work out");

	return (buf);
}

/*
 * Populate a cathedral configuration from our saved weechat settings.
 */
static int
tunnel_configure(struct tunnel *tun, struct kyrka_cathedral_cfg *cfg,
    u_int64_t flock, u_int8_t peer, u_int16_t group)
{
	u_int64_t		p2p;

	PRECOND(tun != NULL);
	PRECOND(cfg != NULL);

	memset(cfg, 0, sizeof(*cfg));

	TAILQ_INIT(&tun->sendq);
	TAILQ_INIT(&tun->waitq);

	if (gospel_config_cathedral(&tun->cathedral) == -1)
		return (-1);

	memcpy(&tun->peer, &tun->cathedral.addr, sizeof(tun->peer));

	if (gospel_config_uint16("kek-id", &tun->tid, 16) == -1) {
		gospel_log("[cfg] plugins.gospel.kek-id missing or invalid");
		return (-1);
	}

	if (gospel_config_uint64("flock", &cfg->flock_src, 16) == -1) {
		gospel_log("[cfg] plugins.gospel.flock missing or invalid");
		return (-1);
	}

	if ((cfg->flock_src & 0xff) || (flock & 0xff)) {
		gospel_log("[cfg] plugins.gospel.flock missing or invalid");
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

	if ((cfg->kek = gospel_config_string("kek-path")) == NULL) {
		gospel_log("[cfg] plugins.gospel.kek-path missing or invalid");
		return (-1);
	}

	if (gospel_config_uint64("p2p-enabled", &p2p, 10) == -1 || p2p != 0)
		tun->p2p_allowed = 1;
	else
		tun->p2p_allowed = 0;

	if (group) {
		cfg->flock_src |= LITANY_FLOCK_GROUP_DOMAIN;
		cfg->flock_dst = flock | LITANY_FLOCK_GROUP_DOMAIN;
	} else {
		cfg->flock_src |= LITANY_FLOCK_DOMAIN;
		cfg->flock_dst = flock | LITANY_FLOCK_DOMAIN;
	}

	cfg->udata = tun;
	cfg->send = tunnel_cathedral;

	cfg->tunnel = tun->tid << 8 | peer;
	tun->tid = cfg->tunnel;

	return (0);
}

/*
 * A libkyrka event occurred on the given tunnel, we handle it.
 */
static void
tunnel_event(KYRKA *ctx, union kyrka_event *evt, void *udata)
{
	struct in_addr		in;
	struct tunnel		*tun;
	int			online;

	PRECOND(ctx != NULL);
	PRECOND(evt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	gospel_remembrance_cathedral_alive(&tun->cathedral);

	switch (evt->type) {
	case KYRKA_EVENT_KEYS_INFO:
		gospel_tunnel_log(tun, "[tunnel]: tx=%08x rx=%08x",
		    evt->keys.tx_spi, evt->keys.rx_spi);

		if (tun->online == 0 &&
		    evt->keys.tx_spi != 0 && evt->keys.rx_spi != 0) {
			online = 1;
		} else {
			online = 0;
		}

		if (online) {
			tun->online = 1;
			tunnel_alive(tun);
		}
		break;
	case KYRKA_EVENT_EXCHANGE_INFO:
		gospel_tunnel_log(tun, "[exchange]: %s", evt->exchange.reason);
		break;
	case KYRKA_EVENT_AMBRY_RECEIVED:
		gospel_tunnel_log(tun,
		    "[ambry] generation %08x", evt->ambry.generation);
		break;
	case KYRKA_EVENT_LOGMSG:
		gospel_tunnel_log(tun, "[log]: %s", evt->logmsg.log);
		break;
	case KYRKA_EVENT_PEER_DISCOVERY:
		in.s_addr = evt->peer.ip;

		if (tun->peer.sin_addr.s_addr != evt->peer.ip ||
		    tun->peer.sin_port != evt->peer.port) {
			tun->peer.sin_port = evt->peer.port;
			tun->peer.sin_addr.s_addr = evt->peer.ip;

			if (tun->cathedral.addr.sin_addr.s_addr !=
			    tun->peer.sin_addr.s_addr &&
			    tun->cathedral.addr.sin_port !=
			    tun->peer.sin_port) {
				gospel_tunnel_log(tun,
				    "[peer]: p2p discovery %s:%u",
				    inet_ntoa(in), htons(evt->peer.port));

				if (kyrka_p2p_active(tun->ctx, 1) == -1) {
					gospel_tunnel_log(tun,
					    "[peer]: kyrka_p2p_active");
				}
			} else {
				if (kyrka_p2p_active(tun->ctx, 0) == -1) {
					gospel_tunnel_log(tun,
					    "[peer]: kyrka_p2p_active");
				}
			}
		}
		break;
	default:
		gospel_tunnel_log(tun, "event %u", evt->type);
		break;
	}
}

/*
 * Callback from libkyrka when we have data to be written to
 * the cathedral.
 */
static void
tunnel_cathedral(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	struct sockaddr_in	sin;
	u_int16_t		port;
	struct tunnel		*tun;
	u_int8_t		*data;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	port = be16toh(tun->cathedral.addr.sin_port);
	if (magic == KYRKA_CATHEDRAL_NAT_MAGIC)
		port++;

	sin.sin_family = AF_INET;
	sin.sin_port = htobe16(port);
	sin.sin_addr.s_addr = tun->cathedral.addr.sin_addr.s_addr;

	if ((data = kyrka_packet_sendbuf(tun->ctx, pkt, &len)) == NULL) {
		gospel_tunnel_log(tun, "kyrka_packet_sendbuf: %d",
		    kyrka_last_error(tun->ctx));
		return;
	}

	if (sendto(tun->fd, data, len, 0,
	    (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		gospel_tunnel_log(tun, "sendto: %s", strerror(errno));
	} else {
		tun->tx_bytes += len;
	}
}

/*
 * Callback from libkyrka when we have data to be written to
 * the purgatory side of things.
 */
static void
tunnel_purgatory(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	size_t			len;
	struct tunnel		*tun;
	u_int8_t		*data;

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	if ((data = kyrka_packet_sendbuf(tun->ctx, pkt, &len)) == NULL) {
		gospel_tunnel_log(tun, "kyrka_packet_sendbuf: %d",
		    kyrka_last_error(tun->ctx));
		return;
	}

	if (sendto(tun->fd, data, len, 0,
	    (struct sockaddr *)&tun->peer, sizeof(tun->peer)) == -1) {
		gospel_tunnel_log(tun, "sendto: %s", strerror(errno));
	} else {
		tun->tx_bytes += len;
	}
}

/*
 * Callback from libkyrka when we have data to be written to
 * the heaven side of things.
 */
static void
tunnel_heaven(struct kyrka_packet *pkt, u_int64_t magic, void *udata)
{
	struct litany_msg_data		*msg;
	struct tunnel			*tun;
	char				line[LITANY_MESSAGE_MAX_SIZE + 1];

	PRECOND(pkt != NULL);
	PRECOND(udata != NULL);

	tun = udata;

	if (pkt->length != sizeof(*msg)) {
		gospel_tunnel_log(tun,
		    "ignoring malformed packet (%zu vs %zu bytes)",
		    pkt->length, sizeof(*msg));
		return;
	}

	msg = kyrka_packet_data(pkt);

	msg->id = be64toh(msg->id);
	msg->len = be16toh(msg->len);

	if (msg->id == LITANY_MESSAGE_SYSTEM_ID)
		return;

	if (msg->len > sizeof(msg->data) || msg->len >= sizeof(line)) {
		gospel_tunnel_log(tun,
		    "ignoring message with invalid length (%u)", msg->len);
		return;
	}

	tunnel_alive(tun);

	switch (msg->type) {
	case LITANY_MESSAGE_TYPE_TEXT:
		memcpy(line, msg->data, msg->len);
		line[msg->len] = '\0';
		weechat_utf8_normalize(line, '?');
		gospel_chat_msg(tun->chat, tun, line);
		tunnel_ack_send(tun, msg->id);
		break;
	case LITANY_MESSAGE_TYPE_ACK:
		tunnel_ack_recv(tun, msg->id);
		break;
	case LITANY_MESSAGE_TYPE_HEARTBEAT:
		tunnel_hb_recv(tun, msg);
		break;
	default:
		gospel_tunnel_log(tun, "peer sending us weird things");
		break;
	}
}

/*
 * Mark given tunnel as alive.
 */
static void
tunnel_alive(struct tunnel *tun)
{
	PRECOND(tun != NULL);

	tun->age = gospel_ms();
}

/*
 * We received an ACK, remove pending message matching the id.
 */
static void
tunnel_ack_recv(struct tunnel *tun, u_int64_t id)
{
	struct litany_msg	*msg;

	PRECOND(tun != NULL);

	TAILQ_FOREACH(msg, &tun->waitq, wlist) {
		if (msg->id == id) {
			if (msg->pending)
				TAILQ_REMOVE(&tun->sendq, msg, slist);
			TAILQ_REMOVE(&tun->waitq, msg, wlist);
			free(msg);
			return;
		}
	}

	gospel_tunnel_log(tun, "ack for unknown %" PRIx64, id);
}

/*
 * Send an ACK for the given message id.
 */
static void
tunnel_ack_send(struct tunnel *tun, u_int64_t id)
{
	struct litany_msg	*msg;

	PRECOND(tun != NULL);
	PRECOND(id != LITANY_MESSAGE_SYSTEM_ID);

	if ((msg = calloc(1, sizeof(*msg))) == NULL) {
		gospel_tunnel_log(tun,
		    "failed to allocate new message for ACK");
		return;
	}

	msg->age = gospel_ms();
	msg->data.id = htobe64(id);
	msg->data.type = LITANY_MESSAGE_TYPE_ACK;

	msg->pending = 1;
	TAILQ_INSERT_TAIL(&tun->waitq, msg, wlist);
	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
}

/*
 * Queue a heartbeat for our peer. The heartbeat message are also
 * used to carry our friendly name to the peer.
 */
static void
tunnel_hb_queue(struct tunnel *tun)
{
	struct litany_hb_data		*hb;
	size_t				len;
	struct litany_msg		*msg;
	const char			*name;

	PRECOND(tun != NULL);

	name = gospel_nick_get();
	len = strlen(name);
	VERIFY(len <= LITANY_NICK_MAX_SIZE);

	if ((msg = calloc(1, sizeof(*msg))) == NULL) {
		gospel_tunnel_log(tun,
		    "failed to allocate new message for ACK");
		return;
	}

	msg->data.id = ULONG_MAX;
	msg->data.type = LITANY_MESSAGE_TYPE_HEARTBEAT;

	hb = (struct litany_hb_data *)&msg->data.data[0];

	hb->uid = tun->local_uid;
	hb->typing = gospel_typing_active(tun->chat);
	memcpy(hb->name, name, len);

	msg->pending = 1;
	TAILQ_INSERT_TAIL(&tun->waitq, msg, wlist);
	TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
}

/*
 * We received a heartbeat, look in the data of the body to extract
 * the peer information like nick. If this was the first time we
 * received a heartbeat we log that the peer joined.
 */
static void
tunnel_hb_recv(struct tunnel *tun, struct litany_msg_data *msg)
{
	struct litany_hb_data	*hb;
	struct t_gui_buffer	*cbuf;
	struct t_gui_nick	*nick;
	const char		*name;
	char			next[LITANY_NICK_MAX_SIZE + 1];

	PRECOND(tun != NULL);
	PRECOND(msg != NULL);
	PRECOND(msg->type == LITANY_MESSAGE_TYPE_HEARTBEAT);

	hb = (struct litany_hb_data *)msg->data;
	VERIFY(sizeof(tun->name) - 1 == sizeof(hb->name));

	memset(next, 0, sizeof(next));
	memcpy(next, hb->name, sizeof(hb->name));

	if (gospel_nick_valid(next) == -1)
		(void)snprintf(next, sizeof(next), "%02x", tun->peerid);

	cbuf = weechat_buffer_search("gospel", tun->chat->name);

	if (strcmp(tun->name, next)) {
		name = gospel_tunnel_prefixed_nick(tun);
		if (cbuf != NULL && (nick = weechat_nicklist_search_nick(cbuf,
		    NULL, name)) != NULL) {
			weechat_nicklist_remove_nick(cbuf, nick);
		}

		if (tun->name[0] != '\0') {
			weechat_printf(cbuf,
			    "%s\t%schanged name to %s (%02x)",
			    tun->name, weechat_color(".*green"),
			    next, tun->peerid);
		}
	}

	memset(tun->name, 0, sizeof(tun->name));
	memcpy(tun->name, next, sizeof(next));

	name = gospel_tunnel_prefixed_nick(tun);
	if (cbuf != NULL &&
	    weechat_nicklist_search_nick(cbuf, NULL, name) == NULL) {
		(void)weechat_nicklist_add_nick(cbuf, tun->chat->nicks,
		    name, NULL, NULL, NULL, 1);
	}

	gospel_weechat_signal("typing_set_nick", "0x%lx;%s;%s",
	    (unsigned long)cbuf, hb->typing ? "typing" : "off", tun->name);

	if (tun->remote_uid != hb->uid) {
		tun->remote_uid = hb->uid;

		if (tun->group) {
			weechat_printf(cbuf, "%s\t%sjoined the group (%02x)",
			    tun->name, weechat_color(".*green"),
			    tun->peerid);
		} else {
			weechat_printf(cbuf, "%s\t%shas come online",
			    tun->name, weechat_color(".*green"));
		}
	}
}

/*
 * Send the first mesasge in our msg queue to our peer.
 * If no message was present, send a heartbeat instead.
 */
static void
tunnel_msg_send(struct tunnel *tun)
{
	size_t			len;
	struct kyrka_packet	pkt;
	struct litany_msg	*msg;
	u_int8_t		*data;

	PRECOND(tun != NULL);

	/* XXX - might want to rethink this. */
	if (TAILQ_EMPTY(&tun->sendq))
		tunnel_hb_queue(tun);

	msg = TAILQ_FIRST(&tun->sendq);
	VERIFY(msg != NULL);

	msg->pending = 0;
	TAILQ_REMOVE(&tun->sendq, msg, slist);

	if ((data = kyrka_packet_databuf(tun->ctx, &pkt, &len)) == NULL) {
		gospel_tunnel_log(tun, "kyrka_packet_databuf: %d",
		    kyrka_last_error(tun->ctx));
		return;
	}

	VERIFY(sizeof(msg->data) <= len);

	pkt.length = sizeof(msg->data);
	memcpy(data, &msg->data, sizeof(msg->data));

	if (gospel_inet_match(&tun->peer, &tun->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_heaven_input(tun->ctx, &pkt) == -1 &&
	    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_TX_KEY) {
		gospel_tunnel_log(tun, "kyrka_heaven_input: %d",
		    kyrka_last_error(tun->ctx));
	}

	if (msg->data.type != LITANY_MESSAGE_TYPE_TEXT) {
		TAILQ_REMOVE(&tun->waitq, msg, wlist);
		free(msg);
	}
}

/*
 * Requeue pending messages once they become stale. Removes non text msgs
 * if they for some reason have been on the waitq for >= 5 seconds.
 */
static void
tunnel_msg_requeue(struct tunnel *tun)
{
	u_int64_t		now;
	struct litany_msg	*msg, *next;

	PRECOND(tun != NULL);

	now = gospel_ms();

	for (msg = TAILQ_FIRST(&tun->waitq); msg != NULL; msg = next) {
		next = TAILQ_NEXT(msg, wlist);

		if ((now - msg->age) >= 5000) {
			if (msg->data.type != LITANY_MESSAGE_TYPE_TEXT) {
				if (msg->pending)
					TAILQ_REMOVE(&tun->sendq, msg, slist);
				TAILQ_REMOVE(&tun->waitq, msg, wlist);
				free(msg);
			} else if (msg->pending == 0) {
				msg->age = gospel_ms();
				msg->pending = 1;
				TAILQ_INSERT_TAIL(&tun->sendq, msg, slist);
			}
		}
	}
}

/*
 * Callback called once every 500ms, we do tunnel management here,
 * including the sending of queued up packets.
 */
static int
tunnel_weechat_manage(const void *ptr, void *udata, int calls)
{
	union deconst		p;
	u_int64_t		now;
	struct tunnel		*tun;
	struct t_gui_nick	*nick;
	struct t_gui_buffer	*cbuf;
	const char		*name;
	int			is_cathedral;

	PRECOND(ptr != NULL);
	PRECOND(udata == NULL);
	PRECOND(calls == -1);

	p.cp = ptr;
	tun = p.p;

	now = gospel_ms();

	if (tun->online == 1 && (now - tun->age) >= 10000) {
		tun->online = 0;

		cbuf = weechat_buffer_search("gospel", tun->chat->name);

		if (tun->group) {
			weechat_printf(cbuf, "%s\t%sleft the group (%02x)",
			    tun->name, weechat_color(".*red"), tun->peerid);
		} else {
			weechat_printf(cbuf, "%s\t%shas gone offline",
			    tun->name, weechat_color(".*red"));
		}

		name = gospel_tunnel_prefixed_nick(tun);
		if (cbuf != NULL && (nick = weechat_nicklist_search_nick(cbuf,
		    NULL, name)) != NULL) {
			weechat_nicklist_remove_nick(cbuf, nick);
		}
	}

	if (now >= tun->next_mgmt) {
		tun->next_mgmt = now + 5000;

		if (kyrka_key_manage(tun->ctx) == -1 &&
		    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_SECRET) {
			gospel_tunnel_log(tun, "kyrka_key_manage: %d",
			    kyrka_last_error(tun->ctx));
		}

		if (kyrka_cathedral_notify(tun->ctx) == -1) {
			gospel_tunnel_log(tun, "kyrka_cathedral_notify: %d",
			    kyrka_last_error(tun->ctx));
		}

		if (tun->p2p_allowed) {
			if (kyrka_cathedral_nat_detection(tun->ctx) == -1) {
				gospel_tunnel_log(tun,
				    "kyrka_cathedral_nat_detection: %d",
				    kyrka_last_error(tun->ctx));
			}
		}

		if (tun->online)
			tunnel_msg_requeue(tun);
	}

	if (tun->online)
		tunnel_msg_send(tun);

	if (!memcmp(&tun->peer, &tun->cathedral.addr, sizeof(tun->peer)))
		is_cathedral = 1;
	else
		is_cathedral = 0;

	gospel_remembrance_cathedral_check(&tun->cathedral);

	if (is_cathedral &&
	    memcmp(&tun->peer, &tun->cathedral.addr, sizeof(tun->peer))) {
		gospel_log("[tunnel] %" PRIx64 ":%04x cathedral swapped",
		    tun->flock, tun->tid);
		memcpy(&tun->peer, &tun->cathedral.addr, sizeof(tun->peer));
	}

	return (WEECHAT_RC_OK);
}

/*
 * Callback from weechat when there is data to be read on our socket.
 */
static int
tunnel_weechat_socket(const void *ptr, void *udata, int fd)
{
	union deconst		p;
	size_t			len;
	ssize_t			ret;
	struct sockaddr_in	sin;
	struct kyrka_packet	pkt;
	struct tunnel		*tun;
	u_int8_t		*data;
	socklen_t		socklen;

	PRECOND(ptr != NULL);
	PRECOND(udata == NULL);
	PRECOND(fd >= 0);

	p.cp = ptr;
	tun = p.p;

	if ((data = kyrka_packet_recvbuf(tun->ctx, &pkt, &len)) == NULL) {
		gospel_log("kyrka_packet_recvbuf: %d",
		    kyrka_last_error(tun->ctx));
		return (WEECHAT_RC_OK);
	}

	socklen = sizeof(sin);

	if ((ret = recvfrom(tun->fd, data, len, MSG_DONTWAIT,
	    (struct sockaddr *)&sin, &socklen)) == -1) {
		gospel_log("recv: %s", strerror(errno));
		return (WEECHAT_RC_OK);
	}

	pkt.length = ret;

	if (gospel_inet_match(&sin, &tun->cathedral.addr))
		pkt.shroud = KYRKA_PACKET_SHROUD_CATHEDRAL;
	else
		pkt.shroud = KYRKA_PACKET_SHROUD_PEER;

	if (kyrka_purgatory_input(tun->ctx, &pkt) == -1 &&
	    kyrka_last_error(tun->ctx) != KYRKA_ERROR_NO_RX_KEY) {
		gospel_tunnel_log(tun, "kyrka_purgatory_input: %d",
		    kyrka_last_error(tun->ctx));
	} else {
		tun->rx_bytes += ret;
	}

	return (WEECHAT_RC_OK);
}
