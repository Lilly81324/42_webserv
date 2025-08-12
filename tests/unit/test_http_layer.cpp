#include <catch2/catch_all.hpp>
#include "Headers.h"
#include "CookieJar.h"

TEST_CASE("HTTP Layer tests", "[https]")
{
	SECTION("Header tests")
	{
		{
			// Check everything on unset Header
			Headers test;
			Headers empty;

			test.getBegin();
			test.getEnd();
			test.show(std::cout);
			std::cout << test;
			REQUIRE(test.keyExists("Something") == 0);
			REQUIRE(test.serialize() == "\r\n");
			REQUIRE(test.get("Something") == "");
			REQUIRE(test.getLength() == 0);
			REQUIRE(test.isEmpty() == true);
			test.clear();
			test.erase("something");
			test.mergeFrom(empty);
			test.set("", "");
		}
		{
			// Simple tests
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
			REQUIRE(filled.getLength() == 1);
			// Check if Header is empty
			REQUIRE(empty.isEmpty() == true);
			REQUIRE(filled.isEmpty() == false);
			// Test removing things from Header
			filled.erase("Key");
			REQUIRE(filled.isEmpty() == true);
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
			REQUIRE(filled.serialize() == "Type: https\r\nMessage: Hewwo\r\n\r\n");
		}
		{
			// Check case insensitivity
			Headers test;

			test.set("KeY", "0");
			test.set("key", "1");
			test.set("Ke", "2");
			test.set("kE", "3");
			test.set("KEyy", "4");
			test.set("keYY", "5");
			REQUIRE(test.getLength() == 3);
			REQUIRE(test.serialize() == "Ke: 3\r\nKeY: 1\r\nKEyy: 5\r\n\r\n");
		}
	}
	SECTION("CookieJar")
	{
		{
			// Check everything on unset CookieJar
			CookieJar test;
			Headers test2;

			test.getBegin();
			test.getEnd();
			test.show(std::cout);
			std::cout << test;
			REQUIRE(test.keyExists("Something") == 0);
			REQUIRE(test.get("Something") == "");
			REQUIRE(test.getLength() == 0);
			REQUIRE(test.isEmpty() == true);
			test.clear();
			test.erase("something");
			test.set("", "");
			test.setCookieHeaders(test2, "", "", "", 0, false, false, "");
		}
		{
			// Simple tests
			CookieJar filled;
			CookieJar empty;
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
			REQUIRE(filled.getLength() == 1);
			// Check if Header is empty
			REQUIRE(empty.isEmpty() == true);
			REQUIRE(filled.isEmpty() == false);
			// Test removing things from Header
			filled.erase("Key");
			REQUIRE(filled.isEmpty() == true);
			REQUIRE(filled.getLength() == 0);
			// Test removing invalid key
			empty.erase("Key");
			REQUIRE(empty.isEmpty() == true);
			// Test clearing Header;
			empty.clear();
			REQUIRE(empty.isEmpty());
			REQUIRE(empty.getLength() == 0);
		}
		{
			// Check case insensitivity
			CookieJar test;
			test.set("KeY", "0");
			test.set("key", "1");
			test.set("Ke", "2");
			test.set("kE", "3");
			test.set("KEyy", "4");
			test.set("keYY", "5");
			REQUIRE(test.getLength() == 3);
			REQUIRE(test.get("Ke") == "3");
			REQUIRE(test.get("KeY") == "1");
			REQUIRE(test.get("KEyy") == "5");
		}
	}
}