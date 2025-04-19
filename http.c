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
#define MAX_BUFFER_CAPACITY 8 * 1024 * 100 // 100 kB per request
#define MAX_HEADER_COUNT 100 // per request

#define UNREACHABLE 								\
 do { 												\
 	 assert(!"Entered unreachable block of code"); 	\
 	 abort();										\
 } while (0)

#define TODO(msg)									\
 do {												\
 	 assert(!msg);									\
 	 abort();										\
 } while (0)

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
	_exit(0);
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
				fprintf(stderr, "Buffer size exceeded by the client.\n");
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

ssize_t
read_exact(ClientContext* ctx, size_t nbytes)
{
	return read(ctx->sockfd, ctx->buffer + ctx->buffer_size, nbytes);
}

ssize_t
read_chunks(ClientContext* ctx)
{
	(void)ctx;
	TODO("Implement reading of chunked body.");
}

int
read_request(ClientContext* ctx) {
	char tbuffer[512] = { 0 };
	ssize_t n = -1;

	bool read_headers = false;
	enum { UNSPECIFIED, FIXED_LENGTH, CHUNKED, } transfer_type = UNSPECIFIED;
	size_t transfer_param = 0;

	while (true) {
		n = read(ctx->sockfd, tbuffer, sizeof(tbuffer));
		if (n <= 0) return n;

		if (ctx->buffer_size + n >= ctx->buffer_capacity) {
            size_t newcap = ctx->buffer_capacity
                              ? ctx->buffer_capacity * 2
                              : sizeof(tbuffer);
            if (newcap > MAX_BUFFER_CAPACITY) {
                fprintf(stderr, "Client exceeded max buffer capacity\n");
                return 0;
            }
            ctx->buffer = realloc(ctx->buffer, newcap);
            assert(ctx->buffer != NULL);
            ctx->buffer_capacity = newcap;
        }

		memcpy(ctx->buffer+ctx->buffer_size, tbuffer, n);
		ctx->buffer_size += n;

		if (!read_headers) {
			char* p = memmem(ctx->buffer, ctx->buffer_size, "\r\n\r\n", 4);
			if (!p) continue;

			read_headers = true;

			char* head = (char*)malloc(ctx->buffer_size + 1);
			assert(head != NULL);
			memcpy(head, ctx->buffer, ctx->buffer_size);
			head[ctx->buffer_size] = 0;

			char* line = strtok(head, "\r\n");
			line = strtok(NULL, "\r\n");	// we don't care about the first line :3

			while (line) {
				char* cl = strcasestr(line, "content-length");
				if (cl) {
					transfer_type = FIXED_LENGTH;
					transfer_param = strtoul(cl + strlen("content-length:"), NULL, 10);
				}

				char* te = strcasestr(line, "transfer-encoding");
				if (te && strcasestr(line, "chunked") != NULL) {
					transfer_type = CHUNKED;
				}

				line = strtok(NULL, "\r\n");
			}

			free(head);
		}

		switch (transfer_type) {
		case FIXED_LENGTH: {
			ssize_t n = read_exact(ctx, transfer_param);
			if (n < 0) return n;
			return ctx->buffer_size;
		}
		case CHUNKED: {
			ssize_t n = read_chunks(ctx);
			if (n < 0) return n;
			return ctx->buffer_size;
		}
		default:
			// assume no body;
			return ctx->buffer_size;
		return 0;
		}
	}

	UNREACHABLE;
}

void*
client_thread_handler(void* arg)
{
	ClientContext* ctx = (ClientContext*)arg;
	bool keep_alive = false;

	while (true) {
		if (read_request(ctx) < 0) {
			switch (errno) {
			case EAGAIN:
				fprintf(stderr, "Connection (%u) timed out\n", ctx->id);
				break;
			default:
				perror("Failed to read data from client connection");
			}
			break;
		}

		if (ctx->buffer_size == 0) {
			// fprintf(stderr, "Connection %u closed by the client.\n", ctx->id);
			// connection ended by the client - no need to report anything.
			break;
		}
		
		ctx->buffer[ctx->buffer_size + 1] = 0;
		// printf("REQUEST (%lu bytes)\n%.*s\nEND-REQUEST;\n", ctx->buffer_size, (int)ctx->buffer_size, ctx->buffer);

		// write(ctx->sockfd, ctx->buffer, ctx->buffer_size); // echo :)

		char* scalp = strtok(ctx->buffer, "\r\n");
		char* header = strtok(NULL, "\r\n");

		while (header && strlen(header) != 0) {
			// parse headers, whatever :)

			if (strcasestr(header, "Connection") && strcasestr(header, "keep-alive")) {
				keep_alive = true;
			}

			header = strtok(NULL, "\r\n");
		}

		if (header) {
			// there is a body - I don't think we care?
		}

		char* method = strtok(scalp, " ");
		char* path = strtok(NULL, " ");
		char* version = strtok(NULL, "\r\n");

		fprintf(stderr, "REQUEST (%u): [%s %s]\n", ctx->id, method, path);

		if (!strcasestr(method, "GET")) {
			const char* err_405 = 
			"HTTP/1.1 405 Method Not Allowed\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 21\r\n"
			"\r\n"
			"Method not supported.";

			write(ctx->sockfd, err_405, strlen(err_405));

			if (keep_alive) continue;
			break;
		}

		char* relpath = malloc(strlen(path) + 2);
		sprintf(relpath, ".%s", path);
		if (strstr(relpath, "..")) {
			const char* err_403 =
			"HTTP/1.1 403 Forbidden\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 31\r\n"
			"\r\n"
			"Only absolute path is supported";

			write(ctx->sockfd, err_403, strlen(err_403));

			if (keep_alive) continue;
			break;
		}

		FILE* fp = fopen(relpath, "rb");
		if (fp == NULL) {
			const char* err_404 =
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 14\r\n"
			"\r\n"
			"File not found";

			write(ctx->sockfd, err_404, strlen(err_404));
			
			if (keep_alive) continue;
			break;
		}

		

		// TODO: try to open specified file
		// TODO: error 404 if not successfull
		// TODO: send file if successfull
		// TODO: path sanitation
		// TODO: autoindexing

		const char* dumb_reply =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"Keep-Alive: timeout=5, max=100\r\n"
		"Content-Length: 35\r\n"
		"\r\n"
		"<h1>I'm going to touch you >:3</h1>";

		write(ctx->sockfd, dumb_reply, strlen(dumb_reply));
		
		ctx->buffer_size = 0;

		if (!keep_alive) break;
	}

	close(ctx->sockfd);
	free(ctx->buffer);
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
		return 1;
	}

	if (!socket_set_timeout(serv_sockfd, DEFAULT_TIMEOUT)) {
		perror("socket_set_timeout()");
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

	UNREACHABLE;	
}
