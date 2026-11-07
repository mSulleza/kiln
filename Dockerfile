# Build image: aarch64 musl toolchain. Runs natively on Apple Silicon.
FROM alpine:3.20
RUN apk add --no-cache build-base linux-headers
