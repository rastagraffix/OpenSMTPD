/*	$OpenBSD: mta_session.c,v 1.26 2012/11/12 14:58:53 eric Exp $	*/

/*
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2008 Gilles Chehade <gilles@poolp.org>
 * Copyright (c) 2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 * Copyright (c) 2012 Eric Faurot <eric@openbsd.org>
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

#include "includes.h"

#include <sys/types.h>
#include "sys-queue.h"
#include "sys-tree.h"
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <inttypes.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

#define MAX_MAIL	100

#define MTA_HIWAT	65535

enum mta_state {
	MTA_INIT,
	MTA_BANNER,
	MTA_EHLO,
	MTA_HELO,
	MTA_STARTTLS,
	MTA_AUTH,
	MTA_READY,
	MTA_MAIL,
	MTA_RCPT,
	MTA_DATA,
	MTA_BODY,
	MTA_EOM,
	MTA_RSET,
	MTA_QUIT,
};

#define MTA_FORCE_ANYSSL	0x0001
#define MTA_FORCE_SMTPS		0x0002
#define MTA_FORCE_TLS     	0x0004
#define MTA_FORCE_PLAIN		0x0008
#define MTA_WANT_SECURE		0x0010
#define MTA_USE_AUTH		0x0020
#define MTA_USE_CERT		0x0040

#define MTA_TLS_TRIED		0x0080

#define MTA_TLS			0x0100
#define MTA_VERIFIED   		0x0200

#define MTA_EXT_STARTTLS	0x01
#define MTA_EXT_AUTH		0x02
#define MTA_EXT_PIPELINING	0x04

struct mta_session {
	uint64_t		 id;
	struct mta_relay	*relay;
	struct mta_route	*route;

	int			 flags;

	int			 attempt;
	int			 use_smtps;
	int			 use_starttls;
	int			 use_smtp_tls;
	int			 ready;

	struct iobuf		 iobuf;
	struct io		 io;
	int			 ext;

	int			 msgcount;

	enum mta_state		 state;
	struct mta_task		*task;
	struct envelope		*currevp;
	FILE			*datafp;
};

static void mta_session_init(void);
static void mta_io(struct io *, int);
static void mta_free(struct mta_session *);
static void mta_on_ptr(void *, void *, void *);
static void mta_connect(struct mta_session *);
static void mta_enter_state(struct mta_session *, int);
static void mta_flush_task(struct mta_session *, int, const char *);
static void mta_envelope_fail(struct envelope *, struct mta_route *, int);
static void mta_error(struct mta_session *, const char *, ...);
static void mta_send(struct mta_session *, char *, ...);
static ssize_t mta_queue_data(struct mta_session *);
static void mta_response(struct mta_session *, char *);
static const char * mta_strstate(int);
static int mta_check_loop(FILE *);
static void mta_start_tls(struct mta_session *);
static int mta_verify_certificate(struct mta_session *);

static struct tree wait_ptr;
static struct tree wait_fd;
static struct tree wait_ssl_init;
static struct tree wait_ssl_verify;

static void
mta_session_init(void)
{
	static int init = 0;

	if (!init) {
		tree_init(&wait_ptr);
		tree_init(&wait_fd);
		tree_init(&wait_ssl_init);
		tree_init(&wait_ssl_verify);
		init = 1;
	}
}

void
mta_session(struct mta_relay *relay, struct mta_route *route)
{
	struct mta_session	*s;

	mta_session_init();

	s = xcalloc(1, sizeof *s, "mta_session");
	s->id = generate_uid();
	s->relay = relay;
	s->route = route;
	s->io.sock = -1;

	if (relay->flags & RELAY_SSL && relay->flags & RELAY_AUTH)
		s->flags |= MTA_USE_AUTH;
	if (relay->cert)
		s->flags |= MTA_USE_CERT;
	switch (relay->flags & (RELAY_SSL|RELAY_TLS_OPTIONAL)) {
		case RELAY_SSL:
			s->flags |= MTA_FORCE_ANYSSL;
			s->flags |= MTA_WANT_SECURE;
			break;
		case RELAY_SMTPS:
			s->flags |= MTA_FORCE_SMTPS;
			s->flags |= MTA_WANT_SECURE;
			break;
		case RELAY_STARTTLS:
			s->flags |= MTA_FORCE_TLS;
			s->flags |= MTA_WANT_SECURE;
			break;
		case RELAY_TLS_OPTIONAL:
			/* do not force anything, try tls then smtp */
			break;
		default:
			s->flags |= MTA_FORCE_PLAIN;
	}

	log_debug("debug: mta: %p: spawned for relay %s", s,
	    mta_relay_to_text(relay));
	stat_increment("mta.session", 1);

	if (route->dst->ptrname || route->dst->lastptrquery) {
		mta_on_ptr(NULL, s, route->dst->ptrname);
	} else if (waitq_wait(&route->dst->ptrname, mta_on_ptr, s)) {
		dns_query_ptr(s->id, s->route->dst->sa);
		tree_xset(&wait_ptr, s->id, s);
	}
}

