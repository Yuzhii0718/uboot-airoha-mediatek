// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Failsafe Web UI - the page inventory.
 *
 * One table lists every page of the Web UI: its id (which is also the
 * sidebar nav id whenever the UI has an entry for it), the embedded HTML
 * resource that is served for it and, optionally, its page script.
 * Everything that used to be maintained per page is derived from it:
 *
 *   - the URI registrations (core.c used to list the bootloader
 *     and UI pages, each module the rest),
 *   - the JS name whitelist of js_handler() (the registered URI *is* the
 *     asset name, so the handler needs no second list),
 *   - the sidebar entries main.js shows (it asks GET /ui/pages).
 *
 * A page is therefore added in four places, and nowhere else:
 *
 *   1. an entry in failsafe_pages[] below,
 *   2. its Kconfig option (failsafe/Kconfig) - the same option gates the
 *      assets in the fsdata Makefile of the selected UI, so the table
 *      cannot drift from what is embedded,
 *   3. its assets (failsafe/embedded/fsdata-bootstrap|brutalism/),
 *   4. its module, which only registers its own API endpoints.
 */

#include <malloc.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <net/mtk_httpd.h>

#include <failsafe/internal.h>
#include <failsafe/helpers.h>
#include <cprint.h>

struct failsafe_page {
	const char *id;		/* page id / sidebar nav id, NULL when the
				 * entry only has endpoints (sysinfo) */
	const char *html;	/* embedded page, served at this URI */
	const char *js;		/* page script, NULL when it has none */
	void (*register_handlers)(struct httpd_instance *inst);
};

/*
 * Every page of the Web UI, plus the module that owns its endpoints.  The
 * resources of an entry are registered by failsafe_register_pages(); the
 * ids are what GET /ui/pages reports and what main.js uses to show or hide
 * the sidebar entries.
 *
 * Pages that only exist in the MediaTek layouts are gated by the layout
 * options (the Airoha boards use the FIP layout), the advanced ones by the
 * option of the module that implements them.
 */
static const struct failsafe_page failsafe_pages[] = {
	/* Always built */
	{ "firmware",	"/index.html",		"/main.js",
	  upgrade_register_handlers },
	{ "booting",	"/booting.html",	NULL, NULL },
	{ "flashing",	"/flashing.html",	NULL, NULL },
	{ "fail",	"/fail.html",		NULL, NULL },
	{ "initramfs",	"/initramfs.html",	NULL, NULL },
	{ "reboot",	"/reboot.html",		NULL, NULL },

	/* No page of its own: the index page fetches it (see main.js) */
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_ADVANCED)
	{ NULL,		NULL,			NULL,
	  sysinfo_register_handlers },
#endif

	/* Bootloader layouts (see the "Failsafe layout" choice) */
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_LAYOUT_UBOOT)
	{ "uboot",	"/uboot.html",		NULL, NULL },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_LAYOUT_FIP)
	{ "bl2",	"/bl2.html",		NULL, NULL },
	{ "fip",	"/fip.html",		NULL, NULL },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_LAYOUT_CHAINLOADER)
	{ "chainloader", "/chainloader.html",	NULL, NULL },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_GPT)
	{ "gpt",	"/gpt.html",		NULL, NULL },
#endif

	/* Advanced pages, each with its own module */
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_CONSOLE)
	{ "console",	"/console.html",	"/console_js.js",
	  console_register_handlers },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_ENV)
	{ "env",	"/env.html",		"/env_js.js",
	  env_register_handlers },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_UBI)
	{ "ubi",	"/ubi.html",		"/ubi_js.js",
	  ubi_register_handlers },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_FLASH)
	{ "flash",	"/flash.html",		"/flash_js.js",
	  flash_register_handlers },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_SIMG)
	{ "simg",	"/simg.html",		"/simg_js.js",
	  simg_register_handlers },
#endif
#if IS_ENABLED(CONFIG_WEBUI_FAILSAFE_UI_BOOTSTRAP)
	/* Served by the bootstrap UI: the settings, theme and favicon
	 * resources (see modules/theme.c). */
	{ "settings",	"/settings.html",	"/settings_js.js",
	  theme_register_handlers },
#endif
};

/*
 * GET /ui/pages - the pages this firmware was built with.
 *
 * main.js asks for it once and shows exactly the sidebar entries that
 * exist; an older firmware without the endpoint is handled by probing the
 * page URLs instead, so both builds work in the same UI.
 */
static void pages_handler(enum httpd_uri_handler_status status,
			  struct httpd_request *request,
			  struct httpd_response *response)
{
	char *buf;
	int len = 0, size, i, n = 0;

	if (status != HTTP_CB_NEW)
		return;

	/* "{\"pages\":[\"chainloader\",...]}" */
	size = (int)strlen("{\"pages\":[]}") +
	       (int)ARRAY_SIZE(failsafe_pages) * 20;

	buf = malloc(size);
	if (!buf) {
		failsafe_http_reply_text(response, 500, "out of memory");
		return;
	}

	len = buf_appendf(buf, size, len, "{\"pages\":[");
	for (i = 0; i < ARRAY_SIZE(failsafe_pages); i++) {
		/* Only real pages are reported, not the API-only entries */
		if (!failsafe_pages[i].id)
			continue;

		len = buf_appendf(buf, size, len, "%s\"%s\"",
				  n++ ? "," : "", failsafe_pages[i].id);
	}
	buf_appendf(buf, size, len, "]}");

	failsafe_http_reply_json_alloc(response, 200, buf, buf);
}

void failsafe_register_pages(struct httpd_instance *inst)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(failsafe_pages); i++) {
		if (failsafe_pages[i].html)
			httpd_register_uri_handler(inst,
						   failsafe_pages[i].html,
						   &html_handler, NULL);

		/* The URI is the asset name, which is what js_handler()
		 * hands to the embedded filesystem. */
		if (failsafe_pages[i].js)
			httpd_register_uri_handler(inst,
						   failsafe_pages[i].js,
						   &js_handler, NULL);

		/* The endpoints of the module that goes with the page */
		if (failsafe_pages[i].register_handlers)
			failsafe_pages[i].register_handlers(inst);
	}

	httpd_register_uri_handler(inst, "/ui/pages", &pages_handler, NULL);
}
