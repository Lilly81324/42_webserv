#if !defined(CONTENTLENGHTREADER_H)
#define CONTENTLENGHTREADER_H

#include "BodyReader.h"

class ContentLenghtReader : public IBodyReader
{
public:
	ContentLenghtReader(std::size_t total, std::string &dst) : need(total), got(0), dst(dst) {}
	std::size_t consume(const char *data, std::size_t len);
	bool complete() const { return need == 0; }
	std::size_t bytes_received() const { return got; }

private:
	std::size_t need, got;
	std::string &dst;
};

#endif //  CONTENTLENGHTREADER_H
