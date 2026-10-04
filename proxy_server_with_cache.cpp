// proxy_server_with_cache.cpp
// Multithreaded HTTP proxy with an LRU response cache and HTTPS (CONNECT) tunnelling.

#include "proxy_parse.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_BYTES 4096                          // size of each recv() chunk
#define MAX_CLIENTS 400                         // max number of client connections served at a time
#define MAX_SIZE (200 * (1 << 20))              // size of the cache (bytes)
#define MAX_ELEMENT_SIZE (10 * (1 << 20))       // max size of a single cached response
#define IO_TIMEOUT_SEC 30                       // recv timeout on client/origin sockets
#define TUNNEL_IDLE_MS 60000                    // CONNECT tunnel idle timeout

/* ------------------------------------------------------------------ */
/* Connection limiter (portable replacement for an unnamed semaphore;  */
/* sem_init is not supported on macOS).                                */
/* ------------------------------------------------------------------ */

static std::mutex slot_mutex;
static std::condition_variable slot_cv;
static int active_clients = 0;

static void acquire_slot()
{
    std::unique_lock<std::mutex> lk(slot_mutex);
    slot_cv.wait(lk, [] { return active_clients < MAX_CLIENTS; });
    ++active_clients;
}

static void release_slot()
{
    {
        std::lock_guard<std::mutex> lk(slot_mutex);
        --active_clients;
    }
    slot_cv.notify_one();
}

/* ------------------------------------------------------------------ */
/* LRU cache                                                           */
/*                                                                     */
/* lru.front() is the most recently used entry. Every public function */
/* takes the lock exactly once; helpers ending in _locked expect the   */
/* caller to already hold it (this is what avoids self-deadlock).      */
/* Responses are shared_ptr<const string>, so a thread serving a hit   */
/* keeps its data alive even if the entry is evicted meanwhile.        */
/* ------------------------------------------------------------------ */

struct CacheEntry {
    std::string key;
    std::shared_ptr<const std::string> data;
    size_t size;    // bytes charged against MAX_SIZE
};

static std::list<CacheEntry> lru;
static std::unordered_map<std::string, std::list<CacheEntry>::iterator> cache_index;
static size_t cache_size = 0;
static std::mutex cache_mutex;

static void evict_lru_locked()
{
    if (lru.empty()) return;
    CacheEntry& victim = lru.back();
    cache_size -= victim.size;
    cache_index.erase(victim.key);
    lru.pop_back();
}

/* cache_find: returns the cached response for key (and marks it most recently used), or nullptr */
static std::shared_ptr<const std::string> cache_find(const std::string& key)
{
    std::lock_guard<std::mutex> guard(cache_mutex);
    auto it = cache_index.find(key);
    if (it == cache_index.end()) return nullptr;
    lru.splice(lru.begin(), lru, it->second);   // move to front; iterator stays valid
    return it->second->data;
}

/* cache_add: store a response, evicting least recently used entries until it fits.
   Returns true if the response was stored. */
static bool cache_add(const std::string& key, std::string&& response)
{
    size_t element_size = response.size() + key.size() + sizeof(CacheEntry);
    if (element_size > MAX_ELEMENT_SIZE) return false;

    auto data = std::make_shared<const std::string>(std::move(response));

    std::lock_guard<std::mutex> guard(cache_mutex);
    if (cache_index.count(key)) return false;   // another thread cached it first

    while (cache_size + element_size > MAX_SIZE && !lru.empty()) {
        evict_lru_locked();
    }
    lru.push_front(CacheEntry{key, std::move(data), element_size});
    cache_index[key] = lru.begin();
    cache_size += element_size;
    return true;
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static void set_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}

