/* --- IoRing.h --- */

/* ------------------------------------------
Author: Hydra
Date: 9/1/2025
------------------------------------------ */

#ifndef IORING_H
#define IORING_H

#include <vector>
#include <string.h>
#include <cstddef>

class IoRing
{
public:
	explicit IoRing(std::size_t cap = 64 * 1024);

	char *writePtr() { return &buf[0] + w; };

	std::size_t writeAvail() const { return buf.size() - w; };

	void wrote(std::size_t n) { w += n; };

	char *readPtr() { return &buf[0] + r; };
	std::size_t readAvail() const { return w - r;};

	void consumed(std::size_t n) { r += n;};

	void compactIfNeeded()
	{
		if (r > 0 && (r > buf.size()/2 || w == buf.size())) {
			const std::size_t n = w - r;
			if (n) memmove(&buf[0], &buf[0] + r, n);
			r = 0; w = n;
		  }
	};

private:
	std::vector<char> buf;
	std::size_t r, w;
};

#endif // IORING_H
