/*  Copyright (C) 2015 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <tap/basic.h>

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "libknot/errcode.h"
#include "libknot/internal/net.h"
#include "libknot/internal/sockaddr.h"

const struct timeval TIMEOUT = { 5, 0 };
const struct timeval TIMEOUT_SHORT = { 0, 500000 };

/*!
 * \brief Get loopback socket address with unset port.
 */
static struct sockaddr_storage addr_local(void)
{
	struct sockaddr_storage addr = { 0 };
	struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;

	addr6->sin6_family = AF_INET6;
	addr6->sin6_addr = in6addr_loopback;

	return addr;
}

/*!
 * \brief Get unreachable address.
 *
 * The address is taken from TEST-NET-1 so it won't be hopefully used.
 */
static struct sockaddr_storage addr_unreachable(void)
{
	struct sockaddr_storage addr = { 0 };
	sockaddr_set(&addr, AF_INET, "192.168.2.42", 4);

	return addr;
}

/*!
 * \brief Get address of a socket.
 */
static struct sockaddr_storage addr_from_socket(int sock)
{
	struct sockaddr_storage addr = { 0 };
	socklen_t len = sizeof(addr);
	getsockname(sock, (struct sockaddr *)&addr, &len);

	return addr;
}

static const char *socktype_name(int type)
{
	switch (type) {
	case SOCK_STREAM: return "TCP";
	case SOCK_DGRAM:  return "UDP";
	default:          return "unknown";
	}
}

static bool socktype_is_stream(int type)
{
	return type == SOCK_STREAM;
}

/* -- mock server ---------------------------------------------------------- */

#define LISTEN_BACKLOG 5

struct server_ctx;
typedef struct server_ctx server_ctx_t;

typedef void (*server_cb)(int sock, void *data);

/*!
 * \brief Server context.
 */
struct server_ctx {
	int sock;
	int type;
	bool terminate;
	server_cb handler;
	void *handler_data;

	pthread_t thr;
	pthread_mutex_t mx;
};

static int select_read(int sock)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	struct timeval tv = TIMEOUT;

	return select(sock + 1, &fds, NULL, NULL, &tv);
}

static void server_handle(server_ctx_t *ctx)
{
	int remote = ctx->sock;

	assert(ctx->type == SOCK_STREAM || ctx->type == SOCK_DGRAM);

	if (socktype_is_stream(ctx->type)) {
		remote = accept(ctx->sock, 0, 0);
		if (remote < 0) {
			return;
		}
	}

	pthread_mutex_lock(&ctx->mx);
	server_cb handler = ctx->handler;
	pthread_mutex_unlock(&ctx->mx);
	handler(remote, ctx->handler_data);

	if (socktype_is_stream(ctx->type)) {
		close(remote);
	}
}

/*!
 * \brief Simple server.
 *
 * Terminated when a one-byte message is delivered.
 */
static void *server_main(void *_ctx)
{
	server_ctx_t *ctx = _ctx;

	for (;;) {
		pthread_mutex_lock(&ctx->mx);
		bool terminate = ctx->terminate;
		pthread_mutex_unlock(&ctx->mx);
		if (terminate) {
			break;
		}

		int r = select_read(ctx->sock);
		if (r == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				break;
			}
		} else if (r == 0) {
			continue;
		}

		assert(r == 1);
		server_handle(ctx);
	}

	return NULL;
}

static bool server_start(server_ctx_t *ctx, int sock, int type,
                         server_cb handler, void *handler_data)
{
	memset(ctx, 0, sizeof(*ctx));

	ctx->sock = sock;
	ctx->type = type;
	ctx->handler = handler;
	ctx->handler_data = handler_data;

	ctx->terminate = false;

	pthread_mutex_init(&ctx->mx, NULL);
	return (pthread_create(&ctx->thr, NULL, server_main, ctx) == 0);
}

static void server_stop(server_ctx_t *ctx)
{
	pthread_mutex_lock(&ctx->mx);
	ctx->terminate = true;
	pthread_mutex_unlock(&ctx->mx);

	pthread_kill(ctx->thr, SIGUSR1);
	pthread_join(ctx->thr, NULL);
}

/* -- tests ---------------------------------------------------------------- */

