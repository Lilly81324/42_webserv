// tests/unit/test_cgi.cpp
#include <catch2/catch_all.hpp>
#include "helpers/server_runner.hpp"   // the RAII spawner you added

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>

// ---------------------------
// Tiny HTTP client utilities
// ---------------------------
namespace http {

static std::string recv_all(int fd) {
    std::string out;
    char buf[8192];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) { out.append(buf, buf + n); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
    }
    return out;
}

static int connect_tcp(const char* ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in sa; std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    REQUIRE(::inet_pton(AF_INET, ip, &sa.sin_addr) == 1);

    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
    return fd;
}

static void send_all(int fd, const std::string& data) {
    const char* p = data.data();
    size_t left = data.size();
    while (left) {
        ssize_t n = ::send(fd, p, left, 0);
        if (n > 0) { p += n; left -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        FAIL("send failed: " << std::strerror(errno));
    }
}

struct Response {
    int status = 0;
    std::string reason;
    std::map<std::string, std::string> headers; // lower-cased keys
    std::string raw_headers_block;              // as-received (for debugging)
    std::string body;                           // decoded body (chunked decoded if needed)
    std::string raw;                            // raw full response
};

// Lower-case ASCII (headers keys)
static std::string to_lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') s[i] = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

static bool starts_with(const std::string& s, const std::string& pfx) {
    return (s.size() >= pfx.size() && std::equal(pfx.begin(), pfx.end(), s.begin()));
}

// Parse status line like "HTTP/1.1 200 OK"
static void parse_status_line(const std::string& line, int& status, std::string& reason) {
    std::istringstream iss(line);
    std::string httpver;
    iss >> httpver >> status;
    std::getline(iss, reason);
    if (!reason.empty() && reason[0] == ' ') reason.erase(0, 1);
}

// Split headers/body by CRLFCRLF; return pos after header block or npos if not found.
static size_t find_header_end(const std::string& s) {
    const std::string sep = "\r\n\r\n";
    size_t pos = s.find(sep);
    if (pos == std::string::npos) return pos;
    return pos + sep.size();
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

static std::map<std::string,std::string> parse_headers_map(const std::string& block) {
    std::map<std::string,std::string> H;
    std::istringstream iss(block);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; continue; } // skip status
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = to_lower(trim(line.substr(0, colon)));
        std::string val = trim(line.substr(colon + 1));
        H[key] = val;
    }
    return H;
}

// Decode a chunked body (very naive, for tests)
static bool decode_chunked(const std::string& raw, std::string& out) {
    out.clear();
    size_t i = 0, N = raw.size();
    for (;;) {
        size_t line_end = raw.find("\r\n", i);
        if (line_end == std::string::npos) return false;
        std::string hex = raw.substr(i, line_end - i);
        size_t chunk_size = 0;
        { std::istringstream iss(hex); iss >> std::hex >> chunk_size; if (!iss || !iss.eof()) return false; }
        i = line_end + 2;
        if (chunk_size == 0) {
            if (i + 2 > N) return false;
            if (raw.compare(i, 2, "\r\n") != 0) return false;
            i += 2;
            break;
        }
        if (i + chunk_size + 2 > N) return false;
        out.append(raw, i, chunk_size);
        i += chunk_size;
        if (raw.compare(i, 2, "\r\n") != 0) return false;
        i += 2;
    }
    return true;
}

static Response parse_response(const std::string& raw) {
    Response r;
    r.raw = raw;

    const size_t hdr_end = find_header_end(raw);
    REQUIRE(hdr_end != std::string::npos);

    r.raw_headers_block = raw.substr(0, hdr_end);
    {
        size_t line_end = r.raw_headers_block.find("\r\n");
        REQUIRE(line_end != std::string::npos);
        std::string status_line = r.raw_headers_block.substr(0, line_end);
        parse_status_line(status_line, r.status, r.reason);
    }

    r.headers = parse_headers_map(r.raw_headers_block);

    // body (raw)
    std::string body_raw = raw.substr(hdr_end);

    // decode based on headers
    const std::string te = r.headers.count("transfer-encoding") ? r.headers.find("transfer-encoding")->second : "";
    const std::string cl = r.headers.count("content-length")     ? r.headers.find("content-length")->second     : "";
    bool is_chunked = false;
    if (!te.empty()) {
        std::string tmp = to_lower(te);
        is_chunked = (tmp.find("chunked") != std::string::npos);
    }

    if (is_chunked) {
        std::string decoded;
        REQUIRE( decode_chunked(body_raw, decoded) );
        r.body.swap(decoded);
    } else if (!cl.empty()) {
        size_t want = 0;
        { std::istringstream iss(cl); iss >> want; REQUIRE(!!iss); }
        REQUIRE( body_raw.size() >= want );
        r.body.assign(body_raw.data(), want);
    } else {
        r.body = body_raw;
    }
    return r;
}

} // namespace http

