#!/bin/sh
# quick smoke test, starts the server and hits each route. stops at the first fail
PORT=18080
URL=http://127.0.0.1:$PORT
./server $PORT public >/dev/null & PID=$!
trap 'kill $PID' EXIT
sleep 1

check() {
  if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$3', got '$2'"; exit 1; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

check "get /"                 "$(code $URL/)" 200
check "html content type"     "$(curl -s -o /dev/null -w '%{content_type}' $URL/)" "text/html; charset=utf-8"
check "css content type"      "$(curl -s -o /dev/null -w '%{content_type}' $URL/style.css)" "text/css"
check "query string"          "$(code "$URL/style.css?v=2")" 200
check "404"                   "$(code $URL/nope.html)" 404
check "delete 405"            "$(code -X DELETE $URL/)" 405
check "garbage 400"           "$(printf 'nonsense\r\n\r\n' | nc 127.0.0.1 $PORT | head -1 | tr -d '\r')" "HTTP/1.1 400 Bad Request"
echo "all passed"
