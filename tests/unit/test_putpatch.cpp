#include <catch2/catch_all.hpp>
#include "PutPatchHandler.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "RequestContext.h"
#include "HTTPCODES.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

void	checkFileContent(int length, const std::string &filename, const std::string &result)
{
		char buffer[length + 1];
		std::ifstream in;

		in.open(filename.c_str(), std::ios::in | std::ios::binary);
		REQUIRE(in.is_open());
		REQUIRE(in.good());
		memset((void *)&buffer, 0, length + 1);
		in.read(buffer, length);
		in.close();
		std::string content(buffer);
		REQUIRE(content == result);
}

TEST_CASE("PUT_HANDLER", "[handler][put]")
{
	SECTION("Normal test with Content from body")
	{
		char cwd[4096];
		getcwd(cwd, sizeof(cwd));
		std::string path(cwd);
		path += "DeleteMe.txt";
		{
			HttpRequest req;
			HttpResponse res;
			RequestContext ctx;
			PutPatchHandler pat;
			
			std::remove(path.c_str());
			std::string parse;
			parse = "PUT " + path + " HttpVersion/1.1\r\nContent-Length: 7\r\n\r\nAbc123\n";
			req.parse(parse.c_str(), parse.length());
			REQUIRE(pat.handle_put(req, res, ctx) == HTTP_FILE_CREATED);
			checkFileContent(7, path, "Abc123\n");
			std::remove(path.c_str());
		}
		{
			HttpRequest req;
			HttpResponse res;
			RequestContext ctx;
			PutPatchHandler pat;

			std::remove(path.c_str());
			std::string parse;
			parse = "PUT " + path + " HttpVersion/1.1\r\nContent-Length: 1\r\n\r\nX";
			req.parse(parse.c_str(), parse.length());
			REQUIRE(pat.handle_put(req, res, ctx) == HTTP_FILE_CREATED);
			checkFileContent(1, path, "X");
			std::remove(path.c_str());
		}

	}
}