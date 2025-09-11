#if !defined(BODYREADER_H)
#define BODYREADER_H

#include <cstddef>
#include <string>
#include "UniqueFD.h"

class IBodyReader
{
	public:
		virtual ~IBodyReader() {}
		virtual std::size_t consume(const char *data, std::size_t len) = 0;
		virtual bool complete() const = 0;
		virtual std::size_t bytes_received() const = 0;
};

#endif //  BODYREADER_H
