#include <catch2/catch_all.hpp>
#include "Headers.h"

TEST_CASE("HTTP Layer tests", "[https]")
{
	SECTION("Header tests")
	{
		Headers filled;
		Headers empty;

		filled.set("Key", "Value");
		// Checking value of invalid keys
		REQUIRE(empty.get("nonsense") == "");
		REQUIRE(filled.get("nonsense") == "");
		// Checking existance of invalid keys
		REQUIRE(empty.keyExists("nonsense") == false);
		REQUIRE(filled.keyExists("nonsense") == false);
		// Checking value of valid keys
		REQUIRE(empty.get("Key") == "");
		REQUIRE(filled.get("Key") == "Value");
		// Checking existance of valid keys
		REQUIRE(empty.keyExists("Key") == false);
		REQUIRE(filled.keyExists("Key") == true);
		// Check that keys get overwritten instead of added
		filled.set("Key", "WRONG");
		REQUIRE(filled.keyCount("Key") == 1);
		REQUIRE(filled.getLength() == 1);
		// Check if Header is empty
		REQUIRE(empty.isEmpty() == true);
		REQUIRE(filled.isEmpty() == false);
		// Test removing things from Header
		filled.erase("Key");
		REQUIRE(filled.isEmpty() == true);
		REQUIRE(filled.keyCount("Key") == 0);
		REQUIRE(filled.getLength() == 0);
		// Test removing invalid key
		empty.erase("Key");
		REQUIRE(empty.isEmpty() == true);
		// "Test" merging Headers
		empty.set("Type", "https");
		empty.set("Message", "Hewwo");
		filled.mergeFrom(empty);
		REQUIRE(filled.getLength() == 2);
		// Test clearing Header;
		empty.clear();
		REQUIRE(empty.isEmpty());
		REQUIRE(empty.getLength() == 0);
		// Check Serialization
		REQUIRE(filled.serialize() == "Message:Hewwo\nType:https\n");
		// Check everything on unset Header
		Headers neww;
		neww.getBegin();
		neww.getEnd();
		neww.show(std::cout);
		std::cout << neww;
		REQUIRE(neww.keyExists("Something") == 0);
		REQUIRE(neww.keyCount("Something") == 0);
		REQUIRE(neww.serialize() == "");
		REQUIRE(neww.get("Something") == "");
		REQUIRE(neww.getLength() == 0);
		REQUIRE(neww.isEmpty() == true);
		neww.clear();
		neww.erase("something");
		neww.mergeFrom(empty);
		neww.set("", "");
	}
}