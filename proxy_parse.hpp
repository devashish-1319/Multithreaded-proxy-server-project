#ifndef PROXY_PARSE_HPP
#define PROXY_PARSE_HPP

#include <string>
#include <vector>
#include <cstddef>
#include <cstdarg>

#define DEBUG 1
#define DEFAULT_NHDRS 8
#define MAX_REQ_LEN 65535
#define MIN_REQ_LEN 4

/* -------------------------------
   ParsedHeader (C++ version)
   ------------------------------- */
struct ParsedHeader {
    std::string key;
    size_t keylen = 0;

    std::string value;
    size_t valuelen = 0;
};

/* -------------------------------
   ParsedRequest (C++ version)
   ------------------------------- */
struct ParsedRequest {
    std::string method;
    std::string protocol;
    std::string host;
    std::string port;
    std::string path;
    std::string version;

    std::string buf;  // request-line text
    size_t buflen = 0;

    std::vector<ParsedHeader> headers;
    size_t headersused = 0;  // preserved for compatibility
    size_t headerslen  = 0;  // preserved but unused (vector handles sizing)
};

/* -------------------------------
   Public API (identical names)
   ------------------------------- */

struct ParsedRequest* ParsedRequest_create();

int ParsedRequest_parse(struct ParsedRequest *parse,
                        const char *buf,
                        int buflen);

void ParsedRequest_destroy(struct ParsedRequest *pr);

int ParsedRequest_unparse(struct ParsedRequest *pr,
                          char *buf,
                          size_t buflen);

int ParsedRequest_unparse_headers(struct ParsedRequest *pr,
                                  char *buf,
                                  size_t buflen);

size_t ParsedRequest_totalLen(struct ParsedRequest *pr);

/* ParsedHeader helpers */
size_t ParsedHeader_headersLen(struct ParsedRequest *pr);

int ParsedHeader_set(struct ParsedRequest *pr,
                     const char *key,
                     const char *value);

struct ParsedHeader* ParsedHeader_get(struct ParsedRequest *pr,
                                      const char *key);

int ParsedHeader_remove(struct ParsedRequest *pr,
                        const char *key);

/* Debug printing */
void debug(const char *format, ...);

#endif // PROXY_PARSE_HPP
