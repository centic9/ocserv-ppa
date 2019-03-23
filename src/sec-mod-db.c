/*
 * Copyright (C) 2014 Red Hat
 * Copyright (C) 2017 Nikos Mavrogiannopoulos
 *
 * This file is part of ocserv.
 *
 * ocserv is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * ocserv is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <system.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <common.h>
#include <syslog.h>
#include <vpn.h>
#include <base64-helper.h>
#include <tlslib.h>
#include <sec-mod.h>
#include <ccan/hash/hash.h>
#include <ccan/htable/htable.h>

#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>

static size_t rehash(const void *_e, void *unused)
{
	const client_entry_st *e = _e;

	return hash_any(e->sid, sizeof(e->sid), 0);
}

void *sec_mod_client_db_init(sec_mod_st *sec)
{
	struct htable *db = talloc(sec, struct htable);
	if (db == NULL)
		return NULL;

	htable_init(db, rehash, NULL);
	sec->client_db = db;

	return db;
}

void sec_mod_client_db_deinit(sec_mod_st *sec)
{
struct htable *db = sec->client_db;

	htable_clear(db);
	talloc_free(db);
}

/* The number of elements */
unsigned sec_mod_client_db_elems(sec_mod_st *sec)
{
struct htable *db = sec->client_db;

	if (db)
		return db->elems;
	else
		return 0;
}

client_entry_st *new_client_entry(sec_mod_st *sec, struct vhost_cfg_st *vhost, const char *ip, unsigned pid)
{
	struct htable *db = sec->client_db;
	client_entry_st *e, *te;
	int ret;
	int retries = 3;
	time_t now;

	e = talloc_zero(db, client_entry_st);
	if (e == NULL) {
		return NULL;
	}

	strlcpy(e->acct_info.remote_ip, ip, sizeof(e->acct_info.remote_ip));
	e->acct_info.id = pid;
	e->vhost = vhost;

	do {
		ret = gnutls_rnd(GNUTLS_RND_RANDOM, e->sid, sizeof(e->sid));
		if (ret < 0) {
			seclog(sec, LOG_ERR, "error generating SID");
			goto fail;
		}

		/* check if in use */
		te = find_client_entry(sec, e->sid);
	} while(te != NULL && retries-- >= 0);

	if (te != NULL) {
		seclog(sec, LOG_ERR,
		       "could not generate a unique SID!");
		goto fail;
	}

	calc_safe_id(e->sid, SID_SIZE, (char *)e->acct_info.safe_id, sizeof(e->acct_info.safe_id));
	now = time(0);
	e->exptime = now + vhost->perm_config.config->cookie_timeout + AUTH_SLACK_TIME;
	e->created = now;

	if (htable_add(db, rehash(e, NULL), e) == 0) {
		seclog(sec, LOG_ERR,
		       "could not add client entry to hash table");
		goto fail;
	}

	return e;

 fail:
	talloc_free(e);
	return NULL;
}

static bool client_entry_cmp(const void *_c1, void *_c2)
{
	const struct client_entry_st *c1 = _c1;
	struct client_entry_st *c2 = _c2;

	if (memcmp(c1->sid, c2->sid, SID_SIZE) == 0)
		return 1;
	return 0;
}

client_entry_st *find_client_entry(sec_mod_st *sec, uint8_t sid[SID_SIZE])
{
	struct htable *db = sec->client_db;
	client_entry_st t;

	memcpy(t.sid, sid, SID_SIZE);

	return htable_get(db, rehash(&t, NULL), client_entry_cmp, &t);
}

static void clean_entry(sec_mod_st *sec, client_entry_st * e)
{
	sec_auth_user_deinit(sec, e);
	talloc_free(e->msg_str);
	talloc_free(e);
}

void cleanup_client_entries(sec_mod_st *sec)
{
	struct htable *db = sec->client_db;
	client_entry_st *t;
	struct htable_iter iter;
	time_t now = time(0);

	t = htable_first(db, &iter);
	while (t != NULL) {
		if IS_CLIENT_ENTRY_EXPIRED_FULL(sec, t, now, 1) {
			htable_delval(db, &iter);
			clean_entry(sec, t);
		}
		t = htable_next(db, &iter);

	}
}

void del_client_entry(sec_mod_st *sec, client_entry_st * e)
{
	struct htable *db = sec->client_db;

	htable_del(db, rehash(e, NULL), e);
	clean_entry(sec, e);
}

void expire_client_entry(sec_mod_st *sec, client_entry_st * e)
{
	time_t now;

	if (e->in_use > 0)
		e->in_use--;
	if (e->in_use == 0) {
		if (e->vhost->perm_config.config->persistent_cookies == 0 && (e->discon_reason == REASON_SERVER_DISCONNECT ||
		    e->discon_reason == REASON_SESSION_TIMEOUT)) {
			seclog(sec, LOG_INFO, "invalidating session of user '%s' "SESSION_STR,
			       e->acct_info.username, e->acct_info.safe_id);
			/* immediately disconnect the user */
			del_client_entry(sec, e);
		} else {
			now = time(0);
			/* We intentionally don't close the session immediatelly on
			 * REASON_USER_DISCONNECT, as some anyconect clients
			 * explicitly disconnect with the intention to reconnect
			 * seconds later. */
			if (e->discon_reason == REASON_USER_DISCONNECT) {
				if (!e->vhost->perm_config.config->persistent_cookies || (now+AUTH_SLACK_TIME >= e->exptime))
					e->exptime = now + AUTH_SLACK_TIME;
			} else {
				e->exptime = now + e->vhost->perm_config.config->cookie_timeout + AUTH_SLACK_TIME;
			}
			seclog(sec, LOG_INFO, "temporarily closing session for %s "SESSION_STR, e->acct_info.username, e->acct_info.safe_id);
		}
	}
}
