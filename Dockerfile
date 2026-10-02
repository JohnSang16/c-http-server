FROM alpine:3.20
# zscaler breaks https here. apk still checks package signatures so http is fine
RUN sed -i 's/https/http/' /etc/apk/repositories && apk add --no-cache build-base
WORKDIR /app
COPY . .
RUN make
EXPOSE 8080
CMD ["./server", "8080", "public"]
