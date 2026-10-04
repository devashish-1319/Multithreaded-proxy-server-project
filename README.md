# Multithreaded Proxy Server

A multithreaded HTTP/HTTPS forward proxy with an LRU response cache, written in C++17 on top of POSIX sockets.

## Features

- **Concurrent clients**: one detached worker thread per connection, capped at 400 live connections. Extra clients wait in the kernel's listen backlog.
- **HTTP `GET` proxying**: parses the request, forwards it to the origin, and streams the response back to the client.
- **LRU cache**: responses are cached in memory (200 MB total, 10 MB per entry).
  - Lookup and eviction are O(1) (`std::list` + `std::unordered_map`).
  - The key is `host:port/path`, so different browsers and headers share entries.
  - Only complete `200` responses are cached. Partial transfers and errors are not.
  - Cached data is reference-counted, so evicting an entry can't free data another thread is still sending.
- **HTTPS via `CONNECT`**: opens a TCP tunnel and relays raw bytes in both directions (handles half-close, 60 s idle timeout). TLS is never decrypted.
- **Robustness**: `SIGPIPE` ignored, socket timeouts, thread-safe DNS (`getaddrinfo`, IPv4 and IPv6), proper 400/501/502 responses, and `accept()` errors don't stop the server.

## Build and run

```sh
make
./proxy 8080
```

Requires a C++17 compiler (`g++` or `clang++`) on macOS or Linux.

## Try it

```sh
# plain HTTP (run it twice: the second request is served from the cache)
curl -x localhost:8080 http://example.com/

# HTTPS through a CONNECT tunnel
curl -x localhost:8080 https://example.com/
```

The server log prints `Cache MISS` / `Cache HIT` for each `GET`, and `CONNECT tunnel open/closed` for HTTPS.

## Layout

| File | Purpose |
| --- | --- |
| `proxy_server_with_cache.cpp` | Server loop, worker threads, cache, `GET` forwarding, `CONNECT` tunnel |
| `proxy_parse.cpp` / `.hpp` | HTTP request parser (`std::string` / `std::vector` based) |
| `makefile` | Build rules |

## How a request flows

1. `main` waits for a free connection slot, then `accept()`s and starts a detached thread with the client's fd.
2. The worker reads until the end of the headers.
3. `CONNECT` goes to the tunnel. Anything other than `GET` gets `501`.
4. For `GET`, the request is parsed and the cache key is built. A hit is served from memory. A miss is forwarded to the origin, streamed to the client, and cached if it was a complete `200`.

## Limitations

- Only `GET` is supported for plain HTTP (no `POST`, `HEAD`, etc.).
- Cache entries never expire and `Cache-Control` / `Vary` are ignored.
- The proxy has no authentication or allow-list, so don't expose it to an untrusted network.
- The parser expects absolute-form request URLs (`GET http://host/path`), which is what browsers send to proxies.
- Thread-per-connection is simple but won't scale like an event loop or thread pool.
