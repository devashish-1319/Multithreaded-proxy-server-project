// proxy_parse.cpp
// C++17 modern rewrite of proxy_parse.c (Option A - std::string + std::vector)
// Keeps original function names and behaviour but uses RAII and C++ containers.

#include "proxy_parse.hpp" // you should update the header to declare C++ structs
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sstream>
#include <algorithm>

#ifndef DEBUG
#define DEBUG 0
#endif

static const char *root_abs_path = "/";

void debug(const char * format, ...) {
    if (!DEBUG) return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/* ---------------------------
   ParsedHeader / ParsedRequest helpers
   --------------------------- */

/* Note: header and request types are defined in proxy_parse.hpp as:
 *
 * struct ParsedHeader {
 *   std::string key;
 *   std::string value;
 *   size_t keylen;
 *   size_t valuelen;
 * };
 *
 * struct ParsedRequest {
 *   std::string method;
 *   std::string protocol;
 *   std::string host;
 *   std::string port;
 *   std::string path;
 *   std::string version;
 *   std::string buf;     // request-line text
 *   int buflen;
 *   std::vector<ParsedHeader> headers;
 * };
 *
 * Functions below operate on those types.
 */

/* ParsedHeader_set
 * - Replaces any existing header with the same key
 * - Returns 0 on success, -1 on allocation/other failure (keeps old behaviour)
 */
int ParsedHeader_set(struct ParsedRequest *pr, const char * key, const char * value) {
    if (!pr || !key) return -1;
    std::string k = key;
    std::string v = value ? value : "";

    // find existing
    auto it = std::find_if(pr->headers.begin(), pr->headers.end(),
                           [&k](const ParsedHeader &ph){ return ph.key == k; });
    if (it != pr->headers.end()) {
        it->value = v;
        it->keylen = it->key.size() + 1;
        it->valuelen = it->value.size() + 1;
        return 0;
    }

    ParsedHeader ph;
    ph.key = std::move(k);
    ph.value = std::move(v);
    ph.keylen = ph.key.size() + 1;
    ph.valuelen = ph.value.size() + 1;
    try {
        pr->headers.push_back(std::move(ph));
    } catch (...) {
        return -1;
    }
    return 0;
}

/* ParsedHeader_get
 * - Returns pointer to header inside pr->headers or NULL if not found
 */
struct ParsedHeader* ParsedHeader_get(struct ParsedRequest *pr, const char * key) {
    if (!pr || !key) return nullptr;
    std::string k = key;
    for (auto &h : pr->headers) {
        if (h.key == k) return &h;
    }
    return nullptr;
}

/* ParsedHeader_remove
 * - Removes header with given key. Returns 0 on success, -1 if not found.
 */
int ParsedHeader_remove(struct ParsedRequest *pr, const char *key) {
    if (!pr || !key) return -1;
    std::string k = key;
    auto it = std::find_if(pr->headers.begin(), pr->headers.end(),
                           [&k](const ParsedHeader &ph){ return ph.key == k; });
    if (it == pr->headers.end()) return -1;
    pr->headers.erase(it);
    return 0;
}

/* ParsedHeader_create
 * - Initialize header container (vector initial capacity)
 */
void ParsedHeader_create(struct ParsedRequest *pr) {
    if (!pr) return;
    pr->headers.clear();
    pr->headers.reserve(DEFAULT_NHDRS); // DEFAULT_NHDRS should be in header
}

/* ParsedHeader_lineLen
 * - Length of a header line when serialized: key + ": " + value + "\r\n"
 */
size_t ParsedHeader_lineLen(const struct ParsedHeader * ph) {
    if (!ph) return 0;
    if (ph->key.empty()) return 0;
    return ph->key.size() + 2 + ph->value.size() + 2;
}

/* ParsedHeader_headersLen
 * - total bytes for all headers when serialized + final "\r\n"
 */
size_t ParsedHeader_headersLen(struct ParsedRequest *pr) {
    if (!pr) return 0;
    size_t len = 0;
    for (const auto &h : pr->headers) {
        len += ParsedHeader_lineLen(&h);
    }
    len += 2; // final CRLF after headers
    return len;
}

/* ParsedHeader_printHeaders
 * - Writes serialized headers into buf. buflen must be >= headersLen.
 * - Returns 0 on success, -1 on insufficient buffer.
 */
int ParsedHeader_printHeaders(struct ParsedRequest * pr, char * buf, size_t buflen) {
    if (!pr || !buf) return -1;
    size_t needed = ParsedHeader_headersLen(pr);
    if (buflen < needed) {
        debug("buffer for printing headers too small: need %zu got %zu\n", needed, buflen);
        return -1;
    }
    char *current = buf;
    for (const auto &h : pr->headers) {
        if (!h.key.empty()) {
            size_t klen = h.key.size();
            size_t vlen = h.value.size();
            memcpy(current, h.key.data(), klen);
            current += klen;
            memcpy(current, ": ", 2);
            current += 2;
            memcpy(current, h.value.data(), vlen);
            current += vlen;
            memcpy(current, "\r\n", 2);
            current += 2;
        }
    }
    memcpy(current, "\r\n", 2);
    return 0;
}

/* ParsedHeader_destroyOne - not strictly necessary with std::string,
   but kept to mimic C API behavior where we would reset fields */
void ParsedHeader_destroyOne(struct ParsedHeader * ph) {
    if (!ph) return;
    ph->key.clear();
    ph->value.clear();
    ph->keylen = 0;
    ph->valuelen = 0;
}

/* ParsedHeader_destroy - clear all headers */
void ParsedHeader_destroy(struct ParsedRequest * pr) {
    if (!pr) return;
    for (auto &h : pr->headers) ParsedHeader_destroyOne(&h);
    pr->headers.clear();
}

/* ParsedHeader_parse
 * - Parses a single header line "Key: Value\r\n" from 'line' and sets it on pr
 * - Returns 0 on success, -1 on malformed header
 */
int ParsedHeader_parse(struct ParsedRequest * pr, char * line) {
    if (!pr || !line) return -1;
    // find colon
    char *colon = strchr(line, ':');
    if (!colon) {
        debug("No colon in header line: %s\n", line);
        return -1;
    }
    // key: from line start to colon (exclusive)
    std::string key(line, colon - line);
    // skip ": "
    char *valstart = colon + 1;
    if (*valstart == ' ') ++valstart;
    // find CRLF
    char *crlf = strstr(valstart, "\r\n");
    std::string value;
    if (crlf) value.assign(valstart, crlf - valstart);
    else value.assign(valstart);
    // trim whitespace
    auto trim = [](std::string &s){
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || isspace((unsigned char)s.back()))) s.pop_back();
        size_t i = 0;
        while (i < s.size() && isspace((unsigned char)s[i])) ++i;
        if (i) s.erase(0, i);
    };
    trim(key);
    trim(value);

    if (key.empty()) {
        debug("Empty header key\n");
        return -1;
    }
    if (ParsedHeader_set(pr, key.c_str(), value.c_str()) < 0) {
        debug("Failed to set header %s\n", key.c_str());
        return -1;
    }
    return 0;
}

