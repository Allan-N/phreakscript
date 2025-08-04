/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2022, Naveen Albert
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Off-Hook (Call Waiting) Caller ID Application
 *
 * \author Naveen Albert <asterisk@phreaknet.org>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include "asterisk/file.h"
#include "asterisk/ulaw.h"
#include "asterisk/alaw.h"
#include "asterisk/pbx.h"
#include "asterisk/channel.h"
#include "asterisk/app.h"
#include "asterisk/module.h"
#include "asterisk/frame.h"
#include "asterisk/callerid.h"
#include "asterisk/adsi.h"
#include "asterisk/format_cache.h"

#ifdef HAVE_DAHDI
/* Don't even include chan_dahdi header files unless we have DAHDI */
#include "../channels/sig_analog.h" /* use READ_SIZE */
#include "../channels/chan_dahdi.h"
/* XXX from chan_dahdi.c */
#define AST_LAW(p) (((p)->law == DAHDI_LAW_ALAW) ? ast_format_alaw : ast_format_ulaw)
#else
#define READ_SIZE 160
#endif

/*** DOCUMENTATION
	<application name="SendCWCID" language="en_US">
		<synopsis>
			Sends an in-band Call Waiting Caller ID.
		</synopsis>
		<syntax>
			<parameter name="number">
				<para>Caller ID number. 15 characters maximum.</para>
				<para>Defaults to CALLERID(num).</para>
			</parameter>
			<parameter name="name">
				<para>Caller ID name. 15 characters maximum.</para>
				<para>Defaults to CALLERID(name).</para>
			</parameter>
			<parameter name="presentation">
				<para>Caller ID presentation.</para>
				<para>Defaults to CALLERID(pres).</para>
			</parameter>
			<parameter name="redirecting">
				<para>Redirecting reason, e.g. REDIRECTING(reason).</para>
				<para>Default is none (not sent).</para>
			</parameter>
			<parameter name="timezone">
				<para>TZ-format timezone to use for date/time.</para>
				<para>Default is the system time zone.</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="c">
						<para>Do not generate a CPE Alerting Signal (CAS).</para>
					</option>
					<option name="d">
						<para>Do not require a DTMF acknowledgment from the CPE.</para>
					</option>
					<option name="l">
						<para>Send long distance call qualifier.</para>
					</option>
					<option name="n">
						<para>Use native DAHDI spill method. This may provide more
						ideal audio when possible.</para>
						<para>This can only be used with FXS channels with DAHDI. Otherwise, it will
						fall back to using the channel tech-agnostic method.</para>
						<para>This module must be compiled with DAHDI present to enable
						this option.</para>
					</option>
					<option name="s">
						<para>Do not generate a Subscriber Alerting Signal (SAS).</para>
						<para>Note that if the c option is used, this option is implicitly true.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Generates an in-band Call Waiting Caller ID. This can be used if you are
			handling Call Waiting in the dialplan as opposed to using the channel driver.
			This application functions using the same idea as an "orange box".</para>
			<para>On DAHDI channels, the native channel driver hooks for Call Waiting Caller ID
			can be used. On all other channels, it will be generated as linear audio.</para>
			<variablelist>
				<variable name="CWCIDSTATUS">
					<value name="SUCCESS">
						Call Waiting Caller ID successfully sent to CPE.
					</value>
					<value name="UNSUPPORTED">
						CPE does not support off-hook Caller ID. This is not set if the d option is used.
					</value>
					<value name="FAILURE">
						General failure occured.
					</value>
					<value name="HANGUP">
						Channel hung up before spill could complete.
					</value>
				</variable>
			</variablelist>
		</description>
	</application>
 ***/

enum cwcid_option_flags {
	OPT_NO_CAS = (1 << 0),
	OPT_NO_SAS = (1 << 1),
	OPT_NO_ACK = (1 << 2),
	OPT_NATIVE = (1 << 3),
	OPT_QUALIFIER = (1 << 4),
};

