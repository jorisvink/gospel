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

#ifndef __H_LITANY_PROTO_H
#define __H_LITANY_PROTO_H

/* The flock domains litany uses. */
#define LITANY_FLOCK_DOMAIN		0x0b
#define LITANY_FLOCK_GROUP_DOMAIN	0x0c

/* Id of 0 are reserved for system messages. */
#define LITANY_MESSAGE_SYSTEM_ID	0

/* The maximum number of bytes per message. */
#define LITANY_MESSAGE_MAX_SIZE		512

/* The maximum size for a nickname. */
#define LITANY_NICK_MAX_SIZE		16

/* The different type of messages. */
#define LITANY_MESSAGE_TYPE_TEXT	1
#define LITANY_MESSAGE_TYPE_ACK		2
#define LITANY_MESSAGE_TYPE_HEARTBEAT	3

/* 
 * Data carried in a heartbeat packet, must be ever max
 * LITANY_MESSAGE_MAX_SIZE bytes large.
 */
struct litany_hb_data {
	u_int64_t		uid;
	u_int8_t		name[LITANY_NICK_MAX_SIZE];
} __attribute__((packed));

/*
 * A message containing some data that we are sending to the
 * other side every few seconds until that side ACKs its transfer.
 */
struct litany_msg_data {
	u_int64_t		id;
	u_int16_t		len;
	u_int8_t		type;
	u_int8_t		data[LITANY_MESSAGE_MAX_SIZE];
} __attribute__((packed));

struct litany_msg {
	u_int64_t		id;
	time_t			age;
	struct litany_msg_data	data;
	TAILQ_ENTRY(litany_msg)	list;
};

TAILQ_HEAD(litany_msg_list, litany_msg);

#endif
