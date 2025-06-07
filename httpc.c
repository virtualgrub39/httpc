#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "config.h"

#define log_err(msg) fprintf(stderr, "«error» " msg "\n")
#define log_errf(fmt, ...) fprintf(stderr, "«error» " fmt "\n", __VA_ARGS__)
#define log_info(msg) fprintf(stdout, "«info» " msg "\n")
#define log_infof(fmt, ...) fprintf(stdout, "«info» " fmt "\n", __VA_ARGS__)

#define UNUSED(arg) ((void)arg);
#define UNREACHABLE                                                            \
    do {                                                                       \
        log_errf(                                                              \
          "Entered unreachable part of code: %s:%u", __FILE__, __LINE__);      \
        abort();                                                               \
    }
#define FALSE (0)
#define TRUE (1)
typedef unsigned int uint;

typedef void (*event_callback_t)(int fd, uint32_t events, void* user_data);

typedef struct {
    event_callback_t callback;
    void* user_data;
    uint32_t events;
} EventData;

typedef struct {
    int key; // fd
    EventData value;
} EventEntry;

typedef struct {
    int epoll_fd;
    EventEntry* events;
    int max_events;
} EventManager;

static int serv_sockfd;
EventManager em;

int
em_init(EventManager* em, uint max_events)
{
    assert(em != NULL);

    em->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (em->epoll_fd < 0) {
        perror("epoll_create1() failed");
        return -1;
    }

    em->events = NULL;
    em->max_events = max_events;

    return 0;
}

int
em_add_event(EventManager* em,
             int fd,
             uint32_t events,
             event_callback_t callback,
             void* user_data)
{
    assert(em != NULL);
    assert(callback != NULL);
    assert(fd > 0);

    if (hmgeti(em->events, fd) >= 0)
        return 0;

    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(em->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        perror("epoll_ctl(ADD) failed");
        return -1;
    }

    EventData entry = {
        .callback = callback,
        .events = events,
        .user_data = user_data,
    };

    hmput(em->events, fd, entry);

    return 0;
}

int
em_remove_events(EventManager* em, int fd)
{
    assert(em != NULL);
    assert(fd > 0);

    int idx = hmgeti(em->events, fd);
    if (idx < 0)
        return -1;

    if (epoll_ctl(em->epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        perror("epoll_ctl(DEL) failed");
        return -1;
    }

    UNUSED(hmdel(em->events, fd));

    return 0;
}

int
em_modify_event(EventManager* em, int fd, uint32_t events)
{
    assert(em != NULL && fd > 0);
    
    int idx = hmgeti(em->events, fd);
    if (idx < 0) return -1;
    
    struct epoll_event ev = { .events = events, .data.fd = fd };
    if (epoll_ctl(em->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        perror("epoll_ctl(MOD) failed");
        return -1;
    }
    
    em->events[idx].value.events = events;
    return 0;
}

int
em_poll_events(EventManager* em, int timeout_ms)
{
    struct epoll_event* events =
      malloc(em->max_events * sizeof(struct epoll_event));
    assert(events != NULL);

    int ne = epoll_wait(em->epoll_fd, events, em->max_events, timeout_ms);
    if (ne < 0) {
        free(events);
        if (errno == EINTR)
            return 0;
        perror("epoll_wait() failed");
        return -1;
    }

    for (int i = 0; i < ne; ++i) {
        int fd = events[i].data.fd;
        uint32_t event_mask = events[i].events;

        if (hmgeti(em->events, fd) < 0)
            continue;

        EventData* data = &hmget(em->events, fd);
        assert(data->callback != NULL);
        data->callback(fd, event_mask, data->user_data);
    }

    free(events);
    return 0;
}

int
em_get_event_cout(const EventManager* em)
{
    if (!em || !em->events) {
        return 0;
    }
    return hmlen(em->events);
}

#define READ_EVENTS (EPOLLIN | EPOLLRDHUP | EPOLLET)
#define WRITE_EVENTS (EPOLLOUT)

void
em_cleanup(EventManager* em)
{
    assert(em != NULL);

    if (em->epoll_fd >= 0) {
        close(em->epoll_fd);
        em->epoll_fd = -1;
    }

    if (em->events) {
        hmfree(em->events);
        em->events = NULL;
    }
}

void
signal_handler(int sig)
{
    UNUSED(sig);

    fprintf(stdout, "\nShutting down server...\n");

    em_cleanup(&em);
    if (serv_sockfd >= 0) {
        close(serv_sockfd);
        serv_sockfd = -1;
    }

    exit(0);
}

int
create_server_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        perror("socket() failed");
        return fd;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt(SO_REUSEADDR) failed");
        goto create_server_socket_error;
    }

    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind() failed");
        goto create_server_socket_error;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen() failed");
        goto create_server_socket_error;
    }

    return fd;

create_server_socket_error:
    close(fd);
    return -1;
}

int
set_sock_blocking(int fd, int blocking)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL) failed");
        return -1;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        perror("fcntl(F_SETFL) failed");
        return -1;
    }

    return 0;
}

void
client_disconnect(int fd)
{
    assert(fd >= 0);

    em_remove_events(&em, fd);
    close(fd);
}

void
client_data_cb(int fd, uint32_t events, void* data)
{
    UNUSED(data);

    if (events & EPOLLRDHUP) {
        log_infof("client %u disconnected", fd);
        client_disconnect(fd);
        return;
    }

    char buffer[1024];
    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) 
            return;
        perror("recv() failed");
        client_disconnect(fd);
        return;
    }
    if (n == 0) {
        log_infof("client %u disconnected", fd);
        client_disconnect(fd);
        return;
    }

    send(fd, buffer, n, 0);

    return;
}

void
serv_accept_cb(int fd, uint32_t events, void* data)
{
    if (events & EPOLLRDHUP) {
        em_remove_events(&em, fd);
        return;
    }
    else if (!(events & EPOLLIN)) {
        log_errf("received invalid event: 0x%02X", events);
        return;
    }

    int client_sockfd = accept(fd, NULL, 0);
    if (client_sockfd < 0) {
        perror("accept() failed");
        return;
    }

    if (set_sock_blocking(client_sockfd, FALSE) < 0)
        return;

    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(TCP_NODELAY) failed");
        close(fd);
        return;
    }

    log_infof("client %u connected", fd);

    if (em_add_event(&em, client_sockfd, READ_EVENTS, client_data_cb, data) < 0)
        return;
}

int
main(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // TODO: read port from user
    int port = HTTPC_DEFAULT_PORT;

    serv_sockfd = create_server_socket(port);
    if (serv_sockfd < 0)
        exit(EXIT_FAILURE);

    if (set_sock_blocking(serv_sockfd, FALSE) < 0)
        exit(EXIT_FAILURE);

    if (em_init(&em, MAX_EVENTS) < 0)
        goto main_error_close_serv_sockfd;

    em_add_event(&em, serv_sockfd, READ_EVENTS, serv_accept_cb, NULL);

    log_infof("Server listening on :%u", port);

    while (TRUE == TRUE) {
        if (em_poll_events(&em, -1) < 0)
            break;
    }

    // main_error_em_cleanup:
    em_cleanup(&em);
main_error_close_serv_sockfd:
    close(serv_sockfd);
    return EXIT_FAILURE;
}
