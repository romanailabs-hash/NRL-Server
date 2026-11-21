
#include "nrl_http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_EVENTS 256
#define REQUEST_BUF_SIZE 65536
#define RESPONSE_BUF_SIZE 262144
#define MAX_REQUEST_BODY 1048576
#define MAX_PATH_SIZE 256
#define MAX_CLIENTS 1024

typedef struct {
    int fd;
    char req_buf[REQUEST_BUF_SIZE];
    size_t req_len;
    char resp_buf[RESPONSE_BUF_SIZE];
    size_t resp_len;
    size_t resp_sent;
    int keep_alive;
    unsigned long long last_active;
} client_t;

struct http_server {
    int listen_fd;
    int epoll_fd;
    char *token;
    char bind_addr[64];
    unsigned short port;
    int running;
    client_t *clients[MAX_CLIENTS];
    int num_clients;
    http_callback_t cb_health;
    http_callback_t cb_stats;
    http_callback_t cb_tick;
    http_callback_t cb_run;
    http_callback_t cb_jobs;
    void *cb_data;
};

static unsigned long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

typedef struct { char method[16]; char path[MAX_PATH_SIZE]; char *body; size_t body_len; char token[256]; int has_token; } http_request_t;

static int parse_request(const char *buf, size_t len, http_request_t *req) {
    memset(req, 0, sizeof(*req));
    const char *p = buf, *end = buf + len;
    int i = 0;
    while (p < end && *p != ' ' && i < 15) req->method[i++] = *p++;
    req->method[i] = '\0'; if (p >= end || *p != ' ') return -1; p++;
    i = 0;
    while (p < end && *p != ' ' && i < MAX_PATH_SIZE - 1) req->path[i++] = *p++;
    req->path[i] = '\0';
    const char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) { body_start += 4; req->body = (char *)body_start; req->body_len = len - (size_t)(body_start - buf); if (req->body_len > MAX_REQUEST_BODY) req->body_len = MAX_REQUEST_BODY; }
    const char *auth = strstr(buf, "Authorization: Bearer ");
    if (auth) { auth += 21; const char *auth_end = strstr(auth, "\r\n"); if (auth_end) { size_t token_len = (size_t)(auth_end - auth); if (token_len < sizeof(req->token)) { memcpy(req->token, auth, token_len); req->token[token_len] = '\0'; req->has_token = 1; } } }
    return 0;
}

static size_t build_json_response(char *buf, size_t cap, int status, const char *json) {
    size_t body_len = json ? strlen(json) : 0;
    const char *status_str = (status == 200) ? "OK" : (status == 400) ? "Bad Request" : (status == 401) ? "Unauthorized" : (status == 404) ? "Not Found" : (status == 429) ? "Too Many Requests" : (status == 500) ? "Internal Server Error" : "Unknown";
    int n = snprintf(buf, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "Server: nrl-server/1.0\r\n"
        "\r\n"
        "%s",
        status, status_str, body_len, json ? json : "");
    return (size_t)(n > 0 ? n : 0);
}

static void handle_health(client_t *c, http_server_t *s) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"service\":\"nrl-server\",\"uptime_sec\":%llu}", (unsigned long long)(monotonic_ms()/1000ULL));
    c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, 200, buf);
}

static void route_request(client_t *c, http_server_t *s, http_request_t *r) {
    if (s->token && s->token[0] && strncmp(r->path, "/v1/", 4) == 0) {
        if (strcmp(r->token, s->token) != 0) {
            c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, 401, "{\"error\":\"unauthorized\"}");
            return;
        }
    }
    if (strcmp(r->path, "/health") == 0 && strcmp(r->method, "GET") == 0) { handle_health(c, s); }
    else if (strcmp(r->path, "/v1/tick") == 0 && strcmp(r->method, "POST") == 0) {
        char *res = s->cb_tick ? s->cb_tick(s->cb_data) : NULL;
        c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, res ? 200 : 500, res ? res : "{\"error\":\"tick failed\"}");
        free(res);
    }
    else if (strcmp(r->path, "/v1/run") == 0 && strcmp(r->method, "POST") == 0) {
        char *res = s->cb_run ? s->cb_run(s->cb_data) : NULL;
        c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, res ? 200 : 500, res ? res : "{\"error\":\"run failed\"}");
        free(res);
    }
    else if (strcmp(r->path, "/v1/stats") == 0 && strcmp(r->method, "GET") == 0) {
        char *res = s->cb_stats ? s->cb_stats(s->cb_data) : NULL;
        c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, res ? 200 : 500, res ? res : "{\"error\":\"stats unavailable\"}");
        free(res);
    }
    else if (strncmp(r->path, "/v1/jobs/", 9) == 0) {
        char *res = s->cb_jobs ? s->cb_jobs(s->cb_data) : NULL;
        c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, res ? 200 : 500, res ? res : "{\"error\":\"jobs unavailable\"}");
        free(res);
    }
    else { c->resp_len = build_json_response(c->resp_buf, RESPONSE_BUF_SIZE, 404, "{\"error\":\"not found\"}"); }
}

