/*
 * Copyright (C) 2013-2016 Nikos Mavrogiannopoulos
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
/* for recvmsg */
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <limits.h>
#include <nettle/sha1.h>
#include "common.h"
#include "defs.h"
#include "common/base64-helper.h"

const char *_vhost_prefix(const char *name)
{
	static char tmp[128];

	snprintf(tmp, sizeof(tmp), "vhost:%s: ", name);
	return tmp;
}

/* A hash of the input, to a 20-byte output. The goal is one-wayness.
 */
static void safe_hash(const uint8_t *data, unsigned data_size, uint8_t output[20])
{
	struct sha1_ctx ctx;

	sha1_init(&ctx);

	sha1_update(&ctx, data_size, data);
	sha1_digest(&ctx, 20, output);
}


char *calc_safe_id(const uint8_t *data, unsigned size, char *output, unsigned output_size)
{
	uint8_t safe_id[20];

	safe_hash(data, size, safe_id);
	oc_base64_encode((char*)safe_id, 20, output, output_size);

	return output;
}

/* Note that meaning slightly changes depending on whether we are
 * referring to the cookie or the session itself.
 */
const char *ps_status_to_str(int status, unsigned cookie)
{
	switch (status) {
		case PS_AUTH_COMPLETED:
			if (cookie)
				return "authenticated";
			else
				return "connected";
		case PS_AUTH_INIT:
		case PS_AUTH_CONT:
			return "authenticating";
		case PS_AUTH_INACTIVE:
			return "pre-auth";
		case PS_AUTH_FAILED:
			return "auth failed";
		default:
			return "unknown";
	}
}

const char *cmd_request_to_str(unsigned _cmd)
{
	cmd_request_t cmd = _cmd;
	static char tmp[32];

	switch (cmd) {
	case AUTH_COOKIE_REP:
		return "auth cookie reply";
	case AUTH_COOKIE_REQ:
		return "auth cookie request";
	case RESUME_STORE_REQ:
		return "resume data store request";
	case RESUME_DELETE_REQ:
		return "resume data delete request";
	case RESUME_FETCH_REQ:
		return "resume data fetch request";
	case RESUME_FETCH_REP:
		return "resume data fetch reply";
	case CMD_UDP_FD:
		return "udp fd";
	case CMD_TUN_MTU:
		return "tun mtu change";
	case CMD_TERMINATE:
		return "terminate";
	case CMD_SESSION_INFO:
		return "session info";
	case CMD_BAN_IP:
		return "ban IP";
	case CMD_BAN_IP_REPLY:
		return "ban IP reply";

	case CMD_SEC_CLI_STATS:
		return "sm: worker cli stats";
	case CMD_SECM_CLI_STATS:
		return "sm: main cli stats";
	case CMD_SEC_AUTH_INIT:
		return "sm: auth init";
	case CMD_SEC_AUTH_CONT:
		return "sm: auth cont";
	case CMD_SEC_AUTH_REPLY:
		return "sm: auth rep";
	case CMD_SEC_DECRYPT:
		return "sm: decrypt";
	case CMD_SEC_SIGN:
		return "sm: sign";
	case CMD_SECM_STATS:
		return "sm: stats";
	case CMD_SECM_SESSION_CLOSE:
		return "sm: session close";
	case CMD_SECM_SESSION_OPEN:
		return "sm: session open";
	case CMD_SECM_SESSION_REPLY:
		return "sm: session reply";
	case CMD_SECM_BAN_IP:
		return "sm: ban IP";
	case CMD_SECM_BAN_IP_REPLY:
		return "sm: ban IP reply";
	case CMD_SECM_RELOAD:
		return "sm: reload";
	case CMD_SECM_RELOAD_REPLY:
		return "sm: reload reply";
	case CMD_SECM_LIST_COOKIES:
		return "sm: list cookies";
	case CMD_SECM_LIST_COOKIES_REPLY:
		return "sm: list cookies reply";
	default:
		snprintf(tmp, sizeof(tmp), "unknown (%u)", _cmd);
		return tmp;
	}
}

const char *discon_reason_to_str(unsigned reason)
{
	static char tmp[32];

	switch (reason) {
	case 0:
	case REASON_ANY:
		return "unspecified";
	case REASON_USER_DISCONNECT:
		return "user disconnected";
	case REASON_SERVER_DISCONNECT:
		return "server disconnected";
	case REASON_IDLE_TIMEOUT:
		return "idle timeout";
	case REASON_DPD_TIMEOUT:
		return "DPD timeout";
	case REASON_ERROR:
		return "unspecified error";
	case REASON_SESSION_TIMEOUT:
		return "session timeout";
	default:
		snprintf(tmp, sizeof(tmp), "unknown (%u)", reason);
		return tmp;
	}
}

