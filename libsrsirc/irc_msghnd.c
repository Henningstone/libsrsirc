/* irc_ext.c - some more IRC functionality (see also irc.c)
 * libsrsirc - a lightweight serious IRC lib - (C) 2012-15, Timo Buhrmester
 * See README for contact-, COPYING for license information. */

#define LOG_MODULE MOD_IRC

#if HAVE_CONFIG_H
# include <config.h>
#endif


#include "irc_msghnd.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <platform/base_string.h>

#include <logger/intlog.h>

#include "common.h"
#include "conn.h"
#include "irc_track_int.h"
#include "msg.h"

#include <libsrsirc/defs.h>
#include <libsrsirc/util.h>


static uint8_t
handle_001(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	lsi_ut_freearr(ctx->logonconv[0]);
	ctx->logonconv[0] = lsi_ut_clonearr(msg);
	lsi_com_strNcpy(ctx->mynick, (*msg)[2], sizeof ctx->mynick);


	// XXX this shouldn't be required.  Isn't the first argument of a
	// numeric message *always* just our nick anyway?
	char *tmp;
	if ((tmp = strchr(ctx->mynick, '@')))
		*tmp = '\0';
	if ((tmp = strchr(ctx->mynick, '!')))
		*tmp = '\0';

	// XXX this is probably not required either because
	// we do this for 004 already, and if we don't see any 004 (does any
	// server actually do this?) irc_connect() will fail in the first place.
	// and what about `myhost`, anyway?
	lsi_com_strNcpy(ctx->umodes, DEF_UMODES, sizeof ctx->umodes);
	lsi_com_strNcpy(ctx->cmodes, DEF_CMODES, sizeof ctx->cmodes);
	ctx->ver[0] = '\0';

	ctx->service = false;

	return 0;
}

static uint8_t
handle_002(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	lsi_ut_freearr(ctx->logonconv[1]);
	ctx->logonconv[1] = lsi_ut_clonearr(msg);

	return 0;
}

static uint8_t
handle_003(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	lsi_ut_freearr(ctx->logonconv[2]);
	ctx->logonconv[2] = lsi_ut_clonearr(msg);

	return 0;
}

static uint8_t
handle_004(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 7)
		return PROTO_ERR;

	lsi_ut_freearr(ctx->logonconv[3]);
	ctx->logonconv[3] = lsi_ut_clonearr(msg);

	lsi_com_strNcpy(ctx->myhost, (*msg)[3],sizeof ctx->myhost);
	lsi_com_strNcpy(ctx->umodes, (*msg)[5], sizeof ctx->umodes);
	lsi_com_strNcpy(ctx->cmodes, (*msg)[6], sizeof ctx->cmodes);
	lsi_com_strNcpy(ctx->ver, (*msg)[4], sizeof ctx->ver);

	return LOGON_COMPLETE;
}

static uint8_t
handle_PING(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	/* We only handle PINGs at logon. It's the user's job afterwards. */
	if (!logon)
		return 0;

	char buf[256];
	snprintf(buf, sizeof buf, "PONG :%s\r\n", (*msg)[2]);

	return lsi_conn_write(ctx->con, buf) ? 0 : IO_ERR;
}

/* This handles 432, 433, 436 and 437 all of which signal us that
 * we can't have the nickname we wanted */
static uint8_t
handle_XXX(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	if (!logon)
		return 0;

	if (!ctx->cb_mut_nick)
		return OUT_OF_NICKS;

	ctx->cb_mut_nick(ctx->mynick, sizeof ctx->mynick);

	if (!ctx->mynick[0])
		return OUT_OF_NICKS;

	char buf[MAX_NICK_LEN];
	snprintf(buf, sizeof buf, "NICK %s\r\n", ctx->mynick);
	return lsi_conn_write(ctx->con, buf) ? 0 : IO_ERR;
}

static uint8_t
handle_464(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	W("(%p) wrong server password", (void *)ctx);
	return AUTH_ERR;
}

/* Successful service logon.  I guess we don't get to see a 004, but haven't
 * really tried this yet */
static uint8_t
handle_383(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	lsi_com_strNcpy(ctx->mynick, (*msg)[2],sizeof ctx->mynick);
	char *tmp;
	if ((tmp = strchr(ctx->mynick, '@')))
		*tmp = '\0';
	if ((tmp = strchr(ctx->mynick, '!')))
		*tmp = '\0';

	lsi_com_strNcpy(ctx->myhost, (*msg)[0] ? (*msg)[0] : ctx->con->host,
	    sizeof ctx->myhost);

	lsi_com_strNcpy(ctx->umodes, DEF_UMODES, sizeof ctx->umodes);
	lsi_com_strNcpy(ctx->cmodes, DEF_CMODES, sizeof ctx->cmodes);
	ctx->ver[0] = '\0';
	ctx->service = true;
	D("(%p) got beloved 383", (void *)ctx);

	return LOGON_COMPLETE;
}

static uint8_t
handle_484(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	ctx->restricted = true;
	I("(%p) we're 'restricted'", (void *)ctx);
	return 0;
}

static uint8_t
handle_465(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	W("(%p) we're banned", (void *)ctx);
	ctx->banned = true;
	free(ctx->banmsg);
	ctx->banmsg = lsi_b_strdup((*msg)[3] ? (*msg)[3] : "");

	return 0; /* well if we are, the server will sure disconnect us */
}

static uint8_t
handle_466(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	W("(%p) we will be banned", (void *)ctx);

	return 0; /* so what, bitch? */
}