void
mta_session_imsg(struct mproc *p, struct imsg *imsg)
{
	uint64_t			id;
	struct mta_session	       *s;
	struct mta_host		       *h;
	struct dns_resp_msg	       *resp_dns;
	struct ca_vrfy_resp_msg	       *resp_ca_vrfy;
	struct ca_cert_resp_msg	       *resp_ca_cert;
	void     		       *ssl;

	switch (imsg->hdr.type) {

	case IMSG_QUEUE_MESSAGE_FD:
		id = *(uint64_t*)(imsg->data);
		if (imsg->fd == -1)
			fatalx("mta: cannot obtain msgfd");
		s = tree_xpop(&wait_fd, id);
		s->datafp = fdopen(imsg->fd, "r");
		if (s->datafp == NULL)
			fatal("mta: fdopen");

		if (mta_check_loop(s->datafp)) {
			log_debug("debug: mta: loop detected");
			fclose(s->datafp);
			s->datafp = NULL;
			mta_flush_task(s, IMSG_DELIVERY_LOOP,
			    "Loop detected");
			mta_enter_state(s, MTA_READY);
		} else {
			mta_enter_state(s, MTA_MAIL);
		}
		io_reload(&s->io);
		return;

	case IMSG_DNS_PTR:
		resp_dns = imsg->data;
		s = tree_xpop(&wait_ptr, resp_dns->reqid);
		h = s->route->dst;
		h->lastptrquery = time(NULL);
		if (!resp_dns->error)
			h->ptrname = xstrdup(resp_dns->u.ptr, "mta: ptr");
		waitq_run(&h->ptrname, h->ptrname);
		return;

	case IMSG_LKA_SSL_INIT:
		resp_ca_cert = imsg->data;
		s = tree_xpop(&wait_ssl_init, resp_ca_cert->reqid);

		if (resp_ca_cert->status == CA_FAIL) {
			log_info("relay: Disconnecting session %016" PRIx64
			    ": CA failure", s->id);
			mta_free(s);
			return;
		}

		resp_ca_cert = xmemdup(imsg->data, sizeof *resp_ca_cert, "mta:ca_cert");
		if (resp_ca_cert == NULL)
			fatal(NULL);
		resp_ca_cert->cert = xstrdup((char *)imsg->data +
		    sizeof *resp_ca_cert, "mta:ca_cert");

		resp_ca_cert->key = xstrdup((char *)imsg->data +
		    sizeof *resp_ca_cert + resp_ca_cert->cert_len,
		    "mta:ca_key");

		ssl = ssl_mta_init(resp_ca_cert->cert, resp_ca_cert->cert_len,
		    resp_ca_cert->key, resp_ca_cert->key_len);
		if (ssl == NULL)
			fatal("mta: ssl_mta_init");
		io_start_tls(&s->io, ssl);

		bzero(resp_ca_cert->cert, resp_ca_cert->cert_len);
		bzero(resp_ca_cert->key, resp_ca_cert->key_len);
		free(resp_ca_cert);

		return;

	case IMSG_LKA_SSL_VERIFY:
		resp_ca_vrfy = imsg->data;
		s = tree_xpop(&wait_ssl_verify, resp_ca_vrfy->reqid);

		if (resp_ca_vrfy->status == CA_OK)
			s->flags |= MTA_VERIFIED;

		mta_io(&s->io, IO_TLSVERIFIED);
		io_resume(&s->io, IO_PAUSE_IN);
		io_reload(&s->io);
		return;

	default:
		errx(1, "mta_session_imsg: unexpected %s imsg",
		    imsg_to_str(imsg->hdr.type));
	}
}

