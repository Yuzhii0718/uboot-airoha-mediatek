// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 *
 * Failsafe Web UI - LED status indication
 *
 * The failsafe subsystem owns every LED touched around the recovery
 * workflow: waiting for the button (btnchk), the idle web UI, an
 * upgrade in progress and its result.  Boards describe their
 * indication purely from the environment, so neither board code nor
 * device tree knowledge is needed here:
 *
 *   failsafe_led_idle     waiting for a key press (btnchk)
 *   failsafe_led_ready    web failsafe UI running and idle
 *   failsafe_led_upgrade  an image was received and is being validated
 *                         and/or written to storage
 *   failsafe_led_success  upgrade finished successfully
 *   failsafe_led_fail     upgrade (or its validation) failed
 *
 * The two historical single-LED variables stay supported as fallbacks
 * (failsafe_led_idle <- btnchk_led, failsafe_led_ready <- failsafe_led)
 * so existing defenvs keep working unchanged.
 *
 * Effect syntax:
 *
 *   <effect> := <frame> [ ';' <frame> ]...
 *   <frame>  := 'off' | <spec> [ ',' <spec> ]...
 *   <spec>   := <led-label> [ <state> [ <period-ms> ] ]
 *   <state>  := on | off | toggle | blink
 *
 * A frame is applied as a whole and every LED referenced anywhere in
 * the effect is switched off first, so only the LEDs listed in the
 * current frame stay on.  That makes rotating effects a one-liner:
 *
 *   failsafe_led_upgrade = red:wan on;green:lan on;blue:status on
 *
 * blinks red, green and blue in turn, while
 *
 *   failsafe_led_ready   = green:power on
 *
 * just switches a single LED on.  A frame written as 'off' (or 'none',
 * or '-') blanks all referenced LEDs, which is handy for flashes:
 *
 *   failsafe_led_success = green:power on;off
 *
 * Frames are cycled every 'failsafe_led_period' milliseconds (default
 * 250, which is also the default 'blink' period).  A '<state>' that was
 * omitted defaults to 'on' - except for the idle phase, where it
 * defaults to 'blink' to match the historical btnchk LED.  LED labels
 * are resolved through the LED uclass, so labels that do not exist on
 * the board are silently ignored and one environment works on several
 * boards.  An unset or empty variable means "no LED activity for this
 * phase"; nothing is blinking unless it was asked for.
 */

#include <cyclic.h>
#include <dm.h>
#include <env.h>
#include <led.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <time.h>
#include <vsprintf.h>

#include <failsafe/led.h>
#include <cprint.h>

/*
 * This file is only built when CONFIG_WEBUI_FAILSAFE_LED is enabled, which
 * in turn depends on CONFIG_LED (see failsafe/Makefile / failsafe/Kconfig).
 * Without it the inline stubs from <failsafe/led.h> are used instead, so
 * every caller stays free of #ifdef.
 */

#define LED_LABEL_MAX		32
#define LED_MAX			8
#define FRAME_MAX		8
#define SPEC_MAX		8
#define EFFECT_MAX		256

#define PERIOD_DEFAULT_MS	250
#define PERIOD_MIN_MS		20
#define PERIOD_MAX_MS		60000

/* granularity of the background tick (cyclic framework) */
#define CYCLIC_DELAY_US		(50 * 1000)

#define CYCLIC_NAME		"failsafe_led"

struct failsafe_led_dev {
	char label[LED_LABEL_MAX];
	struct udevice *dev;
	bool resolved;
	u8 applied;		/* enum led_state_t, LEDST_COUNT = unknown */
	int applied_period;
};

struct failsafe_led_spec {
	u8 led;			/* index into fsled.leds[] */
	u8 state;		/* enum led_state_t */
	u16 period;		/* blink period, 0 = default */
};

struct failsafe_led_frame {
	struct failsafe_led_spec specs[SPEC_MAX];
	u8 count;
	bool all_off;
};

