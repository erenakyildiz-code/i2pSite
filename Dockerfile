# Build Stage
FROM alpine:3.19 AS builder
RUN apk add --no-cache gcc musl-dev
WORKDIR /app
COPY server.c .
RUN gcc server.c -o server -lpthread

# Runtime Stage
FROM alpine:3.19
WORKDIR /app

# Add non-root user
RUN addgroup -S i2puser && adduser -S i2puser -G i2puser

# Copy binary and resources
COPY --from=builder /app/server .
COPY wwwroot/ ./wwwroot/

# Permissions
RUN chown -R i2puser:i2puser /app

# Run as non-root
USER i2puser

EXPOSE 8080
CMD ["./server"]
