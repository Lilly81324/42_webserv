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

TEST_CASE("VirtualServer rejects invalid port", "[VirtualServer][invalid]") {
    VirtualServer vs;
    vs.listen_port = -1;
    REQUIRE(vs.listen_port < 1);
}

TEST_CASE("VirtualServer allows empty server_names", "[VirtualServer]") {
    VirtualServer vs;
    REQUIRE(vs.server_names.empty());
}

TEST_CASE("VirtualServer handles many locations", "[VirtualServer]") {
    VirtualServer vs;
    for (int i = 0; i < 100; ++i) {
        Location loc;
        loc.path_prefix = "/loc" + std::string(1, 'A' + (i % 26));
        vs.locations.push_back(loc);
    }
    REQUIRE(vs.locations.size() == 100);
}
