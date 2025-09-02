#if !defined(CHUNKED_READER_H)
#define CHUNKED_READER_H

#include "BodyReader.h"

class ChunkedReader : public IBodyReader
{
	public:
		ChunkedReader(std::string &dst) : state(SIZE), chunk_left(0), line(), got(), dst(dst) {}

	private:
		enum ChunkState
		{
			SIZE,
			DATA,
			CRLF,
			DONE,
		};
		ChunkState state;
		std::size_t chunk_left;
		std::string line;
		std::size_t got;
		std::string &dst;
};

#endif // CHUNKED_READER_H
