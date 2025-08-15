#include <catch2/catch_all.hpp>
#include "VirtualServer.h"

TEST_CASE("VirtualServer default construction", "[VirtualServer]")
{
	VirtualServer vs;
	REQUIRE(vs.listen_port == 0);
	REQUIRE(vs.listen_host.empty());
	REQUIRE(vs.root.empty());
	REQUIRE(vs.index_files.empty());
	REQUIRE(vs.server_names.empty());
	REQUIRE(vs.locations.empty());
}

TEST_CASE("VirtualServer can store locations", "[VirtualServer]")
{
	VirtualServer vs;
	Location loc;
	loc.path_prefix = "/api";
	loc.root = "/var/www/api";
	vs.locations.push_back(loc);
	REQUIRE(vs.locations.size() == 1);
	REQUIRE(vs.locations[0].path_prefix == "/api");
	REQUIRE(vs.locations[0].root == "/var/www/api");
}
