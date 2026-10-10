/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe Web UI - Main entry point
 *
 * This file is the sole entry point for the failsafe subsystem.
 * All feature modules live under modules/ and expose a
 * *_register_handlers() function that is called from here.
 */

#include <command.h>
#include <console.h>
#include <errno.h>
#include <env.h>
#include <net.h>
#include <net/mtk_tcp.h>
#include <net/mtk_httpd.h>
#ifdef CONFIG_MTK_DHCPD
#include <net/mtk_dhcpd.h>
#endif
#ifdef CONFIG_MTK_DNSD
#include <net/mtk_dnsd.h>
#endif
#ifdef CONFIG_MTK_TELNETD
#include <net/mtk_telnetd.h>
#endif
#include <linux/string.h>
#include <linux/delay.h>
#include <rand.h>
#include <u-boot/schedule.h>
#include <vsprintf.h>
#include <failsafe/fw_type.h>
#include <failsafe/fs.h>
#include <failsafe/led.h>

#include <failsafe/internal.h>
#include <cprint.h>

/* ------------------------------------------------------------------ */
/*  Defines for default IP/netmask when env vars are not set             */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_IPADDR
#define HTTPD_DEFAULT_IPADDR	CONFIG_IPADDR
#else
#define HTTPD_DEFAULT_IPADDR	"192.168.1.1"
#endif
#ifdef CONFIG_NETMASK
# define HTTPD_DEFAULT_NETMASK	CONFIG_NETMASK
#else
#define HTTPD_DEFAULT_NETMASK	"255.255.255.0"
#endif

/* ------------------------------------------------------------------ */
/*  Core state (local to the main loop)                                */
/* ------------------------------------------------------------------ */

static bool failsafe_httpd_running;
static bool services_auto_started;
static bool mtk_tcp_done_flag;
static bool eth_needs_reinit;

/* ------------------------------------------------------------------ */
/*  Shared state (defined in upgrade.c, used here)                     */
/* ------------------------------------------------------------------ */

/* addressed in modules/upgrade.c */
extern u32 upload_data_id;
extern const void *upload_data;
extern size_t upload_size;
extern bool auto_action_pending;
extern bool reboot_pending;
extern bool boot_system_pending;
extern failsafe_fw_t fw_type;

/* ------------------------------------------------------------------ */
/*  Weak overrides                                                     */
/* ------------------------------------------------------------------ */

int __weak failsafe_validate_image(const void *data, size_t size, failsafe_fw_t fw)
{
	return 0;
}

int __weak failsafe_write_image(const void *data, size_t size, failsafe_fw_t fw)
{
	return -ENOSYS;
}

int __weak failsafe_bl2_version_info(const void *data, size_t size,
				     failsafe_fw_t fw,
				     struct failsafe_version_info *info)
{
	(void)data;
	(void)size;
	(void)fw;

	if (info) {
		info->found = false;
		info->version[0] = '\0';
		info->build_date[0] = '\0';
	}

	return -ENOENT;
}

int __weak failsafe_atf_version_info(struct failsafe_version_info *bl2,
				     struct failsafe_version_info *bl31)
{
	if (bl2) {
		bl2->found = false;
		bl2->version[0] = '\0';
		bl2->build_date[0] = '\0';
	}

	if (bl31) {
		bl31->found = false;
		bl31->version[0] = '\0';
		bl31->build_date[0] = '\0';
	}

	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/*  Network reinit notification                                        */
/* ------------------------------------------------------------------ */

/**
 * failsafe_notify_network_cmd_done() - signal that a network command finished
 *
 * Called by telnetd (and potentially web console in the future) after
 * run_command() executes a network command (tftp, ping, ...) whose inner
 * net_loop() calls eth_halt() on exit.
 *
 * The poll loop responds by calling eth_init() to bring ethernet back up
 * and re-registering the DHCP UDP handler.  These operations MUST happen
 * at the poll-loop level, OUTSIDE the eth_rx() → TCP callback chain, to
 * avoid corrupting the DMA receive-descriptor state.
 */
void failsafe_notify_network_cmd_done(void)
{
	eth_needs_reinit = true;
}

/* ------------------------------------------------------------------ */
/*  UI handlers (index, not_found, style, js, html)                    */
/* ------------------------------------------------------------------ */

static void not_found_handler(enum httpd_uri_handler_status status,
			      struct httpd_request *request,
			      struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		failsafe_output_file(response, "404.html", "text/html");
		response->info.code = 404;
	}
}

static void index_handler(enum httpd_uri_handler_status status,
			  struct httpd_request *request,
			  struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		if (failsafe_output_file(response, "index.html", "text/html"))
			not_found_handler(status, request, response);
	}
}

