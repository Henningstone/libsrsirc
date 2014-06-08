/* irc_track.c - (optionally) track users and channels
 * libsrsirc - a lightweight serious IRC lib - (C) 2014, Timo Buhrmester
 * See README for contact-, COPYING for license information. */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#define LOG_MODULE MOD_TRACK

#include <stdlib.h>
#include <string.h>

#include <intlog.h>

#include <libsrsirc/util.h>
#include <libsrsirc/irc_ext.h>

#include "intdefs.h"
#include "common.h"
#include "msg.h"
#include "ucbase.h"

#include <libsrsirc/irc_track.h>

static uint8_t h_JOIN(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_311(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_332(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_333(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_352(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_353(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_366(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_PART(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_QUIT(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_NICK(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_KICK(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_MODE(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_PRIVMSG(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_NOTICE(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_324(irc h, tokarr *msg, size_t nargs, bool logon);
static uint8_t h_TOPIC(irc h, tokarr *msg, size_t nargs, bool logon);

bool
trk_init(irc h)
{
	bool fail = false;
	fail = fail || !msg_reghnd(h, "JOIN", h_JOIN, "track");
	fail = fail || !msg_reghnd(h, "311", h_311, "track");
	fail = fail || !msg_reghnd(h, "332", h_332, "track");
	fail = fail || !msg_reghnd(h, "333", h_333, "track");
	fail = fail || !msg_reghnd(h, "353", h_353, "track");
	fail = fail || !msg_reghnd(h, "352", h_352, "track");
	fail = fail || !msg_reghnd(h, "366", h_366, "track");
	fail = fail || !msg_reghnd(h, "PART", h_PART, "track");
	fail = fail || !msg_reghnd(h, "QUIT", h_QUIT, "track");
	fail = fail || !msg_reghnd(h, "NICK", h_NICK, "track");
	fail = fail || !msg_reghnd(h, "KICK", h_KICK, "track");
	fail = fail || !msg_reghnd(h, "MODE", h_MODE, "track");
	fail = fail || !msg_reghnd(h, "PRIVMSG", h_PRIVMSG, "track");
	fail = fail || !msg_reghnd(h, "NOTICE", h_NOTICE, "track");
	fail = fail || !msg_reghnd(h, "324", h_324, "track");
	fail = fail || !msg_reghnd(h, "TOPIC", h_TOPIC, "track");

	if (fail || !ucb_init(h)) {
		msg_unregall(h, "track");
		return false;
	}

	h->endofnames = true;
	return true;
}

void
trk_deinit(irc h)
{
	ucb_deinit(h);
	msg_unregall(h, "track");
}


static uint8_t
h_JOIN(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 3)
		return PROTO_ERR;

	char nick[MAX_NICK_LEN];
	ut_pfx2nick(nick, sizeof nick, (*msg)[0]);

	bool me = ut_istrcmp(nick, h->mynick, h->casemap) == 0;
	chan c = get_chan(h, (*msg)[2], !me);

	if (me) {
		if (!c && !add_chan(h, (*msg)[2])) {
			E("not tracking chan '%s'", (*msg)[2]);
			return ALLOC_ERR;
		}
	} else {
		if (!c) {
			W("we don't know channel '%s'!", (*msg)[2]);
			return 0;
		}

		user u = get_user(h, (*msg)[0], false);
		bool uadd = false;
		if (!u) {
			uadd = true;
			if (!(u = add_user(h, (*msg)[0])))
				return ALLOC_ERR;
		}

		if (!add_memb(h, c, u, "")) {
			E("chan '%s' desynced", c->name);
			if (uadd)
				drop_user(h, u);
			return ALLOC_ERR;
		}
	}

	return 0;
}
/* 352    RPL_WHOREPLY
 *            "<channel> <user> <host> <server> <nick>
 *            ( "H" / "G" > ["*"] [ ( "@" / "+" ) ]
 *            :<hopcount> <real name>"
 */

/*:rajaniemi.freenode.net 352 nvmme CHAN UNAME HOST SRV NICK H :0 irc netcat*/
static uint8_t
h_352(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 10)
		return PROTO_ERR;

	user u = get_user(h, (*msg)[7], true);
	if (!u)
		return 0;

	if (!u->uname || ut_istrcmp(u->uname, (*msg)[4], h->casemap) != 0) {
		if (u->uname)
			W("username for '%s' changed from '%s' to '%s'!",
			    u->nick, u->uname, (*msg)[4]);

		com_update_strprop(&u->uname, (*msg)[4]);
	}

	if (!u->host || ut_istrcmp(u->host, (*msg)[5], h->casemap) != 0) {
		if (u->host)
			W("host for '%s' changed from '%s' to '%s'!",
			    u->nick, u->host, (*msg)[5]);

		com_update_strprop(&u->host, (*msg)[5]);
	}

	const char *fname = strchr((*msg)[9], ' ');
	if (!fname)
		return PROTO_ERR;
	
	if (!u->fname || ut_istrcmp(u->fname, fname+1, h->casemap) != 0) {
		if (u->fname)
			W("fullname for '%s' changed from '%s' to '%s'!",
			    u->nick, u->fname, fname+1);

		com_update_strprop(&u->fname, fname+1);
	}

	return 0;
}

/*:rajaniemi.freenode.net 315 nvmme #fstd :End of /WHO list.*/
//static uint8_t
//h_315(irc h, tokarr *msg, size_t nargs, bool logon)

static uint8_t
h_332(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 5)
		return PROTO_ERR;

	chan c = get_chan(h, (*msg)[3], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[3]);
		return 0;
	}
	free(c->topic);
	if (!(c->topic = com_strdup((*msg)[4])))
		return ALLOC_ERR;

	return 0;
}

static uint8_t
h_333(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 6)
		return PROTO_ERR;

	chan c = get_chan(h, (*msg)[3], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[3]);
		return 0;
	}
	free(c->topicnick);
	if (!(c->topicnick = com_strdup((*msg)[4])))
		return ALLOC_ERR;
	
	c->tstopic = (uint64_t)strtoull((*msg)[5], NULL, 10);

	return 0;
}

static uint8_t
h_353(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 6)
		return PROTO_ERR;

	chan c = get_chan(h, (*msg)[4], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[4]);
		return 0;
	}

	if (h->endofnames) {
		clear_memb(h, c);
		h->endofnames = false;
	}

	char nick[MAX_NICK_LEN];
	const char *p = (*msg)[5];
	for (;;) {
		char mpfx[2] = {'\0', '\0'};

		if (strchr(h->m005modepfx[1], *p))
			mpfx[0] = *p++;

		const char *end = strchr(p, ' ');
		if (!end)
			end = p + strlen(p);

		size_t len = end - p;
		if (len >= sizeof nick)
			len = sizeof nick - 1;

		com_strNcpy(nick, p, len + 1);

		user u = get_user(h, nick, false);
		bool uadd = false;
		if (!u) {
			uadd = true;
			if (!(u = add_user(h, nick)))
				return ALLOC_ERR;
		}

		if (!add_memb(h, c, u, mpfx)) {
			if (uadd)
				drop_user(h, u);
			return ALLOC_ERR;
		}

		if (!*end)
			break;
		p = end + 1;
	}

	return 0;
}

static uint8_t
h_366(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	h->endofnames = true;

	chan c = get_chan(h, (*msg)[3], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[3]);
		return 0;
	}
	c->desync = false;

	return 0;
}

static uint8_t
h_PART(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 3)
		return PROTO_ERR;

	char nick[MAX_NICK_LEN];
	ut_pfx2nick(nick, sizeof nick, (*msg)[0]);
	touch_user(h, (*msg)[0], true);

	chan c = get_chan(h, (*msg)[2], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[2]);
		return 0;
	}

	if (ut_istrcmp(nick, h->mynick, h->casemap) == 0)
		drop_chan(h, c);
	else {
		user u = get_user(h, (*msg)[0], true);
		if (u)
			drop_memb(h, c, u, true, true);
	}

	return 0;
}

static uint8_t
h_QUIT(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0])
		return PROTO_ERR;

	touch_user(h, (*msg)[0], true);

	user u = get_user(h, (*msg)[0], true);
	if (u)
		drop_user(h, u);

	return 0;
}

static uint8_t
h_KICK(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	touch_user(h, (*msg)[0], true);
	chan c = get_chan(h, (*msg)[2], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[2]);
		return 0;
	}

	if (ut_istrcmp((*msg)[3], h->mynick, h->casemap) == 0)
		drop_chan(h, c);
	else {
		user u = get_user(h, (*msg)[3], true);
		if (u)
			drop_memb(h, c, u, true, true);
	}

	return 0;
}

