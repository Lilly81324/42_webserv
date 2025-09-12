#include <catch2/catch_all.hpp>
#include "StaticHandler.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "RequestContext.h"
#include "ServerConfig.h"
#include "HTTPCODES.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static bool fileExists(const char *file)
{
	return (access(file, F_OK) == 0);
}

static void makeFile(const char *file, const std::string &content)
{
	std::ofstream stream;

	stream.open(file, std::ios::out | std::ios::binary);
	if (stream.is_open())
	{
		stream.write(content.c_str(), content.length());
		stream.close();
	}
}

/**
 * @brief Checks if a file has specified contents
 * @param length Amount of bytes the Content will be
 * @param filename Name of the file to check
 * @param goal The Content that the file should have
 * @warning length and goal have to match 
 */
static void	checkFileContent(int length, const std::string &filename, const std::string &goal)
{
		char buffer[length + 1];
		std::ifstream in;

		in.open(filename.c_str(), std::ios::in | std::ios::binary);
		REQUIRE(in.is_open());
		REQUIRE(in.good());
		memset((void *)&buffer, 0, length + 1);
		in.read(buffer, length);
		REQUIRE(in.gcount() == length);
		std::string content(buffer, length);
		REQUIRE(content == goal);
		REQUIRE(in.good());
		in.read(buffer, 1);
		REQUIRE(!in.good());
		REQUIRE(in.eof());
		in.close();
}

TEST_CASE("Delete Static Handler", "[handler][static][delete]")
{// Prepare ctx.effective root to be root of folder and ctx.rel_path to be filename
	// Also prepare a file to contain the bodys information
	RequestContext ctx;
	StaticHandler handle;
	char cwd[4096];
	getcwd(cwd, sizeof(cwd));
	ctx.effective_root = std::string(cwd) + std::string("/www/upload");
	ctx.rel_path = "/DeleteMe.txt";
	ctx.temp_filename = std::string(cwd) + "/DeleteMeBodyFile.txt";
	const char *bodyFile = ctx.temp_filename.c_str();
	std::string fileString = (ctx.effective_root + ctx.rel_path);
	const char *file = fileString.c_str();
	SECTION("Normal File Deletion")
	{
		{	// Basic test
			HttpRequest req;
			HttpResponse res;

			makeFile(file, "123");
			std::string parse;
			parse = "DELETE / HttpVersion/1.1\r\nContent-Length: 1\r\n\r\nX";
			req.parse(parse.c_str(), parse.length());
			REQUIRE(handle.handle(req, res, ctx) == true);
			REQUIRE(res.getStatusCode() == HTTP_OK);
			REQUIRE(!fileExists(file));
		}
		{	// Deleting file that user doesnt have permission for
			// Right now behaviour expects it to work
			// May be changed in the future
			HttpRequest req;
			HttpResponse res;

			makeFile(file, "123");
			chmod(file, 0);
			std::string parse;
			parse = "DELETE / HttpVersion/1.1\r\nContent-Length: 1\r\n\r\nX";
			req.parse(parse.c_str(), parse.length());
			REQUIRE(handle.handle(req, res, ctx) == true);
			REQUIRE(res.getStatusCode() == HTTP_OK);
			REQUIRE(!fileExists(file));
		}
	}
	SECTION("Invalid File Deletion")
	{
		{	// Deleting non existant
			HttpRequest req;
			HttpResponse res;

			std::remove(file);
			std::string parse;
			parse = "DELETE / HttpVersion/1.1\r\nContent-Length: 1\r\n\r\nX";
			req.parse(parse.c_str(), parse.length());
			REQUIRE(handle.handle(req, res, ctx) == true);
			REQUIRE(res.getStatusCode() == HTTP_NOT_FOUND);
		}
		{	// Deleting in bad folder
			// TODO: Add way of testing for DELETEing files in directories that dont allow it
		}
	}
}