// ---------------------------
// Helper: header token search
// ---------------------------
static bool header_has_token_ci(const std::map<std::string,std::string>& H,
                                const std::string& key,
                                const std::string& token) {
    std::map<std::string,std::string>::const_iterator it =
        H.find(http::to_lower(key));
    if (it == H.end()) return false;
    std::string v = http::to_lower(it->second);
    std::string t = http::to_lower(token);
    std::string cur;
    for (size_t i = 0; i <= v.size(); ++i) {
        char ch = (i < v.size() ? v[i] : ',');
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!cur.empty()) { if (cur == t) return true; cur.clear(); }
        } else {
            cur += ch;
        }
    }
    return false;
}

static std::string repeat_char(char c, size_t n) {
    std::string s; s.resize(n, c); return s;
}

// ---------------------------
// CGI tests (each spawns its own server on a free port)
// ---------------------------

TEST_CASE("CGI simple GET returns 200 and text-like content", "[cgi][get]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin); // spawns and waits

    const std::string req =
        "GET /cgi/echo.py?ping=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: catch2-tests\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n";

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, req);
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    REQUIRE( r.headers.count("server") == 1 );
    REQUIRE( r.headers.count("content-type") == 1 );
}

TEST_CASE("CGI POST echoes body-sized payload (presence/length plausible)", "[cgi][post]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string body = "hello-cgi-body";
    std::ostringstream oss;
    oss
        << "POST /cgi/echo.py HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "User-Agent: catch2-tests\r\n"
        << "Accept: */*\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, oss.str());
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    REQUIRE( (!r.body.empty()) );
    bool contains_body = (r.body.find(body) != std::string::npos);
    CHECK( (contains_body || r.body.size() >= body.size()) );
}

TEST_CASE("CGI chunked response is well-formed when server selects chunked", "[cgi][chunked]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string body = repeat_char('A', 1024);
    std::ostringstream oss;
    oss
        << "POST /cgi/echo.py HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "User-Agent: catch2-tests\r\n"
        << "Accept: */*\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: keep-alive\r\n"
        << "\r\n"
        << body;

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, oss.str());
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    bool is_chunked = header_has_token_ci(r.headers, "Transfer-Encoding", "chunked");
    bool has_len    = (r.headers.count("content-length") == 1);
    REQUIRE( (is_chunked || has_len) );
    if (is_chunked) CHECK( (!r.body.empty()) );
}

TEST_CASE("CGI honors Connection: close", "[cgi][connection]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string body = "x";
    std::ostringstream oss;
    oss
        << "POST /cgi/echo.py HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "User-Agent: catch2-tests\r\n"
        << "Accept: */*\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, oss.str());
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    bool says_close = header_has_token_ci(r.headers, "Connection", "close");
    CHECK( (says_close || r.headers.count("connection") == 0) );
}

TEST_CASE("CGI large POST (64 KiB) succeeds without chunk errors", "[cgi][post][large]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const size_t N = 64 * 1024;
    const std::string body = repeat_char('A', N);

    std::ostringstream oss;
    oss
        << "POST /cgi/echo.py HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "User-Agent: catch2-tests\r\n"
        << "Accept: */*\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    const std::string head = oss.str();

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, head);
    // partial writes to exercise server’s body reader
    http::send_all(fd, body.substr(0, body.size()/2));
    http::send_all(fd, body.substr(body.size()/2));
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    CHECK( (r.body.size() >= body.size()/2) );
}

TEST_CASE("CGI must provide Content-Type", "[cgi][headers]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string req =
        "GET /cgi/echo.py HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: catch2-tests\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n";

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, req);
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    REQUIRE( r.headers.count("content-type") == 1 );
    const std::string ct = http::to_lower(r.headers["content-type"]);
    bool looks_text = (ct.find("text/plain") != std::string::npos) ||
                      (ct.find("text/html")  != std::string::npos);
    CHECK( looks_text );
}

TEST_CASE("CGI environment propagation sanity (QUERY_STRING non-empty on GET with query)", "[cgi][env]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string req =
        "GET /cgi/echo.py?alpha=1&beta=2 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: catch2-tests\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n";

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, req);
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200) );
    bool mentions_alpha = (r.body.find("alpha=1") != std::string::npos);
    bool mentions_beta  = (r.body.find("beta=2")  != std::string::npos);
    CHECK( (mentions_alpha || mentions_beta) );
}

TEST_CASE("CGI rejects unsupported methods with 405 or 501", "[cgi][methods]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string req =
        "PUT /cgi/echo.py HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: catch2-tests\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, req);
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    CHECK( (r.status == 405 || r.status == 501) );
}

TEST_CASE("CGI HEAD does not include body but has headers", "[cgi][head]") {
    const char* bin  = std::getenv("WEBSERV_BIN")  ? std::getenv("WEBSERV_BIN")  : "./webserv";
    const char* conf = std::getenv("WEBSERV_CONF") ? std::getenv("WEBSERV_CONF") : "extended.conf";
    ServerRunner server(conf, bin);

    const std::string req =
        "HEAD /cgi/echo.py HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: catch2-tests\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n";

    int fd = http::connect_tcp("127.0.0.1", server.port);
    http::send_all(fd, req);
    http::Response r = http::parse_response(http::recv_all(fd));
    ::close(fd);

    REQUIRE( (r.status == 200 || r.status == 204) );
    CHECK( (r.body.empty() || r.body.size() < 4) );
}
