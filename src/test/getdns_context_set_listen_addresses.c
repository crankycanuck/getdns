/*
 * Copyright (c) 2013, NLNet Labs, Verisign, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names of the copyright holders nor the
 *   names of its contributors may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Verisign, Inc. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "getdns_context_set_listen_addresses.h"
#include "getdns/getdns_extra.h"
#include "types-internal.h"
#include "debug.h"
#include <netdb.h>

#define DNS_REQUEST_SZ          4096
#define DOWNSTREAM_IDLE_TIMEOUT 5000
#define TCP_LISTEN_BACKLOG      16

typedef struct listen_set listen_set;
typedef enum listen_set_action {
	to_stay, to_add, to_remove
} listen_set_action;

typedef struct connection connection;
typedef struct listener listener;
struct listener {
	getdns_eventloop_event   event;
	socklen_t                addr_len;
	struct sockaddr_storage  addr;
	int                      fd;
	getdns_transport_list_t  transport;

	listen_set_action        action;
	listener                *to_replace;
	listen_set              *set;

	/* Should be per context eventually */
	connection              *connections;
};

/* listen set is temporarily a singly linked list node, to associate the set
 * with a context.  Eventually it has to become a context attribute.
 */
struct listen_set {
	getdns_context           *context;
	listen_set               *next;
	getdns_request_handler_t  handler;

	size_t                    count;
	listener                  items[];
};

typedef struct tcp_to_write tcp_to_write;
struct tcp_to_write {
	size_t        write_buf_len;
	size_t        written;
	tcp_to_write *next;
	uint8_t       write_buf[];
};

struct connection {
	listener               *l;
	struct sockaddr_storage remote_in;
	socklen_t               addrlen;

	connection             *next;
	connection            **prev_next;
};

typedef struct tcp_connection {
	/* A TCP connection is a connection */
	listener               *l;
	struct sockaddr_storage remote_in;
	socklen_t               addrlen;

	connection             *next;
	connection            **prev_next;
	/************************************/

	int                     fd;
	getdns_eventloop_event  event;

	uint8_t                *read_buf;
	size_t                  read_buf_len;
	uint8_t                *read_pos;
	size_t                  to_read;

	tcp_to_write           *to_write;
	size_t                  to_answer;
} tcp_connection;


static void free_listen_set_when_done(listen_set *set);
static void tcp_connection_destroy(tcp_connection *conn)
{
	struct mem_funcs *mf;
	getdns_eventloop *loop;

	tcp_to_write *cur, *next;

	if (!(mf = priv_getdns_context_mf(conn->l->set->context)))
		return;

	if (getdns_context_get_eventloop(conn->l->set->context, &loop))
		return;

	if (conn->event.read_cb||conn->event.write_cb||conn->event.timeout_cb)
		loop->vmt->clear(loop, &conn->event);

	if (conn->fd >= 0)
		(void) close(conn->fd);
	GETDNS_FREE(*mf, conn->read_buf);

	for (cur = conn->to_write; cur; cur = next) {
		next = cur->next;
		GETDNS_FREE(*mf, cur);
	}
	if (conn->to_answer > 0)
		return;

	/* Unlink this connection */
	if ((*conn->prev_next = conn->next))
		conn->next->prev_next = conn->prev_next;

	free_listen_set_when_done(conn->l->set);
	GETDNS_FREE(*mf, conn);
}

