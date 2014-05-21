/* irc_io.c - Semi back-end (btw irc_io and irc_basic) implementation
 * libsrsirc - a lightweight serious IRC lib - (C) 2012, Timo Buhrmester
 * See README for contact-, COPYING for license information. */

#if HAVE_CONFIG_H
# include <config.h>
#endif

/* C */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

/* POSIX */
#include <unistd.h>
#include <sys/select.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#ifdef WITH_SSL
/* ssl */
# include <openssl/ssl.h>
# include <openssl/err.h>
#endif

#include <common.h>
#include <libsrsirc/irc_util.h>
#include "irc_io.h"
#include <intlog.h>

#include "proxy.h"

#include "irc_con.h"

#define INV -1
#define OFF 0
#define ON 1


#ifdef WITH_SSL
static bool s_sslinit;
#endif

iconn
icon_init(void)
{
	iconn r = NULL;
	int preverrno = errno;
	errno = 0;

	if (!(r = malloc(sizeof *r)))
		goto icon_init_fail;

	r->linebuf = NULL;
	r->overbuf = NULL;
	r->host = NULL;

	if ((!(r->linebuf = malloc(LINEBUF_SZ)))
	    || (!(r->overbuf = malloc(OVERBUF_SZ)))
	    || (!(r->host = strdup(DEF_HOST))))
		goto icon_init_fail;

	/* these 2 neccessary? */
	memset(r->linebuf, 0, LINEBUF_SZ);
	memset(r->overbuf, 0, OVERBUF_SZ);

	errno = preverrno;
	r->port = DEF_PORT;
	r->phost = NULL;
	r->pport = 0;
	r->ptype = -1;
	r->sck = -1;
	r->state = OFF;
	r->mehptr = NULL;
	r->colon_trail = false;
#ifdef WITH_SSL
	r->ssl = false;
	r->shnd = NULL;
	r->sctx = NULL;
#endif

	D("(%p) irc_con initialized", r);

	return r;

icon_init_fail:
	EE("failed to initialize irc_con handle");
	if (r) {
		free(r->host);
		free(r->overbuf);
		free(r->linebuf);
		free(r);
	}

	return NULL;
}

bool
icon_reset(iconn hnd)
{
	D("(%p) resetting", hnd);

#ifdef WITH_SSL
	if (hnd->shnd) {
		D("(%p) shutting down ssl", hnd);
		SSL_shutdown(hnd->shnd);
		SSL_free(hnd->shnd);
		hnd->shnd = NULL;
	}
#endif

	if (hnd->sck != -1) {
		D("(%p) closing socket %d", hnd, hnd->sck);
		close(hnd->sck);
	}

	hnd->sck = -1;
	hnd->state = OFF;
	hnd->linebuf[0] = '\0';
	memset(hnd->overbuf, 0, OVERBUF_SZ);
	hnd->mehptr = NULL;

	return true;
}

bool
icon_dispose(iconn hnd)
{
	icon_reset(hnd);

#ifdef WITH_SSL
	icon_set_ssl(hnd, false); //dispose ssl context if existing
#endif

	free(hnd->host);
	free(hnd->phost);
	free(hnd->linebuf);
	free(hnd->overbuf);
	hnd->state = INV;

	D("(%p) disposed", hnd);
	free(hnd);

	return true;
}