static void
mta_free(struct mta_session *s)
{
	struct mta_relay *relay;
	struct mta_route *route;

	log_debug("debug: mta: %p: session done", s);

	io_clear(&s->io);
	iobuf_clear(&s->iobuf);

	if (s->task)
		fatalx("current task should have been deleted already");
	if (s->datafp)
		fclose(s->datafp);

	relay = s->relay;
	route = s->route;
	free(s);
	stat_decrement("mta.session", 1);
	mta_route_collect(relay, route);
}

static void
mta_on_ptr(void *tag, void *arg, void *data)
{
	struct mta_session *s = arg;

	mta_connect(s);
}

static void
mta_connect(struct mta_session *s)
{
	struct sockaddr_storage	 ss;
	struct sockaddr		*sa;
	int			 portno;
	const char		*schema = "smtp+tls://";

	io_clear(&s->io);
	iobuf_clear(&s->iobuf);

	s->use_smtps = s->use_starttls = s->use_smtp_tls = 0;

	switch (s->attempt) {
	case 0:
		if (s->flags & MTA_FORCE_SMTPS)
			s->use_smtps = 1;	/* smtps */
		else if (s->flags & (MTA_FORCE_TLS|MTA_FORCE_ANYSSL))
			s->use_starttls = 1;	/* tls, tls+smtps */
		else if (!(s->flags & MTA_FORCE_PLAIN))
			s->use_smtp_tls = 1;
		break;
	case 1:
		if (s->flags & MTA_FORCE_ANYSSL) {
			s->use_smtps = 1;	/* tls+smtps */
			break;
		}
	default:
		goto fail;
	}
	portno = s->use_smtps ? 465 : 25;

	/* Override with relay-specified port */
	if (s->relay->port)
		portno = s->relay->port;

	memmove(&ss, s->route->dst->sa, SA_LEN(s->route->dst->sa));
	sa = (struct sockaddr *)&ss;
	sa_set_port(sa, portno);

	s->attempt += 1;

	if (s->use_smtp_tls)
		schema = "smtp+tls://";
	else if (s->use_starttls)
		schema = "tls://";
	else if (s->use_smtps)
		schema = "smtps://";
	else
		schema = "smtp://";
	log_debug("debug: mta: %p: connecting to %s%s:%i (%s)",
	    s, schema, sa_to_text(sa), portno, s->route->dst->ptrname);

	mta_enter_state(s, MTA_INIT);
	iobuf_xinit(&s->iobuf, 0, 0, "mta_connect");
	io_init(&s->io, -1, s, mta_io, &s->iobuf);
	io_set_timeout(&s->io, 300000);
	if (io_connect(&s->io, sa, s->route->src->sa) == -1) {
		/*
		 * This error is most likely a "no route",
		 * so there is no need to try again.
		 */
		log_debug("debug: mta: io_connect failed: %s", s->io.error);
		if (errno == EADDRNOTAVAIL)
			mta_source_error(s->relay, s->route, s->io.error);
		else
			mta_error(s, "Connection failed: %s", s->io.error);
		mta_free(s);
	}
	return;

fail:
	mta_error(s, "Could not connect");
	mta_free(s);
	return;
}