static struct {
	bool active;
	enum failsafe_led_phase phase;
	int period_ms;
	u8 n_leds;		/* labels referenced by the current effect */
	u8 n_present;		/* of those, the ones present on the board */
	u8 warned;		/* phases already reported as "no LED" */
	u8 n_frames;
	u8 frame;
	ulong next_tick;
	bool cyclic_done;
	struct cyclic_info cyclic;
	struct failsafe_led_dev leds[LED_MAX];
	struct failsafe_led_frame frames[FRAME_MAX];
} fsled;

static const char * const fsled_env[FAILSAFE_LED_COUNT] = {
	[FAILSAFE_LED_IDLE]	= "failsafe_led_idle",
	[FAILSAFE_LED_READY]	= "failsafe_led_ready",
	[FAILSAFE_LED_UPGRADE]	= "failsafe_led_upgrade",
	[FAILSAFE_LED_SUCCESS]	= "failsafe_led_success",
	[FAILSAFE_LED_FAIL]	= "failsafe_led_fail",
};

/*
 * Historical variables: they only ever described one LED, so they are
 * honoured when the phase-specific variable is not set.
 */
static const char * const fsled_env_legacy[FAILSAFE_LED_COUNT] = {
	[FAILSAFE_LED_IDLE]	= "btnchk_led",
	[FAILSAFE_LED_READY]	= "failsafe_led",
};

/*
 * State used for a LED spec that carries no explicit state.  The idle
 * phase blinks by default, exactly like the historical btnchk LED.
 */
static enum led_state_t fsled_default_state(enum failsafe_led_phase phase)
{
	return phase == FAILSAFE_LED_IDLE ? LEDST_BLINK : LEDST_ON;
}

static bool fsled_is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int fsled_led_index(const char *label)
{
	struct failsafe_led_dev *ld;
	int i;

	for (i = 0; i < fsled.n_leds; i++) {
		if (!strcmp(fsled.leds[i].label, label))
			return i;
	}

	if (fsled.n_leds >= LED_MAX)
		return -1;

	/* the slot may still hold the state of a previous phase */
	ld = &fsled.leds[fsled.n_leds];
	ld->dev = NULL;
	ld->resolved = false;
	ld->applied = LEDST_COUNT;
	ld->applied_period = 0;
	strlcpy(ld->label, label, LED_LABEL_MAX);

	return fsled.n_leds++;
}

/*
 * Split a LED spec into at most @max whitespace separated tokens.
 * Everything behind the last token is ignored.
 */
