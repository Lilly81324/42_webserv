/* --- PutPatchHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/16/2025
------------------------------------------ */

#include "PutPatchHandler.h"
#include "HTTPCODES.h"

PutPatchHandler::PutPatchHandler()
{
	// Constructor
}

PutPatchHandler::~PutPatchHandler()
{
	// Destructor
}

/**
 * @brief Writes into path file based off req body
 * 
 * Writes HttpRequest->body into a file created by
 * opening the HttpRequest->path
 * Writes in one chunk and performs test if opening and writing
 * was validly performed
 * @param path Name of the file to create and write
 * @param cont Vector of characters containing the content to write into a file
 */
int	writeFilePath(const char *path, const std::vector<char> &cont)
{
	std::ofstream to; 

	// Try to open target in binary and truncating mode
	to.open(path, std::ios::binary | std::ios::trunc);
	if (!to.is_open())
		return (HTTP_FORBIDDEN);
	
	// Write in a single write from our <cont> into our target
	to.write(cont.data(), cont.size());

	// If error in target
	if (!to.good())
	{
		to.close();
		return (HTTP_INV_SERVER_ERR);
	}
	to.close();
	return (0);
}

/**
 * @brief Writes into path file based off temp file
 * 
 * Writes Temporary File containing body into a file 
 * created by opening the HttpRequest->path. 
 * Writes in multiple chunks, at most PUT_WRITE_BUFFER_SIZE bytes big
 * Repeats writing until EOF or error hit
 * @param path Name of the file to create and write
 * @param cont Name of the temporary file holding the body information
 */
int	writeFileTemp(const char *path, const std::string &source)
{
	std::ofstream to; 
	std::ifstream from;
	char buffer[PUT_WRITE_BUFFER_SIZE];

	// Try to open target in binary and truncating mode
	to.open(path, std::ios::binary | std::ios::trunc);
	if (!to.is_open())
		return (HTTP_FORBIDDEN);

	// Try to open source (hehe)
	from.open(source.c_str());
	if (!from.is_open())
	{
		to.close();
		return (HTTP_FORBIDDEN);
	}

	// While the <from> streams badbit and failbit are both set (stream still has data)
	while (from)
	{
		// Read data into buffer, so we can see how much was written
		from.read(buffer, PUT_WRITE_BUFFER_SIZE);
		std::streamsize written = from.gcount();

		// Write as much into <to> as was read from <from>
		if (written > 0)
			to.write(buffer, written);
		
		// If error in <to> fstream
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
 * 
 * Checks if file exists, then tries to write to file
 * Writing is based on if the context (ctx) temp_file_used is set to true
 * If true, then the file gets written based on a teporary files data
 * If false, then the file gets written based on the Requests entire body
 * Returns different codes based on if file already existed and was modified
 * or if the file was created completely new
 * @param req Request that specifies the file to overwrite
 * @param res Response, which should be prepared by this function
 * @param ctx Context, how the file should be created
 * @returns HTTP_FILE_CREATED if new resource created 
 * @returns HTTP_OK if existing file updated
 * @returns HTTP_FORBIDDEN if no write permissions on this file
 * @returns HTTP_INV_SERVER_ERR on unexpected errors
 */
int	PutPatchHandler::handle_put(HttpRequest &req, HttpResponse &res, RequestContext &ctx)
{
	int			status;
	bool		exists;
	
	// Make path for file to put
	const char *path = req.getPath().c_str();

	// Store if the file is new or not, for later
	exists = access(path, F_OK) + 1;

	// Create file and write to it
	if (ctx.temp_file_used)
		status = writeFileTemp(path, ctx.temp_filename);
	else
		status = writeFilePath(path, req.getBody());

	// If Any exit code encountered in writing, exit with that
	if (status != 0)
		return (status);
	
	// If File was created from fresh -> 201, otherwise -> 200
	if (exists)
		return (HTTP_OK);
	return (HTTP_FILE_CREATED);
}
