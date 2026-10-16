
# c-http-server

http/1.1 server in C. raw sockets, hand rolled parser, one epoll loop, no threads or libraries.

## run

```
make docker-test    # build + smoke tests
make docker-bench   # ab, 20k requests, 100 concurrent, keep-alive
make docker-run     # http://localhost:8080
```

has to go through docker since mac doesnt have epoll. got ~177k req/s with `ab -k -n 20000 -c 100` on an m-series mac (inside docker)

## what it does

- GET serves files out of public/ (`/` goes to index.html), content type from the extension, query strings ignored
- POST /echo sends the body back
- 400 for bad requests and `..` paths, 404, 405, 413 over 16KB, 414, 431
- keep-alive (default on 1.1, opt in on 1.0) and pipelined requests

## how it works

easiest to read server.c from bottom up.

main sets up a non blocking listening socket (socket, bind, listen). non blocking means accept/read/write return EAGAIN right away instead of waiting.

epoll_wait sleeps until some socket is ready and only hands back those. thats how one thread handles a ton of connections, it never sits waiting on one slow client. every socket gets registered with a pointer to its struct conn and epoll gives that pointer back on each event (NULL is the listening socket).

accept_all takes every pending connection until EAGAIN.

on_readable reads until EAGAIN and appends to c->in. tcp is a byte stream so one read can be half a request or 3 of them stuck together, you cant assume one read is one request. read returning 0 means the client stopped sending, but it might still want the response so it answers first and then closes.

try_handle is the parser. a request is complete once the headers end (`\r\n\r\n`) and Content-Length bytes of body have arrived. until then it returns 0 and waits for more. after handling one it memmoves the leftover bytes to the front since thats probably the next pipelined request.

flush sends the response. write can be partial too if the kernel send buffer is full, so then it switches the socket to EPOLLOUT and finishes later.

so each connection just goes reading -> writing -> reading again, or closed

## limits / todo

- 16KB buffer per connection, no big uploads
- files get read fully into memory, should use sendfile() for big ones
- path check just rejects anything with `..` (realpath + prefix check would be better)
- no chunked encoding, no url decoding, no https, no idle timeouts

## next

1. idle timeout, close conns that are quiet for 10s (epoll_wait timeout + last active time on the conn)
2. sendfile() for big files
3. edge triggered (EPOLLET), see what breaks
4. HEAD
