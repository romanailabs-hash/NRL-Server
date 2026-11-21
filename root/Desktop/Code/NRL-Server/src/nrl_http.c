#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

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

/*
 * nrl_http.c — Fast Non-Blocking HTTP/1.1 + JSON Server
 *
 * Uses epoll edge-triggered for max throughput.
 * Supports: /health, /v1/stats, /v1/tick, /v1/run, /v1/jobs/<id>, /v1/jobs/<id>/kill
 */

#define MAX_EVENTS 256
#define REQUEST_BUF_SIZE 65536
#define RESPONSE_BUF_SIZE 262144
#define MAX_REQUEST_BODY 1048576  /* 1 MB max request body */
#define MAX_PATH_SIZE 256
#define MAX_CLIENTS 1024

/* -- Client connection state -- */
typedef struct {
    int fd;
    char req_buf[REQUEST_BUF_SIZE];
    size_t req_len;
    char resp_buf[RESPONSE_BUF_SIZE];
    size_t resp_len;
    size_t resp_sent;
    int keep_alive;
    unsigned long long last_active;  /* monotonic time ms */
} client_t;

/* -- Server state -- */
struct http_server {
    int listen_fd;
    int epoll_fd;
    char *token;
    char bind_addr[64];
    unsigned short port;
    int running;
    
    client_t *clients[MAX_CLIENTS];
    int num_clients;
    
    /* Server callbacks */
    http_callback_t cb_health;
    http_callback_t cb_stats;
    http_callback_t cb_tick;
    http_callback_t cb_run;
    http_callback_t cb_jobs;
    void *cb_data;
};

/* -- Internal helpers -- */
static unsigned long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static char *url_decode(const char *src, size_t len) {
    char *dst = malloc(len + 1);
    if (!dst) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len && src[i]; i++) {
        if (src[i] == '%' && i + 2 < len) {
            unsigned int code;
            if (sscanf(src + i + 1, "%2x", &code) == 1) {
                dst[j++] = (char)code;
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return dst;
}

/* Simple JSON builder (minimal, no external dep) */
static char *json_escape(const char *s, size_t *out_len)
{
    if (!s) { *out_len = 0; return strdup(""); }
    size_t len = strlen(s);
    size_t cap = len * 2 + 2;
    char *out = malloc(cap);
    if (!out) { *out_len = 0; return NULL; }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
            case '"': out[j++] = '\\'; out[j++] = '"'; break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
            case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
            case '\t': out[j++] = '\\'; out[j++] = 't'; break;
            default:
                if ((unsigned char)s[i] < 0x20) {
                    j += (size_t)snprintf(out + j, cap - j, "\\u%04x", (unsigned char)s[i]);
                } else {
                    out[j++] = s[i];
                }
                break;
        }
        if (j >= cap - 1) break;
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* -- Request parsing -- */
typedef struct {
    char method[16];
    char path[MAX_PATH_SIZE];
    char *body;
    size_t body_len;
    char token[256];
    int has_token;
} http_request_t;

static int parse_request(const char *buf, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));
    
    /* Parse request line: METHOD /path HTTP/1.1 */
    const char *p = buf;
    const char *end = buf + len;
    
    /* Method */
    int i = 0;
    while (p < end && *p != ' ' && i < 15) {
        req->method[i++] = *p++;
    }
    req->method[i] = '\0';
    if (p >= end || *p != ' ') return -1;
    p++; /* skip space */
    
    /* Path */
    i = 0;
    while (p < end && *p != ' ' && i < MAX_PATH_SIZE - 1) {
        req->path[i++] = *p++;
    }
    req->path[i] = '\0';
    
    /* Find double CRLF separating headers from body */
    const char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        req->body = (char *)body_start;
        req->body_len = len - (size_t)(body_start - buf);
        if (req->body_len > MAX_REQUEST_BODY) req->body_len = MAX_REQUEST_BODY;
    }
    
    /* Check for Authorization header */
    const char *auth = strstr(buf, "Authorization: Bearer ");
    if (auth) {
        auth += 21; /* Skip "Authorization: Bearer " */
        const char *auth_end = strstr(auth, "\r\n");
        if (auth_end) {
            size_t token_len = (size_t)(auth_end - auth);
            if (token_len < sizeof(req->token)) {
                memcpy(req->token, auth, token_len);
                req->token[token_len] = '\0';
                req->has_token = 1;
            }
        }
    }
    
    /* Check for Connection: keep-alive */
    if (strstr(buf, "Connection: keep-alive")) {
        /* Will be set by caller if needed */
    }
    
    return 0;
}

/* -- Response builders -- */
static size_t build_response(char *buf, size_t cap, int status,
                              const char *content_type,
                              const char *body)
{
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(buf, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Server: nrl-server/1.0\r\n"
        "\r\n"
        "%s",
        status,
        status == 200 ? "OK" :
        status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" :
        status == 404 ? "Not Found" :
        status == 429 ? "Too Many Requests" :
        status == 500 ? "Internal Server Error" : "Unknown",
        content_type ? content_type : "text/plain",
        body_len,
        body ? body : "");
    return (size_t)(n > 0 ? n : 0);
}

