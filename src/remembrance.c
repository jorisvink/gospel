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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gospel.h"

static void	remembrance_log_active(const char *);

/* Current active remembrances. */
static struct kyrka_event_remembrance	remembrances;

/*
 * Check if remembrance is active.
 */
int
gospel_remembrance_active(void)
{
	if (gospel_config_string("remembrance-path") != NULL)
		return (1);

	return (0);
}

/*
 * Initialise the remembrance system.
 */
void
gospel_remembrance_init(void)
{
	int		fd;
	ssize_t		ret;
	const char	*path;

	if ((path = gospel_config_string("remembrance-path")) == NULL) {
		gospel_log("[remembrance] not configured");
		return;
	}

	if ((fd = open(path, O_RDONLY)) == -1) {
		if (errno != ENOENT) {
			gospel_log("[remembrance] cannot open '%s' (%s)",
			    path, strerror(errno));
		}
		return;
	}

	for (;;) {
		ret = read(fd, &remembrances, sizeof(remembrances));
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			gospel_log("[remembrance]: read error (%s)",
			    strerror(errno));
			goto cleanup;
		}

		if ((size_t)ret != sizeof(remembrances)) {
			gospel_log("[remembrance]: short read (%zd/%zu)",
			    ret, sizeof(remembrances));
			goto cleanup;
		}

		break;
	}

	remembrance_log_active("loaded");

cleanup:
	(void)close(fd);
}

/*
 * Save a receive remembrance from our cathedral into the configured
 * remembrances path. If we no path exists we ignore this.
 */
void
gospel_remembrance_save(struct kyrka_event_remembrance *rem)
{
	ssize_t			ret;
	const char		*path;
	int			fd, len;
	char			tmp[256];

	PRECOND(rem != NULL);

	if ((path = gospel_config_string("remembrance-path")) == NULL)
		return;

	if (!memcmp(&remembrances, rem, sizeof(*rem)))
		return;

	len = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (len == -1 || (size_t)len >= sizeof(tmp)) {
		gospel_log("[remembrance] tmp buffer too small");
		return;
	}

	if ((fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600)) == -1) {
		gospel_log("[remembrance] cannot open '%s' (%s)",
		    tmp, strerror(errno));
		goto cleanup;
	}

	for (;;) {
		if ((ret = write(fd, rem, sizeof(*rem))) == -1) {
			if (errno == EINTR)
				continue;
			gospel_log("[remembrance] write error (%s)",
			    strerror(errno));
			goto cleanup;
		}

		if ((size_t)ret != sizeof(*rem)) {
			gospel_log("[remembrance] short write (%zd/%zu)",
			    ret, sizeof(*rem));
			goto cleanup;
		}

		break;
	}

	if (close(fd) == -1) {
		gospel_log("[remembrance] write errors (%s)", strerror(errno));
		goto cleanup;
	}

	fd = -1;

	if (rename(tmp, path) == -1) {
		gospel_log("[remembrance] rename error (%s)", strerror(errno));
		(void)unlink(tmp);
	}

	memcpy(&remembrances, rem, sizeof(*rem));
	remembrance_log_active("saved");

cleanup:
	if (fd != -1) {
		(void)close(fd);
		if (unlink(tmp) == -1) {
			gospel_log("[remembrance] unlink error (%s)",
			    strerror(errno));
		}
	}
}

/*
 * Mark the given cathedral as alive.
 */
void
gospel_remembrance_cathedral_alive(struct cathedral *cat)
{
	struct timespec		ts;

	PRECOND(cat != NULL);

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	cat->last = ts.tv_sec;
}

/*
 * Check if the given cathedral has timed out. If it has we will
 * select a new one if possible from our remembrance list.
 */
void
gospel_remembrance_cathedral_check(struct cathedral *cat)
{
	struct timespec		ts;

	PRECOND(cat != NULL);

	if (gospel_remembrance_active() == 0)
		return;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	if ((ts.tv_sec - cat->last) >= GOSPEL_CATHEDRAL_TIMEOUT) {
		gospel_log("[cathedral:%p] cathedral timed out", (void *)cat);
		(void)gospel_remembrance_cathedral_select(cat);
	}
}

/*
 * Select a new random cathedral based on our remembrances.
 */
int
gospel_remembrance_cathedral_select(struct cathedral *cat)
{
	struct in_addr		in;
	u_int32_t		idx;
	int			count, attempts;

	PRECOND(cat != NULL);
	PRECOND(gospel_remembrance_active() == 1);

	count = 0;

	for (idx = 0; idx < KYRKA_CATHEDRALS_MAX; idx++) {
		if (remembrances.ips[idx] == 0 || remembrances.ports[idx] == 0)
			break;
		count++;
	}

	if (count == 0)
		return (-1);

	for (attempts = 0; attempts < KYRKA_CATHEDRALS_MAX; attempts++) {
		nyfe_random_bytes(&idx, sizeof(idx));
		idx = idx & (count - 1);
		if (cat->addr.sin_addr.s_addr != remembrances.ips[idx] ||
		    cat->addr.sin_port != remembrances.ports[idx])
			break;
	}

	if (attempts == KYRKA_CATHEDRALS_MAX) {
		gospel_log("[cathedral:%p] failed to select new cathedral",
		    (void *)cat);
		return (-1);
	}

	cat->addr.sin_port = remembrances.ports[idx];
	cat->addr.sin_addr.s_addr = remembrances.ips[idx];

	in.s_addr = cat->addr.sin_addr.s_addr;
	gospel_log("[cathedral:%p] swapping to %s:%u", (void *)cat,
	    inet_ntoa(in), ntohs(cat->addr.sin_port));

	gospel_remembrance_cathedral_alive(cat);

	return (0);
}

/*
 * Log a message together with the number of remembrances currently active.
 */
static void
remembrance_log_active(const char *msg)
{
	int		idx;

	PRECOND(msg != NULL);
	PRECOND(gospel_remembrance_active() == 1);

	for (idx = 0; idx < KYRKA_CATHEDRALS_MAX; idx++) {
		if (remembrances.ips[idx] == 0 || remembrances.ports[idx] == 0)
			break;
	}

	gospel_log("[remembrance] %d cathedrals %s", idx, msg);
}
