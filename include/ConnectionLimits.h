#if !defined(CONNECTION_LIMITS_H)
#define CONNECTION_LIMITS_H

#include <cstddef>

struct ConnectionLimits
{
	std::size_t max_header_bytes;
	std::size_t max_body_bytes;
	ConnectionLimits(std::size_t h, std::size_t b): max_header_bytes(h), max_body_bytes(b){};
};

#endif // CONNECTION_LIMITS_H
