#ifndef CGISPEC_H
#define CGISPEC_H

#include <string>

struct CgiSpec {
    std::string bin;
    int         timeout_ms;
    CgiSpec() : bin(""), timeout_ms(0) {}
    CgiSpec(const std::string& b, int t) : bin(b), timeout_ms(t) {}
};

#endif // CGISPEC_H
