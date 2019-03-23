/*
 * Copyright (C) 2014, 2015 Red Hat
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
#include <tlslib.h>
#include <main.h>
#include <main-ban.h>
#include <arpa/inet.h>
#include <ccan/hash/hash.h>
#include <ccan/htable/htable.h>

static size_t rehash(const void *_e, void *unused)
{
	ban_entry_st *e = (void*)_e;
	return hash_any(e->ip.ip, e->ip.size, 0);
}

/* The first argument is the entry from the hash, and
 * the second is the entry from check_if_banned().
 */
static bool ban_entry_cmp(const void *_c1, void *_c2)
{
	const struct ban_entry_st *c1 = _c1;
	struct ban_entry_st *c2 = _c2;

	if (c1->ip.size == c2->ip.size && memcmp(c1->ip.ip, c2->ip.ip, c1->ip.size) == 0)
		return 1;
	return 0;
}


void *main_ban_db_init(main_server_st *s)
{
	struct htable *db = talloc(s, struct htable);
	if (db == NULL) {
		fprintf(stderr, "error initializing ban DB\n");
		exit(1);
	}

	htable_init(db, rehash, NULL);
	s->ban_db = db;

	return db;
}

void main_ban_db_deinit(main_server_st *s)
{
struct htable *db = s->ban_db;

	if (db != NULL) {
		htable_clear(db);
		talloc_free(db);
	}
}

unsigned main_ban_db_elems(main_server_st *s)
{
struct htable *db = s->ban_db;

	if (db)
		return db->elems;
	else
		return 0;
}

static void massage_ipv6_address(ban_entry_st *t)
{
	if (t->ip.size == 16) {
		memset(&t->ip.ip[8], 0, 8);
	}
}

/* returns -1 if the user is already banned, and zero otherwise */
static
int add_ip_to_ban_list(main_server_st *s, const unsigned char *ip, unsigned ip_size, unsigned score)
{
	struct htable *db = s->ban_db;
	struct ban_entry_st *e;
	ban_entry_st t;
	time_t now = time(0);
	time_t expiration = now + GETCONFIG(s)->min_reauth_time;
	int ret = 0;
	char str_ip[MAX_IP_STR];
	const char *p_str_ip = NULL;
	unsigned print_msg;

	if (db == NULL || GETCONFIG(s)->max_ban_score == 0 || ip == NULL || (ip_size != 4 && ip_size != 16))
		return 0;

	memcpy(t.ip.ip, ip, ip_size);
	t.ip.size = ip_size;

	/* In IPv6 treat a /64 as a single address */
	massage_ipv6_address(&t);

	e = htable_get(db, rehash(&t, NULL), ban_entry_cmp, &t);
	if (e == NULL) { /* new entry */
		e = talloc_zero(db, ban_entry_st);
		if (e == NULL) {
			return 0;
		}

		memcpy(&e->ip, &t.ip, sizeof(e->ip));
		e->last_reset = now;

		if (htable_add(db, rehash(e, NULL), e) == 0) {
			mslog(s, NULL, LOG_INFO,
			       "could not add ban entry to hash table");
			goto fail;
		}
	} else {
		if (now > e->last_reset + GETCONFIG(s)->ban_reset_time) {
			e->score = 0;
			e->last_reset = now;
		}
	}

	/* if the user is already banned, don't increase the expiration time
	 * on further attempts, or the user will never be unbanned if he
	 * periodically polls the server */
	if (e->score < GETCONFIG(s)->max_ban_score) {
		e->expires = expiration;
		print_msg = 0;
	} else
		print_msg = 1;
	e->score += score;

	if (ip_size == 4)
		p_str_ip = inet_ntop(AF_INET, ip, str_ip, sizeof(str_ip));
	else
		p_str_ip = inet_ntop(AF_INET6, ip, str_ip, sizeof(str_ip));

	if (GETCONFIG(s)->max_ban_score > 0 && e->score >= GETCONFIG(s)->max_ban_score) {
		if (print_msg && p_str_ip) {
			mslog(s, NULL, LOG_INFO, "added IP '%s' (with score %d) to ban list, will be reset at: %s", str_ip, e->score, ctime(&e->expires));
		}
		ret = -1;
	} else {
		if (p_str_ip) {
			mslog(s, NULL, LOG_DEBUG, "added %d points (total %d) for IP '%s' to ban list", score, e->score, str_ip);
		}
		ret = 0;
	}

	return ret;
 fail:
	talloc_free(e);
	return ret;
}

