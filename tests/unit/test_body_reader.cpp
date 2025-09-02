#include <catch2/catch_all.hpp>

#include "FileBodyReader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Helper: small file slurp
static std::string slurp(const std::string &p)
{
	FILE *f = std::fopen(p.c_str(), "rb");
	REQUIRE(f != 0);
	std::string s;
	char buf[1024];
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

TEST_CASE("FileBodyReader consumes and flushes to disk without blocking", "[filebody]")
{
	FileBodyReader fr("/tmp");
	const char *p1 = "hello ";
	const char *p2 = "world";
	REQUIRE(fr.consume(p1, std::strlen(p1)) == 6);
	REQUIRE(fr.consume(p2, std::strlen(p2)) == 5);

	// try a few small bounded flushes
	std::size_t n1 = fr.flush_to_disk(4);
	REQUIRE(n1 <= 4);
	std::size_t n2 = fr.flush_to_disk(64 * 1024);
	REQUIRE(n1 + n2 <= 11);

	// drain remaining pending
	for (int i = 0; i < 4 && fr.flush_to_disk(64 * 1024) > 0; ++i)
	{
	}

	fr.set_done(true);
	REQUIRE(fr.complete());

	const std::string path = fr.getBodyFilePath();
	REQUIRE_FALSE(path.empty());

	struct stat st;
	REQUIRE(::stat(path.c_str(), &st) == 0);
	REQUIRE(fr.bytes_received() == (std::size_t)st.st_size);
	REQUIRE(slurp(path) == "hello world");
}