static void tcp_write_cb(void *userarg)
{
	tcp_connection *conn = (tcp_connection *)userarg;
	struct mem_funcs *mf;
	getdns_eventloop *loop;

	tcp_to_write *to_write;
	ssize_t written;

	assert(userarg);

	if (!(mf = priv_getdns_context_mf(conn->l->set->context)))
		return;

	if (getdns_context_get_eventloop(conn->l->set->context, &loop))
		return;

	/* Reset tcp_connection idle timeout */
	loop->vmt->clear(loop, &conn->event);

	if (!conn->to_write) {
		conn->event.write_cb = NULL;
		(void) loop->vmt->schedule(loop, conn->fd,
		    DOWNSTREAM_IDLE_TIMEOUT, &conn->event);
		return;
	}
	to_write = conn->to_write;
	if (conn->fd == -1 || 
	    (written = write(conn->fd, &to_write->write_buf[to_write->written],
	    to_write->write_buf_len - to_write->written)) == -1) {

		/* IO error, close connection */
		conn->event.read_cb = conn->event.write_cb =
		    conn->event.timeout_cb = NULL;
		tcp_connection_destroy(conn);
		return;
	}
	to_write->written += written;
	if (to_write->written == to_write->write_buf_len) {
		conn->to_write = to_write->next;
		GETDNS_FREE(*mf, to_write);
	}
	if (!conn->to_write)
		conn->event.write_cb = NULL;

	(void) loop->vmt->schedule(loop, conn->fd,
	    DOWNSTREAM_IDLE_TIMEOUT, &conn->event);
}

void
_getdns_cancel_reply(getdns_context *context, getdns_transaction_t request_id)
{
	/* TODO: Check request_id at context->outbound_requests */
	connection *conn = (connection *)(intptr_t)request_id;
	struct mem_funcs *mf;

	if (!context || !conn)
		return;

	if (conn->l->transport == GETDNS_TRANSPORT_TCP) {
		tcp_connection *conn = (tcp_connection *)(intptr_t)request_id;

		if (conn->to_answer > 0 && --conn->to_answer == 0 &&
		    conn->fd == -1)
			tcp_connection_destroy(conn);

	} else if (conn->l->transport == GETDNS_TRANSPORT_UDP &&
	    (mf = priv_getdns_context_mf(conn->l->set->context))) {
		listen_set *set = conn->l->set;

		/* Unlink this connection */
		if ((*conn->prev_next = conn->next))
			conn->next->prev_next = conn->prev_next;
		GETDNS_FREE(*mf, conn);
		free_listen_set_when_done(set);
	}
}

getdns_return_t
getdns_reply(
    getdns_context *context, getdns_transaction_t request_id, getdns_dict *reply)
{
	/* TODO: Check request_id at context->outbound_requests */
	connection *conn = (connection *)(intptr_t)request_id;
	struct mem_funcs *mf;
	getdns_eventloop *loop;
	uint8_t buf[65536];
	size_t len;
	getdns_return_t r;

	if (!context || !reply || !conn)
		return GETDNS_RETURN_INVALID_PARAMETER;

	if (!(mf = priv_getdns_context_mf(conn->l->set->context)))
		return GETDNS_RETURN_GENERIC_ERROR;;

	if ((r = getdns_context_get_eventloop(conn->l->set->context, &loop)))
		return r;

	len = sizeof(buf);
	if ((r = getdns_msg_dict2wire_buf(reply, buf, &len)))
		return r;

	else if (conn->l->transport == GETDNS_TRANSPORT_UDP) {
		listener *l = conn->l;

		if (conn->l->fd >= 0 && sendto(conn->l->fd, buf, len, 0,
		    (struct sockaddr *)&conn->remote_in, conn->addrlen) == -1) {
			/* IO error, cleanup this listener */
			loop->vmt->clear(loop, &conn->l->event);
			close(conn->l->fd);
			conn->l->fd = -1;
		}
		/* Unlink this connection */
		if ((*conn->prev_next = conn->next))
			conn->next->prev_next = conn->prev_next;

		GETDNS_FREE(*mf, conn);
		if (l->fd < 0)
			free_listen_set_when_done(l->set);

	} else if (conn->l->transport == GETDNS_TRANSPORT_TCP) {
		tcp_connection *conn = (tcp_connection *)(intptr_t)request_id;
		tcp_to_write **to_write_p;
		tcp_to_write *to_write;

		if (conn->fd == -1) {
			if (conn->to_answer > 0)
				--conn->to_answer;
			tcp_connection_destroy(conn);
			return GETDNS_RETURN_GOOD;
		}
		if (!(to_write = (tcp_to_write *)GETDNS_XMALLOC(
		    *mf, uint8_t, sizeof(tcp_to_write) + len + 2)))
			return GETDNS_RETURN_MEMORY_ERROR;

		to_write->write_buf_len = len + 2;
		to_write->write_buf[0] = (len >> 8) & 0xFF;
		to_write->write_buf[1] = len & 0xFF;
		to_write->written = 0;
		to_write->next = NULL;
		(void) memcpy(to_write->write_buf + 2, buf, len);

		/* Appen to_write to conn->to_write list */
		for ( to_write_p = &conn->to_write
		    ; *to_write_p
		    ; to_write_p = &(*to_write_p)->next)
			; /* pass */
		*to_write_p = to_write;

		loop->vmt->clear(loop, &conn->event);
		conn->event.write_cb = tcp_write_cb;
		if (conn->to_answer > 0)
			conn->to_answer--;
		(void) loop->vmt->schedule(loop,
		    conn->fd, DOWNSTREAM_IDLE_TIMEOUT,
		    &conn->event);
	}
	/* TODO: other transport types */

	return r;
}

