CFLAGS = -std=c11 -Wall -Wextra -O2 -g

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

# macOS has no epoll, so these run everything inside a Linux container
docker-run:
	docker build -t c-http-server . && docker run --rm -p 8080:8080 c-http-server

clean:
	rm -f server

.PHONY: docker-run clean
