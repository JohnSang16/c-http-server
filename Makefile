CFLAGS = -std=c11 -Wall -Wextra -O2 -g

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

test: server
	sh test.sh

# macOS has no epoll, so these run everything inside a Linux container
docker-test:
	docker build -t c-http-server . && docker run --rm c-http-server make test

docker-run:
	docker build -t c-http-server . && docker run --rm -p 8080:8080 c-http-server

clean:
	rm -f server

.PHONY: test docker-test docker-run clean