static void tcp_read_cb(void *userarg)
{
	tcp_connection *conn = (tcp_connection *)userarg;
	ssize_t bytes_read;
	getdns_return_t r;
	struct mem_funcs *mf;
	getdns_eventloop *loop;
	getdns_dict *request_dict;

	assert(userarg);

	if (!(mf = priv_getdns_context_mf(conn->l->set->context)))
		return;

	if ((r = getdns_context_get_eventloop(conn->l->set->context, &loop)))
		return;

	/* Reset tcp_connection idle timeout */
	loop->vmt->clear(loop, &conn->event);
	(void) loop->vmt->schedule(loop, conn->fd,
	    DOWNSTREAM_IDLE_TIMEOUT, &conn->event);

	if ((bytes_read = read(conn->fd, conn->read_pos, conn->to_read)) == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return; /* Come back to do the read later */

		/* IO error, close connection */
		tcp_connection_destroy(conn);
		return;
	}
	if (bytes_read == 0) {
		/* remote end closed connection, cleanup */
		tcp_connection_destroy(conn);
		return;
	}
	assert(bytes_read <= conn->to_read);

	conn->to_read  -= bytes_read;
	conn->read_pos += bytes_read;
	if (conn->to_read)
		return; /* More to read */

	if (conn->read_pos - conn->read_buf == 2) {
		/* read length of dns msg to read */
		conn->to_read = (conn->read_buf[0] << 8) | conn->read_buf[1];
		if (conn->to_read > conn->read_buf_len) {
			GETDNS_FREE(*mf, conn->read_buf);
			while (conn->to_read > conn->read_buf_len)
				conn->read_buf_len *= 2;
			if (!(conn->read_buf = GETDNS_XMALLOC(
			    *mf, uint8_t, conn->read_buf_len))) {
				/* Memory error */
				tcp_connection_destroy(conn);
				return;
			}
		}
		if (conn->to_read < 12) {
			/* Request smaller than DNS header, FORMERR */
			tcp_connection_destroy(conn);
			return;
		}
		conn->read_pos = conn->read_buf;
		return;  /* Read DNS message */
	}
	if ((r = getdns_wire2msg_dict(conn->read_buf,
	    (conn->read_pos - conn->read_buf), &request_dict)))
		; /* FROMERR on input, ignore */

	else {
		conn->to_answer++;

		/* Call request handler */
		conn->l->set->handler(
		    conn->l->set->context, request_dict, (intptr_t)conn);

		conn->read_pos = conn->read_buf;
		conn->to_read = 2;
		return; /* Read more requests */
	}
	conn->read_pos = conn->read_buf;
	conn->to_read = 2;
	 /* Read more requests */
}

static void tcp_timeout_cb(void *userarg)
{
	tcp_connection *conn = (tcp_connection *)userarg;

	assert(userarg);

	if (conn->to_answer) {
		getdns_eventloop *loop;

		if (getdns_context_get_eventloop(conn->l->set->context, &loop))
			return;

		loop->vmt->clear(loop, &conn->event);
		(void) loop->vmt->schedule(loop,
		    conn->fd, DOWNSTREAM_IDLE_TIMEOUT,
		    &conn->event);
	} else
		tcp_connection_destroy(conn);
}

