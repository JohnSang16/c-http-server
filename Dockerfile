FROM alpine:3.20
# Plain-http mirror sidesteps TLS-intercepting proxies (Zscaler); apk still verifies package signatures.
RUN sed -i 's/https/http/' /etc/apk/repositories && apk add --no-cache build-base curl apache2-utils
WORKDIR /app
COPY . .
RUN make
EXPOSE 8080
CMD ["./server", "8080", "public"]
