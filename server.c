// server.c: a tiny HTTP/1.1 server on a single-threaded epoll event loop.
// Linux only (epoll doesn't exist on macOS), so build and run it in the Docker container.
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// ponytail: fixed per-connection buffer caps a request (headers + body) at 16KB, bigger gets 413.
// Grow it dynamically if you ever want real uploads.
#define MAX_REQ 16384
#define MAX_EVENTS 128

// One of these per open client socket. epoll hands us this pointer back on every event.
struct conn {
    int fd;
    char in[MAX_REQ];       // bytes read so far, may hold a partial request or several pipelined ones
    size_t inlen;
    char *out;              // the response being sent, NULL when idle
    size_t outlen, outsent;
    int close_after;        // close the socket once `out` is fully written
};

static const char *root = "public";
static int epfd;

static void die(const char *msg) { perror(msg); exit(1); }

static const char *mime_type(const char *path) {
    static const char *map[][2] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css"},
        {".js", "text/javascript"},            {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"}, {".png", "image/png"},
        {".jpg", "image/jpeg"},                {".svg", "image/svg+xml"},
    };
    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot && i < sizeof map / sizeof map[0]; i++)
        if (strcasecmp(dot, map[i][0]) == 0) return map[i][1];
    return "application/octet-stream";
}

// Build status line + headers + body into one buffer. Sending it happens later, in flush().
static void queue_response(struct conn *c, int status, const char *reason, const char *ctype,
                           const char *body, size_t bodylen, int keep_alive) {
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, reason, ctype, bodylen, keep_alive ? "keep-alive" : "close");
    if (!(c->out = malloc(n + bodylen))) die("malloc");
    memcpy(c->out, hdr, n);
    memcpy(c->out + n, body, bodylen);
    c->outlen = n + bodylen;
    c->outsent = 0;
    c->close_after = !keep_alive;
}

static void send_error(struct conn *c, int status, const char *reason, int keep_alive) {
    queue_response(c, status, reason, "text/plain; charset=utf-8", reason, strlen(reason), keep_alive);
}

// Malformed request: we can't trust where it ends, so reply, drop the buffer, and close.
static int reject(struct conn *c, int status, const char *reason) {
    send_error(c, status, reason, 0);
    c->inlen = 0;
    return 1;
}

static void serve_file(struct conn *c, const char *path, int keep_alive) {
    // ponytail: refusing any ".." blocks directory traversal but also rejects legal names like "a..b".
    // Swap for realpath() + a prefix check against root if that ever matters.
    if (path[0] != '/' || strstr(path, "..")) { reject(c, 400, "Bad Request"); return; }

    char full[2048];
    snprintf(full, sizeof full, "%s%s%s", root, path, path[strlen(path) - 1] == '/' ? "index.html" : "");

    struct stat st;
    FILE *f;
    if (stat(full, &st) < 0 || !S_ISREG(st.st_mode) || !(f = fopen(full, "rb"))) {
        send_error(c, 404, "Not Found", keep_alive);
        return;
    }
    // ponytail: whole file goes into memory (and gets copied once more in queue_response).
    // Fine for small static sites; use sendfile() to stream large files.
    char *body = malloc(st.st_size ? st.st_size : 1);
    if (!body) die("malloc");
    size_t n = fread(body, 1, st.st_size, f);
    fclose(f);
    queue_response(c, 200, "OK", mime_type(full), body, n, keep_alive);
    free(body);
}