static int fsled_split(char *s, char *tok[], int max)
{
	char *p = s;
	int n = 0;

	while (n < max && *p) {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (!*p)
			break;

		tok[n++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}

	return n;
}

static void fsled_parse_spec(char *text, enum failsafe_led_phase phase,
			     struct failsafe_led_frame *fr)
{
	enum led_state_t state = fsled_default_state(phase);
	ulong period = 0;
	char *tok[3];
	int n, idx;

	n = fsled_split(text, tok, ARRAY_SIZE(tok));
	if (n < 1)
		return;

	if (n > 1) {
		if (!strcmp(tok[1], "on")) {
			state = LEDST_ON;
		} else if (!strcmp(tok[1], "off")) {
			state = LEDST_OFF;
		} else if (!strcmp(tok[1], "toggle")) {
			state = LEDST_TOGGLE;
		} else if (!strcmp(tok[1], "blink")) {
			state = LEDST_BLINK;
		} else if (fsled_is_digit(tok[1][0])) {
			/* '<label> <ms>' is shorthand for a blink */
			state = LEDST_BLINK;
			period = simple_strtoul(tok[1], NULL, 10);
		} else {
			/* unknown state: ignore this LED completely */
			return;
		}
	}

	if (n > 2 && fsled_is_digit(tok[2][0]))
		period = simple_strtoul(tok[2], NULL, 10);

	if (state != LEDST_BLINK)
		period = 0;
	else if (period > 0xffff)
		period = 0xffff;

	if (fr->count >= SPEC_MAX)
		return;

	idx = fsled_led_index(tok[0]);
	if (idx < 0)
		return;

	fr->specs[fr->count].led = idx;
	fr->specs[fr->count].state = state;
	fr->specs[fr->count].period = period;
	fr->count++;
}

static void fsled_parse_frame(char *text, enum failsafe_led_phase phase)
{
	struct failsafe_led_frame *fr;
	char *p, *comma;

	while (*text == ' ' || *text == '\t')
		text++;

	p = text + strlen(text);
	while (p > text && (p[-1] == ' ' || p[-1] == '\t'))
		*--p = '\0';

	/* empty frame ('a on;;b on') is skipped */
	if (!*text || fsled.n_frames >= FRAME_MAX)
		return;

	fr = &fsled.frames[fsled.n_frames];

	if (!strcmp(text, "off") || !strcmp(text, "none") ||
	    !strcmp(text, "-")) {
		fr->all_off = true;
		fsled.n_frames++;
		return;
	}

	p = text;
	while (p && *p) {
		comma = strchr(p, ',');
		if (comma)
			*comma = '\0';

		fsled_parse_spec(p, phase, fr);

		p = comma ? comma + 1 : NULL;
	}

	if (fr->count)
		fsled.n_frames++;
}

static void fsled_parse_period(void)
{
	const char *val = env_get("failsafe_led_period");
	ulong ms = (val && val[0]) ? simple_strtoul(val, NULL, 10) : 0;

	if (!ms)
		ms = PERIOD_DEFAULT_MS;
	else if (ms < PERIOD_MIN_MS)
		ms = PERIOD_MIN_MS;
	else if (ms > PERIOD_MAX_MS)
		ms = PERIOD_MAX_MS;

	fsled.period_ms = (int)ms;
}

static void fsled_parse(enum failsafe_led_phase phase)
{
	const char *val;
	char buf[EFFECT_MAX];
	char *p, *semi;

	val = env_get(fsled_env[phase]);
	if ((!val || !val[0]) && fsled_env_legacy[phase])
		val = env_get(fsled_env_legacy[phase]);

	if (!val || !val[0])
		return;

	strlcpy(buf, val, sizeof(buf));

	p = buf;
	while (p && *p && fsled.n_frames < FRAME_MAX) {
		semi = strchr(p, ';');
		if (semi)
			*semi = '\0';

		fsled_parse_frame(p, phase);

		p = semi ? semi + 1 : NULL;
	}
}

/*
 * Program one LED to the requested state.  Re-programming a LED that is
 * already in the requested state is skipped: a software blink is
 * restarted (and loses its phase) by each redundant
 * led_set_state(LEDST_BLINK) / led_set_period() call.
 *
 * Returns true when the LED is in (or already was in) the requested
 * state, false when it is not usable at all - a driver that rejects the
 * operation is treated just like a LED that is not present.
 */
static bool fsled_program(struct failsafe_led_dev *ld,
			  enum led_state_t state, int period)
{
	if (!ld->dev)
		return false;

	if (state == LEDST_BLINK) {
		if (ld->applied == LEDST_BLINK &&
		    ld->applied_period == period)
			return true;

		if (led_set_period(ld->dev, period)) {
			/* no blink support: fall back to steady on */
			if (ld->applied == LEDST_ON)
				return true;

			if (led_set_state(ld->dev, LEDST_ON))
				return false;

			ld->applied = LEDST_ON;
			return true;
		}

		if (led_set_state(ld->dev, LEDST_BLINK))
			return false;

		ld->applied = LEDST_BLINK;
		ld->applied_period = period;
		return true;
	}

	if (ld->applied == state)
		return true;

	if (led_set_state(ld->dev, state))
		return false;

	ld->applied = state;
	ld->applied_period = 0;
	return true;
}

static void fsled_apply(void)
{
	enum led_state_t want[LED_MAX];
	int period[LED_MAX];
	struct failsafe_led_frame *fr = &fsled.frames[fsled.frame];
	int i, j;

	/* LEDs not mentioned by the current frame are switched off */
	for (i = 0; i < fsled.n_leds; i++) {
		want[i] = LEDST_OFF;
		period[i] = 0;
	}

	if (!fr->all_off) {
		for (j = 0; j < fr->count; j++) {
			const struct failsafe_led_spec *spec = &fr->specs[j];

			want[spec->led] = spec->state;
			period[spec->led] = spec->period ?
				spec->period : fsled.period_ms;
		}
	}

	fsled.n_present = 0;

	for (i = 0; i < fsled.n_leds; i++) {
		struct failsafe_led_dev *ld = &fsled.leds[i];

		if (!ld->resolved) {
			ld->resolved = true;
			if (led_get_by_label(ld->label, &ld->dev))
				ld->dev = NULL;
		}

		/* labels that do not exist on this board are skipped */
		if (!ld->dev)
			continue;

		if (fsled_program(ld, want[i], period[i]))
			fsled.n_present++;
	}
}

static void fsled_cyclic_cb(struct cyclic_info *c)
{
	(void)c;

	failsafe_led_poll();
}

static void fsled_cyclic_register(void)
{
	if (fsled.cyclic_done)
		return;

	fsled.cyclic_done = true;

	/*
	 * A background tick keeps rotating effects alive inside long
	 * blocking operations (flash erase / write), whose wait loops end
	 * up in udelay() -> schedule() -> cyclic_run().  No-op without
	 * CONFIG_CYCLIC.
	 */
	cyclic_register(&fsled.cyclic, fsled_cyclic_cb, CYCLIC_DELAY_US,
			CYCLIC_NAME);
}

void failsafe_led_set_phase(enum failsafe_led_phase phase)
{
	if (phase >= FAILSAFE_LED_COUNT)
		return;

	/* already showing this phase */
	if (fsled.active && fsled.phase == phase)
		return;

	failsafe_led_off();

	fsled.phase = phase;
	fsled_parse_period();
	fsled_parse(phase);

	/* no effect configured for this phase: LEDs stay off */
	if (!fsled.n_frames)
		return;

	fsled.active = true;
	fsled.frame = 0;
	fsled.next_tick = get_timer(0) + fsled.period_ms;

	fsled_apply();

	/*
	 * Not a single label of this effect exists on the board (or the LED
	 * subsystem is not ready): stay idle instead of running an effect
	 * that can never light anything up.  Tell the user once per phase so
	 * a wrong label is not silently ignored.
	 */
	if (!fsled.n_present) {
		if (!(fsled.warned & (1 << phase))) {
			fsled.warned |= 1 << phase;
			cprintln(CAUTION,
				 "failsafe: no LED of '%s' exists on this "
				 "board", fsled_env[phase]);
		}

		fsled.active = false;
		return;
	}

	fsled_cyclic_register();
}

void failsafe_led_poll(void)
{
	if (!fsled.active || !fsled.n_frames)
		return;

	if (!time_after_eq(get_timer(0), fsled.next_tick))
		return;

	fsled.next_tick = get_timer(0) + fsled.period_ms;

	fsled.frame++;
	if (fsled.frame >= fsled.n_frames)
		fsled.frame = 0;

	fsled_apply();
}

void failsafe_led_off(void)
{
	int i;

	for (i = 0; i < fsled.n_leds; i++) {
		struct failsafe_led_dev *ld = &fsled.leds[i];

		if (!ld->dev || ld->applied == LEDST_OFF)
			continue;

		led_set_state(ld->dev, LEDST_OFF);
		ld->applied = LEDST_OFF;
	}

	if (fsled.cyclic_done) {
		cyclic_unregister(&fsled.cyclic);
		fsled.cyclic_done = false;
	}

	fsled.active = false;
	fsled.n_leds = 0;
	fsled.n_frames = 0;
	fsled.frame = 0;
}
