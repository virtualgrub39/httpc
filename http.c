#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#define MAX_CONNECTIONS 10

int serv_sockfd;
const char* serv_directory; 

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

void*
client_thread_handler(void* arg)
{
	int client_sockfd = *(int*)arg;

	write(client_sockfd, ":3\r\n", 4);
	close(client_sockfd);

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
		if (end != NULL) {
			printf("Invalid port\n");
			return 1;
		}
	}

	if ((serv_sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		perror("socket creation failed");
		return 1;
	}
	
	struct sockaddr_in bind_addr = { 0 };
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons(bind_port);
	if (inet_pton(bind_addr.sin_family, bind_ipv4_addr, &bind_addr.sin_addr) < 0) {
		fprintf(stderr, "Provided address is not a valid ipv4 address.\n");
		close(serv_sockfd);
		return 1;
	}

	if (bind(serv_sockfd, (struct sockaddr*)&bind_addr, sizeof bind_addr) < 0) {
		perror("Failed to bind to the provided address");
		return 1;
	}

	// struct timeval timeout = { 0 };      
    // timeout.tv_sec = 10;
    
//     if (setsockopt (serv_sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0) {
//         perror("setsockopt failed");
// 		goto end;    	
//     }
// 
//     if (setsockopt (serv_sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0) {
//     	perror("setsockopt failed");
// 		goto end;
// 	}

	listen(serv_sockfd, MAX_CONNECTIONS);

	signal(SIGINT, sigint_handler);	

	while (1) {
		int client_sockfd = accept(serv_sockfd, NULL, NULL);
		if (client_sockfd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
		
			perror("Failed to accept connection");
			continue;
		}

		pthread_t client_thread;
		pthread_create(&client_thread, NULL, client_thread_handler, (void*)&client_sockfd);
		pthread_detach(client_thread);
	}
	
	// unreachable :)
	return 0;
}
