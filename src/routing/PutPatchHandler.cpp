/* --- PutPatchHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/16/2025
------------------------------------------ */

#include "PutPatchHandler.h"
#include "HTTPCODES.h"

PutPatchHandler::PutPatchHandler() {
    // Constructor
}

PutPatchHandler::~PutPatchHandler() {
    // Destructor
}

/**
 * Checks wether file can be written to or not
 * @param path path of the file
 * @returns true if writeable
 * @returns false if not
 */
bool checkWritePermission(const std::string &path)
{
	int	status;
	struct stat buffer;

	if (access(path.c_str(), W_OK) == 0)
		return (true);
	return (false);
}

/**
 * @brief Writes into path file based off the content of filename
 */
int	writeFileTemp(HttpRequest &req, const std::string &filename)
{
	
}

/**
 * Writes HttpRequest->body into a file created by opening
 * the HttpRequest->path. 
 * Writes in one chunk and performs test if opening and writing
 * was validly performed
 * @brief Writes into path file based off req body
 * @param path Name of the file to create and write
 * @param cont Vector of characters containing the content to write into a file
 */
int	writeFilePath(const std::string &path, const std::vector<char> &cont)
{
	std::ofstream to; 

	to.open(path.c_str());
	if (!to.is_open())
		return (HTTP_FORBIDDEN);
	to.write(cont.data(), cont.size());
	if (!to.good())
	{
		to.close();
		return (HTTP_INV_SERVER_ERR);
	}
	to.close();
	return (0);
}

/**
 * Writes Temporary File containing body into a file 
 * created by opening the HttpRequest->path. 
 * Writes in one chunk and performs test if opening and writing
 * was validly performed
 * @brief Writes into path file based off req body
 * @param path Name of the file to create and write
 * @param cont Vector of characters containing the content to write into a file
 */
int	writeFileTemp(const std::string &path, const std::string &source)
{
	std::ofstream to; 
	std::ifstream from;
	char buffer[PUT_WRITE_BUFFER_SIZE];

	// Try to open target
	to.open(path.c_str());
	if (!to.is_open())
		return (HTTP_FORBIDDEN);

	// Try to open source (hehe)
	from.open(source.c_str());
	if (!from.is_open())
	{
		to.close();
		return (HTTP_FORBIDDEN);
	}

	while (from)
	{
		from.read(buffer, PUT_WRITE_BUFFER_SIZE);
		std::streamsize written = from.gcount();
		if (written > 0)
			to.write(buffer, written);
		if (!to.good())
		{
			from.close();
			to.close();
			return (HTTP_INV_SERVER_ERR);
		}
	}
	from.close();
	to.close();
	return (0);
}

/**
 * @brief Creates or overwrites a file
 * @param req Request that specifies the file to overwrite
 * @param res Response, which should be prepared by this function
 * @param ctx Context, how the file should be created
 * @returns HTTP_FILE_CREATED if new resource created 
 * @returns HTTP_OK if existing file updated
 * @returns HTTP_FORBIDDEN if no write permissions on this file
 * @returns HTTP_CONFLICT if server forbids the file from being created
 * @returns HTTP_INV_SERVER_ERR on unexpected errors
 */
int	PutPatchHandler::handle_put(HttpRequest &req, HttpResponse &res, RequestContext &ctx)
{
	std::string	path;
	int			fd;
	int			status;
	bool		exists;
	
	// Make path
	path = req.getPath();

	// Check if file exists, for the right exit code
	exists = access(path.c_str(), F_OK);

	// Check writing permissions
	if (access(path.c_str(), W_OK) != 0)
		return (HTTP_FORBIDDEN);

	// Create file and write to it
	if (ctx.temp_file_used)
		status = writeFileTemp(req.getPath(), ctx.temp_filename);
	else
		status = writeFilePath(req.getPath(), req.getBody());

	if (status != 0)
		return (status);
	
	if (exists)
		return (HTTP_OK);
	return (HTTP_FILE_CREATED);
}
