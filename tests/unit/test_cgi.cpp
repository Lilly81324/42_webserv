#include <catch2/catch_all.hpp>

#define private public           // expose private for test
#include "CgiHandler.h"
#undef private

#include "CgiRegistry.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "VirtualServer.h"
#include "HEADER_ENTRIES.h"

// Small helpers
static std::map<std::string,std::string> envVecToMap(const std::vector<std::string>& v) {
    std::map<std::string,std::string> m;
    for (size_t i = 0; i < v.size(); ++i) {
        std::string::size_type eq = v[i].find('=');
        if (eq == std::string::npos) continue;
        m[v[i].substr(0, eq)] = v[i].substr(eq + 1);
    }
    return m;
}

TEST_CASE("CgiRegistry: location overrides global and ext normalization", "[cgi][registry]") {
    // global and per-location maps
    std::map<std::string, CgiSpec> globalMap;
    std::map<std::string, CgiSpec> locMap;

    globalMap[".php"] = CgiSpec("/usr/bin/php-cgi", 3000);
    globalMap[".py"]  = CgiSpec("/usr/bin/python3", 3000);

    // locally override .php and add .pl
    locMap[".php"] = CgiSpec("/opt/custom/php-cgi", 2500);
    locMap[".pl"]  = CgiSpec("/usr/bin/perl", 2000);

    CgiRegistry reg;
    reg.setSources(&locMap, &globalMap);

    // prefers location over global
    {
        const CgiSpec* s = reg.findByExtension(".php");
        REQUIRE(s != NULL);
        REQUIRE(s->bin == "/opt/custom/php-cgi");
        REQUIRE(s->timeout_ms == 2500);
    }
    // normalization: "py" (no dot) should still match ".py"
    {
        const CgiSpec* s = reg.findByExtension("py");
        REQUIRE(s != NULL);
        REQUIRE(s->bin == "/usr/bin/python3");
    }
    // location-only ext
    {
        const CgiSpec* s = reg.findByExtension(".pl");
        REQUIRE(s != NULL);
        REQUIRE(s->bin == "/usr/bin/perl");
    }
    // unknown
    {
        const CgiSpec* s = reg.findByExtension(".rb");
        REQUIRE(s == NULL);
    }
}

TEST_CASE("CgiHandler::buildEnv builds a correct CGI environment", "[cgi][env]") {
    // Prepare a realistic POST request with headers and a short body.
    const std::string raw =
        "POST /cgi-bin/hello.php?name=Bob&x=1 HTTP/1.1\r\n"
        "Host: www.example.com:8080\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 5\r\n"
        "X-Forwarded-For: 203.0.113.9\r\n"
        "\r\n"
        "abcde";

    HttpRequest req;
    REQUIRE(req.parse(raw.c_str(), raw.size()) == true);

    // Minimal VS context for env derivation
    VirtualServer vs;
    vs.listen_host = "0.0.0.0";
    vs.listen_port = 8080;
    vs.server_names.push_back("www.example.com");
    vs.root = "/var/www/html";

    // Build environment via CgiHandler private helper (exposed for test)
    CgiHandler h;
    std::vector<std::string> envv;
    int rc = h.buildEnv(req, vs, envv);
    REQUIRE(rc > 0);

    const std::map<std::string,std::string> env = envVecToMap(envv);

    // Core CGI variables
    REQUIRE(env.find("GATEWAY_INTERFACE") != env.end());
    CHECK(env.find("GATEWAY_INTERFACE")->second == "CGI/1.1");

    CHECK(env.find("REQUEST_METHOD") != env.end());
    CHECK(env.find("REQUEST_METHOD")->second == "POST");

    CHECK(env.find("QUERY_STRING") != env.end());
    CHECK(env.find("QUERY_STRING")->second == "name=Bob&x=1");

    CHECK(env.find("SERVER_PROTOCOL") != env.end());
    CHECK(env.find("SERVER_PROTOCOL")->second == "HTTP/1.1");

    CHECK(env.find("CONTENT_TYPE") != env.end());
    CHECK(env.find("CONTENT_TYPE")->second == "application/x-www-form-urlencoded");

    CHECK(env.find("CONTENT_LENGTH") != env.end());
    CHECK(env.find("CONTENT_LENGTH")->second == "5");

    // Server identity: derive from Host header if present (host + explicit port)
    CHECK(env.find("SERVER_NAME") != env.end());
    CHECK(env.find("SERVER_NAME")->second == "www.example.com");

    CHECK(env.find("SERVER_PORT") != env.end());
    CHECK(env.find("SERVER_PORT")->second == "8080");

    // Remote address (prefer X-Forwarded-For if present)
    CHECK(env.find("REMOTE_ADDR") != env.end());
    CHECK(env.find("REMOTE_ADDR")->second == "203.0.113.9");

    // Script naming
    CHECK(env.find("SCRIPT_NAME") != env.end());
    CHECK(env.find("SCRIPT_NAME")->second == "/cgi-bin/hello.php");

    CHECK(env.find("SCRIPT_FILENAME") != env.end());
    CHECK(env.find("SCRIPT_FILENAME")->second == "/var/www/html/cgi-bin/hello.php");
}

TEST_CASE("CgiHandler::buildEnv defaults without Host/XFF", "[cgi][env][defaults]") {
    // No Host / no XFF, GET without body
    const std::string raw =
        "GET /cgi/echo.py HTTP/1.0\r\n"
        "\r\n";

    HttpRequest req;
    REQUIRE(req.parse(raw.c_str(), raw.size()) == true);

    VirtualServer vs;
    vs.listen_host = "127.0.0.1";
    vs.listen_port = 8079;
    vs.server_names.clear();
    vs.root = "/srv/www";

    CgiHandler h;
    std::vector<std::string> envv;
    REQUIRE(h.buildEnv(req, vs, envv) > 0);
    const std::map<std::string,std::string> env = envVecToMap(envv);

    // Defaults: SERVER_NAME falls back to VS listen_host (or first server_name if any)
    CHECK(env.find("SERVER_NAME") != env.end());
    CHECK(env.find("SERVER_NAME")->second == "127.0.0.1");

    CHECK(env.find("SERVER_PORT") != env.end());
    CHECK(env.find("SERVER_PORT")->second == "8079");

    // No body → CONTENT_LENGTH may be absent or "0" depending on your policy.
    // We only assert that if present, it's "0".
    std::map<std::string,std::string>::const_iterator it = env.find("CONTENT_LENGTH");
    if (it != env.end())
        CHECK(it->second == "0");

    // Query string empty for no "?..."
    CHECK(env.find("QUERY_STRING") != env.end());
    CHECK(env.find("QUERY_STRING")->second == "");

    // Script filename rooted under VS root
    CHECK(env.find("SCRIPT_FILENAME") != env.end());
    CHECK(env.find("SCRIPT_FILENAME")->second == "/srv/www/cgi/echo.py");
}
