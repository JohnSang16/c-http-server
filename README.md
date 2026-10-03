**AI-generated code with a simulated human commit history (research experiment).**

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