static void tcp_accept_cb(void *userarg)
{
	listener *l = (listener *)userarg;
	tcp_connection *conn;
	struct mem_funcs *mf;
	getdns_eventloop *loop;
	getdns_return_t r;

	assert(userarg);

	if (!(mf = priv_getdns_context_mf(l->set->context)))
		return;

	if ((r = getdns_context_get_eventloop(l->set->context, &loop)))
		return;

	if (!(conn = GETDNS_MALLOC(*mf, tcp_connection)))
		return;

	(void) memset(conn, 0, sizeof(tcp_connection));

	conn->l = l;
	conn->addrlen = sizeof(conn->remote_in);
	if ((conn->fd = accept(l->fd,
	    (struct sockaddr *)&conn->remote_in, &conn->addrlen)) == -1) {
		/* IO error, cleanup this listener */
		loop->vmt->clear(loop, &l->event);
		close(l->fd);
		l->fd = -1;
		GETDNS_FREE(*mf, conn);
		return;
	}
	if (!(conn->read_buf = malloc(DNS_REQUEST_SZ))) {
		/* Memory error */
		GETDNS_FREE(*mf, conn);
		return;
	}
	conn->read_buf_len = DNS_REQUEST_SZ;
	conn->read_pos = conn->read_buf;
	conn->to_read = 2;
	conn->event.userarg = conn;
	conn->event.read_cb = tcp_read_cb;
	conn->event.timeout_cb = tcp_timeout_cb;

	/* Insert connection */
	if ((conn->next = l->connections))
		conn->next->prev_next = &conn->next;
	conn->prev_next = &l->connections;
	l->connections = (connection *)conn;

	(void) loop->vmt->schedule(loop, conn->fd,
	    DOWNSTREAM_IDLE_TIMEOUT, &conn->event);
}

static void udp_read_cb(void *userarg)
{
	listener *l = (listener *)userarg;
	connection *conn;
	struct mem_funcs *mf;
	getdns_eventloop *loop;
	getdns_dict *request_dict;

	/* Maximum reasonable size for requests */
	uint8_t buf[4096];
	ssize_t len;
	getdns_return_t r;
	
	assert(userarg);

	if (l->fd == -1)
		return;

	if (!(mf = priv_getdns_context_mf(l->set->context)))
		return;

	if ((r = getdns_context_get_eventloop(l->set->context, &loop)))
		return;

	if (!(conn = GETDNS_MALLOC(*mf, connection)))
		return;

	conn->l = l;
	conn->addrlen = sizeof(conn->remote_in);
	if ((len = recvfrom(l->fd, buf, sizeof(buf), 0,
	    (struct sockaddr *)&conn->remote_in, &conn->addrlen)) == -1) {
		/* IO error, cleanup this listener. */
		loop->vmt->clear(loop, &l->event);
		close(l->fd);
		l->fd = -1;

#if 0 && defined(SERVER_DEBUG) && SERVER_DEBUG
	} else {
		char addrbuf[100];
		char hexbuf[4096], *hexptr;
		size_t l, i, j;

		if (conn->remote_in.ss_family == AF_INET) {
			if (inet_ntop(AF_INET,
			    &((struct sockaddr_in*)&conn->remote_in)->sin_addr,
			    addrbuf, sizeof(addrbuf))) {

				l = strlen(addrbuf);
				(void) snprintf(addrbuf + l,
				    sizeof(addrbuf) - l, ":%d", 
				    (int)((struct sockaddr_in*)
				    &conn->remote_in)->sin_port);
			} else
				(void) strncpy(
				    addrbuf, "error ipv4", sizeof(addrbuf));

		} else if (conn->remote_in.ss_family == AF_INET6) {
			addrbuf[0] = '[';
			if (inet_ntop(AF_INET6,
			    &((struct sockaddr_in6*)
			    &conn->remote_in)->sin6_addr,
			    addrbuf, sizeof(addrbuf))) {

				l = strlen(addrbuf);
				(void) snprintf(addrbuf + l,
				    sizeof(addrbuf) - l, ":%d", 
				    (int)((struct sockaddr_in6*)
				    &conn->remote_in)->sin6_port);
			} else
				(void) strncpy(
				    addrbuf, "error ipv6", sizeof(addrbuf));

		} else {
			(void) strncpy(
			    addrbuf, "unknown address", sizeof(addrbuf));
		}
		*(hexptr = hexbuf) = 0;
		for (i = 0; i < len; i++) {
			if (i % 12 == 0) {
				hexptr += snprintf(hexptr,
				    sizeof(hexbuf) - (hexptr - hexbuf) - 1,
				    "\n%.4x", (int)i);
			} else if (i % 4 == 0) {
				hexptr += snprintf(hexptr,
				    sizeof(hexbuf) - (hexptr - hexbuf) - 1,
				    " ");
			}
			if (hexptr - hexbuf > sizeof(hexbuf))
				break;
			hexptr += snprintf(hexptr,
			    sizeof(hexbuf) - (hexptr - hexbuf) - 1,
			    " %.2x", (int)buf[i]);
			if (hexptr - hexbuf > sizeof(hexbuf))
				break;
		}
		DEBUG_SERVER("Received %d bytes from %s: %s\n",
		    (int)len, addrbuf, hexbuf);
	}
	if (len == -1) {
		; /* pass */
#endif

	} else if ((r = getdns_wire2msg_dict(buf, len, &request_dict)))
		; /* FROMERR on input, ignore */

	else {
		/* Insert connection */
		if ((conn->next = l->connections))
			conn->next->prev_next = &conn->next;
		conn->prev_next = &l->connections;
		l->connections = conn;

		/* Call request handler */
		l->set->handler(l->set->context, request_dict, (intptr_t)conn);
		return;
	}
	GETDNS_FREE(*mf, conn);
}

