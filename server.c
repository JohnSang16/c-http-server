#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *root = "public";

static void send_resp(int fd, int status, const char *reason, const char *body, size_t len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, reason, len);
    write(fd, hdr, n);
    write(fd, body, len);
}

static void handle(int fd) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    char method[16], path[1024];
    if (sscanf(buf, "%15s %1023s", method, path) != 2) { send_resp(fd, 400, "Bad Request", "bad request", 11); return; }
    printf("%s %s\n", method, path);

    char full[2048];
    snprintf(full, sizeof full, "%s%s", root, strcmp(path, "/") == 0 ? "/index.html" : path);
    FILE *f = fopen(full, "rb");
    if (!f) { send_resp(fd, 404, "Not Found", "not found", 9); return; }
    struct stat st;
    fstat(fileno(f), &st);
    char *body = malloc(st.st_size);
    size_t got = fread(body, 1, st.st_size, f);
    fclose(f);
    send_resp(fd, 200, "OK", body, got);
    free(body);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    if (argc > 2) root = argv[2];

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }
    printf("listening on %d\n", port);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        handle(cfd);
        close(cfd);
    }
}