static void handler_echo(int sock, void *_server)
{
	server_ctx_t *server = _server;
	uint8_t buffer[16] = { 0 };

	struct sockaddr_storage remote = { 0 };
	struct sockaddr_storage *addr = NULL;
	if (!socktype_is_stream(server->type)) {
		addr = &remote;
	}

	struct timeval tv = TIMEOUT;
	int in = net_recv(sock, buffer, sizeof(buffer), addr, &tv);
	if (in <= 0) {
		return;
	}

	tv = TIMEOUT;
	net_send(sock, buffer, in, addr, &tv);
}

static void test_connected(int type)
{
	const char *name = socktype_name(type);
	const struct sockaddr_storage local = addr_local();

	int r;

	// setup server socket

	int server = net_bound_socket(type, &local, 0);
	ok(server >= 0, "%s: server, create bound socket", name);

	if (socktype_is_stream(type)) {
		r = listen(server, LISTEN_BACKLOG);
		ok(r == 0, "%s: server, start listening", name);
	}

	// initialize server

	server_ctx_t server_ctx = { 0 };
	r = server_start(&server_ctx, server, type, handler_echo, &server_ctx);
	ok(r, "%s: server, start", name);

	// connected socket, send and receive

	int sock;
	struct timeval tv;

	const struct sockaddr_storage server_addr = addr_from_socket(server);
	sock = net_connected_socket(type, &server_addr, NULL);
	ok(sock >= 0, "%s: client, create connected socket", name);

	r = net_is_connected(sock);
	ok(r, "%s: client, is connected", name);

	const uint8_t out[] = "test message";
	const size_t out_len = sizeof(out);
	if (socktype_is_stream(type)) {
		tv = TIMEOUT;
		r = net_stream_send(sock, out, out_len, &tv);
	} else {
		r = net_dgram_send(sock, out, out_len, NULL);
	}
	ok(r == out_len, "%s: client, send message", name);

	uint8_t in[128] = { 0 };
	tv = TIMEOUT;
	if (socktype_is_stream(type)) {
		r = net_stream_recv(sock, in, sizeof(in), &tv);
	} else {
		r = net_dgram_recv(sock, in, sizeof(in), &tv);
	}
	ok(r == out_len && memcmp(out, in, out_len) == 0,
	   "%s: client, receive message", name);

	close(sock);

	// cleanup

	server_stop(&server_ctx);
	close(server);
}

static void handler_noop(int sock, void *data)
{
}

static void test_unconnected(void)
{
	int r = 0;
	int sock = -1;
	const struct sockaddr_storage local = addr_local();
	struct timeval tv = { 0 };

	uint8_t buffer[] = { 'k', 'n', 'o', 't' };
	ssize_t buffer_len = sizeof(buffer);

	// server

	int server = net_bound_socket(SOCK_DGRAM, &local, 0);
	ok(server >= 0, "UDP, create server socket");

	server_ctx_t server_ctx = { 0 };
	r = server_start(&server_ctx, server, SOCK_DGRAM, handler_noop, NULL);
	ok(r, "UDP, start server");

	// UDP

	sock = net_unbound_socket(SOCK_DGRAM, &local);
	ok(sock >= 0, "UDP, create unbound socket");

	ok(!net_is_connected(sock), "UDP, is not connected");

	r = net_dgram_send(sock, buffer, buffer_len, NULL);
	ok(r == KNOT_ECONN, "UDP, send failure on unconnected socket");

	tv = TIMEOUT_SHORT;
	r = net_dgram_recv(sock, buffer, buffer_len, &tv);
	ok(r == KNOT_ETIMEOUT, "UDP, receive timeout on unconnected socket");

	struct sockaddr_storage server_addr = addr_from_socket(server);
	r = net_dgram_send(sock, buffer, buffer_len, &server_addr);
	ok(r == buffer_len, "UDP, send on defined address");

	close(sock);

	// TCP

	sock = net_unbound_socket(SOCK_STREAM, &local);
	ok(sock >= 0, "TCP, create unbound socket");

	ok(!net_is_connected(sock), "TCP, is not connected");

#ifdef __linux__
	const int expected = KNOT_ECONN;
	const char *expected_msg = "failure";
#else
	const int expected = KNOT_ETIMEOUT;
	const char *expected_msg = "timeout";
#endif

	tv = TIMEOUT_SHORT;
	r = net_stream_send(sock, buffer, buffer_len, &tv);
	ok(r == expected, "TCP, send %s on unconnected socket", expected_msg);

	tv = TIMEOUT_SHORT;
	r = net_stream_recv(sock, buffer, sizeof(buffer), &tv);
	ok(r == expected, "TCP, receive %s on unconnected socket", expected_msg);

	close(sock);

	// server termination

	server_stop(&server_ctx);
	close(server);
}