/* ---------------------------
   ParsedRequest methods
   --------------------------- */

/* ParsedRequest_create: allocate a new object on the heap (like original C)
 * Returns pointer or NULL on allocation failure.
 */
struct ParsedRequest* ParsedRequest_create() {
    try {
        ParsedRequest *pr = new ParsedRequest();
        // initialize fields
        pr->method.clear();
        pr->protocol.clear();
        pr->host.clear();
        pr->port.clear();
        pr->path.clear();
        pr->version.clear();
        pr->buf.clear();
        pr->buflen = 0;
        ParsedHeader_create(pr);
        return pr;
    } catch (...) {
        return nullptr;
    }
}

/* ParsedRequest_destroy: free the request (mirror original semantics) */
void ParsedRequest_destroy(struct ParsedRequest *pr) {
    if (!pr) return;
    ParsedHeader_destroy(pr);
    pr->buf.clear();
    pr->method.clear();
    pr->protocol.clear();
    pr->host.clear();
    pr->port.clear();
    pr->path.clear();
    pr->version.clear();
    delete pr;
}

/* ParsedRequest_requestLineLen
 * - returns number of bytes the request-line will occupy when serialized (not including trailing CRLF already added separately)
 */
size_t ParsedRequest_requestLineLen(struct ParsedRequest *pr) {
    if (!pr || pr->buf.empty()) return 0;
    size_t len = 0;
    // METHOD + ' ' + protocol + "://" + host [+ ':' + port] + path + ' ' + version + "\r\n"
    len += pr->method.size(); // method
    len += 1;                 // space
    len += pr->protocol.size(); // protocol
    len += 3;                 // "://"
    len += pr->host.size();
    if (!pr->port.empty()) {
        len += 1;             // ':'
        len += pr->port.size();
    }
    len += pr->path.size();   // path (at least '/')
    len += 1;                 // space before version
    len += pr->version.size();
    len += 2;                 // CRLF
    return len;
}