static void
mta_enter_state(struct mta_session *s, int newstate)
{
	int			 oldstate;
	ssize_t			 q;

    again:
	oldstate = s->state;

	log_trace(TRACE_MTA, "mta: %p: %s -> %s", s,
	    mta_strstate(oldstate),
	    mta_strstate(newstate));

	s->state = newstate;

	/* don't try this at home! */
#define mta_enter_state(_s, _st) do { newstate = _st; goto again; } while (0)

	switch (s->state) {
	case MTA_INIT:
	case MTA_BANNER:
		break;

	case MTA_EHLO:
		s->ext = 0;
		mta_send(s, "EHLO %s", env->sc_hostname);
		break;

	case MTA_HELO:
		s->ext = 0;
		mta_send(s, "HELO %s", env->sc_hostname);
		break;

	case MTA_STARTTLS:
		if (s->flags & MTA_TLS) /* already started */
			mta_enter_state(s, MTA_AUTH);
		else if ((s->ext & MTA_EXT_STARTTLS) == 0) {
			if (s->flags & MTA_FORCE_TLS || s->flags & MTA_WANT_SECURE) {
				mta_error(s, "TLS required but not supported by remote host");
				mta_connect(s);
			}
			else
				/* server doesn't support starttls, do not use it */
				mta_enter_state(s, MTA_AUTH);
		}
		else
			mta_send(s, "STARTTLS");
		break;

	case MTA_AUTH:
		if (s->relay->secret && s->flags & MTA_TLS)
			mta_send(s, "AUTH PLAIN %s", s->relay->secret);
		else if (s->relay->secret) {
			log_debug("debug: mta: %p: not using AUTH on non-TLS "
			    "session", s);
			mta_error(s, "Refuse to AUTH over unsecure channel");
			mta_connect(s);
		} else {
			mta_enter_state(s, MTA_READY);
		}
		break;

	case MTA_READY:
		/* Ready to send a new mail */
		if (s->ready == 0) {
			s->ready = 1;
			mta_route_ok(s->relay, s->route);
		}

		if (s->msgcount >= MAX_MAIL) {
			log_debug("debug: mta: "
			    "%p: cannot send more message to relay %s", s,
			    mta_relay_to_text(s->relay));
			mta_enter_state(s, MTA_QUIT);
			break;
		}

		s->task = mta_route_next_task(s->relay, s->route);
		if (s->task == NULL) {
			log_debug("debug: mta: %p: no task for relay %s",
			    s, mta_relay_to_text(s->relay));
			mta_enter_state(s, MTA_QUIT);
			break;
		}

		log_debug("debug: mta: %p: handling next task for relay %s", s,
			    mta_relay_to_text(s->relay));

		stat_increment("mta.task.running", 1);
		m_compose(p_queue, IMSG_QUEUE_MESSAGE_FD, s->task->msgid, 0, -1,
		    &s->id, sizeof(s->id));
		tree_xset(&wait_fd, s->id, s);
		break;

	case MTA_MAIL:
		if (s->task->sender.user[0] && s->task->sender.domain[0])
			mta_send(s, "MAIL FROM: <%s@%s>",
			    s->task->sender.user, s->task->sender.domain);
		else
			mta_send(s, "MAIL FROM: <>");
		break;

	case MTA_RCPT:
		if (s->currevp == NULL)
			s->currevp = TAILQ_FIRST(&s->task->envelopes);
		mta_send(s, "RCPT TO: <%s@%s>",
		    s->currevp->dest.user,
		    s->currevp->dest.domain);
		break;

	case MTA_DATA:
		fseek(s->datafp, 0, SEEK_SET);
		mta_send(s, "DATA");
		break;

	case MTA_BODY:
		if (s->datafp == NULL) {
			log_trace(TRACE_MTA, "mta: %p: end-of-file", s);
			mta_enter_state(s, MTA_EOM);
			break;
		}

		if ((q = mta_queue_data(s)) == -1) {
			mta_free(s);
			break;
		}

		log_trace(TRACE_MTA, "mta: %p: >>> [...%zi bytes...]", s, q);
		break;

	case MTA_EOM:
		mta_send(s, ".");
		break;

	case MTA_RSET:
		mta_send(s, "RSET");
		break;

	case MTA_QUIT:
		mta_send(s, "QUIT");
		break;

	default:
		fatalx("mta_enter_state: unknown state");
	}
#undef mta_enter_state
}

/*
 * Handle a response to an SMTP command
 */