static void rm_listen_set(listen_set **root, listen_set *set)
{
	assert(root);

	while (*root && *root != set)
		root = &(*root)->next;

	*root = set->next;
	set->next = NULL;
}

static listen_set *lookup_listen_set(listen_set *root, getdns_context *key)
{
	while (root && root->context != key)
		root = root->next;

	return root;
}

static void free_listen_set_when_done(listen_set *set)
{
	struct mem_funcs *mf;
	size_t            i;

	assert(set);
	assert(set->context);

	if (!(mf = priv_getdns_context_mf(set->context)))
		return;

	DEBUG_SERVER("To free listen set: %p\n", set);
	for (i = 0; i < set->count; i++) {
		listener *l = &set->items[i];

		if (l->fd >= 0)
			return;

		if (l->connections)
			return;
	}
	GETDNS_FREE(*mf, set);
	DEBUG_SERVER("Listen set: %p freed\n", set);
}

static void remove_listeners(listen_set *set)
{
	struct mem_funcs *mf;
	getdns_eventloop *loop;
	size_t            i;

	assert(set);
	assert(set->context);

	if (!(mf = priv_getdns_context_mf(set->context)))
		return;

	if (getdns_context_get_eventloop(set->context, &loop))
		return;

	for (i = 0; i < set->count; i++) {
		listener *l = &set->items[i];
		tcp_connection **conn_p;

		if (l->action != to_remove || l->fd == -1)
			continue;

		loop->vmt->clear(loop, &l->event);
		close(l->fd);
		l->fd = -1;

		if (l->transport != GETDNS_TRANSPORT_TCP)
			continue;
		
		conn_p = (tcp_connection **)&l->connections;
		while (*conn_p) {
			tcp_connection_destroy(*conn_p);
			if (*conn_p && (*conn_p)->to_answer > 0)
				conn_p = (tcp_connection **)&(*conn_p)->next;
		}
	}
	free_listen_set_when_done(set);
}

