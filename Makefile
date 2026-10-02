CFLAGS = -std=c11 -Wall -Wextra -O2 -g

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f server