static uint8_t
h_NICK(irc h, tokarr *msg, size_t nargs, bool logon)
{
	uint8_t res = 0;

	if (!(*msg)[0] || nargs < 3)
		return PROTO_ERR;

	touch_user(h, (*msg)[0], true);

	bool aerr;
	if (!rename_user(h, (*msg)[0], (*msg)[2], &aerr)) {
		E("failed renaming user '%s' ('%s')", (*msg)[0], (*msg)[2]);
		if (aerr)
			res |= ALLOC_ERR;
	}
	return res;
}

static uint8_t
h_TOPIC(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	touch_user(h, (*msg)[0], true);
	char nick[MAX_NICK_LEN];
	ut_pfx2nick(nick, sizeof nick, (*msg)[0]);

	chan c = get_chan(h, (*msg)[2], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[2]);
		return 0;
	}
	free(c->topic);
	free(c->topicnick);
	c->topicnick = NULL;
	if (!(c->topic = com_strdup((*msg)[3])) || !(c->topicnick = com_strdup(nick)))
		return ALLOC_ERR;

	return 0;
}

static uint8_t
h_MODE_chanmode(irc h, tokarr *msg, size_t nargs, bool logon)
{
	uint8_t res = 0;

	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	if (!strchr((*msg)[0], '.')) //servermode
		touch_user(h, (*msg)[0], true);

	chan c = get_chan(h, (*msg)[2], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[2]);
		return 0;
	}

	size_t num;
	char **p = ut_parse_MODE(h, msg, &num, false);
	if (!p)
		return ALLOC_ERR;

	for (size_t i = 0; i < num; i++) {
		bool enab = p[i][0] == '+';
		char *ptr = strchr(h->m005modepfx[0], p[i][1]);
		if (ptr) {
			char sym = h->m005modepfx[1][ptr - h->m005modepfx[0]];
			update_modepfx(h, c, p[i] + 3, sym, enab); //XXX check?
		} else {
			if (enab) {
				if (!add_chanmode(h, c, p[i] + 1))
					res |= ALLOC_ERR;
			} else
				drop_chanmode(h, c, p[i] + 1);
		}
	}
	
	for (size_t i = 0; i < num; i++)
		free(p[i]);
		
	free(p);

	return res;
}