static void
mta_response(struct mta_session *s, char *line)
{
	struct envelope	       *evp;
	int			delivery;

	switch (s->state) {

	case MTA_BANNER:
		mta_enter_state(s, MTA_EHLO);
		break;

	case MTA_EHLO:
		if (line[0] != '2') {
			/* rejected at ehlo state */
			if ((s->flags & MTA_USE_AUTH) ||
			    (s->flags & MTA_WANT_SECURE)) {
				mta_error(s, "EHLO rejected: %s", line);
				mta_free(s);
				return;
			}
			mta_enter_state(s, MTA_HELO);
			return;
		}
		if (!(s->flags & MTA_FORCE_PLAIN))
			mta_enter_state(s, MTA_STARTTLS);
		else
			mta_enter_state(s, MTA_READY);
		break;

	case MTA_HELO:
		if (line[0] != '2') {
			mta_error(s, "HELO rejected: %s", line);
			mta_free(s);
			return;
		}
		mta_enter_state(s, MTA_READY);
		break;

	case MTA_STARTTLS:
		if (line[0] != '2') {
			if (!(s->flags & MTA_WANT_SECURE)) {
				mta_enter_state(s, MTA_AUTH);
				return;
			}
			/* XXX mark that the MX doesn't support STARTTLS */
			mta_error(s, "STARTTLS rejected: %s", line);
			mta_free(s);
			return;
		}

		mta_start_tls(s);
		break;

	case MTA_AUTH:
		if (line[0] != '2') {
			mta_error(s, "AUTH rejected: %s", line);
			mta_free(s);
			return;
		}
		mta_enter_state(s, MTA_READY);
		break;

	case MTA_MAIL:
		if (line[0] != '2') {
			if (line[0] == '5')
				delivery = IMSG_DELIVERY_PERMFAIL;
			else
				delivery = IMSG_DELIVERY_TEMPFAIL;
			mta_flush_task(s, delivery, line);
			mta_enter_state(s, MTA_RSET);
			return;
		}
		mta_enter_state(s, MTA_RCPT);
		break;

	case MTA_RCPT:
		evp = s->currevp;
		s->currevp = TAILQ_NEXT(s->currevp, entry);
		if (line[0] != '2') {
			if (line[0] == '5')
				delivery = IMSG_DELIVERY_PERMFAIL;
			else
				delivery = IMSG_DELIVERY_TEMPFAIL;
			TAILQ_REMOVE(&s->task->envelopes, evp, entry);
			envelope_set_errormsg(evp, "%s", line);
			mta_envelope_fail(evp, s->route, delivery);
			if (TAILQ_EMPTY(&s->task->envelopes)) {
				mta_flush_task(s, IMSG_DELIVERY_OK,
				    "No envelope");
				mta_enter_state(s, MTA_RSET);
				break;
			}
		}
		if (s->currevp == NULL)
			mta_enter_state(s, MTA_DATA);
		else
			mta_enter_state(s, MTA_RCPT);
		break;

	case MTA_DATA:
		if (line[0] == '2' || line[0] == '3') {
			mta_enter_state(s, MTA_BODY);
			break;
		}
		if (line[0] == '5')
			delivery = IMSG_DELIVERY_PERMFAIL;
		else
			delivery = IMSG_DELIVERY_TEMPFAIL;
		mta_flush_task(s, delivery, line);
		mta_enter_state(s, MTA_RSET);
		break;

	case MTA_EOM:
		if (line[0] == '2') {
			delivery = IMSG_DELIVERY_OK;
			s->msgcount++;
		}
		else if (line[0] == '5')
			delivery = IMSG_DELIVERY_PERMFAIL;
		else
			delivery = IMSG_DELIVERY_TEMPFAIL;
		mta_flush_task(s, delivery, line);
		mta_enter_state(s, MTA_READY);
		break;

	case MTA_RSET:
		mta_enter_state(s, MTA_READY);
		break;

	default:
		fatalx("mta_response() bad state");
	}
}