/* ParsedRequest_printRequestLine
 * - Writes the first request-line into buf. On success *tmp contains bytes written.
 * - Returns 0 on success, -1 on insufficient buffer or invalid state.
 */
int ParsedRequest_printRequestLine(struct ParsedRequest *pr,
                                   char * buf, size_t buflen,
                                   size_t *tmp) {
    if (!pr || !buf || !tmp) return -1;
    size_t need = ParsedRequest_requestLineLen(pr);
    if (buflen < need) {
        debug("not enough memory for first line: need %zu got %zu\n", need, buflen);
        return -1;
    }
    char *cur = buf;
    // METHOD
    memcpy(cur, pr->method.data(), pr->method.size());
    cur += pr->method.size();
    *cur++ = ' ';
    // protocol://host[:port]
    memcpy(cur, pr->protocol.data(), pr->protocol.size());
    cur += pr->protocol.size();
    memcpy(cur, "://", 3);
    cur += 3;
    memcpy(cur, pr->host.data(), pr->host.size());
    cur += pr->host.size();
    if (!pr->port.empty()) {
        *cur++ = ':';
        memcpy(cur, pr->port.data(), pr->port.size());
        cur += pr->port.size();
    }
    // path
    memcpy(cur, pr->path.data(), pr->path.size());
    cur += pr->path.size();
    *cur++ = ' ';
    // version
    memcpy(cur, pr->version.data(), pr->version.size());
    cur += pr->version.size();
    memcpy(cur, "\r\n", 2);
    cur += 2;
    *tmp = static_cast<size_t>(cur - buf);
    return 0;
}

/* ParsedRequest_unparse
 * - Recreate the entire buffer (request-line + headers) into provided buf
 */
int ParsedRequest_unparse(struct ParsedRequest *pr, char *buf, size_t buflen) {
    if (!pr || pr->buf.empty()) return -1;
    size_t tmp = 0;
    if (ParsedRequest_printRequestLine(pr, buf, buflen, &tmp) < 0) return -1;
    if (ParsedHeader_printHeaders(pr, buf + tmp, buflen - tmp) < 0) return -1;
    return 0;
}

/* ParsedRequest_unparse_headers */
int ParsedRequest_unparse_headers(struct ParsedRequest *pr, char *buf, size_t buflen) {
    if (!pr || pr->buf.empty()) return -1;
    if (ParsedHeader_printHeaders(pr, buf, buflen) < 0) return -1;
    return 0;
}

/* ParsedRequest_totalLen */
size_t ParsedRequest_totalLen(struct ParsedRequest *pr) {
    if (!pr || pr->buf.empty()) return 0;
    return ParsedRequest_requestLineLen(pr) + ParsedHeader_headersLen(pr);
}

/* ParsedRequest_parse
 * - parse buffer of length buflen containing request and trailing CRLFCRLF
 * - returns 0 on success, -1 on failure (mimics original)
 */
