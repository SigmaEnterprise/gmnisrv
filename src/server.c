nclude <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "config.h"
#include "gemini.h"
#include "log.h"
#include "server.h"
#include "tls.h"

int
server_init(struct gmnisrv_server *server, struct gmnisrv_config *conf)
{
	server->conf = conf;
	for (struct gmnisrv_bind *b = conf->binds; b; b = b->next) {
		++server->nlisten;
	}

	if (server->nlisten >= 1024) {
		fprintf(stderr, "Too many listen addresses (max 1023)\n");
		return 1;
	}
	server->nfds = server->nlisten;
	server->fds = calloc(1024, sizeof(struct pollfd));
	server->fdsz = 1024;
	if (!server->fds) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	server->clientsz = 1024;
	server->clients = calloc(server->clientsz, sizeof(struct gmnisrv_client));
	if (!server->clients) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	size_t i = 0;
	for (struct gmnisrv_bind *b = conf->binds; b; b = b->next) {
		int sockfd = socket(b->family, SOCK_STREAM, 0);
		if (sockfd < 0) {
			fprintf(stderr,
				"Failed to establish socket %s: %s\n",
				b->name, strerror(errno));
			return 1;
		}
		static const int t = 1;
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t));

		struct sockaddr_in in = {0};
		struct sockaddr_in6 in6 = {0};
		struct sockaddr *addr;
		size_t addrsz;
		if (b->family == AF_INET) {
			in.sin_family = AF_INET;
			in.sin_port = htons(b->port);
			memcpy(&in.sin_addr, b->addr, sizeof(struct in_addr));
			addr = (struct sockaddr *)&in;
			addrsz = sizeof(in);
		} else if (b->family == AF_INET6) {
			in6.sin6_family = AF_INET6;
			in6.sin6_port = htons(b->port);
			memcpy(&in6.sin6_addr, b->addr, sizeof(struct in6_addr));
			addr = (struct sockaddr *)&in6;
			addrsz = sizeof(in6);
#ifdef IPV6_V6ONLY
			setsockopt(sockfd, IPPROTO_IPV6,
				IPV6_V6ONLY, &t, sizeof(t));
#endif
		} else {
			return 1;
		}

		int r = bind(sockfd, addr, addrsz);
		if (r == -1) {
			fprintf(stderr,
				"Failed to bind socket %s: %s\n",
				b->name, strerror(errno));
			close(sockfd);
			return 1;
		}

		r = listen(sockfd, 16);
		if (r == -1) {
			fprintf(stderr, "Failed to listen on %s: %s\n",
				b->name, strerror(errno));
			close(sockfd);
			return 1;
		}
		server_log("listening on %s", b->name);

		server->fds[i].fd = sockfd;
		server->fds[i].events = POLLIN;
		++i;
	}

	return 0;
}

static struct pollfd *
alloc_pollfd(struct gmnisrv_server *server)
{
	if (server->nfds >= server->fdsz) {
		size_t fdsz = server->fdsz * 2;
		struct pollfd *new = realloc(server->fds,
			fdsz * sizeof(struct pollfd));
		if (!new) {
			fprintf(stderr, "<serv>\tOut of file descriptors!\n");
			return NULL;
		}
		server->fds = new;
		server->fdsz = fdsz;
	}
	return &server->fds[server->nfds++];
}

static void
accept_client(struct gmnisrv_server *server, int fd)
{
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);

	int sockfd = accept(fd, &addr, &addrlen);
	if (sockfd == -1) {
		server_error("accept error: %s", strerror(errno));
		if (errno == EMFILE || errno == ENFILE) {
			size_t to_close = server->nclients < 3 ? server->nclients : 3;
			for (size_t i = 0; i < to_close; ++i) {
				disconnect_client(server, &server->clients[0]);
			}
			sockfd = accept(fd, &addr, &addrlen);
			if (sockfd == -1) {
				return;
			}
			goto accepted;
		}
		return;
	}
accepted:;

	/* Set O_NONBLOCK on the *accepted* socket, not the listener */
	int flags = fcntl(sockfd, F_GETFL);
	if (flags == -1) {
		close(sockfd);
		server_error("fcntl F_GETFL: %s", strerror(errno));
		return;
	}
	if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
		close(sockfd);
		server_error("fcntl F_SETFL: %s", strerror(errno));
		return;
	}

	if (server->nclients >= server->clientsz) {
		size_t clientsz = server->clientsz * 2;
		struct gmnisrv_client *new = realloc(server->clients,
			clientsz * sizeof(struct gmnisrv_client));
		if (!new) {
			client_error(&addr, "disconnecting due to OOM condition"
