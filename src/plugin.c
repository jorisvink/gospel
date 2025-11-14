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

#include "gospel.h"

WEECHAT_PLUGIN_VERSION("0.1");
WEECHAT_PLUGIN_LICENSE("ISC");
WEECHAT_PLUGIN_PRIORITY(6000);

WEECHAT_PLUGIN_NAME("gospel");
WEECHAT_PLUGIN_AUTHOR("Joris Vink <joris@sanctorum.se>");
WEECHAT_PLUGIN_DESCRIPTION(N_("Gospel - A litany protocol implementation"));

/* Required by weechat plugins. */
struct t_weechat_plugin		*weechat_plugin = NULL;

/*
 * The weechat plugin entry point. We simply call gospel_init() from here.
 */
int
weechat_plugin_init(struct t_weechat_plugin *plugin, int argc, char **argv)
{
	PRECOND(plugin != NULL);
	PRECOND(argc >= 0);
	PRECOND((argc > 0 && argv != NULL) || argv == NULL);
	PRECOND(weechat_plugin == NULL);

	weechat_plugin = plugin;

	if (gospel_init() == -1) {
		gospel_cleanup();
		return (WEECHAT_RC_ERROR);
	}

	return (WEECHAT_RC_OK);
}

/*
 * Called when the plugin is unloaded, cleanup all the things.
 */
int
weechat_plugin_end(struct t_weechat_plugin *plugin)
{
	PRECOND(plugin != NULL);

	gospel_cleanup();

	return (WEECHAT_RC_OK);
}