static void
mta_io(struct io *io, int evt)
{
	struct mta_session	*s = io->arg;
	char			*line, *msg;
	size_t			 len;
	const char		*error;
	int			 cont;
	const char		*schema;

	log_trace(TRACE_IO, "mta: %p: %s %s", s, io_strevent(evt),
	    io_strio(io));

	switch (evt) {

	case IO_CONNECTED:
		if (s->use_smtp_tls)
			schema = "smtp+tls://";
		else if (s->use_starttls)
			schema = "tls://";
		else if (s->use_smtps)
			schema = "smtps://";
		else
			schema = "smtp://";
		log_debug("debug: mta: %p: connected to %s%s (%s)",
		    s, schema, sa_to_text(s->route->dst->sa), s->route->dst->ptrname);

		if (s->use_smtps) {
			io_set_write(io);
			mta_start_tls(s);
		}
		else {
			mta_enter_state(s, MTA_BANNER);
			io_set_read(io);
		}
		break;

	case IO_TLSREADY:
		log_info("relay: Started TLS on session %016"PRIx64": %s",
		    s->id, ssl_to_text(s->io.ssl));
		s->flags |= MTA_TLS;

		if (mta_verify_certificate(s)) {
			io_pause(&s->io, IO_PAUSE_IN);
			break;
		}

	case IO_TLSVERIFIED:
		if (SSL_get_peer_certificate(s->io.ssl))
			log_info("relay: Server certificate verification %s "
			    "on session %016"PRIx64,
			    (s->flags & MTA_VERIFIED) ? "succeeded" : "failed",
			    s->id);

		if (s->use_smtps) {
			mta_enter_state(s, MTA_BANNER);
			io_set_read(io);
		}
		else
			mta_enter_state(s, MTA_EHLO);
		break;

	case IO_DATAIN:
	    nextline:
		line = iobuf_getline(&s->iobuf, &len);
		if (line == NULL) {
			if (iobuf_len(&s->iobuf) >= SMTP_LINE_MAX) {
				mta_error(s, "Input too long");
				mta_free(s);
				return;
			}
			iobuf_normalize(&s->iobuf);
			break;
		}

		log_trace(TRACE_MTA, "mta: %p: <<< %s", s, line);

		if ((error = parse_smtp_response(line, len, &msg, &cont))) {
			mta_error(s, "Bad response: %s", error);
			mta_free(s);
			return;
		}

		/* read extensions */
		if (s->state == MTA_EHLO) {
			if (strcmp(msg, "STARTTLS") == 0)
				s->ext |= MTA_EXT_STARTTLS;
			else if (strncmp(msg, "AUTH", 4) == 0)
				s->ext |= MTA_EXT_AUTH;
			else if (strcmp(msg, "PIPELINING") == 0)
				s->ext |= MTA_EXT_PIPELINING;
		}

		if (cont)
			goto nextline;

		if (s->state == MTA_QUIT) {
			mta_free(s);
			return;
		}

		io_set_write(io);
		mta_response(s, line);
		iobuf_normalize(&s->iobuf);

		if (iobuf_len(&s->iobuf)) {
			log_debug("debug: mta: remaining data in input buffer");
			mta_error(s, "Remote host sent too much data");
			mta_free(s);
		}
		break;

	case IO_LOWAT:
		if (s->state == MTA_BODY)
			mta_enter_state(s, MTA_BODY);

		if (iobuf_queued(&s->iobuf) == 0)
			io_set_read(io);
		break;

	case IO_TIMEOUT:
		log_debug("debug: mta: %p: connection timeout", s);
		mta_error(s, "Connection timeout");
		if (!s->ready)
			mta_connect(s);
		else
			mta_free(s);
		break;

	case IO_ERROR:
		log_debug("debug: mta: %p: IO error: %s", s, io->error);
		mta_error(s, "IO Error: %s", io->error);
		if (!s->ready)
			mta_connect(s);
		else
			mta_free(s);
		break;

	case IO_DISCONNECTED:
		log_debug("debug: mta: %p: disconnected in state %s",
		    s, mta_strstate(s->state));
		mta_error(s, "Connection closed unexpectedly");
		if (!s->ready)
			mta_connect(s);
		else
			mta_free(s);
		break;

	default:
		fatalx("mta_io() bad event");
	}
}

static void
mta_send(struct mta_session *s, char *fmt, ...)
{
	va_list  ap;
	char	*p;
	int	 len;

	va_start(ap, fmt);
	if ((len = vasprintf(&p, fmt, ap)) == -1)
		fatal("mta: vasprintf");
	va_end(ap);

	log_trace(TRACE_MTA, "mta: %p: >>> %s", s, p);

	iobuf_xfqueue(&s->iobuf, "mta_send", "%s\r\n", p);

	free(p);
}

/*
 * Queue some data into the input buffer
 */