int ParsedRequest_parse(struct ParsedRequest * parse, const char *buf, int buflen) {
    if (!parse || !buf) return -1;
    if (!parse->buf.empty()) {
        debug("parse object already assigned to a request\n");
        return -1;
    }
    if (buflen < MIN_REQ_LEN || buflen > MAX_REQ_LEN) {
        debug("invalid buflen %d\n", buflen);
        return -1;
    }

    // create temporary NUL-terminated string
    std::string tmp_buf(buf, buf + buflen);

    // find end of headers
    size_t hdrpos = tmp_buf.find("\r\n\r\n");
    if (hdrpos == std::string::npos) {
        debug("invalid request: no end of headers\n");
        return -1;
    }

    // Extract request line (up to first CRLF)
    size_t rl_end = tmp_buf.find("\r\n");
    if (rl_end == std::string::npos) {
        debug("invalid request: no CRLF in request-line\n");
        return -1;
    }
    parse->buf = tmp_buf.substr(0, rl_end); // store request-line text
    parse->buflen = static_cast<int>(parse->buf.size() + 1);

    // Tokenize request-line: METHOD SP REQUEST-URI SP HTTP/VERSION
    std::istringstream rl_stream(parse->buf);
    std::string method, full_addr, version;
    if (!(rl_stream >> method)) {
        debug("invalid request line, no whitespace\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }
    parse->method = method;
    if (parse->method != "GET") {
        debug("invalid request line, method not 'GET': %s\n", parse->method.c_str());
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }
    if (!(rl_stream >> full_addr)) {
        debug("invalid request line, no full address\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }
    if (!(rl_stream >> version)) {
        debug("invalid request line, missing version\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }
    parse->version = version;

    if (parse->version.size() < 5 || parse->version.substr(0,5) != "HTTP/") {
        debug("invalid request line, unsupported version %s\n", parse->version.c_str());
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }

    // parse protocol://host[:port]/path
    size_t proto_pos = full_addr.find("://");
    if (proto_pos == std::string::npos) {
        debug("invalid request line, missing protocol\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }
    parse->protocol = full_addr.substr(0, proto_pos);
    size_t after_proto = proto_pos + 3;
    if (after_proto >= full_addr.size()) {
        debug("invalid request line, missing host+path\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }
    std::string rem = full_addr.substr(after_proto); // host[:port]/path...

    // find first '/' in rem (path start)
    size_t slash_pos = rem.find('/');
    if (slash_pos == std::string::npos) {
        // no path provided; per original, missing absolute path is invalid
        debug("invalid request line, missing absolute path\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }

    std::string hostport = rem.substr(0, slash_pos); // host[:port]
    std::string pathpart = rem.substr(slash_pos);    // includes leading '/'

    if (pathpart.empty()) {
        // replace with "/"
        parse->path = std::string(root_abs_path);
    } else {
        // original code disallowed path beginning with two '/'
        if (pathpart.size() >= 2 && pathpart[0] == '/' && pathpart[1] == '/') {
            debug("invalid request line, path cannot begin with two slash characters\n");
            parse->buf.clear();
            parse->buflen = 0;
            return -1;
        }
        // ensure path begins with '/'
        if (pathpart[0] != '/') {
            // prefix '/'
            parse->path = std::string(root_abs_path) + pathpart;
        } else {
            parse->path = pathpart;
        }
    }

    // split host and optional port
    size_t colon_pos = hostport.find(':');
    if (colon_pos == std::string::npos) {
        parse->host = hostport;
        parse->port.clear();
    } else {
        parse->host = hostport.substr(0, colon_pos);
        parse->port = hostport.substr(colon_pos + 1);
        // validate numeric port if present
        if (!parse->port.empty()) {
            char *endptr = nullptr;
            errno = 0;
            (void)strtol(parse->port.c_str(), &endptr, 10);
            if (endptr == parse->port.c_str() || errno == EINVAL) {
                debug("invalid request line, bad port: %s\n", parse->port.c_str());
                parse->buf.clear();
                parse->buflen = 0;
                parse->path.clear();
                parse->host.clear();
                parse->port.clear();
                return -1;
            }
            // p==0 is not necessarily invalid (but matches earlier behavior check against EINVAL)
        }
    }

    if (parse->host.empty()) {
        debug("invalid request line, missing host\n");
        parse->buf.clear();
        parse->buflen = 0;
        return -1;
    }

    /* Parse headers line by line */
    size_t header_start = rl_end + 2; // position after first CRLF
    size_t pos = header_start;
    while (pos < tmp_buf.size()) {
        // if CRLF immediately => end of headers
        if (tmp_buf[pos] == '\r' && (pos + 1) < tmp_buf.size() && tmp_buf[pos+1] == '\n') {
            break;
        }
        // find end of this header line
        size_t line_end = tmp_buf.find("\r\n", pos);
        if (line_end == std::string::npos) break; // something wrong
        // copy line to a mutable buffer to use ParsedHeader_parse which expects char*
        std::string line = tmp_buf.substr(pos, line_end - pos + 2); // include CRLF
        // make a writable buffer
        std::vector<char> wbuf(line.begin(), line.end());
        wbuf.push_back('\0');
        if (ParsedHeader_parse(parse, wbuf.data()) != 0) {
            debug("failed to parse header line: %s\n", line.c_str());
            return -1;
        }
        pos = line_end + 2;
    }

    return 0;
}