// Try to parse one complete request out of c->in.
// Returns 1 if a response was queued, 0 if we need more bytes first.
static int try_handle(struct conn *c) {
    // Headers end at the first blank line. Until we see it, the request isn't complete.
    char *hend = memmem(c->in, c->inlen, "\r\n\r\n", 4);
    if (!hend) return c->inlen == MAX_REQ ? reject(c, 431, "Request Header Fields Too Large") : 0;
    size_t hdr_len = hend - c->in + 4;

    // Request line, e.g. "GET /index.html HTTP/1.1"
    char *rl_end = memmem(c->in, hdr_len, "\r\n", 2);
    size_t rl_len = rl_end - c->in;
    char line[1100], method[16], path[1024], version[16];
    if (rl_len >= sizeof line) return reject(c, 414, "URI Too Long");
    memcpy(line, c->in, rl_len);
    line[rl_len] = '\0';
    if (sscanf(line, "%15s %1023s %15s", method, path, version) != 3 || strncmp(version, "HTTP/1.", 7) != 0)
        return reject(c, 400, "Bad Request");

    // Header lines. We only care about two of them.
    long long clen = 0;
    int keep_alive = strcmp(version, "HTTP/1.1") == 0;  // 1.1 defaults to keep-alive, 1.0 to close
    for (char *p = rl_end + 2; p < hend + 2;) {
        char *eol = memmem(p, hend + 2 - p, "\r\n", 2);
        size_t len = eol - p;
        if (len > 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            char num[32], *endp;
            size_t nlen = len - 15 < sizeof num - 1 ? len - 15 : sizeof num - 1;
            memcpy(num, p + 15, nlen);
            num[nlen] = '\0';
            clen = strtoll(num, &endp, 10);
            while (*endp == ' ' || *endp == '\t') endp++;
            if (endp == num || *endp || clen < 0) return reject(c, 400, "Bad Request");
        } else if (len > 11 && strncasecmp(p, "Connection:", 11) == 0) {
            char val[64];  // header values are case-insensitive: ab sends "Keep-Alive"
            size_t vlen = len - 11 < sizeof val - 1 ? len - 11 : sizeof val - 1;
            memcpy(val, p + 11, vlen);
            val[vlen] = '\0';
            if (strcasestr(val, "close")) keep_alive = 0;
            else if (strcasestr(val, "keep-alive")) keep_alive = 1;
        }
        p = eol + 2;
    }

    // Body is exactly Content-Length bytes after the headers. Wait until all of it has arrived.
    if (hdr_len + clen > MAX_REQ) return reject(c, 413, "Content Too Large");
    if (c->inlen < hdr_len + clen) return 0;
    const char *body = c->in + hdr_len;

    char *query = strchr(path, '?');
    if (query) *query = '\0';

    if (strcmp(method, "GET") == 0)
        serve_file(c, path, keep_alive);
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/echo") == 0)
        queue_response(c, 200, "OK", "text/plain; charset=utf-8", body, clen, keep_alive);
    else if (strcmp(method, "POST") == 0)
        send_error(c, 404, "Not Found", keep_alive);
    else
        send_error(c, 405, "Method Not Allowed", keep_alive);

    // Drop the request we just handled. Whatever's left is the start of the next pipelined one.
    size_t used = hdr_len + clen;
    if (c->inlen) {  // reject() may already have emptied the buffer
        memmove(c->in, c->in + used, c->inlen - used);
        c->inlen -= used;
    }
    return 1;
}

static void close_conn(struct conn *c) {
    close(c->fd);  // closing the fd also removes it from epoll
    free(c->out);
    free(c);
}

// Tell epoll what we're waiting for on this socket: more request bytes (EPOLLIN) or room to write (EPOLLOUT).
static void set_events(struct conn *c, uint32_t events) {
    struct epoll_event ev = {.events = events, .data.ptr = c};
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

// Write as much of the pending response as the socket will take right now.
static void flush(struct conn *c) {
    while (c->out) {
        while (c->outsent < c->outlen) {
            ssize_t n = write(c->fd, c->out + c->outsent, c->outlen - c->outsent);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) { set_events(c, EPOLLOUT); return; }  // kernel send buffer full, resume when writable
            if (n < 0) { close_conn(c); return; }
            c->outsent += n;
        }
        free(c->out);
        c->out = NULL;
        if (c->close_after) { close_conn(c); return; }
        try_handle(c);  // a pipelined request may already be sitting in the buffer
    }
    set_events(c, EPOLLIN);
}

static void on_readable(struct conn *c) {
    int eof = 0;
    // Read everything available right now. Non-blocking, so EAGAIN means "drained, come back later".
    while (c->inlen < MAX_REQ) {
        ssize_t n = read(c->fd, c->in + c->inlen, MAX_REQ - c->inlen);
        if (n > 0) { c->inlen += n; continue; }
        if (n == 0) { eof = 1; break; }  // client finished sending (may still want our reply)
        if (errno == EINTR) continue;
        if (errno == EAGAIN) break;
        close_conn(c);
        return;
    }
    if (try_handle(c)) {
        if (eof) c->close_after = 1;
        flush(c);
    } else if (eof) {
        close_conn(c);
    }
}

static void accept_all(int lfd) {
    // ponytail: on EMFILE (out of fds) the listen socket stays readable and we spin.
    // Real servers keep a spare fd to accept-and-close with; add that if you load test past ulimit -n.
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) return;  // EAGAIN: no more pending connections
        struct conn *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    if (argc > 2) root = argv[2];
    signal(SIGPIPE, SIG_IGN);  // writing to a socket the client closed should return EPIPE, not kill us

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd < 0) die("socket");
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);  // lets us restart without waiting out TIME_WAIT
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) die("bind");
    if (listen(lfd, SOMAXCONN) < 0) die("listen");

    if ((epfd = epoll_create1(0)) < 0) die("epoll_create1");
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};  // NULL ptr marks the listening socket
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    printf("listening on :%d, serving %s/\n", port, root);
    fflush(stdout);

    // The event loop. One thread, many sockets: sleep until the kernel says some of them are ready.
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait");
        }
        for (int i = 0; i < n; i++) {
            struct conn *c = events[i].data.ptr;
            if (!c) accept_all(lfd);
            else if (events[i].events & (EPOLLERR | EPOLLHUP)) close_conn(c);
            else if (events[i].events & EPOLLIN) on_readable(c);
            else if (events[i].events & EPOLLOUT) flush(c);
        }
    }
}
