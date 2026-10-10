// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Last error of a failsafe operation - see <failsafe/error.h>.
 */

#include <errno.h>
#include <stdio.h>
#include <vsprintf.h>
#include <failsafe/error.h>
#include <cprint.h>

static char failsafe_err_msg[FAILSAFE_ERROR_MSG_SIZE];
static int failsafe_err_code;

int failsafe_error(int code, const char *fmt, ...)
{
	va_list args;

	/* A failure always has a code: callers passing 0 mean "make one up",
	 * and -EIO is the least specific one that still says "it broke".
	 */
	if (!code)
		code = -EIO;

	va_start(args, fmt);
	vsnprintf(failsafe_err_msg, sizeof(failsafe_err_msg), fmt, args);
	va_end(args);

	failsafe_err_code = code;

	cprintln(ERROR, "Failsafe: %s (code %d)", failsafe_err_msg, code);

	return code;
}

void failsafe_error_reset(void)
{
	failsafe_err_msg[0] = '\0';
	failsafe_err_code = 0;
}

int failsafe_error_code(void)
{
	return failsafe_err_code;
}

const char *failsafe_error_msg(void)
{
	return failsafe_err_msg;
}