static ssize_t
mta_queue_data(struct mta_session *s)
{
	char	*ln;
	size_t	 len, q;

	q = iobuf_queued(&s->iobuf);

	while (iobuf_queued(&s->iobuf) < MTA_HIWAT) {
		if ((ln = fgetln(s->datafp, &len)) == NULL)
			break;
		if (ln[len - 1] == '\n')
			ln[len - 1] = '\0';
		iobuf_xfqueue(&s->iobuf, "mta_queue_data", "%s%s\r\n",
		    *ln == '.' ? "." : "", ln);
	}

	if (ferror(s->datafp)) {
		mta_flush_task(s, IMSG_DELIVERY_TEMPFAIL,
		    "Error reading content file");
		return (-1);
	}

	if (feof(s->datafp)) {
		fclose(s->datafp);
		s->datafp = NULL;
	}

	return (iobuf_queued(&s->iobuf) - q);
}

static void
mta_flush_task(struct mta_session *s, int delivery, const char *error)
{
	struct envelope	*e;
	const char	*pfx;
	char		 relay[MAX_LINE_SIZE];
	size_t		 n;

	switch (delivery) {
	case IMSG_DELIVERY_OK:
		pfx = "Ok";
		break;
	case IMSG_DELIVERY_TEMPFAIL:
		pfx = "TempFail";
		break;
	case IMSG_DELIVERY_PERMFAIL:
	case IMSG_DELIVERY_LOOP:
		pfx = "PermFail";
		break;
	default:
		errx(1, "unexpected delivery status %i", delivery);
	}

	snprintf(relay, sizeof relay, "relay=%s, ",
	    mta_host_to_text(s->route->dst));

	n = 0;
	while ((e = TAILQ_FIRST(&s->task->envelopes))) {
		TAILQ_REMOVE(&s->task->envelopes, e, entry);
		envelope_set_errormsg(e, "%s", error);
		log_envelope(e, relay, pfx, error);
		m_compose(p_queue, delivery, 0, 0, -1, e, sizeof(*e));
		free(e);
		n++;
	}

	free(s->task);
	s->task = NULL;

	stat_decrement("mta.envelope", n);
	stat_decrement("mta.task.running", 1);
	stat_decrement("mta.task", 1);
}

static void
mta_envelope_fail(struct envelope *evp, struct mta_route *route, int delivery)
{
	char relay[MAX_LINE_SIZE], stat[MAX_LINE_SIZE];
	const char *pfx;

	if (delivery == IMSG_DELIVERY_TEMPFAIL)
		pfx = "TempFail";
	else
		pfx = "PermFail";

	snprintf(relay, sizeof relay, "relay=%s, ",
	    mta_host_to_text(route->dst));

	snprintf(stat, sizeof stat, "RemoteError (%s)", &evp->errorline[4]);
	log_envelope(evp, relay, pfx, stat);
	m_compose(p_queue, delivery, 0, 0, -1, evp, sizeof(*evp));
}

static void
mta_error(struct mta_session *s, const char *fmt, ...)
{
	va_list  ap;
	char	*error;
	int	 len;

	/*
	 * If not connected yet, and the error is not local, just ignore it
	 * and try to reconnect.
	 */
	if (s->state == MTA_INIT && 
	    (errno == ETIMEDOUT || errno == ECONNREFUSED))
		return;

	va_start(ap, fmt);
	if ((len = vasprintf(&error, fmt, ap)) == -1)
		fatal("mta: vasprintf");
	va_end(ap);

	mta_route_error(s->relay, s->route, error);

	if (s->task)
		mta_flush_task(s, IMSG_DELIVERY_TEMPFAIL, error);

	free(error);
}

static int
mta_check_loop(FILE *fp)
{
	char	*buf, *lbuf;
	size_t	 len;
	uint32_t rcvcount = 0;
	int	 ret = 0;

	lbuf = NULL;
	while ((buf = fgetln(fp, &len))) {
		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		else {
			/* EOF without EOL, copy and add the NUL */
			lbuf = xmalloc(len + 1, "mta_check_loop");
			memcpy(lbuf, buf, len);
			lbuf[len] = '\0';
			buf = lbuf;
		}

		if (strchr(buf, ':') == NULL && !isspace((int)*buf))
			break;

		if (strncasecmp("Received: ", buf, 10) == 0) {
			rcvcount++;
			if (rcvcount == MAX_HOPS_COUNT) {
				ret = 1;
				break;
			}
		}
		if (lbuf) {
			free(lbuf);
			lbuf  = NULL;
		}
	}
	if (lbuf)
		free(lbuf);

	fseek(fp, SEEK_SET, 0);
	return ret;
}

