#include <catch2/catch_all.hpp>
#include "HttpResponse.h"
#include "HEADER_ENTRIES.h"  // for HDR_CONTENT_LENGTH / HDR_CONTENT_TYPE (if you want the optional header checks)

TEST_CASE("HttpResponse basic API", "[HttpResponse]") {
    HttpResponse r;

    SECTION("Default construction is sane") {
        // We don’t assert on defaults that vary between versions (e.g., http_version),
        // only that object is usable and starts with empty body.
        REQUIRE(r.body.size() == 0);
        REQUIRE(r.bodyLength == 0);
    }

    SECTION("setStatus / getters round-trip") {
        r.setStatus(404, "Not Found");
        REQUIRE(r.getStatusCode() == 404);
        REQUIRE(r.getReason() == std::string("Not Found"));

        r.setStatus(500, "Internal Server Error");
        REQUIRE(r.getStatusCode() == 500);
        REQUIRE(r.getReason() == std::string("Internal Server Error"));
    }

    SECTION("clearBody empties body and sets Content-Length: 0") {
        // Seed some body and bodyLength to ensure clearBody() resets both.
        const char msg[] = "hello";
        r.body.assign(msg, msg + sizeof(msg) - 1);
        r.bodyLength = r.body.size();

        r.clearBody();

        REQUIRE(r.body.empty());
        REQUIRE(r.bodyLength == 0);

        // Optional: verify Content-Length header set by clearBody()
        REQUIRE(r.headers.get(HDR_CONTENT_LENGTH) == std::string("0"));
    }

    SECTION("Manual body assignment updates bodyLength when you set it") {
        // This mirrors how the pipeline typically finalizes Content-Length & bodyLength.
        const char msg[] = "hello";
        r.body.assign(msg, msg + sizeof(msg) - 1);
        r.bodyLength = r.body.size();

        REQUIRE(r.body.size() == 5);
        REQUIRE(r.bodyLength == 5);
    }

    SECTION("Headers container is usable") {
        // Not asserting on defaults — just verify setters/getters.
        r.headers.set(HDR_CONTENT_TYPE, "text/plain");
        REQUIRE(r.headers.get(HDR_CONTENT_TYPE) == std::string("text/plain"));
    }
}