static size_t build_json_response(char *buf, size_t cap, int status, const char *json)
{
    return build_response(buf, cap, status, "application/json", json);
}

/* -- JSON endpoint handlers -- */
static void handle_health_endpoint(client_t *client, http_server_t *server)
{
    char *json = NULL;
    if (server->cb_health) {
        json = server->cb_health(server->cb_data);
    }
    if (!json) {
        unsigned long long uptime = monotonic_ms() / 1000ULL;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"service\":\"nrl-server\",\"uptime_sec\":%llu}",
            (unsigned long long)uptime);
        json = strdup(buf);
    }
    client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 200, json);
    free(json);
}

static void handle_stats_endpoint(client_t *client, http_server_t *server,
                                   http_request_t *req)
{
    (void)req;
    char *json = NULL;
    if (server->cb_stats) {
        json = server->cb_stats(server->cb_data);
    }
    if (!json) {
        json = strdup("{\"error\":\"stats not available\"}");
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 500, json);
    } else {
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 200, json);
    }
    free(json);
}

static void handle_tick_endpoint(client_t *client, http_server_t *server,
                                  http_request_t *req)
{
    char *json = NULL;
    if (server->cb_tick) {
        json = server->cb_tick(server->cb_data);
    }
    if (!json) {
        json = strdup("{\"error\":\"tick handler not available\"}");
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 500, json);
    } else {
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 200, json);
    }
    free(json);
}

static void handle_run_endpoint(client_t *client, http_server_t *server,
                                 http_request_t *req)
{
    char *json = NULL;
    if (server->cb_run) {
        json = server->cb_run(server->cb_data);
    }
    if (!json) {
        json = strdup("{\"error\":\"run handler not available\"}");
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 500, json);
    } else {
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 200, json);
    }
    free(json);
}

static void handle_jobs_endpoint(client_t *client, http_server_t *server,
                                  http_request_t *req)
{
    char *json = NULL;
    if (server->cb_jobs) {
        json = server->cb_jobs(server->cb_data);
    }
    if (!json) {
        json = strdup("{\"error\":\"jobs handler not available\"}");
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 500, json);
    } else {
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 200, json);
    }
    free(json);
}

static void handle_404(client_t *client, http_server_t *server)
{
    (void)server;
    const char *err = "{\"error\":\"not found\"}";
    client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 404, err);
}

static void handle_401(client_t *client, http_server_t *server)
{
    (void)server;
    const char *err = "{\"error\":\"unauthorized\"}";
    client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 401, err);
}

/* -- Route request to appropriate handler -- */
static void route_request(client_t *client, http_server_t *server, http_request_t *req)
{
    /* Check auth for /v1/* endpoints (but not /health) */
    if (server->token && server->token[0]) {
        if (strncmp(req->path, "/v1/", 4) == 0) {
            if (strncmp(req->token, server->token, 256) != 0) {
                handle_401(client, server);
                return;
            }
        }
    }
    
    /* Route by path */
    if (strcmp(req->path, "/health") == 0 &&
        strcmp(req->method, "GET") == 0) {
        handle_health_endpoint(client, server);
    } else if (strcmp(req->path, "/v1/stats") == 0 &&
               strcmp(req->method, "GET") == 0) {
        handle_stats_endpoint(client, server, req);
    } else if (strcmp(req->path, "/v1/tick") == 0 &&
               strcmp(req->method, "POST") == 0) {
        handle_tick_endpoint(client, server, req);
    } else if (strcmp(req->path, "/v1/run") == 0 &&
               strcmp(req->method, "POST") == 0) {
        handle_run_endpoint(client, server, req);
    } else if (strncmp(req->path, "/v1/jobs/", 9) == 0) {
        handle_jobs_endpoint(client, server, req);
    } else {
        handle_404(client, server);
    }
}

/* -- Client I/O -- */
static client_t *accept_client(http_server_t *server)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int fd = accept4(server->listen_fd, (struct sockaddr *)&addr, &addrlen,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return NULL;
    
    /* Set TCP_NODELAY */
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    /* Add to epoll */
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET | EPOLLRDHUP,
                              .data.fd = fd };
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return NULL;
    }
    
    /* Allocate client state */
    client_t *client = calloc(1, sizeof(client_t));
    if (!client) { close(fd); return NULL; }
    client->fd = fd;
    client->keep_alive = 1;
    client->last_active = monotonic_ms();
    
    /* Store in server client array */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i] == NULL) {
            server->clients[i] = client;
            server->num_clients++;
            break;
        }
    }
    
    return client;
}

static void close_client(http_server_t *server, client_t *client, int idx)
{
    if (!client) return;
    if (client->fd >= 0) {
        epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
    }
    if (idx >= 0 && idx < MAX_CLIENTS) {
        server->clients[idx] = NULL;
        server->num_clients--;
    }
    free(client);
}

