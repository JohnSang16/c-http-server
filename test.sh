#!/bin/sh
# Smoke test: start the server, hit every route, fail loudly on the first mismatch.
PORT=18080
URL=http://127.0.0.1:$PORT
./server $PORT public >/dev/null & PID=$!
trap 'kill $PID' EXIT
sleep 1

check() {
  if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$3', got '$2'"; exit 1; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

check "GET / serves index.html"  "$(code $URL/)" 200
check "html content type"        "$(curl -s -o /dev/null -w '%{content_type}' $URL/)" "text/html; charset=utf-8"
check "css content type"         "$(curl -s -o /dev/null -w '%{content_type}' $URL/style.css)" "text/css"
check "query string ignored"     "$(code "$URL/style.css?v=2")" 200
check "missing file is 404"      "$(code $URL/nope.html)" 404
check "DELETE is 405"            "$(code -X DELETE $URL/)" 405
check "path traversal is 400"    "$(code --path-as-is $URL/../etc/passwd)" 400
check "garbage request is 400"   "$(printf 'nonsense\r\n\r\n' | nc 127.0.0.1 $PORT | head -1 | tr -d '\r')" "HTTP/1.1 400 Bad Request"
echo "all passed"