bool
icon_connect(iconn hnd,
    unsigned long softto_us, unsigned long hardto_us)
{
	if (!hnd || hnd->state != OFF)
		return false;

	int64_t tsend = hardto_us ? ic_timestamp_us() + hardto_us : 0;

	char *host = hnd->ptype != -1 ? hnd->phost : hnd->host;
	unsigned short port = hnd->ptype != -1 ? hnd->pport : hnd->port;

	{
		char pxspec[64];
		pxspec[0] = '\0';
		if (hnd->ptype != -1)
			snprintf(pxspec, sizeof pxspec, " via %s:%s:%hu",
			    pxtypestr(hnd->ptype), hnd->phost, hnd->pport);

		D("(%p) wanna connect to %s:%hu%s, sto: %luus, hto: %luus",
		    hnd, hnd->host, hnd->port, pxspec,
		    softto_us, hardto_us);
	}

	struct sockaddr sa;
	size_t addrlen;

	int sck = ic_consocket(host, port, &sa, &addrlen,
	    softto_us, hardto_us);

	if (sck < 0) {
		W("(%p) ic_consocket failed for %s:%hu", hnd, host, port);
		return false;
	}

	D("(%p) connected socket %d for %s:%hu", hnd, sck, host, port);

	hnd->sck = sck; //must be set here for proxy_logon

	int64_t trem = 0;
	if (hnd->ptype != -1) {
		if (tsend) {
			trem = tsend - ic_timestamp_us();
			if (trem <= 0) {
				W("(%p) timeout", hnd);
				close(sck);
				hnd->sck = -1;
				return false;
			}
		}

		bool ok = false;
		D("(%p) logging on to proxy", hnd);
		if (hnd->ptype == IRCPX_HTTP)
			ok = proxy_logon_http(hnd->sck, hnd->host,
			    hnd->port, (unsigned long)trem);
		else if (hnd->ptype == IRCPX_SOCKS4)
			ok = proxy_logon_socks4(hnd->sck, hnd->host,
			    hnd->port, (unsigned long)trem);
		else if (hnd->ptype == IRCPX_SOCKS5)
			ok = proxy_logon_socks5(hnd->sck, hnd->host,
			    hnd->port, (unsigned long)trem);

		if (!ok) {
			W("(%p) proxy logon failed", hnd);
			close(sck);
			hnd->sck = -1;
			return false;
		}
		D("(%p) sent proxy logon sequence", hnd);
	}

	D("(%p) setting to blocking mode", hnd);
	errno = 0;
	if (fcntl(sck, F_SETFL, 0) == -1) {
		WE("(%p) failed to clear nonblocking mode", hnd);
		close(sck);
		hnd->sck = -1;
		return false;
	}
#ifdef WITH_SSL
	if (hnd->ssl) {
		bool fail = false;
		fail = !(hnd->shnd = SSL_new(hnd->sctx));
		fail = fail || !SSL_set_fd(hnd->shnd, sck);
		int r = SSL_connect(hnd->shnd);
		D("ssl_connect: %d", r);
		if (r != 1) {
			int rr = SSL_get_error(hnd->shnd, r);
			D("rr: %d", rr);
		}
		fail = fail || (r != 1);
		if (fail) {
			ERR_print_errors_fp(stderr);
			if (hnd->shnd) {
				SSL_free(hnd->shnd);
				hnd->shnd = NULL;
			}

			close(sck);
			W("connect bailing out; couldn't initiate ssl");
			return false;
		}
	}
#endif

	hnd->state = ON;

	D("(%p) %s connection to ircd established",
	    hnd, hnd->ptype == -1?"TCP":"proxy");

	return true;
}

int
icon_read(iconn hnd, char *(*tok)[MAX_IRCARGS],
    unsigned long to_us)
{
	if (!hnd || hnd->state != ON)
		return -1;

	int64_t tsend = to_us ? ic_timestamp_us() + to_us : 0;
	D("(%p) wanna read (timeout: %lu)", hnd, to_us);

	int n;
	/* read a line, ignoring empty lines */
	do {
		int64_t trem;
		if (tsend) {
			trem = tsend - ic_timestamp_us();
			if (trem <= 0) {
				V("(%p) timeout hit", hnd);
				return 0;
			}
		}
		n = ircio_read_ex(hnd->sck,
#ifdef WITH_SSL
		    hnd->shnd,
#endif
		    hnd->linebuf, LINEBUF_SZ, hnd->overbuf, OVERBUF_SZ,
		    &hnd->mehptr, tok, tsend ? (unsigned long)trem : 0ul);

		if (n < 0) { //read error
			W("(%p) read failed", hnd);
			icon_reset(hnd);
			return -1;
		}
	} while(n == 0);

	size_t last = 2;
	for(; last < COUNTOF(*tok) && (*tok)[last]; last++);

	if (last > 2)
		hnd->colon_trail = (*tok)[last-1][-1] == ':';

	D("(%p) done reading", hnd);

	return 1;
}