static int handle_client_read(http_server_t *server, client_t *client)
{
    ssize_t n = read(client->fd, client->req_buf + client->req_len,
                     REQUEST_BUF_SIZE - client->req_len - 1);
    if (n <= 0) return -1;
    
    client->req_len += (size_t)n;
    client->req_buf[client->req_len] = '\0';
    client->last_active = monotonic_ms();
    
    /* Check if we have a complete request (detected by double CRLF) */
    if (strstr(client->req_buf, "\r\n\r\n") == NULL) {
        if (client->req_len >= REQUEST_BUF_SIZE - 1) return -1; /* Overflow */
        return 0; /* Need more data */
    }
    
    /* Parse and route */
    http_request_t req;
    if (parse_request(client->req_buf, client->req_len, &req) < 0) {
        const char *err = "{\"error\":\"bad request\"}";
        client->resp_len = build_json_response(client->resp_buf, RESPONSE_BUF_SIZE, 400, err);
    } else {
        route_request(client, server, &req);
    }
    
    /* Switch to write mode */
    struct epoll_event ev = { .events = EPOLLOUT | EPOLLET | EPOLLRDHUP,
                              .data.fd = client->fd };
    epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
    
    return 1;
}

static int handle_client_write(http_server_t *server, client_t *client)
{
    while (client->resp_sent < client->resp_len) {
        ssize_t n = write(client->fd,
                          client->resp_buf + client->resp_sent,
                          client->resp_len - client->resp_sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        client->resp_sent += (size_t)n;
    }
    return 1; /* Response fully sent */
}

/* ---- Public API ---- */

http_server_t *http_server_create(const char *bind_addr, unsigned short port,
                                   const char *token)
{
    http_server_t *server = calloc(1, sizeof(http_server_t));
    if (!server) return NULL;
    
    strncpy(server->bind_addr, bind_addr ? bind_addr : "127.0.0.1",
            sizeof(server->bind_addr) - 1);
    server->port = port > 0 ? port : 8080;
    if (token) server->token = strdup(token);
    
    /* Create socket */
    server->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server->listen_fd < 0) {
        free(server);
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server->port),
        .sin_addr = { .s_addr = inet_addr(server->bind_addr) }
    };
    
    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server->listen_fd);
        free(server->token);
        free(server);
        return NULL;
    }
    
    if (listen(server->listen_fd, SOMAXCONN) < 0) {
        close(server->listen_fd);
        free(server->token);
        free(server);
        return NULL;
    }
    
    /* Create epoll fd */
    server->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (server->epoll_fd < 0) {
        close(server->listen_fd);
        free(server->token);
        free(server);
        return NULL;
    }
    
    /* Add listen fd to epoll */
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET,
                              .data.fd = server->listen_fd };
    epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->listen_fd, &ev);
    
    return server;
}

void http_server_set_callbacks(http_server_t *server, http_server_handlers_t *handlers,
                                void *cb_data)
{
    if (!server || !handlers) return;
    server->cb_health = handlers->on_health;
    server->cb_stats = handlers->on_stats;
    server->cb_tick = handlers->on_tick;
    server->cb_run = handlers->on_run;
    server->cb_jobs = handlers->on_jobs;
    server->cb_data = cb_data;
}

int http_server_run(http_server_t *server)
{
    if (!server) return -1;
    
    server->running = 1;
    struct epoll_event events[MAX_EVENTS];
    
    while (server->running) {
        int nfds = epoll_wait(server->epoll_fd, events, MAX_EVENTS, 100); /* 100ms timeout */
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            
            if (fd == server->listen_fd) {
                /* New connection */
                while (1) {
                    client_t *client = accept_client(server);
                    if (!client) break;
                }
            } else {
                /* Find client */
                client_t *client = NULL;
                int client_idx = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (server->clients[j] && server->clients[j]->fd == fd) {
                        client = server->clients[j];
                        client_idx = j;
                        break;
                    }
                }
                if (!client) continue;
                
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    close_client(server, client, client_idx);
                    continue;
                }
                
                if (events[i].events & EPOLLIN) {
                    if (handle_client_read(server, client) < 0) {
                        close_client(server, client, client_idx);
                        continue;
                    }
                }
                
                if (events[i].events & EPOLLOUT) {
                    int rc = handle_client_write(server, client);
                    if (rc < 0) {
                        close_client(server, client, client_idx);
                        continue;
                    }
                    if (rc > 0) {
                        /* Response fully sent */
                        if (client->keep_alive) {
                            /* Reset for next request */
                            client->req_len = 0;
                            client->resp_len = 0;
                            client->resp_sent = 0;
                            struct epoll_event ev2 = { .events = EPOLLIN | EPOLLET | EPOLLRDHUP,
                                                        .data.fd = client->fd };
                            epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, client->fd, &ev2);
                        } else {
                            close_client(server, client, client_idx);
                        }
                    }
                }
            }
        }
    }
    
    return 0;
}

void http_server_stop(http_server_t *server)
{
    if (server) server->running = 0;
}

void http_server_destroy(http_server_t *server)
{
    if (!server) return;
    
    server->running = 0;
    
    /* Close all clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i]) {
            close_client(server, server->clients[i], i);
        }
    }
    
    /* Close server resources */
    if (server->epoll_fd >= 0) close(server->epoll_fd);
    if (server->listen_fd >= 0) close(server->listen_fd);
    
    free(server->token);
    free(server);
}