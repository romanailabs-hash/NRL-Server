#ifndef NRL_HTTP_H
#define NRL_HTTP_H

typedef struct http_server http_server_t;
typedef char *(*http_callback_t)(void *data);

typedef struct {
    http_callback_t on_health;
    http_callback_t on_stats;
    http_callback_t on_tick;
    http_callback_t on_run;
    http_callback_t on_jobs;
} http_server_handlers_t;

http_server_t *http_server_create(const char *bind_addr, unsigned short port, const char *token);
void http_server_set_callbacks(http_server_t *server, http_server_handlers_t *handlers, void *cb_data);
int http_server_run(http_server_t *server);
void http_server_stop(http_server_t *server);
void http_server_destroy(http_server_t *server);

#endif