static uint8_t
h_MODE_usermode(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (nargs < 4)
		return PROTO_ERR;

	const char *p = (*msg)[3];
	bool enab = true;
	char c;
	while ((c = *p++)) {
		if (c == '+' || c == '-') {
			enab = c == '+';
			continue;
		}
		char *m = strchr(h->myumodes, c);
		if (enab == !!m) {
			W("user mode '%c' %s set ('%s')", c,
			    enab?"already":"not", h->myumodes);
			continue;
		}

		if (!enab) {
			*m++ = '\0';
			while (*m) {
				m[-1] = *m;
				*m++ = '\0';
			}
		} else {
			char mch[] = {c, '\0'};
			com_strNcat(h->myumodes, mch, sizeof h->myumodes);
		}
	}

	return 0;
}

static uint8_t
h_MODE(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	if (strchr(h->m005chantypes, (*msg)[2][0]))
		return h_MODE_chanmode(h, msg, nargs, logon);
	else
		return h_MODE_usermode(h, msg, nargs, logon);
}

static uint8_t
h_PRIVMSG(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	if (!logon && !strchr((*msg)[0], '.'))
		touch_user(h, (*msg)[0], false);

	return 0;
}

static uint8_t
h_NOTICE(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 4)
		return PROTO_ERR;

	if (!logon && !strchr((*msg)[0], '.'))
		touch_user(h, (*msg)[0], false);

	return 0;
}