ssize_t force_write(int sockfd, const void *buf, size_t len)
{
	int left = len;
	int ret;
	const uint8_t *p = buf;

	while (left > 0) {
		ret = write(sockfd, p, left);
		if (ret == -1) {
			if (errno != EAGAIN && errno != EINTR)
				return ret;
			else
				ms_sleep(50);
		}

		if (ret > 0) {
			left -= ret;
			p += ret;
		}
	}

	return len;
}

ssize_t force_read(int sockfd, void *buf, size_t len)
{
	int left = len;
	int ret;
	uint8_t *p = buf;

	while (left > 0) {
		ret = read(sockfd, p, left);
		if (ret == -1) {
			if (errno != EAGAIN && errno != EINTR)
				return ret;
		} else if (ret == 0 && left != 0) {
			errno = ENOENT;
			return -1;
		}

		if (ret > 0) {
			left -= ret;
			p += ret;
		}
	}

	return len;
}

ssize_t force_read_timeout(int sockfd, void *buf, size_t len, unsigned sec)
{
	int left = len;
	int ret;
	uint8_t *p = buf;
	struct pollfd pfd;

	while (left > 0) {
		if (sec > 0) {
			pfd.fd = sockfd;
			pfd.events = POLLIN;
			pfd.revents = 0;

			do {
				ret = poll(&pfd, 1, sec * 1000);
			} while (ret == -1 && errno == EINTR);

			if (ret == -1 || ret == 0) {
				errno = ETIMEDOUT;
				return -1;
			}
		}

		ret = read(sockfd, p, left);
		if (ret == -1) {
			if (errno != EAGAIN && errno != EINTR)
				return ret;
		} else if (ret == 0 && left != 0) {
			errno = ENOENT;
			return -1;
		}

		if (ret > 0) {
			left -= ret;
			p += ret;
		}
	}

	return len;
}

void set_non_block(int fd)
{
	int val;

	val = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, val | O_NONBLOCK);
}

void set_block(int fd)
{
	int val;

	val = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, val & (~O_NONBLOCK));
}

ssize_t recv_timeout(int sockfd, void *buf, size_t len, unsigned sec)
{
	int ret;
	struct pollfd pfd;

	pfd.fd = sockfd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	do {
		ret = poll(&pfd, 1, sec * 1000);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1 || ret == 0) {
		errno = ETIMEDOUT;
		return -1;
	}

	return recv(sockfd, buf, len, 0);
}

