#include <catch2/catch_all.hpp>

#include "FileBodyReader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

TEST_CASE("FileBodyReader: staged consume + bounded flush", "[filebody]")
{
	FileBodyReader fr("/tmp");
	const char *p1 = "hello ";
	const char *p2 = "world";
	REQUIRE(fr.consume(p1, std::strlen(p1)) == 6);
	REQUIRE(fr.consume(p2, std::strlen(p2)) == 5);

	// Try a small then large flush; allow 0 on first if kernel says no-progress
	(void)fr.flush_to_disk(4);
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
	REQUIRE(::unlink(path.c_str()) == 0);

}

TEST_CASE("FileBodyReader: multiple small chunks + compaction", "[filebody][pending]")
{
	FileBodyReader fr("/tmp");
	const std::string s(8192, 'x');
	for (int i = 0; i < 8; ++i)
		REQUIRE(fr.consume(s.data(), s.size()) == s.size());

	// Drain in bounded steps; no correctness assertion on counts, only eventual completion
	std::size_t flushed = 0;
	for (int tries = 0; tries < 64; ++tries)
		flushed += fr.flush_to_disk(1024);

	fr.set_done(true);
	REQUIRE(fr.complete());
	REQUIRE(flushed > 0);
	REQUIRE(::unlink(fr.getBodyFilePath().c_str()) == 0);
}
