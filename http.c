#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <limits.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>

#define MAX_CONNECTIONS 10
#define DEFAULT_TIMEOUT 5 // TODO: rename?
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
	fprintf(stderr, "httpc - the most basic http 1.1 server. Copyleft Hatsune Miku 2025\n");
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

		if (!read_headers) { // TODO: get rid of this shit? I don't think we'll
							 // need the body, and we read the headers only to know how to receive it.
			char* p = memmem(ctx->buffer, ctx->buffer_size, "\r\n\r\n", 4);
			if (!p) continue;

			read_headers = true;

			char* head = (char*)malloc(ctx->buffer_size + 1);
			assert(head != NULL);
			memcpy(head, ctx->buffer, ctx->buffer_size);
			head[ctx->buffer_size] = 0;

			char* line = strtok(head, "\r\n");
			line = strtok(NULL, "\r\n");

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

const char*
get_extension(const char *path)
{
    const char *basename = strrchr(path, '/');
    if (basename)
        basename++;
    else
        basename = path;

    const char *dot = strrchr(basename, '.');
    if (!dot)
        return NULL;

    if (dot == basename)
        return NULL;

    return dot + 1;
}


const char*
get_content_type(const char* path)
{
	const char* ext = get_extension(path);
	if (!ext || *ext == 0) {
		return "application/octet-stream";
	}

	if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) {
		return "text/html";
	}
	else if (strcasecmp(ext, "css") == 0) {
		return "text/css";
	}
	else if (strcasecmp(ext, "js") == 0) {
		return "application/javascript"; // EEWWWWWW
	}
	else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
		return "image/jpeg";
	}
	else if (strcasecmp(ext, "png") == 0) {
		return "image/png";
	}
	else if (strcasecmp(ext, "gif") == 0) {
		return "image/gif";
	}
	else if (strcasecmp(ext, "ico") == 0) {
		return "image/vnd.microsoft.icon"; // MICROSOFT?! IN MY CODEBASE?!
	}
	// every other file type just doesn't exist (I'm delusional)
	return "application/octet-stream";
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

		if (!method || !path || !version) {
			const char* err_400 =
			"HTTP/1.1 400 Bad Request\r\n"
			"\r\n";

			write(ctx->sockfd, err_400, strlen(err_400));
			
			goto owari_da;
		}

		fprintf(stderr, "REQUEST (%u): [%s %s]\n", ctx->id, method, path);

		if (strcasestr(version, "HTTP/1.1") == NULL) {
			const char* err_505 =
			"HTTP/1.1 505 HTTP version not supported\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 71\r\n"
			"\r\n"
			"Request made in unsupported version of HTTP.<br>Supported versions: 1.1";

			write(ctx->sockfd, err_505, strlen(err_505));

			goto owari_da;
		}

		if (strcasestr(method, "GET") == NULL) {
			const char* err_405 = 
			"HTTP/1.1 405 Method Not Allowed\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 21\r\n"
			"\r\n"
			"Method not supported.";

			write(ctx->sockfd, err_405, strlen(err_405));

			goto owari_da;
		}

		char temp[PATH_MAX] = { 0 };
		strcat(temp, serv_directory);
		strcat(temp, path);
		char rpath[PATH_MAX] = { 0 };

		const char* err_404 =
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Type: text/html\r\n"
		"Content-Length: 14\r\n"
		"\r\n"
		"File not found";

		if (!realpath(temp, rpath)) {
			write(ctx->sockfd, err_404, strlen(err_404));
			
			goto owari_da;
		}

		const char* err_403 =
		"HTTP/1.1 403 Forbidden\r\n"
		"Content-Type: text/html\r\n"
		"Content-Length: 13\r\n"
		"\r\n"
		"Access Denied";

		struct stat st = { 0 };
		if (stat(rpath, &st) < 0) {
			write(ctx->sockfd, err_403, strlen(err_403));
						
			goto owari_da;
		}

		if (S_ISDIR(st.st_mode)) {
			// TODO: autoindexing ?
			write(ctx->sockfd, err_403, strlen(err_403));
		} else if (S_ISREG(st.st_mode)) {
			int filefd = open(rpath, O_RDONLY);

			const char* content_type = get_content_type(rpath);

			write(ctx->sockfd, "HTTP/1.1 200 OK\r\n", 17);

			if (keep_alive) {
				write(ctx->sockfd, "Keep-Alive: timeout=5, max=100\r\n", 32);
				// TODO: write actual defined parameters
			}

			write(ctx->sockfd, "Content-Length: ", 16);
			char len_str[32] = { 0 };
			sprintf(len_str, "%lu\r\n", st.st_size);
			write(ctx->sockfd, len_str, strlen(len_str));

			write(ctx->sockfd, "Content-Type: ", 14);
			write(ctx->sockfd, content_type, strlen(content_type));
			write(ctx->sockfd, "\r\n", 2);

			write(ctx->sockfd, "\r\n", 2);

			sendfile(ctx->sockfd, filefd, NULL, st.st_size); // I'm in love in Linus Torvalds

			close(filefd);
		} else {
			write(ctx->sockfd, err_403, strlen(err_403));
		}

		owari_da:
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

	struct timeval timeout = {0};
	timeout.tv_sec = DEFAULT_TIMEOUT;
	timeout.tv_usec = 0;

	if (setsockopt(serv_sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		perror("setsockopt(SO_RECVTIMEO)");
		return false;
	}

	if (setsockopt(serv_sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
		perror("setsockopt(SO_SNDTIMEO)");
		return false;
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