static client_t *accept_client(http_server_t *s) {
    struct sockaddr_in addr; socklen_t addrlen = sizeof(addr);
    int fd = accept4(s->listen_fd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return NULL;
    int flag = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET | EPOLLRDHUP, .data.fd = fd };
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) { close(fd); return NULL; }
    client_t *c = calloc(1, sizeof(client_t)); if (!c) { close(fd); return NULL; }
    c->fd = fd; c->keep_alive = 1; c->last_active = monotonic_ms();
    for (int i = 0; i < MAX_CLIENTS; i++) { if (s->clients[i] == NULL) { s->clients[i] = c; s->num_clients++; break; } }
    return c;
}

static void close_client(http_server_t *s, client_t *c, int idx) {
    if (!c) return;
    if (c->fd >= 0) { epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL); close(c->fd); }
    if (idx >= 0 && idx < MAX_CLIENTS) { s->clients[idx] = NULL; s->num_clients--; }
    free(c);
}

http_server_t *http_server_create(const char *bind_addr, unsigned short port, const char *token) {
    http_server_t *s = calloc(1, sizeof(http_server_t)); if (!s) return NULL;
    strncpy(s->bind_addr, bind_addr ? bind_addr : "127.0.0.1", sizeof(s->bind_addr)-1);
    s->port = port > 0 ? port : 8080;
    if (token) s->token = strdup(token);
    s->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s->listen_fd < 0) { free(s); return NULL; }
    int opt = 1; setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(s->port), .sin_addr = { .s_addr = inet_addr(s->bind_addr) } };
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s->listen_fd); free(s->token); free(s); return NULL; }
    if (listen(s->listen_fd, SOMAXCONN) < 0) { close(s->listen_fd); free(s->token); free(s); return NULL; }
    s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s->epoll_fd < 0) { close(s->listen_fd); free(s->token); free(s); return NULL; }
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = s->listen_fd };
    epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_fd, &ev);
    return s;
}

void http_server_set_callbacks(http_server_t *s, http_server_handlers_t *h, void *cb_data) {
    if (!s || !h) return;
    s->cb_health = h->on_health; s->cb_stats = h->on_stats; s->cb_tick = h->on_tick; s->cb_run = h->on_run; s->cb_jobs = h->on_jobs; s->cb_data = cb_data;
}

int http_server_run(http_server_t *s) {
    if (!s) return -1;
    s->running = 1;
    struct epoll_event events[MAX_EVENTS];
    while (s->running) {
        int nfds = epoll_wait(s->epoll_fd, events, MAX_EVENTS, 100);
        if (nfds < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == s->listen_fd) { while (accept_client(s)); continue; }
            client_t *c = NULL; int ci = -1;
            for (int j = 0; j < MAX_CLIENTS; j++) { if (s->clients[j] && s->clients[j]->fd == fd) { c = s->clients[j]; ci = j; break; } }
            if (!c) continue;
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) { close_client(s, c, ci); continue; }
            if (events[i].events & EPOLLIN) {
                ssize_t n = read(c->fd, c->req_buf + c->req_len, REQUEST_BUF_SIZE - c->req_len - 1);
                if (n <= 0) { close_client(s, c, ci); continue; }
                c->req_len += (size_t)n; c->req_buf[c->req_len] = '\0';
                if (strstr(c->req_buf, "\r\n\r\n") == NULL) continue;
                http_request_t r; parse_request(c->req_buf, c->req_len, &r);
                route_request(c, s, &r);
                struct epoll_event ev2 = { .events = EPOLLOUT | EPOLLET | EPOLLRDHUP, .data.fd = c->fd };
                epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev2);
            }
            if (events[i].events & EPOLLOUT) {
                ssize_t n = write(c->fd, c->resp_buf + c->resp_sent, c->resp_len - c->resp_sent);
                if (n < 0) { if (errno != EAGAIN) { close_client(s, c, ci); } continue; }
                c->resp_sent += (size_t)n;
                if (c->resp_sent >= c->resp_len) {
                    c->req_len = 0; c->resp_len = 0; c->resp_sent = 0;
                    struct epoll_event ev2 = { .events = EPOLLIN | EPOLLET | EPOLLRDHUP, .data.fd = c->fd };
                    epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev2);
                }
            }
        }
    }
    return 0;
}

void http_server_stop(http_server_t *s) { if (s) s->running = 0; }

void http_server_destroy(http_server_t *s) {
    if (!s) return;
    s->running = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) { if (s->clients[i]) close_client(s, s->clients[i], i); }
    if (s->epoll_fd >= 0) close(s->epoll_fd);
    if (s->listen_fd >= 0) close(s->listen_fd);
    free(s->token); free(s);
}
