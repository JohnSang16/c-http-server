CFLAGS = -std=c11 -Wall -Wextra -O2 -g

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

test: server
	sh test.sh

bench: server
	./server 8080 public & PID=$$!; sleep 1; ab -k -n 20000 -c 100 http://127.0.0.1:8080/; kill $$PID

# macOS has no epoll, so these run everything inside a Linux container
docker-test:
	docker build -t c-http-server . && docker run --rm c-http-server make test

docker-bench:
	docker build -t c-http-server . && docker run --rm c-http-server make bench

docker-run:
	docker build -t c-http-server . && docker run --rm -p 8080:8080 c-http-server

clean:
	rm -f server

.PHONY: test bench docker-test docker-bench docker-run clean
