#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "EventLoop.h"

// Simple test to verify Catch2 is working
TEST_CASE("Basic test", "[basic]") {
    REQUIRE(1 + 1 == 2);
    REQUIRE_FALSE(1 + 1 == 3);
    // Added a comment to test incremental build
}

TEST_CASE("String operations", "[string]") {
    std::string hello = "hello";
    std::string world = "world";
    
    REQUIRE(hello.length() == 5);
    REQUIRE(world.length() == 5);
    REQUIRE(hello + " " + world == "hello world");
}

TEST_CASE("Testing Loops", "[EventLopp]"){

	EventLoop EventLoop;


}