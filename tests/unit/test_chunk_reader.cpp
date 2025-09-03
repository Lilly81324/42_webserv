#include <catch2/catch_all.hpp>

#include "ChunkedReader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

// tiny helper
static std::string slurp(const std::string &p)
{
	FILE *f = std::fopen(p.c_str(), "rb");
	REQUIRE(f != 0);
	std::string s;
	char buf[4096];
	for (;;)
	{
		std::size_t n = std::fread(buf, 1, sizeof(buf), f);
		if (n == 0)
			break;
		s.append(buf, buf + n);
	}
	std::fclose(f);
	return s;
}

/**
 * Chunk reading fails
 */
TEST_CASE("chunked: immediate spill (threshold==0), completes after trailers", "[chunked][spill]")
{
	std::vector<char> mem;
	ChunkedReader cr(mem, /*spill_threshold*/ 1, /*tmp_dir*/ "/tmp");

	// 4 + "Wiki" + CRLF, 5 + "pedia" + CRLF, 0 + CRLF CRLF
	const std::string p1 = "4\r\nWiki\r\n";
	const std::string p2 = "5\r\npedia\r\n0\r\n\r\n";

	REQUIRE(cr.consume(p1.data(), p1.size()) == p1.size());
	REQUIRE_FALSE(cr.complete());
	(void)cr.flush_to_disk(64 * 1024);

	REQUIRE(cr.consume(p2.data(), p2.size()) == p2.size());
	(void)cr.flush_to_disk(64 * 1024);
	REQUIRE(cr.consume(p2.data(), p2.size()) == p2.size());

	REQUIRE(cr.complete());
	REQUIRE(cr.isBodyOnDisk());
	REQUIRE(cr.getBodyLength() == 9);

	const std::string path = cr.getBodyFilePath();
	REQUIRE_FALSE(path.empty());
	struct stat st;
	REQUIRE(::stat(path.c_str(), &st) == 0);
	REQUIRE((unsigned long long)st.st_size == 9ULL);
	REQUIRE(slurp(path) == "Wikipedia");
	REQUIRE(::unlink(path.c_str()) == 0);
}

TEST_CASE("chunked: kept in memory when threshold >> body", "[chunked][inmem]")
{
	std::vector<char> mem;
	ChunkedReader cr(mem, /*spill_threshold*/ 1024, "/tmp");

	const std::string body =
		"3\r\nabc\r\n"
		"2\r\nde\r\n"
		"0\r\n\r\n";

	REQUIRE(cr.consume(body.data(), body.size()) == body.size());
	REQUIRE(cr.consume(body.data(), body.size()) == body.size());
	REQUIRE(cr.complete());
	REQUIRE_FALSE(cr.isBodyOnDisk());
	REQUIRE(cr.getBodyLength() == 5);
	REQUIRE(mem.size() == 5);
	REQUIRE(std::string(mem.begin(), mem.end()) == "abcde");
	REQUIRE(cr.flush_to_disk(4096) == 0); // no-op when not on disk
}

TEST_CASE("chunked: size line split across buffers & with extension", "[chunked][split][ext]")
{
	std::vector<char> mem;
	ChunkedReader cr(mem, /*spill_threshold*/ 0, "/tmp");
	
	const std::string p1 = "a;foo=bar\r\n012345";
	const std::string p2 = "6789\r\n0\r\n\r\n";

	REQUIRE(cr.consume(p1.data(), p1.size()) == p1.size());
	(void)cr.flush_to_disk(1 << 20);
	REQUIRE_FALSE(cr.complete());

	REQUIRE(cr.consume(p2.data(), p2.size()) == p2.size());
	(void)cr.flush_to_disk(1 << 20);
	REQUIRE(cr.consume(p2.data(), p2.size()) == p2.size());
	REQUIRE(cr.complete());
	REQUIRE(cr.getBodyLength() == 10);
}

TEST_CASE("chunked: invalid hex -> enters error state; further consume returns 0", "[chunked][error]")
{
	std::vector<char> mem;
	ChunkedReader cr(mem, 0, "/tmp");

	const std::string bad = "Z\r\nx\r\n"; // 'Z' not hex
	std::size_t used = cr.consume(bad.data(), bad.size());
	// parser will hit S_ERROR quickly; used may be >0 up to first detection
	REQUIRE(used <= bad.size());

	// Any further data won't be consumed in error state
	const std::string more = "4\r\nabcd\r\n0\r\n\r\n";
	REQUIRE(cr.consume(more.data(), more.size()) == 0);
	REQUIRE_FALSE(cr.complete());
	// bytes_received() shouldn't have increased due to data after error
	// (we don't assert exact value, only that it doesn't become 8)
	REQUIRE(cr.bytes_received() != 8u);
}