bool
icon_write(iconn hnd, const char *line)
{
	if (!hnd || hnd->state != ON || !line)
		return false;


	if (ircio_write_ex(hnd->sck,
#ifdef WITH_SSL
	    hnd->shnd,
#endif
	    line) == -1) {
		W("(%p) failed to write '%s'", hnd, line);
		icon_reset(hnd);
		return false;
	}

	char buf[1024];
	strncpy(buf, line, sizeof buf);
	buf[sizeof buf - 1] = '\0';
	char *endptr = buf + strlen(buf) - 1;
	while(endptr >= buf && (*endptr == '\r' || *endptr == '\n'))
		*endptr-- = '\0';

	D("(%p) wrote a line: '%s'", hnd, buf);
	return true;

}

bool
icon_online(iconn hnd)
{
	return hnd->state == ON;
}

bool
icon_colon_trail(iconn hnd)
{
	if (!hnd || hnd->state != ON)
		return false;

	return hnd->colon_trail;
}

bool
icon_set_proxy(iconn hnd, const char *host, unsigned short port,
    int ptype)
{
	char *n = NULL;
	switch(ptype) {
	case IRCPX_HTTP:
	case IRCPX_SOCKS4:
	case IRCPX_SOCKS5:
		if (!host || !(1 <= port && port <= 65535))
			return false;

		if (!(n = strdup(host))) {
			EE("strdup failed");
			return false;
		}
		hnd->pport = port;
		hnd->ptype = ptype;
		free(hnd->phost);
		hnd->phost = n;
		break;
	default:
		E("illegal proxy type %d", ptype);
		return false;
	}

	return true;
}

bool
icon_set_server(iconn hnd, const char *host, unsigned short port)
{
	char *n;
	if (!(n = strdup(host?host:DEF_HOST))) {
		EE("strdup failed");
		return false;
	}
	free(hnd->host);
	hnd->host = n;
	hnd->port = (1 <= port && port <= 65535)?port:DEF_PORT;
	return true;
}

bool
icon_set_ssl(iconn hnd, bool on)
{
#ifndef WITH_SSL
	W("library was not compiled with SSL support");
	return false;
#else

	if (!s_sslinit && on) {
		SSL_load_error_strings();
		SSL_library_init();
		s_sslinit = true;
	}

	if (on && !hnd->sctx) {
		if (!(hnd->sctx = SSL_CTX_new(SSLv23_client_method()))) {
			W("SSL_CTX_new failed, ssl not enabled!");
			return false;
		}
	} else if (!on && hnd->sctx) {
		SSL_CTX_free(hnd->sctx);
		hnd->sctx = NULL;
	}

	hnd->ssl = on;

	return true;
#endif
}

const char*
icon_get_proxy_host(iconn hnd)
{
	return hnd->phost;
}

unsigned short
icon_get_proxy_port(iconn hnd)
{
	return hnd->pport;
}

int
icon_get_proxy_type(iconn hnd)
{
	return hnd->ptype;
}

const char*
icon_get_host(iconn hnd)
{
	return hnd->host;
}

unsigned short
icon_get_port(iconn hnd)
{
	return hnd->port;
}

bool
icon_get_ssl(iconn hnd)
{
#ifdef WITH_SSL
	return hnd->ssl;
#else
	return false;
#endif
}

int
icon_sockfd(iconn hnd)
{
	if (!hnd || hnd->state != ON)
		return -1;

	return hnd->sck;
}