/* send_all: keep sending until everything is written. Returns false on error. */
static bool send_all(int fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* sendErrorMessage: send a small HTML error response. Returns 1 on success, -1 for unknown code. */
int sendErrorMessage(int socket, int status_code)
{
    const char* text;
    switch (status_code) {
        case 400: text = "Bad Request"; break;
        case 403: text = "Forbidden"; break;
        case 404: text = "Not Found"; break;
        case 500: text = "Internal Server Error"; break;
        case 501: text = "Not Implemented"; break;
        case 502: text = "Bad Gateway"; break;
        case 505: text = "HTTP Version Not Supported"; break;
        default: return -1;
    }

    char currentTime[50];
    time_t now = time(0);
    struct tm data;
    gmtime_r(&now, &data);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S GMT", &data);

    std::string body = "<HTML><HEAD><TITLE>" + std::to_string(status_code) + " " + text +
                       "</TITLE></HEAD>\n<BODY><H1>" + std::to_string(status_code) + " " + text +
                       "</H1>\n</BODY></HTML>";
    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " + text + "\r\n" +
                           "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                           "Content-Type: text/html\r\n" +
                           "Connection: close\r\n" +
                           "Date: " + currentTime + "\r\n" +
                           "Server: MultithreadedProxy/1.0\r\n\r\n" + body;

    printf("%d %s\n", status_code, text);
    send_all(socket, response.data(), response.size());
    return 1;
}

/* connectRemoteServer: connect to host:port, return socket fd or -1 on error.
   Uses getaddrinfo (thread-safe, IPv4 + IPv6) and tries each returned address. */
int connectRemoteServer(const char* host_addr, int port_num)
{
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port = std::to_string(port_num);
    if (getaddrinfo(host_addr, port.c_str(), &hints, &res) != 0) {
        fprintf(stderr, "No such host exists: %s\n", host_addr);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_timeout(fd, IO_TIMEOUT_SEC);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) fprintf(stderr, "Error in connecting to %s:%d\n", host_addr, port_num);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Plain HTTP (GET) handling                                           */
/* ------------------------------------------------------------------ */

/* checkHTTPversion: accept HTTP/1.0 and HTTP/1.1 */
int checkHTTPversion(const char* msg)
{
    if (!msg) return -1;
    if (strncmp(msg, "HTTP/1.1", 8) == 0 || strncmp(msg, "HTTP/1.0", 8) == 0) return 1;
    return -1;
}

/* cache_key_for: method is always GET, so host + port + path identifies the resource
   (headers like User-Agent or Cookie must not split the cache). */
static std::string cache_key_for(const ParsedRequest* request, int port)
{
    std::string host = request->host;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return (char)tolower(c); });
    return host + ":" + std::to_string(port) + request->path;
}

/* handle_request: forward the GET to the origin, stream the response to the client,
   and cache it if it was a complete 200 response.
   Returns 0 on success, -1 if the origin could not be reached (caller sends 502). */