static getdns_return_t add_listeners(listen_set *set)
{
	static const int  enable = 1;

	struct mem_funcs *mf;
	getdns_eventloop *loop;
	size_t            i;
	getdns_return_t   r;

	assert(set);
	assert(set->context);

	if (!(mf = priv_getdns_context_mf(set->context)))
		return GETDNS_RETURN_GENERIC_ERROR;

	if ((r = getdns_context_get_eventloop(set->context, &loop)))
		return r;

	r = GETDNS_RETURN_GENERIC_ERROR;
	for (i = 0; i < set->count; i++) {
		listener *l = &set->items[i];

		if (l->action != to_add)
			continue;

		if (l->transport != GETDNS_TRANSPORT_UDP &&
		    l->transport != GETDNS_TRANSPORT_TCP)
			continue;

		if ((l->fd = socket(l->addr.ss_family,
		    ( l->transport == GETDNS_TRANSPORT_UDP
		    ? SOCK_DGRAM : SOCK_STREAM), 0)) == -1)
			/* IO error */
			break;

		if (setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR,
		    &enable, sizeof(int)) < 0)
			; /* Ignore */

		if (bind(l->fd, (struct sockaddr *)&l->addr,
		    l->addr_len) == -1)
			/* IO error */
			break;

		if (l->transport == GETDNS_TRANSPORT_UDP) {
			l->event.userarg = l;
			l->event.read_cb = udp_read_cb;
			if ((r = loop->vmt->schedule(
			    loop, l->fd, -1, &l->event)))
				break;

		} else if (listen(l->fd, TCP_LISTEN_BACKLOG) == -1)
			/* IO error */
			break;

		else {
			l->event.userarg = l;
			l->event.read_cb = tcp_accept_cb;
			if ((r = loop->vmt->schedule(
			    loop, l->fd, -1, &l->event)))
				break;
		}
	}
	if (i < set->count)
		return r;

	return GETDNS_RETURN_GOOD;
}