static uint8_t
handle_ERROR(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	free(ctx->lasterr);
	ctx->lasterr = lsi_b_strdup((*msg)[2] ? (*msg)[2] : "");
	W("sever said ERROR: '%s'", (*msg)[2]);
	return 0; /* not strictly a case for CANT_PROCEED.  We certainly could
	           * proceed, it's the server that doesn't seem willing to */
}

static uint8_t
handle_NICK(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 3)
		return PROTO_ERR;

	char nick[128];
	if (!(*msg)[0])
		return PROTO_ERR;

	lsi_ut_ident2nick(nick, sizeof nick, (*msg)[0]);

	if (!lsi_ut_istrcmp(nick, ctx->mynick, ctx->casemap)) {
		lsi_com_strNcpy(ctx->mynick, (*msg)[2], sizeof ctx->mynick);
		I("my nick is now '%s'", ctx->mynick);
	}

	return 0;
}

static uint8_t
handle_005_CASEMAPPING(irc *ctx, const char *val)
{
	if (lsi_b_strcasecmp(val, "ascii") == 0)
		ctx->casemap = CMAP_ASCII;
	else if (lsi_b_strcasecmp(val, "strict-rfc1459") == 0)
		ctx->casemap = CMAP_STRICT_RFC1459;
	else {
		if (lsi_b_strcasecmp(val, "rfc1459") != 0)
			W("unknown 005 casemapping: '%s'", val);
		ctx->casemap = CMAP_RFC1459;
	}

	return 0;
}

/* XXX not robust enough */
static uint8_t
handle_005_PREFIX(irc *ctx, const char *val)
{
	char str[32];
	if (!val[0])
		return PROTO_ERR;

	lsi_com_strNcpy(str, val + 1, sizeof str);
	char *p = strchr(str, ')');
	if (!p)
		return PROTO_ERR;

	*p++ = '\0';

	size_t slen = strlen(str);
	if (slen == 0 || slen != strlen(p))
		return PROTO_ERR;

	lsi_com_strNcpy(ctx->m005modepfx[0], str, MAX_005_MDPFX);
	lsi_com_strNcpy(ctx->m005modepfx[1], p, MAX_005_MDPFX);

	return 0;
}

static uint8_t
handle_005_CHANMODES(irc *ctx, const char *val)
{
	for (int z = 0; z < 4; ++z)
		ctx->m005chanmodes[z][0] = '\0';

	int c = 0;
	char argbuf[64];
	lsi_com_strNcpy(argbuf, val, sizeof argbuf);
	char *ptr = strtok(argbuf, ",");

	while (ptr) {
		if (c < 4)
			lsi_com_strNcpy(ctx->m005chanmodes[c++], ptr,
			    MAX_005_CHMD);
		ptr = strtok(NULL, ",");
	}

	if (c != 4)
		W("005 chanmodes: expected 4 params, got %i. arg: \"%s\"",
		    c, val);

	return 0;
}

static uint8_t
handle_005_CHANTYPES(irc *ctx, const char *val)
{
	lsi_com_strNcpy(ctx->m005chantypes, val, MAX_005_CHTYP);
	return 0;
}

static uint8_t
handle_005(irc *ctx, tokarr *msg, size_t nargs, bool logon)
{
	uint8_t ret = 0;
	bool have_casemap = false;

	for (size_t z = 3; z < nargs; ++z) {
		if (lsi_b_strncasecmp((*msg)[z], "CASEMAPPING=", 12) == 0) {
			ret |= handle_005_CASEMAPPING(ctx, (*msg)[z] + 12);
			have_casemap = true;
		} else if (lsi_b_strncasecmp((*msg)[z], "PREFIX=", 7) == 0)
			ret |= handle_005_PREFIX(ctx, (*msg)[z] + 7);
		else if (lsi_b_strncasecmp((*msg)[z], "CHANMODES=", 10) == 0)
			ret |= handle_005_CHANMODES(ctx, (*msg)[z] + 10);
		else if (lsi_b_strncasecmp((*msg)[z], "CHANTYPES=", 10) == 0)
			ret |= handle_005_CHANTYPES(ctx, (*msg)[z] + 10);

		if (ret & CANT_PROCEED)
			return ret;
	}

	if (have_casemap && ctx->tracking && !ctx->tracking_enab) {
		/* Now that we know the casemapping used by the server, we
		 * can enable tracking if it was originally (before connecting)
		 * asked for */
		if (!lsi_trk_init(ctx))
			E("failed to enable tracking");
		else {
			ctx->tracking_enab = true;
			I("tracking enabled");
		}
	}

	return ret;
}


bool
lsi_imh_regall(irc *ctx, bool dumb)
{
	bool fail = false;
	if (!dumb) {
		/* In dumb mode, we don't care about the numerics telling us
		 * that we can't have the nick we wanted; we don't reply to
		 * PING and we also don't pay attention to 464 (Wrong server
		 * password).  This is all left to the user */
		fail = fail || !lsi_msg_reghnd(ctx, "PING", handle_PING, "irc");
		fail = fail || !lsi_msg_reghnd(ctx, "432", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(ctx, "433", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(ctx, "436", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(ctx, "437", handle_XXX, "irc");
		fail = fail || !lsi_msg_reghnd(ctx, "464", handle_464, "irc");
	}

	fail = fail || !lsi_msg_reghnd(ctx, "NICK", handle_NICK, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "ERROR", handle_ERROR, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "001", handle_001, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "002", handle_002, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "003", handle_003, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "004", handle_004, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "383", handle_383, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "484", handle_484, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "465", handle_465, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "466", handle_466, "irc");
	fail = fail || !lsi_msg_reghnd(ctx, "005", handle_005, "irc");

	return !fail;
}

void
lsi_imh_unregall(irc *ctx)
{
	lsi_msg_unregall(ctx, "irc");
}
