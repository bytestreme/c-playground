#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/event_struct.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SERVER_ADDRESS "0.0.0.0"
#define SERVER_PORT 8080

#define CHUNK_BUFFER_SIZE_KB 64

static const size_t CHUNK_SIZE = CHUNK_BUFFER_SIZE_KB * 1024;

static void fill_chunk(unsigned char *buf, size_t n) {
    if (buf != NULL) {
        for (size_t i = 0; i < n; i++) {
            buf[i] = (unsigned char) (0);
        }
    }
}

long long safe_parse_ll(const char *s) {
    if (!s || *s == '\0') {
        return -1;
    }

    errno = 0;
    char *end_ptr = NULL;

    long long val = strtoll(s, &end_ptr, 10);

    if (end_ptr == s) {
        return -1;
    }

    if (errno == ERANGE) {
        return -1;
    }

    if (*end_ptr != '\0') {
        return -1;
    }

    return val;
}

static void file_handler(struct evhttp_request *req, __attribute__((unused)) void *arg) {
    struct evkeyvalq params;
    evhttp_parse_query(evhttp_request_get_uri(req), &params);

    const char *size_param = evhttp_find_header(&params, "size");
    if (!size_param) {
        evhttp_send_error(req, 400, "Missing ?size=N");
        return;
    }

    long long total = safe_parse_ll(size_param);
    if (total <= 0) {
        evhttp_send_error(req, 400, "Invalid size");
        return;
    }

    printf("Generating %lld bytes...\n", total);

    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "application/octet-stream");

    char len_buf[64];
    snprintf(len_buf, sizeof(len_buf), "%lld", total);
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Length", len_buf);

    evhttp_send_reply_start(req, 200, "OK");

    struct evbuffer *out = evbuffer_new();
    unsigned char *buf = malloc(CHUNK_SIZE);

    size_t remaining = total;

    while (remaining > 0) {
        size_t to_write = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

        fill_chunk(buf, to_write);

        evbuffer_add(out, buf, to_write);
        evhttp_send_reply_chunk(req, out);
        evbuffer_drain(out, to_write);

        remaining -= to_write;
    }

    free(buf);
    evhttp_send_reply_end(req);
    evbuffer_free(out);
}

static void generic_handler(struct evhttp_request *req, void *arg) {
    const char *path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));

    if (strcmp(path, "/file") == 0)
        return file_handler(req, arg);

    struct evbuffer *out = evbuffer_new();
    evbuffer_add_printf(out, "Use GET /file?size=N\n");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
}

int main() {
    struct event_base *base = event_base_new();
    struct evhttp *http = evhttp_new(base);

    evhttp_set_gencb(http, generic_handler, NULL);

    if (evhttp_bind_socket(http, SERVER_ADDRESS, SERVER_PORT) != 0) {
        fprintf(stderr, "Failed to bind\n");
        return 1;
    }

    printf("Server running on http://%s:%d\n", SERVER_ADDRESS, SERVER_PORT);
    event_base_dispatch(base);

    evhttp_free(http);
    event_base_free(base);
    return 0;
}
