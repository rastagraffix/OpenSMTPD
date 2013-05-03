/*	$OpenBSD: scheduler_null.c,v 1.1 2013/01/26 09:37:23 gilles Exp $	*/

/*
 * Copyright (c) 2013 Eric Faurot <eric@openbsd.org>
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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <ctype.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

struct table_proc_priv {
	pid_t		pid;
	int		running;
	struct imsgbuf	ibuf;
};

static void *table_proc_open(struct table *);
static int table_proc_update(struct table *);
static void table_proc_close(void *);
static int table_proc_lookup(void *, const char *, enum table_service, union lookup *);
static int table_proc_fetch(void *, enum table_service, union lookup *);
static int table_proc_call(struct table_proc_priv *, size_t);

struct table_backend table_backend_proc = {
	K_ANY,
	NULL,
	table_proc_open,
	table_proc_update,
	table_proc_close,
	table_proc_lookup,
	table_proc_fetch,
};

static struct imsg imsg;

extern char	**environ;

static void *
table_proc_open(struct table *table)
{
	int			 sp[2];
	uint32_t		 version;
	struct table_proc_priv	*priv;
	char			*environ_new[2];

	errno = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, sp) < 0) {
		log_warn("warn: table-proc: socketpair");
		return (NULL);
	}
	priv = calloc(1, sizeof(*priv));

	if ((priv->pid = fork()) == -1) {
		log_warn("warn: table-proc: fork");
		goto err;
	}

	if (priv->pid == 0) {
		/* child process */
		dup2(sp[0], STDIN_FILENO);
		if (closefrom(STDERR_FILENO + 1) < 0)
			exit(1);

		environ_new[0] = "PATH=" _PATH_DEFPATH;
		environ_new[1] = (char *)NULL;
		environ = environ_new;
		execle("/bin/sh", "/bin/sh", "-c", table->t_config, (char *)NULL,
		    environ_new);
		fatal("execl");
	}

	/* parent process */
	close(sp[0]);
	imsg_init(&priv->ibuf, sp[1]);
	priv->running = 1;

	version = PROC_TABLE_API_VERSION;
	imsg_compose(&priv->ibuf, PROC_TABLE_OPEN, 0, 0, -1,
	    &version, sizeof(version));
	if (!table_proc_call(priv, 0))
		return (NULL); 	/* XXX cleanup */

	return (priv);
err:
	close(sp[0]);
	close(sp[1]);
	return (NULL);
}

static int
table_proc_update(struct table *table)
{
	struct table_proc_priv	*priv = table->t_handle;
	int r;

	if (!priv->running) {
		log_warnx("warn: table-proc: not running");
		return (-1);
	}

	imsg_compose(&priv->ibuf, PROC_TABLE_UPDATE, 0, 0, -1, NULL, 0);

	if (!table_proc_call(priv, sizeof(r)))
		return (-1);

	memmove(&r, imsg.data, sizeof(r));
	imsg_free(&imsg);

	return (r);
}

static void
table_proc_close(void *arg)
{
	struct table_proc_priv	*priv = arg;

	if (!priv->running) {
		log_warnx("warn: table-proc: not running");
		return;
	}

	imsg_compose(&priv->ibuf, PROC_TABLE_CLOSE, 0, 0, -1, NULL, 0);
	imsg_flush(&priv->ibuf);
}

static int
table_proc_lookup(void *arg, const char *k, enum table_service s,
    union lookup *lk)
{
	struct table_proc_priv	*priv = arg;
	struct ibuf		*buf;
	size_t			 len;
	char			*data;
	int			 r, msg;

	if (!priv->running) {
		log_warnx("warn: table-proc: not running");
		return (-1);
	}

	len = sizeof(s);
	if (k) {
		len += strlen(k) + 1;
		if (lk)
			msg = PROC_TABLE_LOOKUP;
		else
			msg = PROC_TABLE_CHECK;
	}
	else {
		if (!lk)
			return (-1);
		msg = PROC_TABLE_FETCH;
	}

	buf = imsg_create(&priv->ibuf, msg, 0, 0, len);
	imsg_add(buf, &s, sizeof(s));
	if (k)
		imsg_add(buf, k, strlen(k) + 1);
	imsg_close(&priv->ibuf, buf);

	if (!table_proc_call(priv, -1))
		return (-1);

	len = imsg.hdr.len - IMSG_HEADER_SIZE;
	data = imsg.data;

	if (len < sizeof(r)) {
		r = -1;
		goto end;
	}

	memmove(&r, data, sizeof(r));
	data += sizeof(r);
	len -= sizeof(r);

	if (len != 0 && (r != 1 || lk == NULL))
		log_warnx("warn: table-proc: unexpected payload in lookup pkt: %zu", len);

	if (r == 1 && lk) {
		if (len == 0) {
			r = -1;
			log_warnx("warn: table-proc: empty payload in lookup pkt");
		}
		else if (data[len-1] != '\0') {
			r = -1;
			log_warnx("warn: table-proc: payload doesn't end with NUL");
		} else
			r = table_parse_lookup(s, k, data, lk);
	}

    end:
	imsg_free(&imsg);
	return (r);
}

static int
table_proc_fetch(void *arg, enum table_service s, union lookup *lk)
{
	return table_proc_lookup(arg, NULL, s, lk);
}

static int
table_proc_call(struct table_proc_priv *priv, size_t expected)
{
	ssize_t	n;
	size_t	len;

	if (imsg_flush(&priv->ibuf) == -1) {
		log_warn("warn: table-proc: imsg_flush");
		imsg_clear(&priv->ibuf);
		priv->running = 0;
		return (0);
	}

	while (1) {
		if ((n = imsg_get(&priv->ibuf, &imsg)) == -1) {
			log_warn("warn: table-proc: imsg_get");
			break;
		}
		if (n) {
			len = imsg.hdr.len - IMSG_HEADER_SIZE;

			if (imsg.hdr.type == PROC_TABLE_OK) {
				if (expected == (size_t)-1 || len == expected)
					return (1);
				imsg_free(&imsg);
				log_warnx("warn: table-proc: "
				    "bad msg length (%i/%i)",
				    (int)len, (int)expected);
				break;
			}

			log_warnx("warn: table-proc: bad response");
			break;
		}

		if ((n = imsg_read(&priv->ibuf)) == -1) {
			log_warn("warn: table-proc: imsg_read");
			break;
		}

		if (n == 0) {
			log_warnx("warn: table-proc: pipe closed");
			break;
		}
	}

	log_warnx("table-proc: not running anymore");
	imsg_clear(&priv->ibuf);
	priv->running = 0;
	return (0);
}