ssize_t recvmsg_timeout(int sockfd, struct msghdr * msg, int flags,
			unsigned sec)
{
	int ret;

	if (sec) {
		struct pollfd pfd;

		pfd.fd = sockfd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		do {
			ret = poll(&pfd, 1, sec * 1000);
		} while (ret == -1 && errno == EINTR);

		if (ret == -1 || ret == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
	}

	do {
		ret = recvmsg(sockfd, msg, flags);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

int forward_msg(void *pool, int ifd, uint8_t icmd, int ofd, uint8_t ocmd, unsigned timeout)
{
	struct iovec iov[3];
	char data[5];
	uint32_t length;
	ssize_t left;
	uint8_t rcmd;
	struct msghdr hdr;
	int ret;

	iov[0].iov_base = &rcmd;
	iov[0].iov_len = 1;

	iov[1].iov_base = &length;
	iov[1].iov_len = 4;

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 2;

	ret = recvmsg_timeout(ifd, &hdr, 0, timeout);
	if (ret == -1) {
		int e = errno;
		syslog(LOG_ERR, "%s:%u: recvmsg: %s", __FILE__, __LINE__,
		       strerror(e));
		return ERR_BAD_COMMAND;
	}

	if (ret == 0) {
		return ERR_PEER_TERMINATED;
	}

	if (rcmd != icmd) {
		syslog(LOG_ERR, "%s:%u: expected %d, received %d", __FILE__,
		       __LINE__, (int)rcmd, (int)icmd);
		return ERR_BAD_COMMAND;
	}

	data[0] = ocmd;
	memcpy(&data[1], &length, 4);

	/* send headers */
	ret = force_write(ofd, data, 5);
	if (ret != 5) {
		syslog(LOG_ERR, "%s:%u: cannot send headers: %s", __FILE__,
		       __LINE__, strerror(errno));
		return ERR_BAD_COMMAND;
	}

	left = length;

	while (left > 0) {
		char buf[1024];

		ret = recv(ifd, buf, sizeof(buf), 0);
		if (ret == -1 || ret == 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			syslog(LOG_ERR, "%s:%u: cannot send between descriptors: %s", __FILE__,
			       __LINE__, strerror(errno));
			return ERR_BAD_COMMAND;
		}

		ret = force_write(ofd, buf, ret);
		if (ret == -1 || ret == 0) {
			syslog(LOG_ERR, "%s:%u: cannot send between descriptors: %s", __FILE__,
			       __LINE__, strerror(errno));
			return ERR_BAD_COMMAND;
		}

		left -= ret;
	}

	return 0;
}

/* Sends message + socketfd */
int send_socket_msg(void *pool, int fd, uint8_t cmd,
		    int socketfd, const void *msg,
		    pack_size_func get_size, pack_func pack)
{
	struct iovec iov[3];
	struct msghdr hdr;
	union {
		struct cmsghdr cm;
		char control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr *cmptr;
	void *packed = NULL;
	uint32_t length32;
	size_t length = 0;
	int ret;

	memset(&hdr, 0, sizeof(hdr));

	iov[0].iov_base = &cmd;
	iov[0].iov_len = 1;

	if (msg)
		length = get_size(msg);

	if (length >= UINT32_MAX)
		return -1;

	length32 = length;
	iov[1].iov_base = &length32;
	iov[1].iov_len = 4;

	hdr.msg_iov = iov;
	hdr.msg_iovlen = 2;

	if (length > 0) {
		packed = talloc_size(pool, length);
		if (packed == NULL) {
			syslog(LOG_ERR, "%s:%u: memory error", __FILE__,
			       __LINE__);
			return -1;
		}

		iov[2].iov_base = packed;
		iov[2].iov_len = length;

		ret = pack(msg, packed);
		if (ret == 0) {
			syslog(LOG_ERR, "%s:%u: packing error", __FILE__,
			       __LINE__);
			ret = -1;
			goto cleanup;
		}

		hdr.msg_iovlen++;
	}

	if (socketfd != -1) {
		hdr.msg_control = control_un.control;
		hdr.msg_controllen = sizeof(control_un.control);

		cmptr = CMSG_FIRSTHDR(&hdr);
		cmptr->cmsg_len = CMSG_LEN(sizeof(int));
		cmptr->cmsg_level = SOL_SOCKET;
		cmptr->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmptr), &socketfd, sizeof(int));
	}

	do {
		ret = sendmsg(fd, &hdr, 0);
	} while (ret == -1 && errno == EINTR);
	if (ret < 0) {
		int e = errno;
		syslog(LOG_ERR, "%s:%u: %s", __FILE__, __LINE__, strerror(e));
	}

 cleanup:
	if (length > 0)
		safe_memset(packed, 0, length);
	talloc_free(packed);
	return ret;
}

int recv_msg_headers(int fd, uint8_t *cmd, unsigned timeout)
{
	struct iovec iov[3];
	char buffer[5];
	uint32_t l32;
	struct msghdr hdr;
	int ret;

	iov[0].iov_base = buffer;
	iov[0].iov_len = 5;

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 1;

	ret = recvmsg_timeout(fd, &hdr, 0, timeout);
	if (ret == -1) {
		int e = errno;
		syslog(LOG_ERR, "%s:%u: recvmsg: %s", __FILE__, __LINE__,
		       strerror(e));
		return ERR_BAD_COMMAND;
	}

	if (ret == 0) {
		return ERR_PEER_TERMINATED;
	}

	*cmd = buffer[0];
	memcpy(&l32, &buffer[1], 4);

	return l32;
}

int recv_msg_data(int fd, uint8_t *cmd, uint8_t *data, size_t data_size,
		  int *received_fd)
{
	struct iovec iov[3];
	uint32_t l32;
	struct msghdr hdr;
	union {
		struct cmsghdr cm;
		char control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr *cmptr;
	int ret;

	iov[0].iov_base = cmd;
	iov[0].iov_len = 1;

	iov[1].iov_base = &l32;
	iov[1].iov_len = 4;

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 2;

	hdr.msg_control = control_un.control;
	hdr.msg_controllen = sizeof(control_un.control);

	ret = recvmsg_timeout(fd, &hdr, 0, MAIN_SEC_MOD_TIMEOUT);
	if (ret == -1) {
		int e = errno;
		syslog(LOG_ERR, "%s:%u: recvmsg: %s", __FILE__, __LINE__,
		       strerror(e));
		return ERR_BAD_COMMAND;
	}

	if (ret == 0) {
		return ERR_PEER_TERMINATED;
	}

	/* try to receive socket (if any) */
	if (received_fd != NULL) {
		*received_fd = -1;

		if ((cmptr = CMSG_FIRSTHDR(&hdr)) != NULL
		    && cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
			if (cmptr->cmsg_level != SOL_SOCKET
			    || cmptr->cmsg_type != SCM_RIGHTS) {
				syslog(LOG_ERR,
				       "%s:%u: recvmsg returned invalid msg type",
				       __FILE__, __LINE__);
				return ERR_BAD_COMMAND;
			}

			if (CMSG_DATA(cmptr))
				memcpy(received_fd, CMSG_DATA(cmptr), sizeof(int));
		}
	}

	if (l32 > data_size) {
		syslog(LOG_ERR, "%s:%u: recv_msg_data: received more data than expected", __FILE__,
		       __LINE__);
		ret = ERR_BAD_COMMAND;
		goto cleanup;
	}

	ret = force_read_timeout(fd, data, l32, MAIN_SEC_MOD_TIMEOUT);
	if (ret < l32) {
		int e = errno;
		syslog(LOG_ERR, "%s:%u: recvmsg: %s", __FILE__,
		       __LINE__, strerror(e));
		ret = ERR_BAD_COMMAND;
		goto cleanup;
	}

	ret = l32;

 cleanup:
	if (ret < 0 && received_fd != NULL && *received_fd != -1) {
		close(*received_fd);
		*received_fd = -1;
	}
	return ret;
}

int recv_socket_msg(void *pool, int fd, uint8_t cmd,
		    int *socketfd, void **msg, unpack_func unpack,
		    unsigned timeout)
{
	struct iovec iov[3];
	uint32_t length;
	uint8_t rcmd;
	struct msghdr hdr;
	uint8_t *data = NULL;
	union {
		struct cmsghdr cm;
		char control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr *cmptr;
	int ret;
	PROTOBUF_ALLOCATOR(pa, pool);

	iov[0].iov_base = &rcmd;
	iov[0].iov_len = 1;

	iov[1].iov_base = &length;
	iov[1].iov_len = 4;

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 2;

	hdr.msg_control = control_un.control;
	hdr.msg_controllen = sizeof(control_un.control);

	ret = recvmsg_timeout(fd, &hdr, 0, timeout);
	if (ret == -1) {
		int e = errno;
		syslog(LOG_ERR, "%s:%u: recvmsg: %s", __FILE__, __LINE__,
		       strerror(e));
		return ERR_BAD_COMMAND;
	}

	if (ret == 0) {
		return ERR_PEER_TERMINATED;
	}

	if (rcmd != cmd) {
		syslog(LOG_ERR, "%s:%u: expected %d, received %d", __FILE__,
		       __LINE__, (int)rcmd, (int)cmd);
		return ERR_BAD_COMMAND;
	}

	/* try to receive socket (if any) */
	if (socketfd != NULL) {
		if ((cmptr = CMSG_FIRSTHDR(&hdr)) != NULL
		    && cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
			if (cmptr->cmsg_level != SOL_SOCKET
			    || cmptr->cmsg_type != SCM_RIGHTS) {
				syslog(LOG_ERR,
				       "%s:%u: recvmsg returned invalid msg type",
				       __FILE__, __LINE__);
				return ERR_BAD_COMMAND;
			}

			if (CMSG_DATA(cmptr))
				memcpy(socketfd, CMSG_DATA(cmptr), sizeof(int));
			else
				*socketfd = -1;
		} else {
			*socketfd = -1;
		}
	}

	if (length > 0 && msg) {
		data = talloc_size(pool, length);
		if (data == NULL) {
			ret = ERR_MEM;
			goto cleanup;
		}

		ret = force_read_timeout(fd, data, length, timeout);
		if (ret < length) {
			int e = errno;
			syslog(LOG_ERR, "%s:%u: recvmsg: %s", __FILE__,
			       __LINE__, strerror(e));
			ret = ERR_BAD_COMMAND;
			goto cleanup;
		}

		*msg = unpack(&pa, length, data);
		if (*msg == NULL) {
			syslog(LOG_ERR, "%s:%u: unpacking error", __FILE__,
			       __LINE__);
			ret = ERR_MEM;
			goto cleanup;
		}
	}

	ret = 0;

 cleanup:
	talloc_free(data);
	if (ret < 0 && socketfd != NULL && *socketfd != -1) {
		close(*socketfd);
		*socketfd = -1;
	}
	return ret;
}


void _talloc_free2(void *ctx, void *ptr)
{
	talloc_free(ptr);
}

void *_talloc_size2(void *ctx, size_t size)
{
	return talloc_size(ctx, size);
}

/* like recvfrom but also returns the address of our interface.
 *
 * @def_port: is provided to fill in the missing port number
 *   in our_addr.
 */
ssize_t oc_recvfrom_at(int sockfd, void *buf, size_t len, int flags,
		       struct sockaddr * src_addr, socklen_t * addrlen,
		       struct sockaddr * our_addr, socklen_t * our_addrlen,
		       int def_port)
{
	int ret;
	char cmbuf[256];
	struct iovec iov = { buf, len };
	struct cmsghdr *cmsg;
	struct msghdr mh = {
		.msg_name = src_addr,
		.msg_namelen = *addrlen,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = cmbuf,
		.msg_controllen = sizeof(cmbuf),
	};

	do {
		ret = recvmsg(sockfd, &mh, 0);
	} while (ret == -1 && errno == EINTR);
	if (ret < 0) {
		return -1;
	}

	/* find our address */
	for (cmsg = CMSG_FIRSTHDR(&mh); cmsg != NULL;
	     cmsg = CMSG_NXTHDR(&mh, cmsg)) {
#if defined(IP_PKTINFO)
		if (cmsg->cmsg_level == IPPROTO_IP
		    && cmsg->cmsg_type == IP_PKTINFO) {
			struct in_pktinfo *pi = (void *)CMSG_DATA(cmsg);
			struct sockaddr_in *a = (struct sockaddr_in *)our_addr;

			if (*our_addrlen < sizeof(struct sockaddr_in)
			    || pi == NULL)
				return -1;

			a->sin_family = AF_INET;
			memcpy(&a->sin_addr, &pi->ipi_addr,
			       sizeof(struct in_addr));
			a->sin_port = htons(def_port);
			*our_addrlen = sizeof(struct sockaddr_in);
			break;
		}
#elif defined(IP_RECVDSTADDR)
		if (cmsg->cmsg_level == IPPROTO_IP
		    && cmsg->cmsg_type == IP_RECVDSTADDR) {
			struct in_addr *pi = (void *)CMSG_DATA(cmsg);
			struct sockaddr_in *a = (struct sockaddr_in *)our_addr;

			if (*our_addrlen < sizeof(struct sockaddr_in)
			    || pi == NULL)
				return -1;

			a->sin_family = AF_INET;
			memcpy(&a->sin_addr, &pi->s_addr,
			       sizeof(struct in_addr));
			a->sin_port = htons(def_port);
			*our_addrlen = sizeof(struct sockaddr_in);
			break;
		}
#endif
#ifdef IPV6_RECVPKTINFO
		if (cmsg->cmsg_level == IPPROTO_IPV6
		    && cmsg->cmsg_type == IPV6_PKTINFO) {
			struct in6_pktinfo *pi = (void *)CMSG_DATA(cmsg);
			struct sockaddr_in6 *a =
			    (struct sockaddr_in6 *)our_addr;

			if (*our_addrlen < sizeof(struct sockaddr_in6)
			    || pi == NULL)
				return -1;

			a->sin6_family = AF_INET6;
			memcpy(&a->sin6_addr, &pi->ipi6_addr,
			       sizeof(struct in6_addr));
			a->sin6_port = htons(def_port);
			*our_addrlen = sizeof(struct sockaddr_in6);
			break;
		}
#endif
	}
	*addrlen = mh.msg_namelen;

	return ret;
}

#ifndef HAVE_STRLCPY

/*
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
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
 *
 * Copyright 2006  The FreeRADIUS server project
 */

/*
 * Copy src to string dst of size siz.  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
size_t oc_strlcpy(char *dst, char const *src, size_t siz)
{
	char *d = dst;
	char const *s = src;
	size_t n = siz;

	/* Copy as many bytes as will fit */
	if (n != 0 && --n != 0) {
		do {
			if ((*d++ = *s++) == 0)
				break;
		} while (--n != 0);
	}

	/* Not enough room in dst, add NUL and traverse rest of src */
	if (n == 0) {
		if (siz != 0)
			*d = '\0';	/* NUL-terminate dst */
		while (*s++) ;
	}

	return (s - src - 1);	/* count does not include NUL */
}

#endif
