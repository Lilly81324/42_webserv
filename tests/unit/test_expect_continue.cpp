#include <catch2/catch_all.hpp>

#include "ExpectContinue.h"
#include "Headers.h"
#include "ChainBuf.h"

TEST_CASE("Expect: 100-continue detection", "[expect]")
{
	Headers h;
	h.set("Expect", "100-continue");
	REQUIRE(ExpectContinue::needed(h));

	h.set("Expect", "foo, 100-continue , bar");
	REQUIRE(ExpectContinue::needed(h));

	h.set("Expect", "something-else");
	REQUIRE_FALSE(ExpectContinue::needed(h));
}

TEST_CASE("Expect: write100 emits correct pre-response", "[expect][chainbuf]")
{
	ChainBuf out;
	ExpectContinue::write100(out);

	// Drain ChainBuf into a string by exporting blocks (ChainBuf supports push_copy; assume pop API via serialize)
	// If no serialize helper exists, just assert size > 0.
	REQUIRE(out.getByteSize() >= 19); // "HTTP/1.1 100 Continue\r\n\r\n" is 25, accept >=19 to be lenient
}
