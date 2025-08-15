#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "ServerConfig.h"
#include <fstream>
#include <stdexcept>

TEST_CASE("ServerConfig parses minimal config", "[ServerConfig]")
{
	ServerConfig cfg;
	REQUIRE_NOTHROW(cfg.parseFile("tests/unit/config/minimal.conf"));
	REQUIRE(cfg.servers().size() == 1);
	const VirtualServer &vs = cfg.servers()[0];
	REQUIRE(vs.listen_port == 8080);
	REQUIRE(vs.root == "/var/www/html");
	REQUIRE(vs.index_files.size() == 1);
	REQUIRE(vs.index_files[0] == "index.html");
}

TEST_CASE("ServerConfig detects missing brace", "[ServerConfig][invalid]")
{
	ServerConfig cfg;
	REQUIRE_THROWS_AS(cfg.parseFile("tests/unit/config/invalid_missing_brace.conf"), std::runtime_error);
}

TEST_CASE("ServerConfig detects bad port", "[ServerConfig][invalid]")
{
	ServerConfig cfg;
	REQUIRE_THROWS_AS(cfg.parseFile("tests/unit/config/invalid_bad_port.conf"), std::runtime_error);
}

TEST_CASE("ServerConfig parses multiple servers", "[ServerConfig]")
{
	ServerConfig cfg;
	REQUIRE_NOTHROW(cfg.parseFile("tests/unit/config/multi_server.conf"));
	REQUIRE(cfg.servers().size() == 2);
	REQUIRE(cfg.servers()[0].listen_port == 8080);
	REQUIRE(cfg.servers()[1].listen_port == 8081);
}
