FROM alpine:3.20
RUN apk add --no-cache build-base
WORKDIR /app
COPY . .
RUN make
EXPOSE 8080
CMD ["./server", "8080", "public"]