static void test_refused(void)
{
	int r = -1;

	struct sockaddr_storage addr = { 0 };
	struct timeval tv = { 0 };
	uint8_t buffer[1] = { 0 };
	int server, client;

	// unreachable remote

	addr = addr_unreachable();

	client = net_connected_socket(SOCK_STREAM, &addr, NULL);
	ok(client >= 0, "client, connected");

	tv = TIMEOUT_SHORT;
	r = net_stream_send(client, (uint8_t *)"", 1, &tv);
	ok(r == KNOT_ETIMEOUT, "client, timeout on write");
	close(client);

	client = net_connected_socket(SOCK_STREAM, &addr, NULL);
	ok(client >= 0, "client, connected");

	tv = TIMEOUT_SHORT;
	r = net_stream_recv(client, buffer, sizeof(buffer), &tv);
	ok(r == KNOT_ETIMEOUT, "client, timeout on read");
	close(client);

	// listening, not accepting

	addr = addr_local();
	server = net_bound_socket(SOCK_STREAM, &addr, 0);
	ok(server >= 0, "server, create server");
	addr = addr_from_socket(server);

	r = listen(server, LISTEN_BACKLOG);
	ok(r == 0, "server, start listening");

	client = net_connected_socket(SOCK_STREAM, &addr, NULL);
	ok(client >= 0, "client, connect");

	tv = TIMEOUT;
	r = net_stream_send(client, (uint8_t *)"", 1, &tv);
	ok(r == 1, "client, successful write");

	tv = TIMEOUT_SHORT;
	r = net_stream_recv(client, buffer, sizeof(buffer), &tv);
	ok(r == KNOT_ETIMEOUT, "client, timeout on read");

	close(client);

	// listening, closed immediately

	client = net_connected_socket(SOCK_STREAM, &addr, NULL);
	ok(client >= 0, "client, connect");

	r = close(server);
	ok(r == 0, "server, close socket");

	tv = TIMEOUT_SHORT;
	r = net_stream_send(client, (uint8_t *)"", 1, &tv);
	ok(r == KNOT_ECONN, "client, refused on write");

	close(client);
}

struct dns_handler_ctx {
	const uint8_t *expected;
	int len;
	bool raw;
	bool success;
};

static void handler_dns(int sock, void *_ctx)
{
	struct dns_handler_ctx *ctx = _ctx;

	struct timeval tv = TIMEOUT;
	uint8_t in[16] = { 0 };
	int in_len = 0;

	if (ctx->raw) {
		in_len = net_stream_recv(sock, in, sizeof(in), &tv);
	} else {
		in_len = net_dns_tcp_recv(sock, in, sizeof(in), &tv);
	}

	ctx->success = in_len == ctx->len &&
	               (ctx->len < 0 || memcmp(in, ctx->expected, in_len) == 0);
}

static void dns_send_hello(int sock)
{
	struct timeval tv = TIMEOUT;
	net_dns_tcp_send(sock, (uint8_t *)"wimbgunts", 9, &tv);
}

static void dns_send_fragmented(int sock)
{
	struct fragment { const uint8_t *data; size_t len; };

	const struct fragment fragments[] = {
		{ (uint8_t *)"\x00",     1 },
		{ (uint8_t *)"\x08""qu", 3 },
		{ (uint8_t *)"oopisk",   6 },
		{ NULL }
	};

	for (const struct fragment *f = fragments; f->len > 0; f++) {
		struct timeval tv = TIMEOUT_SHORT;
		net_stream_send(sock, f->data, f->len, &tv);
	}
}

static void dns_send_incomplete(int sock)
{
	struct timeval tv = TIMEOUT;
	net_stream_send(sock, (uint8_t *)"\x00\x08""korm", 6, &tv);
}

static void dns_send_trailing(int sock)
{
	struct timeval tv = TIMEOUT;
	net_stream_send(sock, (uint8_t *)"\x00\x05""bloitxx", 9, &tv);
}