static uint8_t
h_324(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 5)
		return PROTO_ERR;

	uint8_t res = 0;

	chan c = get_chan(h, (*msg)[3], true);
	if (!c) {
		W("we don't know channel '%s'!", (*msg)[3]);
		return 0;
	}

	size_t num;
	char **p = ut_parse_MODE(h, msg, &num, true);
	if (!p)
		return ALLOC_ERR;

	clear_chanmodes(h, c);

	for (size_t i = 0; i < num; i++) {
		bool enab = p[i][0] == '+';
		if (enab) {
			if (!add_chanmode(h, c, p[i] + 1))
				res |= ALLOC_ERR;
		} else
			drop_chanmode(h, c, p[i] + 1);
	}
	
	for (size_t i = 0; i < num; i++)
		free(p[i]);
		
	free(p);

	return res;
}

/* 302    RPL_USERHOST  
 * ":*1<reply> *( " " <reply> )"
 * reply = nickname [ "*" ] "=" ( "+" / "-" ) hostname
 */
//static uint8_t
//h_302(irc h, tokarr *msg, size_t nargs, bool logon)
//{
//}

/* 311    RPL_WHOISUSER
 * "<nick> <user> <host> * :<real name>"
 */
static uint8_t
h_311(irc h, tokarr *msg, size_t nargs, bool logon)
{
	if (!(*msg)[0] || nargs < 8)
		return PROTO_ERR;

	user u = get_user(h, (*msg)[3], false);
	if (!u)
		return 0;

	if (!u->uname || ut_istrcmp(u->uname, (*msg)[4], h->casemap) != 0) {
		if (u->uname)
			W("username for '%s' changed from '%s' to '%s'!",
			    u->nick, u->uname, (*msg)[4]);

		com_update_strprop(&u->uname, (*msg)[4]);
	}

	if (!u->host || ut_istrcmp(u->host, (*msg)[5], h->casemap) != 0) {
		if (u->host)
			W("host for '%s' changed from '%s' to '%s'!",
			    u->nick, u->host, (*msg)[5]);

		com_update_strprop(&u->host, (*msg)[5]);
	}

	if (!u->fname || ut_istrcmp(u->fname, (*msg)[7], h->casemap) != 0) {
		if (u->fname)
			W("fullname for '%s' changed from '%s' to '%s'!",
			    u->nick, u->fname, (*msg)[7]);

		com_update_strprop(&u->fname, (*msg)[7]);
	}

	uint8_t res = 0;
}

	

void
trk_dump(irc h, bool full)
{
	ucb_dump(h, full);
}

// JOIN #srsbsns redacted
// :mynick!~myuser@example.org JOIN #srsbsns

/* only if topic is present*/
// :port80c.se.quakenet.org 332 mynick #srsbsns :topic text
// :port80c.se.quakenet.org 333 mynick #srsbsns topicnick 1401460661

/* always */
// :port80c.se.quakenet.org 353 mynick @ #srsbsns :mynick fisted @foo +bar
// :port80c.se.quakenet.org 366 mynick #srsbsns :End of /NAMES list.

// -------

// MODE #srsbsns
// :port80c.se.quakenet.org 324 mynick #srsbsns +sk *
// :port80c.se.quakenet.org 329 mynick #srsbsns 1313083493
