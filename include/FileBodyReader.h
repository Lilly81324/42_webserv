#if !defined( FILEBODYREADER_H)
#define  FILEBODYREADER_H

#include "BodyReader.h"

class FileBodyReader : public IBodyReader
{
	public:
		FileBodyReader(std::size_t soft_limit_bytes, std::string &small_buff);
		~FileBodyReader();
		std::size_t consume(const char *data, std::size_t len);
		bool complete() const;
		std::size_t bytes_received() const;
		bool using_file() const { return fd.valid(); }
		const std::string &temp_path() const { return path; }

	private:
		std::size_t soft_limit;
		std::size_t got;
		bool done;
		std::string &small_buf;
		UniqueFD fd;
		std::string path;
};

#endif //  FILEBODYREADER_H