static void test_dns_tcp(void)
{
	struct testcase {
		const char *name;
		const uint8_t *expected;
		size_t expected_len;
		bool expected_raw;
		void (*send_callback)(int sock);
	};

	const struct testcase testcases[] = {
		{ "single DNS",       (uint8_t *)"wimbgunts", 9, false, dns_send_hello },
		{ "single RAW",       (uint8_t *)"\x00\x09""wimbgunts", 11, true, dns_send_hello },
		{ "fragmented",       (uint8_t *)"quoopisk", 8, false, dns_send_fragmented },
		{ "incomplete",       NULL, KNOT_ECONN, false, dns_send_incomplete },
		{ "trailing garbage", (uint8_t *)"bloit", 5, false, dns_send_trailing },
		{ NULL }
	};

	for (const struct testcase *t = testcases; t->name != NULL; t++) {
		struct dns_handler_ctx handler_ctx = {
			.expected = t->expected,
			.len      = t->expected_len,
			.raw      = t->expected_raw,
			.success  = false
		};

		struct sockaddr_storage addr = addr_local();
		int server = net_bound_socket(SOCK_STREAM, &addr, 0);
		ok(server >= 0, "%s, server, create socket", t->name);

		int r = listen(server, LISTEN_BACKLOG);
		ok(r == 0, "%s, server, start listening", t->name);

		server_ctx_t server_ctx = { 0 };
		r = server_start(&server_ctx, server, SOCK_STREAM, handler_dns, &handler_ctx);
		ok(r, "%s, server, start handler", t->name);

		addr = addr_from_socket(server);
		int client = net_connected_socket(SOCK_STREAM, &addr, NULL);
		ok(client >= 0, "%s, client, create connected socket", t->name);

		t->send_callback(client);

		close(client);
		server_stop(&server_ctx);
		close(server);

		ok(handler_ctx.success, "%s, expected result", t->name);
	}
}

static bool socket_is_blocking(int sock)
{
	return fcntl(sock, F_GETFL, O_NONBLOCK) == 0;
}

static void test_nonblocking_mode(int type)
{
	const char *name = socktype_name(type);
	const struct sockaddr_storage addr = addr_local();

	int client = net_unbound_socket(type, &addr);
	ok(client >= 0, "%s: unbound, create", name);
	ok(!socket_is_blocking(client), "%s: unbound, nonblocking mode", name);
	close(client);

	int server = net_bound_socket(type, &addr, 0);
	ok(server >= 0, "%s: bound, create", name);
	ok(!socket_is_blocking(server), "%s: bound, nonblocking mode", name);

	if (socktype_is_stream(type)) {
		int r = listen(server, LISTEN_BACKLOG);
		ok(r == 0, "%s: bound, start listening", name);
	}

	struct sockaddr_storage server_addr = addr_from_socket(server);
	client = net_connected_socket(type, &server_addr, NULL);
	ok(client >= 0, "%s: connected, create", name);
	ok(!socket_is_blocking(client), "%s: connected, nonblocking mode", name);

	close(client);
	close(server);
}

static void test_bind_multiple(void)
{
	const struct sockaddr_storage addr = addr_local();

	// bind first socket

	int sock_one = net_bound_socket(SOCK_DGRAM, &addr, NET_BIND_MULTIPLE);
	if (sock_one == KNOT_ENOTSUP) {
		skip("not supported on this system");
		return;
	}
	ok(sock_one >= 0, "bind first socket");

	// bind second socket to the same address

	const struct sockaddr_storage addr_one = addr_from_socket(sock_one);
	int sock_two = net_bound_socket(SOCK_DGRAM, &addr_one, NET_BIND_MULTIPLE);
	ok(sock_two >= 0, "bind second socket");

	// compare sockets

	ok(sock_one != sock_two, "descriptors are different");

	const struct sockaddr_storage addr_two = addr_from_socket(sock_two);
	ok(sockaddr_cmp(&addr_one, &addr_two) == 0, "addresses are the same");

	close(sock_one);
	close(sock_two);
}

static void signal_noop(int sig)
{
}

int main(int argc, char *argv[])
{
	plan_lazy();

	signal(SIGUSR1, signal_noop);

	diag("nonblocking mode");
	test_nonblocking_mode(SOCK_DGRAM);
	test_nonblocking_mode(SOCK_STREAM);

	diag("connected sockets");
	test_connected(SOCK_DGRAM);
	test_connected(SOCK_STREAM);

	diag("unconnected sockets");
	test_unconnected();

	diag("refused connections");
	test_refused();

	diag("DNS messages over TCP");
	test_dns_tcp();

	diag("flag NET_BIND_MULTIPLE");
	test_bind_multiple();

	return 0;
}