int add_str_ip_to_ban_list(main_server_st *s, const char *ip, unsigned score)
{
	struct htable *db = s->ban_db;
	ban_entry_st t;
	int ret = 0;

	if (db == NULL || GETCONFIG(s)->max_ban_score == 0 || ip == NULL || ip[0] == 0)
		return 0;

	if (strchr(ip, ':') != 0) {
		ret = inet_pton(AF_INET6, ip, t.ip.ip);
		t.ip.size = 16;
	} else {
		ret = inet_pton(AF_INET, ip, t.ip.ip);
		t.ip.size = 4;
	}
	if (ret != 1) {
		mslog(s, NULL, LOG_INFO,
		       "could not read IP: %s", ip);
		return 0;
	}

	return add_ip_to_ban_list(s, t.ip.ip, t.ip.size, score);
}

/* returns non-zero if there is an IP removed */
int remove_ip_from_ban_list(main_server_st *s, const uint8_t *ip, unsigned size)
{
	struct htable *db = s->ban_db;
	struct ban_entry_st *e;
	ban_entry_st t;
	char txt_ip[MAX_IP_STR];

	if (db == NULL || ip == NULL || size == 0)
		return 0;

	if (size == 4 || size == 16) {
		if (inet_ntop(size==16?AF_INET6:AF_INET, ip, txt_ip, sizeof(txt_ip)) != NULL) {
			mslog(s, NULL, LOG_INFO,
				      "unbanning IP '%s'", txt_ip);
		}

		t.ip.size = size;
		memcpy(&t.ip.ip, ip, size);

		/* In IPv6 treat a /64 as a single address */
		massage_ipv6_address(&t);

		e = htable_get(db, rehash(&t, NULL), ban_entry_cmp, &t);
		if (e != NULL) { /* new entry */
			e->score = 0;
			e->expires = 0;
			return 1;
		}
	}

	return 0;
}

unsigned check_if_banned(main_server_st *s, struct sockaddr_storage *addr, socklen_t addr_size)
{
	struct htable *db = s->ban_db;
	time_t now;
	ban_entry_st t, *e;
	unsigned in_size;
	char txt[MAX_IP_STR];

	if (db == NULL || GETCONFIG(s)->max_ban_score == 0)
		return 0;

	in_size = SA_IN_SIZE(addr_size);
	if (in_size != 4 && in_size != 16) {
	    	mslog(s, NULL, LOG_ERR, "unknown address type for %s", human_addr2((struct sockaddr*)addr, addr_size, txt, sizeof(txt), 0));
		return 0;
	}

	memcpy(t.ip.ip, SA_IN_P_GENERIC(addr, addr_size), SA_IN_SIZE(addr_size));
	t.ip.size = SA_IN_SIZE(addr_size);

	/* In IPv6 treat a /64 as a single address */
	massage_ipv6_address(&t);

	/* add its current connection points */
	add_ip_to_ban_list(s, t.ip.ip, t.ip.size, GETCONFIG(s)->ban_points_connect);

	now = time(0);
	e = htable_get(db, rehash(&t, NULL), ban_entry_cmp, &t);
	if (e != NULL) {
		if (now > e->expires)
			return 0;

		if (e->score >= GETCONFIG(s)->max_ban_score) {
		    	mslog(s, NULL, LOG_INFO, "rejected connection from banned IP: %s", human_addr2((struct sockaddr*)addr, addr_size, txt, sizeof(txt), 0));
			return 1;
		}
	}
	return 0;
}

void cleanup_banned_entries(main_server_st *s)
{
	struct htable *db = s->ban_db;
	ban_entry_st *t;
	struct htable_iter iter;
	time_t now = time(0);

	if (db == NULL)
		return;

	t = htable_first(db, &iter);
	while (t != NULL) {
		if (now >= t->expires && now > t->last_reset + GETCONFIG(s)->ban_reset_time) {
			htable_delval(db, &iter);
			talloc_free(t);
		}
		t = htable_next(db, &iter);

	}
}