AST_APP_OPTIONS(cwcid_option_flags, {
	AST_APP_OPTION('c', OPT_NO_CAS),
	AST_APP_OPTION('d', OPT_NO_ACK),
	AST_APP_OPTION('l', OPT_QUALIFIER),
	AST_APP_OPTION('n', OPT_NATIVE),
	AST_APP_OPTION('s', OPT_NO_SAS),
});

static const char *app = "SendCWCID";
static const char *var = "CWCIDSTATUS";

static int await_ack(struct ast_channel *chan, int ms)
{
	int res = ast_waitfordigit(chan, ms);
	if (res == 'A') {
		ast_verb(4, "CPE is Type III (ADSI) capable\n");
	} else if (res == 'D') {
		ast_verb(4, "CPE is Type II (non-ADSI) capable\n");
	} else if (res > 0 && res != 'A' && res != 'D') {
		ast_log(LOG_WARNING, "Unexpected acknowledgment from CPE: '%c'\n", res);
	} else {
		ast_verb(4, "CPE is not off-hook Caller ID capable\n");
	}
	return res;
}

/* XXX copy of adsi_careful_send from res_adsi. Should be a core function in callerid.c? */
static int cwcid_careful_send(struct ast_channel *chan, unsigned char *buf, int len, int *remain)
{
	/* Sends carefully on a full duplex channel by using reading for timing */
	struct ast_frame *inf;
	struct ast_frame outf = {
		.frametype = AST_FRAME_VOICE,
		.subclass.format = ast_format_ulaw,
	};
	int amt;

	if (!len) {
		ast_log(LOG_WARNING, "Send buffer is empty, returning\n");
		return -1;
	}
	ast_debug(2, "Carefully sending %d bytes\n", len);

	if (remain && *remain) {
		amt = len;

		/* Send remainder if provided */
		if (amt > *remain) {
			amt = *remain;
		} else {
			*remain = *remain - amt;
		}

		outf.data.ptr = buf;
		outf.datalen = amt;
		outf.samples = amt;
		if (ast_write(chan, &outf)) {
			ast_log(LOG_WARNING, "Failed to carefully write frame\n");
			return -1;
		}
		/* Update pointers and lengths */
		buf += amt;
		len -= amt;
	}

	while (len) {
		amt = len;
		/* If we don't get anything at all back in a second, forget
		   about it */
		if (ast_waitfor(chan, 1000) < 1) {
			return -1;
		}
		/* Detect hangup */
		if (!(inf = ast_read(chan))) {
			return -1;
		}

		/* Drop any frames that are not voice */
		if (inf->frametype != AST_FRAME_VOICE) {
			char frametype[32];
			ast_frame_type2str(inf->frametype, frametype, sizeof(frametype));
			ast_frfree(inf);
			ast_debug(1, "Skipping %s\n", frametype);
			continue;
		}

		if (ast_format_cmp(inf->subclass.format, ast_format_ulaw) != AST_FORMAT_CMP_EQUAL) {
			ast_log(LOG_WARNING, "Channel not in ulaw? (in %s)\n", ast_format_get_name(inf->subclass.format));
			ast_frfree(inf);
			return -1;
		}
		/* Send no more than they sent us */
		if (amt > inf->datalen) {
			amt = inf->datalen;
		} else if (remain) {
			*remain = inf->datalen - amt;
		}
		outf.data.ptr = buf;
		outf.datalen = amt;
		outf.samples = amt;
		if (ast_write(chan, &outf)) {
			ast_log(LOG_WARNING, "Failed to carefully write frame\n");
			ast_frfree(inf);
			return -1;
		}
		/* Update pointers and lengths */
		buf += amt;
		len -= amt;
		ast_frfree(inf);
		ast_debug(2, "%d bytes remaining\n", len);
	}
	return 0;
}

