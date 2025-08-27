/* --- HttpResponse.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "HttpResponse.h"
#include <iostream>


std::ostream &operator<<(std::ostream &out, const HttpResponse &r)
{
    out << r.http_version << ' ' << r.status << ' ' << r.reason << "\r\n";

    out << r.headers;

    out << "Content-Length: " << r.body.size() << "\r\n";

    out << "\r\n";


    if (!r.body.empty())
        out.write(reinterpret_cast<const char*>(r.body.data()), static_cast<std::streamsize>(r.body.size()));

    return out;
}