static void
mta_start_tls(struct mta_session *s)
{
	struct ca_cert_req_msg	req_ca_cert;
	void		       *ssl;

	if (s->relay->cert) {
		req_ca_cert.reqid = s->id;
		strlcpy(req_ca_cert.name, s->relay->cert,
		    sizeof req_ca_cert.name);
		m_compose(p_lka, IMSG_LKA_SSL_INIT, 0, 0, -1,
		    &req_ca_cert, sizeof(req_ca_cert));
		tree_xset(&wait_ssl_init, s->id, s);
		return;
	}
	ssl = ssl_mta_init(NULL, 0, NULL, 0);
	if (ssl == NULL)
		fatal("mta: ssl_mta_init");
	io_start_tls(&s->io, ssl);
}

static int
mta_verify_certificate(struct mta_session *s)
{
	struct ca_vrfy_req_msg	req_ca_vrfy;
	struct iovec		iov[2];
	X509		       *x;
	STACK_OF(X509)	       *xchain;
	int			i;

	x = SSL_get_peer_certificate(s->io.ssl);
	if (x == NULL)
		return 0;
	xchain = SSL_get_peer_cert_chain(s->io.ssl);

	/*
	 * Client provided a certificate and possibly a certificate chain.
	 * SMTP can't verify because it does not have the information that
	 * it needs, instead it will pass the certificate and chain to the
	 * lookup process and wait for a reply.
	 *
	 */

	tree_xset(&wait_ssl_verify, s->id, s);

	/* Send the client certificate */
	bzero(&req_ca_vrfy, sizeof req_ca_vrfy);
	req_ca_vrfy.reqid = s->id;
	req_ca_vrfy.cert_len = i2d_X509(x, &req_ca_vrfy.cert);
	if (xchain)
		req_ca_vrfy.n_chain = sk_X509_num(xchain);
	iov[0].iov_base = &req_ca_vrfy;
	iov[0].iov_len = sizeof(req_ca_vrfy);
	iov[1].iov_base = req_ca_vrfy.cert;
	iov[1].iov_len = req_ca_vrfy.cert_len;
	m_composev(p_lka, IMSG_LKA_SSL_VERIFY_CERT, 0, 0, -1,
	    iov, nitems(iov));
	free(req_ca_vrfy.cert);

	if (xchain) {		
		/* Send the chain, one cert at a time */
		for (i = 0; i < sk_X509_num(xchain); ++i) {
			bzero(&req_ca_vrfy, sizeof req_ca_vrfy);
			req_ca_vrfy.reqid = s->id;
			x = sk_X509_value(xchain, i);
			req_ca_vrfy.cert_len = i2d_X509(x, &req_ca_vrfy.cert);
			iov[0].iov_base = &req_ca_vrfy;
			iov[0].iov_len  = sizeof(req_ca_vrfy);
			iov[1].iov_base = req_ca_vrfy.cert;
			iov[1].iov_len  = req_ca_vrfy.cert_len;
			m_composev(p_lka, IMSG_LKA_SSL_VERIFY_CHAIN, 0, 0, -1,
			    iov, nitems(iov));
			free(req_ca_vrfy.cert);
		}
	}

	/* Tell lookup process that it can start verifying, we're done */
	bzero(&req_ca_vrfy, sizeof req_ca_vrfy);
	req_ca_vrfy.reqid = s->id;
	m_compose(p_lka, IMSG_LKA_SSL_VERIFY, 0, 0, -1,
	    &req_ca_vrfy, sizeof req_ca_vrfy);

	return 1;
}

#define CASE(x) case x : return #x

static const char *
mta_strstate(int state)
{
	switch (state) {
	CASE(MTA_INIT);
	CASE(MTA_BANNER);
	CASE(MTA_EHLO);
	CASE(MTA_HELO);
	CASE(MTA_STARTTLS);
	CASE(MTA_AUTH);
	CASE(MTA_READY);
	CASE(MTA_MAIL);
	CASE(MTA_RCPT);
	CASE(MTA_DATA);
	CASE(MTA_BODY);
	CASE(MTA_EOM);
	CASE(MTA_RSET);
	CASE(MTA_QUIT);
	default:
		return "MTA_???";
	}
}