getdns_return_t getdns_context_set_listen_addresses(getdns_context *context,
    getdns_request_handler_t request_handler,
    const getdns_list *listen_addresses)
{
	static const getdns_transport_list_t listen_transports[]
		= { GETDNS_TRANSPORT_UDP, GETDNS_TRANSPORT_TCP };
	static const uint32_t transport_ports[] = { 53, 53 };
	static const size_t n_transports = sizeof( listen_transports)
	                                 / sizeof(*listen_transports);
	static listen_set *root = NULL;

	listen_set        *current_set;
	listen_set        *new_set;
	size_t             new_set_count;

	struct mem_funcs  *mf;
	getdns_eventloop  *loop;

	/* auxiliary variables */
	getdns_return_t r;
	size_t i;
	struct addrinfo hints;

	DEBUG_SERVER("getdns_context_set_listen_addresses(%p, %p, %p)\n", context, request_handler,
	    listen_addresses);
	if (!(mf = priv_getdns_context_mf(context)))
		return GETDNS_RETURN_GENERIC_ERROR;

	if ((r = getdns_context_get_eventloop(context, &loop)))
		return r;

	if (listen_addresses == NULL)
		new_set_count = 0;

	else if ((r = getdns_list_get_length(listen_addresses, &new_set_count)))
		return r;

	if ((current_set = lookup_listen_set(root, context))) {
		for (i = 0; i < current_set->count; i++)
			current_set->items[i].action = to_remove;
	}
	if (new_set_count == 0) {
		if (!current_set)
			return GETDNS_RETURN_GOOD;
		
		rm_listen_set(&root, current_set);
		/* action is already to_remove */
		remove_listeners(current_set);
		return GETDNS_RETURN_GOOD;
	}
	if (!request_handler)
		return GETDNS_RETURN_INVALID_PARAMETER;

	if (!(new_set = (listen_set *)GETDNS_XMALLOC(*mf, uint8_t,
	    sizeof(listen_set) +
	    sizeof(listener) * new_set_count * n_transports)))
		return GETDNS_RETURN_MEMORY_ERROR;

	DEBUG_SERVER("New listen set: %p, current_set: %p\n", new_set, current_set);

	new_set->context = context;
	new_set->next = root;
	new_set->handler = request_handler;
	new_set->count = new_set_count * n_transports;
	(void) memset(new_set->items, 0,
	    sizeof(listener) * new_set_count * n_transports);
	for (i = 0; i < new_set->count; i++)
		new_set->items[i].fd = -1;

	(void) memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family    = AF_UNSPEC;
	hints.ai_flags     = AI_NUMERICHOST;

	for (i = 0; !r && i < new_set_count; i++) {
		getdns_dict             *dict = NULL;
		getdns_bindata          *address_data;
		struct sockaddr_storage  addr;
		getdns_bindata          *scope_id;
		char                     addrstr[1024], *eos;
		size_t                   t;

		if ((r = getdns_list_get_dict(listen_addresses, i, &dict))) {
			if ((r = getdns_list_get_bindata(
			    listen_addresses, i, &address_data)))
				break;

		} else if ((r = getdns_dict_get_bindata(
		    dict, "address_data", &address_data)))
			break;

		if (address_data->size == 4)
			addr.ss_family = AF_INET;
		else if (address_data->size == 16)
			addr.ss_family = AF_INET6;
		else {
			r = GETDNS_RETURN_INVALID_PARAMETER;
			break;
		}
		if (inet_ntop(addr.ss_family,
		    address_data->data, addrstr, 1024) == NULL) {
			r = GETDNS_RETURN_INVALID_PARAMETER;
			break;
		}
		if (dict && getdns_dict_get_bindata(dict,"scope_id",&scope_id)
		    == GETDNS_RETURN_GOOD) {
			if (strlen(addrstr) + scope_id->size > 1022) {
				r = GETDNS_RETURN_INVALID_PARAMETER;
				break;
			}
			eos = &addrstr[strlen(addrstr)];
			*eos++ = '%';
			(void) memcpy(eos, scope_id->data, scope_id->size);
			eos[scope_id->size] = 0;
		}
		for (t = 0; !r && t < n_transports; t++) {
			char portstr[1024];
			getdns_transport_list_t transport
			    = listen_transports[t];
			uint32_t port = transport_ports[t];
			struct addrinfo *ai;
			listener *l = &new_set->items[i*n_transports + t];
			size_t j;
			listener *cl;

			l->fd = -1;
			if (dict)
				(void) getdns_dict_get_int(dict,
				    ( transport == GETDNS_TRANSPORT_TLS
				    ? "tls_port" : "port" ), &port);

			(void) snprintf(portstr, 1024, "%d", (int)port);

			if (getaddrinfo(addrstr, portstr, &hints, &ai)) {
				r = GETDNS_RETURN_INVALID_PARAMETER;
				break;
			}
			if (!ai)
				continue;

			l->addr.ss_family = addr.ss_family;
			l->addr_len = ai->ai_addrlen;
			(void) memcpy(&l->addr, ai->ai_addr, ai->ai_addrlen);
			l->transport = transport;
			l->set = new_set;
			l->connections = NULL;
			freeaddrinfo(ai);

			/* Now determine the action */
			if (!current_set) {
				l->action = to_add;
				continue;
			}
			for (j = 0; j < current_set->count; j++) {
				cl = &current_set->items[j];
				
				if (l->transport == cl->transport &&
				    l->addr_len == cl->addr_len &&
				    !memcmp(&l->addr, &cl->addr, l->addr_len))
					break;
			}
			if (j == current_set->count) {
				/* Not found */
				l->action = to_add;
				continue;
			}
			l->action = to_stay;
			l->to_replace = cl;
			/* So the event can be rescheduled */
		}
	}
	if (r || (r = add_listeners(new_set))) {
		for (i = 0; i < new_set->count; i++)
			new_set->items[i].action = to_remove;

		remove_listeners(new_set);
		return r;
	}
	/* Reschedule all stayers */
	for (i = 0; i < new_set->count; i++) {
		listener *l = &new_set->items[i];

		if (l->action == to_stay) {
			connection *conn;

			loop->vmt->clear(loop, &l->to_replace->event);
			(void) memset(&l->to_replace->event, 0,
			    sizeof(getdns_eventloop_event));

			l->fd = l->to_replace->fd;
			l->event = l->to_replace->event;
			l->connections = l->to_replace->connections;
			for (conn = l->connections; conn; conn = conn->next)
				conn->l = l;

			l->to_replace->connections = NULL;
			l->to_replace->fd = -1;

			/* assume success on reschedule */
			(void) loop->vmt->schedule(loop, l->fd, -1, &l->event);
		}
	}
	if (current_set) {
		rm_listen_set(&root, current_set);
		remove_listeners(current_set); /* Is already remove */
	}
	root = new_set;
	return GETDNS_RETURN_GOOD;
}

