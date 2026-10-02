# c-http-server

An HTTP/1.1 server in a single C file using POSIX sockets, a hand-written request parser, and one `epoll` event loop. It has no libraries and no threads.

```
make docker-test    # build in a Linux container, run the smoke tests
make docker-bench   # 20k requests, 100 concurrent, keep-alive (ab)
make docker-run     # serve ./public on http://localhost:8080
```

Benchmark on an M-series Mac in Docker: **~177k req/s, 0 failures** (`ab -k -n 20000 -c 100`).

## What it does

- `GET` serves static files from `public/`. `/` maps to `index.html`, the `Content-Type` comes from the file extension, and any query string is ignored.
- `POST /echo` sends the request body back.
- Error responses: 400 for a malformed request or path traversal, 404 for a missing file, 405 for any other method, 413 for a request over 16KB, 414 for a URI that's too long, 431 for headers that are too big.
- Keep-alive is supported. HTTP/1.1 keeps the connection open by default and HTTP/1.0 only does so when the client asks. Pipelined requests on the same connection are handled in order.

## How it works

Read `server.c` from the bottom up, because that's the order things actually happen in.

**1. `main`: get a listening socket.** `socket` creates the endpoint, `bind` attaches it to port 8080, and `listen` tells the kernel to start queueing incoming connections. `SOCK_NONBLOCK` matters a lot here. Every call that would normally block (accept, read, write) instead returns right away with `EAGAIN`, meaning "nothing ready yet".

**2. The event loop.** `epoll_wait` puts the process to sleep until at least one socket the kernel is watching is ready, then returns only those sockets. That's the trick behind nginx and Node's libuv: one thread can handle thousands of connections because it never waits on any single one. The other options are a thread per connection (lots of memory, lots of context switching) or blocking calls (one slow client freezes everyone).

Each socket is registered with a pointer to its `struct conn`, and epoll hands that pointer back on every event. A `NULL` pointer means the listening socket.

**3. `accept_all`.** When the listening socket is readable, there are new connections waiting. Accept all of them until `EAGAIN`, give each one a `struct conn`, and register it for `EPOLLIN` ("tell me when there's data to read").

**4. `on_readable`.** Read until `EAGAIN`. **TCP is a byte stream, not a message stream.** One `read` might return half a request, or three requests stuck together. That's why bytes accumulate in `c->in` and nothing assumes one read equals one request. `read` returning 0 means the client is done sending, but the client may still want the reply, so the server answers first and then closes.

**5. `try_handle`: the parser.** A request is complete once:
- the headers have ended at the first `\r\n\r\n`, and
- `Content-Length` more bytes of body have arrived.

Until both are true it returns 0 and waits for more data. Once both are true, it parses the request line with `sscanf`, scans the headers for `Content-Length` and `Connection`, routes the request, builds the response, and then `memmove`s whatever is left to the front of the buffer, because that's the start of the next pipelined request.

**6. `flush`.** `write` can also send only part of the data when the kernel's send buffer is full. When that happens, switch the socket to `EPOLLOUT` ("tell me when I can write") and finish later. When the response is fully sent, either close the connection or switch back to `EPOLLIN` and handle any request that's already buffered.

The whole lifecycle is a small state machine per connection: **reading → (parse) → writing → reading again or closing.** epoll is what moves each connection from one state to the next.

## Deliberate limits

These are marked with `ponytail:` comments in the code, along with how you'd upgrade each one:

- 16KB fixed buffer per connection, so no big uploads
- Static files are read fully into memory (the upgrade is `sendfile()`)
- Path safety is a blunt "no `..`" rule (the upgrade is `realpath()` plus a prefix check)
- No chunked transfer encoding, no percent-decoding of URLs, no HTTPS, no timeouts for idle connections

## Interview talking points

- Why non-blocking I/O plus epoll beats a thread per connection, and what the C10K problem is
- Why parsing has to handle partial reads (TCP framing), and how `Content-Length` defines where a message ends
- Level-triggered vs edge-triggered epoll. This server uses level-triggered: easier to get right, and costs a few extra wakeups
- Why `SIGPIPE` is ignored, and what `SO_REUSEADDR` and `TIME_WAIT` are for

## Exercises (do these by hand)

1. Add an idle timeout: close any connection that's been silent for 10s. Hint: `epoll_wait`'s timeout argument plus a last-active timestamp in `struct conn`.
2. Serve large files with `sendfile()` instead of `malloc` + `fread`.
3. Switch to edge-triggered (`EPOLLET`) and work out what breaks.
4. Add `HEAD` support, which is the same as `GET` but without the body.