int handle_request(int clientSocket, ParsedRequest* request, int server_port, const std::string& key)
{
    if (!request) return -1;

    if (ParsedHeader_set(request, "Connection", "close") < 0) printf("set header key not work\n");
    ParsedHeader_remove(request, "Proxy-Connection");
    if (ParsedHeader_get(request, "Host") == nullptr) {
        if (ParsedHeader_set(request, "Host", request->host.c_str()) < 0) {
            printf("Set \"Host\" header key not working\n");
        }
    }

    std::string sendbuf = "GET " + request->path + " " + request->version + "\r\n";
    size_t hdr_len = ParsedHeader_headersLen(request);
    std::vector<char> hdrbuf(hdr_len + 1);
    if (ParsedRequest_unparse_headers(request, hdrbuf.data(), hdrbuf.size()) < 0) {
        printf("unparse failed\n");
        return -1;
    }
    sendbuf.append(hdrbuf.data(), hdr_len);

    int remoteSocketID = connectRemoteServer(request->host.c_str(), server_port);
    if (remoteSocketID < 0) return -1;

    if (!send_all(remoteSocketID, sendbuf.data(), sendbuf.size())) {
        perror("Error sending request to remote");
        close(remoteSocketID);
        return -1;
    }

    std::string response;       // accumulated for the cache
    bool cacheable = true;      // false once the response is too large to cache
    bool client_ok = true;
    bool complete = false;      // origin closed the connection cleanly
    size_t total_sent = 0;
    char recvbuf[MAX_BYTES];

    while (true) {
        ssize_t n = recv(remoteSocketID, recvbuf, sizeof(recvbuf), 0);
        if (n == 0) { complete = true; break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!send_all(clientSocket, recvbuf, (size_t)n)) {
            perror("Error in sending data to client socket.");
            client_ok = false;
            break;
        }
        total_sent += (size_t)n;
        if (cacheable) {
            if (response.size() + (size_t)n > MAX_ELEMENT_SIZE) {
                cacheable = false;
                std::string().swap(response);
            } else {
                response.append(recvbuf, (size_t)n);
            }
        }
    }
    close(remoteSocketID);

    if (total_sent == 0 && client_ok && !complete) return -1;   // origin failed before sending anything

    // only cache complete 200 responses
    bool ok200 = response.size() >= 12 && response.compare(0, 5, "HTTP/") == 0 && response.compare(9, 3, "200") == 0;
    if (complete && client_ok && cacheable && ok200) {
        cache_add(key, std::move(response));
    }
    printf("Done\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTPS support: CONNECT tunnelling                                   */
/* ------------------------------------------------------------------ */

/* tunnel: shuttle bytes between client and origin until both sides close or the tunnel is idle */
static void tunnel(int client, int remote)
{
    int fds[2] = {client, remote};
    struct pollfd pfd[2];
    char buf[16 * 1024];

    for (int i = 0; i < 2; i++) { pfd[i].fd = fds[i]; pfd[i].events = POLLIN; pfd[i].revents = 0; }

    while (pfd[0].fd >= 0 || pfd[1].fd >= 0) {
        int ready = poll(pfd, 2, TUNNEL_IDLE_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) return;     // idle timeout

        for (int i = 0; i < 2; i++) {
            if (pfd[i].fd < 0 || !(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = recv(fds[i], buf, sizeof(buf), 0);
            if (n > 0) {
                if (!send_all(fds[1 - i], buf, (size_t)n)) return;
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                // this side is finished: pass the half-close along and stop polling it
                shutdown(fds[1 - i], SHUT_WR);
                pfd[i].fd = -1;
            }
        }
    }
}

/* handle_connect: "CONNECT host:port HTTP/1.x" -> open a TCP tunnel, then relay raw bytes */
static void handle_connect(int clientSocket, const std::string& request)
{
    std::istringstream line(request.substr(0, request.find("\r\n")));
    std::string method, target, version;
    line >> method >> target >> version;
    if (target.empty() || version.compare(0, 5, "HTTP/") != 0) {
        sendErrorMessage(clientSocket, 400);
        return;
    }

    std::string host, port = "443";
    if (target[0] == '[') {     // [ipv6]:port
        size_t close_br = target.find(']');
        if (close_br == std::string::npos) { sendErrorMessage(clientSocket, 400); return; }
        host = target.substr(1, close_br - 1);
        if (close_br + 1 < target.size() && target[close_br + 1] == ':') port = target.substr(close_br + 2);
    } else {
        size_t colon = target.rfind(':');
        if (colon == std::string::npos) {
            host = target;
        } else {
            host = target.substr(0, colon);
            port = target.substr(colon + 1);
        }
    }

    char* endptr = nullptr;
    long port_num = strtol(port.c_str(), &endptr, 10);
    if (host.empty() || port.empty() || *endptr != '\0' || port_num < 1 || port_num > 65535) {
        sendErrorMessage(clientSocket, 400);
        return;
    }

    int remote = connectRemoteServer(host.c_str(), (int)port_num);
    if (remote < 0) {
        sendErrorMessage(clientSocket, 502);
        return;
    }

    static const char established[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
    if (send_all(clientSocket, established, sizeof(established) - 1)) {
        printf("CONNECT tunnel open: %s:%ld\n", host.c_str(), port_num);
        tunnel(clientSocket, remote);
        printf("CONNECT tunnel closed: %s:%ld\n", host.c_str(), port_num);
    }
    close(remote);
}

/* ------------------------------------------------------------------ */
/* Per-client worker                                                   */
/* ------------------------------------------------------------------ */

/* read_request: read until the end of the headers ("\r\n\r\n").
   Returns 1 on success, 0 if the client closed, -1 on error/timeout, -2 if the request is too large. */
static int read_request(int socket, std::string& req)
{
    char buf[MAX_BYTES];
    while (req.find("\r\n\r\n") == std::string::npos) {
        if (req.size() > MAX_REQ_LEN) return -2;
        ssize_t n = recv(socket, buf, sizeof(buf), 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        req.append(buf, (size_t)n);
    }
    return 1;
}

static void handle_client(int socket)
{
    set_timeout(socket, IO_TIMEOUT_SEC);

    std::string req;
    int rc = read_request(socket, req);
    if (rc == 0) {
        printf("Client disconnected!\n");
    } else if (rc == -1) {
        perror("Error in receiving from client.");
    } else if (rc == -2) {
        sendErrorMessage(socket, 400);
    } else {
        std::string method = req.substr(0, req.find(' '));

        if (method == "CONNECT") {
            handle_connect(socket, req);
        } else if (method != "GET") {
            printf("This proxy doesn't support method: %s\n", method.c_str());
            sendErrorMessage(socket, 501);
        } else {
            std::unique_ptr<ParsedRequest, void (*)(ParsedRequest*)> request(ParsedRequest_create(), ParsedRequest_destroy);
            if (!request) {
                sendErrorMessage(socket, 500);
            } else if (ParsedRequest_parse(request.get(), req.data(), (int)req.size()) < 0) {
                printf("Parsing failed\n");
                sendErrorMessage(socket, 400);
            } else if (request->host.empty() || request->path.empty() ||
                       checkHTTPversion(request->version.c_str()) != 1) {
                sendErrorMessage(socket, 400);
            } else {
                int server_port = request->port.empty() ? 80 : atoi(request->port.c_str());
                if (server_port < 1 || server_port > 65535) {
                    sendErrorMessage(socket, 400);
                } else {
                    std::string key = cache_key_for(request.get(), server_port);
                    std::shared_ptr<const std::string> cached = cache_find(key);
                    if (cached) {
                        printf("Cache HIT: %s\n", key.c_str());
                        send_all(socket, cached->data(), cached->size());
                    } else {
                        printf("Cache MISS: %s\n", key.c_str());
                        if (handle_request(socket, request.get(), server_port, key) < 0) {
                            sendErrorMessage(socket, 502);
                        }
                    }
                }
            }
        }
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    int port_number = atoi(argv[1]);
    if (port_number < 1 || port_number > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    // flush log lines as they happen, even when stdout is a pipe or file (e.g. `| tee proxy.log`)
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // a client hanging up mid-response must not kill the process
    signal(SIGPIPE, SIG_IGN);

    printf("Setting Proxy Server Port : %d\n", port_number);

    int proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0) {
        perror("Failed to create socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed");

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Port is not free");
        return 1;
    }
    printf("Binding on port: %d\n", port_number);

    if (listen(proxy_socketId, MAX_CLIENTS) < 0) {
        perror("Error while Listening");
        return 1;
    }

    while (true)
    {
        // wait for a free slot *before* accepting: bounds the number of live threads
        // and leaves excess connections queued in the kernel backlog
        acquire_slot();

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr, &client_len);
        if (client_socketId < 0) {
            release_slot();
            if (errno == EMFILE || errno == ENFILE) usleep(100000);   // out of fds: back off
            else if (errno != EINTR && errno != ECONNABORTED) perror("Error in Accepting connection");
            continue;   // a failed accept must not take the server down
        }

        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, str, INET_ADDRSTRLEN);
        printf("Client is connected with port number: %d and ip address: %s \n", ntohs(client_addr.sin_port), str);

        try {
            // fd is passed by value and the thread is detached: nothing to join, nothing shared
            std::thread([client_socketId] {
                handle_client(client_socketId);
                release_slot();
            }).detach();
        } catch (const std::system_error& e) {
            fprintf(stderr, "Could not start thread: %s\n", e.what());
            close(client_socketId);
            release_slot();
        }
    }

    close(proxy_socketId);
    return 0;
}