static void style_handler(enum httpd_uri_handler_status status,
			  struct httpd_request *request,
			  struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		if (failsafe_output_file(response, "style.css", "text/css")) {
			not_found_handler(status, request, response);
			return;
		}
	}
}

/*
 * A registered JS URI is the name of the embedded asset, so the file is
 * served straight from the request URI (the handler only runs for a URI
 * that was registered, see failsafe/pages.c and the modules).  A script
 * that was not embedded for this build - its option is off - is reported
 * as 404 instead of a plain-text error masquerading as gzip-encoded JS.
 */
void js_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status != HTTP_CB_NEW)
		return;

	if (failsafe_output_file(response, request->urih->uri + 1,
				 "text/javascript"))
		not_found_handler(status, request, response);
}

void html_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status != HTTP_CB_NEW)
		return;

	if (failsafe_output_file(response, request->urih->uri + 1, "text/html"))
		not_found_handler(status, request, response);
}

/* ------------------------------------------------------------------ */
/*  Main entry: start_web_failsafe()                                   */
/* ------------------------------------------------------------------ */

int start_web_failsafe(void)
{
	struct httpd_instance *inst;

	inst = httpd_find_instance(80);
	if (inst)
		httpd_free_instance(inst);

	inst = httpd_create_instance(80);
	if (!inst) {
		debug("Error: failed to create HTTP instance on port 80\n");
		return -1;
	}

	/* Register handlers from each module */

	/*
	 * The entry points and the resources that are not pages come from
	 * here; every page (its HTML resource, its script, the endpoints of
	 * its module and the /ui/pages list the Web UI asks for) comes from
	 * the page inventory in failsafe/pages.c.
	 */
	httpd_register_uri_handler(inst, "", &not_found_handler, NULL);
	httpd_register_uri_handler(inst, "/", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/cgi-bin/luci", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/cgi-bin/luci/", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/style.css", &style_handler, NULL);
#ifdef CONFIG_WEBUI_FAILSAFE_I18N
	httpd_register_uri_handler(inst, "/i18n.js", &js_handler, NULL);
#endif

	failsafe_register_pages(inst);

#ifdef CONFIG_MTK_TELNETD
	if (IS_ENABLED(CONFIG_MTK_TELNETD)) {
		const char *enable_str = env_get("telnetd_enable");
		bool enable = true;

		/* Explicitly disabled when telnetd_enable is "0" */
		if (enable_str && !strcmp(enable_str, "0"))
			enable = false;

		if (enable)
			mtk_telnetd_start(mtk_telnetd_env_port("telnet_port", 23));
	}
#endif

	{
		u32 ip = ntohl(net_ip.s_addr);

		cprintln(PROMPT, "\nWeb failsafe UI started");
		cprintln(PROMPT, "URL: http://%u.%u.%u.%u/",
			 (ip >> 24) & 0xff, (ip >> 16) & 0xff,
			 (ip >> 8) & 0xff, ip & 0xff);
		cprintln(PROMPT, "Press Ctrl+C to exit");
	}

	failsafe_httpd_running = true;
	mtk_tcp_done_flag = false;
	eth_needs_reinit = false;
	services_auto_started = false;
	auto_action_pending = false;
	reboot_pending = false;

	/*
	 * Optional LED indication for the idle web UI (env
	 * 'failsafe_led_ready', with the historical 'failsafe_led' as
	 * fallback).  The upgrade module switches to its own phases once an
	 * image is uploaded.
	 */
	failsafe_led_set_phase(FAILSAFE_LED_READY);

	/*
	 * Initialize network subsystem.  net_init() is safe to call
	 * multiple times (only the first call allocates packet buffers).
	 */
	int net_ret = net_init();

	/*
	 * Bring-up traces only: they cost rodata in every production
	 * image, so they are compiled out unless DEBUG is defined.
	 */
	debug("[FAILSAFE] net_init() returned %d\n", net_ret);
	if (eth_is_on_demand_init()) {
		eth_set_current();
		if (!eth_is_active(eth_get_dev())) {
			if (eth_init() < 0) {
				eth_halt();
				mdelay(300);
				if (eth_init() < 0) {
					debug("Error: failed to initialize ethernet\n");
					failsafe_httpd_running = false;
					return -1;
				}
			}
		}
	} else {
		eth_init_state_only();
	}
	debug("[FAILSAFE] eth initialized\n");

	/*
	 * This session owns the network now, so take the whole L3
	 * configuration back from whatever ran before it.  The netabort
	 * listener (which initializes the network before autoboot), a
	 * dhcp/tftp executed by the boot command, or the DHCP server that
	 * main_loop() auto-started may all have left net_ip/net_netmask
	 * pointing at a different subnet -- a board that picked up a LAN
	 * address during boot would then answer ARP for the wrong network
	 * and offer leases from a pool that does not match its own IP, so
	 * a directly connected PC could neither obtain an address nor
	 * reach the web UI.
	 */
	{
		const char *env_ip = env_get("ipaddr");
		const char *env_nm = env_get("netmask");

		net_ip = string_to_ip((env_ip && env_ip[0]) ?
				      env_ip : HTTPD_DEFAULT_IPADDR);
		net_netmask = string_to_ip((env_nm && env_nm[0]) ?
					    env_nm : HTTPD_DEFAULT_NETMASK);
		net_gateway = net_ip;
		net_dns_server = net_ip;
	}

	/*
	 * (Re)start the servers under this session's configuration.
	 *
	 * main_loop() may already have started the DHCP server behind our
	 * back.  Restarting it here guarantees the UDP handler is
	 * installed with this session as its owner and the lease table is
	 * fresh; mtk_dhcpd_start()/mtk_dnsd_start() skip all of that when
	 * they believe the server is already running.
	 */
#ifdef CONFIG_MTK_DHCPD
	mtk_dhcpd_stop();
	mtk_dhcpd_start();
	debug("[FAILSAFE] DHCP server started\n");
#endif
#ifdef CONFIG_MTK_DNSD
	mtk_dnsd_stop();
	mtk_dnsd_start();
	debug("[FAILSAFE] DNS server started\n");
#endif

	/* Reset the MTK TCP subsystem */
	mtk_tcp_start();
	debug("[FAILSAFE] mtk_tcp_start() done\n");

	/*
	 * Non-blocking poll loop.  We call eth_rx() and
	 * mtk_tcp_periodic_check() directly each iteration because the
	 * weak/strong schedule_hook() override does not reliably work
	 * across all link orders.  schedule() is still called for the
	 * cyclic framework, watchdog, and uthread scheduling.
	 *
	 * The loop exits when:
	 *   - Ctrl+C is pressed, or
	 *   - all TCP listeners and connections are gone (mtk_tcp_done_flag).
	 *   - an auto-action (initramfs boot / firmware flash) is pending.
	 *   - a /reboot request has been completed (reboot_pending).
	 *
	 * The reboot is deliberately deferred to this level as well: the
	 * handler only records the request from within the TCP callback,
	 * and do_httpd() runs do_reset() here, outside the eth_rx() →
	 * callback chain.
	 *
	 * When telnetd runs a network command (tftp, ping, …) the inner
	 * net_loop() calls eth_halt() on exit.  telnetd sets the
	 * eth_needs_reinit flag (via failsafe_notify_network_cmd_done)
	 * instead of calling eth_init() inline, because the inline call
	 * would be inside the outer eth_rx() → TCP callback chain and
	 * corrupt DMA receive descriptors.  We call eth_init() here at the
	 * poll-loop level, safely outside the callback chain, and also
	 * re-register the DHCP handler that net_clear_handlers() removed.
	 */
	debug("[FAILSAFE] entering poll loop, done_flag=%d\n", mtk_tcp_done_flag);
	while (!ctrlc() && !mtk_tcp_done_flag && !auto_action_pending &&
	       !reboot_pending && !boot_system_pending) {
#if defined(CONFIG_MTK_TELNETD)
		/*
		 * Run a queued telnet command at poll-loop level, OUTSIDE the
		 * eth_rx() → TCP callback chain.  A command that enters
		 * net_loop() (tftp, ping, ...) from inside that chain would
		 * nest eth_rx() over the same DMA RX ring and corrupt it,
		 * which shows up as a hard hang when the command is aborted.
		 *
		 * This is safe to call before eth_rx(): the command's own
		 * net_loop() (if any) pumps the ethernet itself while it runs.
		 */
		mtk_telnetd_poll();
#endif
		bool need_poll = failsafe_httpd_running;

#ifdef CONFIG_MTK_DHCPD
		need_poll = need_poll || mtk_dhcpd_is_running();
#endif
#ifdef CONFIG_MTK_DNSD
		need_poll = need_poll || mtk_dnsd_is_running();
#endif

		if (!services_auto_started && !failsafe_httpd_running) {
			services_auto_started = true;
#ifdef CONFIG_MTK_DHCPD
			if (!mtk_dhcpd_is_running()) {
				cprintln(NORMAL, "Starting DHCP server...");
				mtk_dhcpd_start();
				need_poll = true;
			}
#endif
#ifdef CONFIG_MTK_DNSD
			if (!mtk_dnsd_is_running()) {
				cprintln(NORMAL, "Starting DNS server...");
				mtk_dnsd_start();
				need_poll = true;
			}
#endif
		}

		if (need_poll) {
#if defined(CONFIG_MTK_TCP)
			/*
			 * Network-command recovery: when telnetd (or
			 * future web-console) executes a network command
			 * whose inner net_loop() calls eth_halt(), the
			 * eth_needs_reinit flag is set.
			 *
			 * We MUST call eth_init() here at the poll-loop
			 * level rather than inside the TCP callback chain,
			 * because the callback runs inside eth_rx() and
			 * calling eth_init() inline would corrupt the
			 * outer eth_rx()'s DMA descriptor iteration.
			 *
			 * net_loop() also calls net_clear_handlers() which
			 * removes the DHCP UDP handler — re-register it.
			 *
			 * Re-initialize not only when a network command asked
			 * for it, but also whenever the interface silently
			 * dropped out of the ACTIVE state.  eth_rx() does
			 * nothing on an interface it does not consider
			 * active -- it simply reports no packets -- so if
			 * anything else claimed or stopped the device
			 * behind our back (the netabort listener used to
			 * keep it started), the web UI, the DHCP server
			 * and the DNS server would all go deaf without a
			 * single error message.  Detect that here, at
			 * poll-loop level, and bring the interface back.
			 */
			if (eth_needs_reinit ||
			    (eth_get_dev() && !eth_is_active(eth_get_dev()))) {
				eth_needs_reinit = false;
				eth_init();
#ifdef CONFIG_MTK_DHCPD
				if (mtk_dhcpd_is_running())
					mtk_dhcpd_start();
#endif
#ifdef CONFIG_MTK_DNSD
				if (mtk_dnsd_is_running())
					mtk_dnsd_start();
#endif
			}

			eth_rx();
			if (mtk_tcp_periodic_check())
				mtk_tcp_done_flag = true;
#endif
		}

		/* keep the (optional) LED effect rotating */
		failsafe_led_poll();
		schedule();
	}

	/*
	 * Stop services in correct order: DNSD first, then DHCPD.
	 * DNSD chains on top of DHCPD handler, so DNSD must be
	 * unstacked before DHCPD to avoid a dangling handler pointer.
	 */
#ifdef CONFIG_MTK_DNSD
	mtk_dnsd_stop();
#endif
#ifdef CONFIG_MTK_DHCPD
	mtk_dhcpd_stop();
#endif
	failsafe_httpd_running = false;
	mtk_tcp_close_all_conn();
	eth_halt();

	/* release the LEDs before the caller boots / resets */
	failsafe_led_off();

	return 0;
}

static int do_httpd(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	int ret;

#ifdef CONFIG_NET_FORCE_IPADDR
	{
		const char *env_ip = env_get("ipaddr");
		const char *env_nm = env_get("netmask");

		net_ip = string_to_ip((env_ip && env_ip[0]) ? env_ip : CONFIG_IPADDR);
		net_netmask = string_to_ip((env_nm && env_nm[0]) ? env_nm : CONFIG_NETMASK);
	}
#endif

	ret = start_web_failsafe();

	if (auto_action_pending) {
		if (fw_type == FW_TYPE_INITRD)
			boot_from_mem((ulong)upload_data);
		else
			do_reset(NULL, 0, 0, NULL);
	} else if (reboot_pending) {
		/*
		 * /reboot was answered and its connection is gone; the
		 * network has been halted by start_web_failsafe(), so it is
		 * safe to reset now.
		 */
		debug("NOTICE: Rebooting now...\n");
		do_reset(NULL, 0, 0, NULL);
	} else if (boot_system_pending) {
		/*
		 * /boot was answered and its connection is gone; the network
		 * has been halted by start_web_failsafe(), so it is safe to
		 * run the per-board 'bootcmd' (which boots the installed
		 * firmware) now, outside the TCP callback chain.
		 */
		debug("NOTICE: Booting system (run bootcmd)...\n");
		run_command("run bootcmd", 0);
	}

	return ret;
}

U_BOOT_CMD(httpd, 1, 0, do_httpd,
	"Start failsafe HTTP server", ""
);

#if IS_ENABLED(CONFIG_WEB_FAILSAFE_AFTER_BOOT_FAILURE)
static int do_bootfailsafe(struct cmd_tbl *cmdtp, int flag, int argc,
			   char *const argv[])
{
	cprintln(CAUTION, "Boot failed, starting web failsafe");

	return do_httpd(cmdtp, flag, argc, argv);
}

U_BOOT_CMD(bootfailsafe, 1, 0, do_bootfailsafe,
	"Start the web failsafe after a boot failure", ""
);
#endif /* CONFIG_WEB_FAILSAFE_AFTER_BOOT_FAILURE */
