#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#define MAX_CONNECTIONS 10
#define DEFAULT_TIMEOUT 5
#define DEFAULT_MAX_REQUESTS 100
#define MAX_BUFFER_CAPACITY 1024*1024 // 10MB

int serv_sockfd;
const char* serv_directory;

typedef struct {
	uint32_t id;
	int sockfd;
	uint16_t max_requests;
	uint16_t current_requests;
	char* buffer;
	size_t buffer_capacity;
	size_t buffer_size;
} ClientContext;

void
sigint_handler(int _)
{
	(void)_;
	close(serv_sockfd);
	_exit(0); // TODO: exit supposedly kills all detached threads. Verify.
}

void
usage(const char* progname)
{
	fprintf(stderr, "httpc - the most basic http 1.1 server. Copyleft :3 Mikolaj Trafisz 2025.\n");
	fprintf(stderr, "usage: %s <ipv4-address>[:port, default=8080] [directory, default='.']\n", progname);
}

bool
socket_set_timeout(int sockfd, uint16_t time_secs)
{
	struct timeval timeout = {0};
	timeout.tv_sec = time_secs;
	timeout.tv_usec = 0;

	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		perror("setsockopt(SO_RECVTIMEO)");
		return false;
	}

	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
		perror("setsockopt(SO_SNDTIMEO)");
		return false;
	}

	return true;
}

bool
read_all(ClientContext* ctx)
{
	char tbuffer[512] = { 0 };
	ssize_t n = -1;

	while ((n = read(ctx->sockfd, tbuffer, sizeof(tbuffer))) >= 0) {
		if (n + ctx->buffer_size >= ctx->buffer_capacity) {
			if (ctx->buffer_size >= MAX_BUFFER_CAPACITY) {
				fprintf(stderr, "Max. buffer size exceeded by the user.\n");
				return false;
			}
			if (ctx->buffer_size == 0) ctx->buffer_capacity = 512;
			else ctx->buffer_capacity *= 2;

			ctx->buffer = realloc(ctx->buffer, ctx->buffer_capacity);
			assert(ctx->buffer != NULL);
		}
		
		memcpy(ctx->buffer+ctx->buffer_size, tbuffer, n);
		ctx->buffer_size += n;

		if (n < (ssize_t)sizeof(tbuffer)) return true;
	}

	return false;
}

void*
client_thread_handler(void* arg)
{
	ClientContext* ctx = (ClientContext*)arg;

	while (true) {
		if (!read_all(ctx)) {
			switch (errno) {
			case EAGAIN:
				fprintf(stderr, "Connection %u timed out\n", ctx->id);
				break;
			default:
				perror("Failed to read data from client connection");
			}
			break;
		}

		if (ctx->buffer_size == 0) {
			fprintf(stderr, "Connection %u closed by the client.\n", ctx->id);
			break;
		}
		
		ctx->buffer[ctx->buffer_size + 1] = 0;
		printf("REQUEST (%lu bytes)\n%.*s\nEND-REQUEST;\n", ctx->buffer_size, (int)ctx->buffer_size, ctx->buffer);

		// write(ctx->sockfd, ctx->buffer, ctx->buffer_size); // echo :)

		// TODO: parse request :3
		
		const char* dumb_reply =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"Keep-Alive: timeout=5, max=100\r\n"
		"Content-Length: 35\r\n"
		"\r\n"
		"<h1>I'm going to touch you >:3</h1>";

		write(ctx->sockfd, dumb_reply, strlen(dumb_reply));
		
		ctx->buffer_size = 0;
	}

	close(ctx->sockfd);
	free(ctx);
	return NULL;
}

int
main(int argc, char** argv)
{
	if (argc == 1 || argc > 3) {
		usage(argv[0]);
		return 1;
	}

	const char* token = strtok(argv[1], ":");
	const char* bind_ipv4_addr = token;
	const char* bind_port_str = strtok(NULL, ":");
	serv_directory = (argc == 3) ? argv[2] : ".";

	unsigned long bind_port = 8080;

	if (bind_port_str != NULL) {
		char* end = NULL;
		bind_port = strtol(bind_port_str, &end, 10);
		if (*end != '\0') {
			printf("Invalid port\n");
			return 1;
		}
	}

	if ((serv_sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		perror("socket creation failed");
		return 1;
	}

	int optval = 1;

	if (setsockopt(serv_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
		perror("setsockopt(SO_REUSEADDR)");
		// TODO: not critical I think - do we crash?
	}
	
	struct sockaddr_in bind_addr = { 0 };
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons(bind_port);
	if (inet_pton(bind_addr.sin_family, bind_ipv4_addr, &bind_addr.sin_addr) <= 0) {
		fprintf(stderr, "Provided address is not a valid ipv4 address.\n");
		close(serv_sockfd);
		return 1;
	}

	if (bind(serv_sockfd, (struct sockaddr*)&bind_addr, sizeof bind_addr) < 0) {
		perror("Failed to bind to the provided address");
		return 1;
	}

	listen(serv_sockfd, MAX_CONNECTIONS);

	uint32_t counter = 1;

	signal(SIGINT, sigint_handler);

	while (1) {
		int client_sockfd = accept(serv_sockfd, NULL, NULL);
		if (client_sockfd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
		
			perror("Failed to accept connection");
			continue;
		}

		optval = 1;
		if (setsockopt(client_sockfd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
			perror("setsockopt(SO_KEEPALIVE)");
			close(client_sockfd);
			continue;
		}

		optval = 5;
		if (setsockopt(client_sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &optval, sizeof(optval)) < 0) {
			perror("setsockopt(TCP_KEEPINTVL)");
			close(client_sockfd);
			continue;
		}

		optval = 2;
		if (setsockopt(client_sockfd, IPPROTO_TCP, TCP_KEEPCNT, &optval, sizeof(optval)) < 0) {
			perror("setsockopt(TCP_KEEPCNT)");
			close(client_sockfd);
			continue;
		}

		socket_set_timeout(client_sockfd, DEFAULT_TIMEOUT);

		ClientContext* ctx = malloc(sizeof(ClientContext));
		assert (ctx != NULL);
		ctx->id = counter++;
		ctx->sockfd = client_sockfd;
		ctx->current_requests = 0;
		ctx->max_requests = DEFAULT_MAX_REQUESTS;
		ctx->buffer = NULL;
		ctx->buffer_size = 0;
		ctx->buffer_capacity = 0;

		pthread_t client_thread;
		pthread_create(&client_thread, NULL, client_thread_handler, ctx);
		pthread_detach(client_thread);
	}
	
	// unreachable :)
	exit(1);
}