static int cwcid_exec(struct ast_channel *chan, const char *data)
{
	char *argcopy = NULL;
	struct ast_flags flags = {0};
	int res = 0;
	int cas = 1, sas = 1, ack = 1;
	char clid[16], cnam[16];
	char *clidnum, *clidname;
	int presentation = -1, redirecting = -1, qualifier = 0;
	int dahdi = 0; /* whether to try to use native DAHDI */
	const char *tz;
#ifdef HAVE_DAHDI
	struct dahdi_pvt *pvt = NULL;
#endif

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(number);
		AST_APP_ARG(name);
		AST_APP_ARG(presentation);
		AST_APP_ARG(redirecting);
		AST_APP_ARG(timezone);
		AST_APP_ARG(options);
	);

	argcopy = ast_strdupa(S_OR(data, ""));
	AST_STANDARD_APP_ARGS(args, argcopy);

	if (!ast_strlen_zero(args.options)) {
		ast_app_parse_options(cwcid_option_flags, &flags, NULL, args.options);
		if (ast_test_flag(&flags, OPT_NO_CAS)) {
			cas = 0;
		}
		if (ast_test_flag(&flags, OPT_NO_SAS)) {
			sas = 1;
		}
		if (ast_test_flag(&flags, OPT_NO_ACK)) {
			ack = 0;
		}
		if (ast_test_flag(&flags, OPT_NATIVE)) {
			dahdi = 1;
		}
		if (ast_test_flag(&flags, OPT_QUALIFIER)) {
			qualifier = 1;
		}
	}

	tz = args.timezone;
	clidnum = !ast_strlen_zero(args.number) ? args.number : ast_channel_caller(chan)->id.number.valid ? ast_channel_caller(chan)->id.number.str : "";
	if (strlen(clidnum) > 15) {
		ast_log(LOG_WARNING, "Caller ID number '%s' is greater than 15 characters and will be truncated\n", clidnum);
	} else if (ast_strlen_zero(clidnum)) {
		ast_log(LOG_WARNING, "Caller ID number is empty\n");
	}
	ast_copy_string(clid, clidnum, sizeof(clid));

	clidname = !ast_strlen_zero(args.name) ? args.name : ast_channel_caller(chan)->id.name.valid ? ast_channel_caller(chan)->id.name.str : "";
	if (strlen(clidname) > 15) {
		ast_log(LOG_WARNING, "Caller ID name '%s' is greater than 15 characters and will be truncated\n", clidname);
	} else if (ast_strlen_zero(clidname)) {
		ast_log(LOG_WARNING, "Caller ID name is empty\n");
	}
	ast_copy_string(cnam, clidname, sizeof(cnam));

	if (!ast_strlen_zero(args.presentation)) {
		presentation = ast_parse_caller_presentation(args.presentation);
		if (presentation < 0) {
			ast_log(LOG_WARNING, "Invalid presentation: '%s'\n", args.presentation);
		}
	}
	if (presentation < 0) {
		presentation = ast_party_id_presentation(&ast_channel_caller(chan)->id);
	}

	if (!ast_strlen_zero(args.redirecting)) {
		redirecting = ast_redirecting_reason_parse(args.redirecting);
		if (redirecting < 0) {
			ast_log(LOG_WARNING, "Invalid redirecting reason: '%s'\n", args.redirecting);
		}
	}

	if (dahdi) {
		dahdi = 0;
#ifdef HAVE_DAHDI
		do { /* check if native DAHDI analog can be used */
			struct dahdi_params dahdip;

			if (strcasecmp(ast_channel_tech(chan)->type, "DAHDI")) {
				ast_log(LOG_WARNING, "%s is not a DAHDI channel\n", ast_channel_name(chan));
				break;
			}

			memset(&dahdip, 0, sizeof(dahdip));
			res = ioctl(ast_channel_fd(chan, 0), DAHDI_GET_PARAMS, &dahdip);

			if (res) {
				ast_log(LOG_WARNING, "Unable to get parameters of %s: %s\n", ast_channel_name(chan), strerror(errno));
				break;
			}
			if (!(dahdip.sigtype & __DAHDI_SIG_FXO)) { /* FXO signaling for FXS stations! */
				ast_log(LOG_WARNING, "%s is not an FXS Channel\n", ast_channel_name(chan));
				break;
			}

			pvt = ast_channel_tech_pvt(chan);
			if (!pvt) {
				ast_log(LOG_WARNING, "No channel pivot?\n");
				break;
			}
			if (!dahdi_analog_lib_handles(pvt->sig, 0, 0)) {
				ast_log(LOG_WARNING, "Channel signalling is not analog");
				break;
			}

			dahdi = 1;
		} while (0);
#else
		ast_log(LOG_WARNING, "DAHDI required for native option but not present\n");
#endif
	}

	ast_stopstream(chan);
	ast_debug(1, "Writing spill on %s using %s spill method\n", ast_channel_name(chan), dahdi ? "DAHDI native" : "generic");

	if (cas) { /* send a CAS, and maybe a SAS... */
		if (dahdi) { /* if we can, use the native DAHDI code to dump the FSK spill */
#ifdef HAVE_DAHDI
			ast_channel_lock(chan);
			if (pvt->cidspill) {
				ast_channel_unlock(chan);
				ast_log(LOG_WARNING, "cidspill already exists??\n"); /* We're probably getting a legitimate call waiting at the same time we're trying to execute this. */
				pbx_builtin_setvar_helper(chan, var, "FAILURE");
				return 0;
			}
			if (!(pvt->cidspill = ast_malloc((sas ? 2400 + 680 : 680) + READ_SIZE * 4))) {
				ast_channel_unlock(chan);
				ast_log(LOG_WARNING, "Failed to malloc cidspill\n");
				pbx_builtin_setvar_helper(chan, var, "FAILURE");
				return -1;
			}
			ast_gen_cas(pvt->cidspill, sas, sas ? 2400 + 680 : 680, AST_LAW(pvt));
			pvt->callwaitcas = 1;
			pvt->cidlen = (sas ? 2400 + 680 : 680) + READ_SIZE * 4;
			pvt->cidpos = 0;
			ast_channel_unlock(chan);

			/* wait for CID spill in dahdi_read (as opposed to calling send_caller directly */
			if (ast_safe_sleep(chan, sas ? 300 + 85 : 85)) {
				ast_debug(1, "ast_safe_sleep returned -1\n");
				pbx_builtin_setvar_helper(chan, var, "HANGUP");
				return -1;
			}
			/* chan_dahdi will free pvt->cidspill */
#endif
		} else {
			unsigned char *cidspill;

			if (ast_set_write_format(chan, ast_format_ulaw)) {
				ast_log(LOG_WARNING, "Unable to set '%s' to signed linear format (write)\n", ast_channel_name(chan));
				pbx_builtin_setvar_helper(chan, var, "FAILURE");
				return -1;
			}
			if (ast_set_read_format(chan, ast_format_ulaw)) {
				ast_log(LOG_WARNING, "Unable to set read format to ULAW\n");
				pbx_builtin_setvar_helper(chan, var, "FAILURE");
				return -1;
			}

			if (!(cidspill = ast_malloc((sas ? 2400 + 680 : 680) + READ_SIZE * 4))) {
				ast_log(LOG_WARNING, "Failed to malloc cidspill\n");
				pbx_builtin_setvar_helper(chan, var, "FAILURE");
				return -1;
			}
			ast_gen_cas(cidspill, sas, sas ? 2400 + 680 : 680, ast_format_ulaw);

			if (cwcid_careful_send(chan, cidspill, sas ? 2400 + 680 : 680, NULL)) {
				ast_free(cidspill);
				ast_log(LOG_WARNING, "Failed to write cidspill\n");
				pbx_builtin_setvar_helper(chan, var, "FAILURE");
				return -1;
			}
			ast_free(cidspill);
		}
	}

	res = await_ack(chan, 500); /* wait up to 500ms for ACK */
	if (res == -1) {
		ast_debug(1, "await_ack returned -1\n");
		pbx_builtin_setvar_helper(chan, var, "HANGUP");
		return -1;
	}
	if (ack) { /* make sure we got the ACK, if we're supposed to check */
		if (res != 'A' && res != 'D') {
			pbx_builtin_setvar_helper(chan, var, "UNSUPPORTED");
			return 0; /* Didn't get ACK, abort. */
		}
	}
	res = 0;

	if (dahdi) { /* send the FSK spill */
#ifdef HAVE_DAHDI
		while (pvt->cidspill) { /* shouldn't happen */
			ast_debug(1, "Waiting for cidspill to finish\n");
			if (ast_safe_sleep(chan, 10)) { /* try not to busy wait */
				pbx_builtin_setvar_helper(chan, var, "HANGUP");
				return -1;
			}
		}

		ast_channel_lock(chan);
		if (!(pvt->cidspill = ast_malloc(MAX_CALLERID_SIZE))) {
			ast_channel_unlock(chan);
			ast_log(LOG_WARNING, "Failed to malloc cidspill\n");
			pbx_builtin_setvar_helper(chan, var, "FAILURE");
			return -1;
		}
		/* similar to my_send_callerid in chan_dahdi.c: */
		pvt->callwaitcas = 0;
		pvt->cidcwexpire = 0;
		pvt->cidlen = ast_callerid_callwaiting_full_tz_generate(pvt->cidspill,
			cnam,
			clid,
			NULL,
			redirecting,
			presentation,
			qualifier,
			AST_LAW(pvt),
			tz);
		pvt->cidlen += READ_SIZE * 4;
		pvt->cidpos = 0;
		pvt->cid_suppress_expire = 0;
		ast_channel_unlock(chan);

		/* wait for CID spill in dahdi_read (as opposed to calling send_caller directly */
		if (ast_safe_sleep(chan, pvt->cidlen / 8)) {
			pbx_builtin_setvar_helper(chan, var, "HANGUP");
			return -1;
		}
		while (pvt->cidspill) { /* shouldn't happen */
			ast_debug(1, "Waiting for cidspill to finish\n");
			if (ast_safe_sleep(chan, 10)) { /* try not to busy wait */
				pbx_builtin_setvar_helper(chan, var, "HANGUP");
				return -1;
			}
		}
		/* chan_dahdi will free pvt->cidspill */
#endif
	} else {
		unsigned char *cidspill;
		int cidlen;

		if (!(cidspill = ast_malloc(MAX_CALLERID_SIZE))) {
			ast_log(LOG_WARNING, "Failed to malloc cidspill\n");
			pbx_builtin_setvar_helper(chan, var, "FAILURE");
			return -1;
		}
		cidlen = ast_callerid_callwaiting_full_tz_generate(cidspill,
			cnam,
			clid,
			NULL,
			redirecting,
			presentation,
			qualifier,
			ast_format_ulaw,
			tz);
		if (cwcid_careful_send(chan, cidspill, cidlen, NULL)) {
			ast_log(LOG_WARNING, "Failed to write cidspill\n");
			pbx_builtin_setvar_helper(chan, var, "FAILURE");
			res = -1;
		}
		ast_free(cidspill);
	}

	ast_debug(1, "res is %d\n", res);
	pbx_builtin_setvar_helper(chan, var, res ? "FAILURE" : "SUCCESS");

	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application_xml(app, cwcid_exec);
}

AST_MODULE_INFO_STANDARD_EXTENDED(ASTERISK_GPL_KEY, "Off-Hook Caller ID Application");
