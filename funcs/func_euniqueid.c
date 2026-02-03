/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2026, Naveen Albert
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
 * \brief Ephemeral Unique ID
 *
 * \author Naveen Albert <asterisk@phreaknet.org>
 * \ingroup functions
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/channel.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"

/*** DOCUMENTATION
	<function name="EPHEMERAL_UNIQUEID" language="en_US">
		<synopsis>
			Returns a unique numeric identifier for a call in queue
		</synopsis>
		<syntax/>
		<description>
			<para>Returns an ephemerally unique ID for the channel across the current set of channels.</para>
			<para>The first time this function is called, it will return 0.</para>
			<para>On subsequent calls, it will return the next available ID after the highest numbered
			ID that is still in use.</para>
			<para>For example, the second call would return 1, the third would return 2, etc.
			If 2 were to hang up, then 2 would be reallocated on the next call.</para>
			<para>This provides an alternative the <literal>UNIQUEID</literal> function, when
			purely numeric (and short and small) IDs are required that only need to be unique
			as long as the channel exists.</para>
			<note><para>Multiple calls from the same channel will allocate multiple IDs.
			Save the result to a variable to avoid this.</para></note>
		</description>
	</function>
 ***/

struct uniqueid {
	int id;
	time_t allocated;
	const char *channel;
	AST_RWLIST_ENTRY(uniqueid) entry;
	char data[];
};

static AST_RWLIST_HEAD_STATIC(uniqueids, uniqueid);

static int euniqueid_read(struct ast_channel *chan, const char *function, char *data, char *buf, size_t maxlen)
{
	int id;
	struct uniqueid *u;
	int highest_id = -1;

	if (!chan) {
		ast_log(LOG_ERROR, "%s requires a channel\n", function);
		snprintf(buf, maxlen, "%d", -1);
		return -1;
	}

	AST_RWLIST_WRLOCK(&uniqueids);
	/* First, purge any IDs that were requested by channels that no longer exist */
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&uniqueids, u, entry) {
		struct ast_channel *ochan = ast_channel_get_by_name(u->channel);
		if (ochan) {
			highest_id = u->id;
			ast_channel_unref(ochan);
		} else {
			/* The channel that requested this unique ID doesn't exist anymore. No longer needed */
			AST_RWLIST_REMOVE_CURRENT(entry);
			ast_free(u);
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	/* This is the ID we will use */
	if (highest_id != -1) {
		id = highest_id + 1;
	} else {
		id = 0;
	}
	u = ast_calloc(1, sizeof(*u) + strlen(ast_channel_name(chan)) + 1);
	if (!u) {
		AST_RWLIST_UNLOCK(&uniqueids);
		snprintf(buf, maxlen, "%d", -1);
		return -1;
	}
	strcpy(u->data, ast_channel_name(chan)); /* Safe */
	u->channel = u->data;
	u->id = id;
	u->allocated = time(NULL);
	AST_RWLIST_INSERT_TAIL(&uniqueids, u, entry);
	AST_RWLIST_UNLOCK(&uniqueids);

	ast_verb(5, "%s has ephemeral unique ID %d\n", ast_channel_name(chan), id);

	snprintf(buf, maxlen, "%d", id);
	return 0;
}

static char *handle_show_channels(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct uniqueid *u;
	time_t now = time(NULL);

	switch (cmd) {
	case CLI_INIT:
		e->command = "euniqueid show channels";
		e->usage =
			"Usage: euniqueid show channels\n"
			"       Show all currently allocated ephemeral unique IDs.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}

	ast_cli(a->fd, "%4s %6s %s\n", "ID", "Age", "Channel");
	AST_RWLIST_WRLOCK(&uniqueids);
	/* Purge any stale items */
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&uniqueids, u, entry) {
		struct ast_channel *ochan = ast_channel_get_by_name(u->channel);
		if (ochan) {
			ast_channel_unref(ochan);
		} else {
			AST_RWLIST_REMOVE_CURRENT(entry);
			ast_free(u);
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_TRAVERSE(&uniqueids, u, entry) {
		int diff = (int) (now - u->allocated);
		ast_cli(a->fd, "%4d %6d %s\n", u->id, diff, u->channel);
	}
	AST_RWLIST_UNLOCK(&uniqueids);

	return CLI_SUCCESS;
}

static struct ast_cli_entry euniqueid_cli[] = {
	AST_CLI_DEFINE(handle_show_channels, "List ephemeral IDs"),
};

static struct ast_custom_function tech_exists_function = {
	.name = "EPHEMERAL_UNIQUEID",
	.read = euniqueid_read,
};

static int unload_module(void)
{
	struct uniqueid *u;
	ast_custom_function_unregister(&tech_exists_function);
	ast_cli_unregister_multiple(euniqueid_cli, ARRAY_LEN(euniqueid_cli));
	AST_RWLIST_WRLOCK(&uniqueids);
	while ((u = AST_RWLIST_REMOVE_HEAD(&uniqueids, entry))) {
		ast_free(u);
	}
	AST_RWLIST_UNLOCK(&uniqueids);
	return 0;
}

static int load_module(void)
{
	ast_cli_register_multiple(euniqueid_cli, ARRAY_LEN(euniqueid_cli));
	return ast_custom_function_register(&tech_exists_function);
}

AST_MODULE_INFO_STANDARD_EXTENDED(ASTERISK_GPL_KEY, "Ephemeral Unique IDs");
