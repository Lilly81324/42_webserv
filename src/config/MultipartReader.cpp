
#include "MultipartReader.h"
#include <cctype>



/* 

static bool starts_with(const std::string& s, const std::string& p)

Lightweight prefix check used throughout the multipart parser to match boundary 
markers and header tokens efficiently. By avoiding regex and heavy abstractions, 
it keeps the hot path fast and allocation-free. Multipart boundaries are deterministic strings; 
a simple comparison suffices. Centralizing this utility avoids repeated compare boilerplate, 
improves readability of boundary logic, and makes future micro-optimizations easy. 
It’s called frequently while scanning buffers for --boundary or --boundary--, 
so a compact, inlineable helper helps overall throughput for large uploads and many parts.


*/

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}


/* 

MultipartReader::MultipartReader()

Initializes the streaming state machine to S_PREAMBLE, 
ready to consume preface bytes until the first boundary line. 
This conservative start guards against clients that include noise or 
leading CRLFs before the initial boundary. By starting in a known state, 
subsequent feeds can safely transition to headers or detect end markers. 
The constructor leaves buffers empty and error string clear, 
ensuring no stale state carries over between uses. 
This predictable baseline is essential for robustness, 
because uploads may be large and trickle in across many feed() 
calls under non-blocking I/O.


*/

MultipartReader::MultipartReader() : st_(S_PREAMBLE) {}



/* 

bool MultipartReader::ieq(char a, char b)

ASCII case-insensitive character equality. 
Used to compare header keys and certain tokens without relying on locale-dependent tolower. 
Multipart headers are ASCII by specification; this narrow focus avoids surprises and keeps code fast. 
Encapsulating the comparison clarifies intent when processing Content-Disposition, boundary parameters, 
and other case-insensitive matches. While small, it contributes to robust header parsing in environments 
where clients vary capitalization (e.g., Content-Disposition vs content-disposition).

*/

bool MultipartReader::ieq(char a, char b) {
    if (a >= 'A' && a <= 'Z')
        a = char(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
        b = char(b - 'A' + 'a');
    return a == b;
}


/* 
std::string MultipartReader::trim(const std::string& s)

Trims leading/trailing spaces and tabs. This is critical when parsing header 
fields and parameters because RFC formatting permits optional whitespace around separators. 
By normalizing tokens early, downstream parsing becomes simpler and less error-prone, 
especially for key=value pairs in Content-Disposition. 
Returning a substring rather than mutating in place avoids accidental side effects. 
It’s invoked repeatedly during header block parsing, 
so its tight loop and minimal branching are important 
for performance under large multipart requests.

*/

std::string MultipartReader::trim(const std::string& s) {
    std::string::size_type a = 0, b = s.size();
    while (a < b && (s[a]==' '||s[a]=='\t')) 
        ++a;
    while (b > a && (s[b-1]==' '||s[b-1]=='\t'))
        --b;
    return s.substr(a, b-a);
}


/* 

bool MultipartReader::parseContentDisposition(const std::string& v, std::string& name, std::string& filename)

Parses a Content-Disposition header for each part, expecting form-data and optional 
parameters name="..." and filename="...". It tokenizes on semicolons, trims whitespace, 
accepts quoted or unquoted values, and fills output fields. 
Failing non-form-data or malformed parameters returns false, triggering an error. 
This function identifies field names and files, enabling sinks to route data to memory or disk 
(e.g., writing uploaded files into storage). 
Accurate parsing here is essential for security and correctness—wrong 
filenames or missing names would confuse form processing or overwrite unintended paths.


*/

bool MultipartReader::parseContentDisposition(const std::string& v,
                                              std::string& name, std::string& filename)
{
    // Expect: form-data; name="field"; filename="file"
    name.clear(); 
    filename.clear();
    std::string s = v;
    // find first semicolon, skip type token (must be form-data)
    std::string::size_type semi = s.find(';');
    std::string type = trim(semi==std::string::npos ? s : s.substr(0, semi));
    if (type != "form-data")
        return false;
    std::string::size_type i = (semi==std::string::npos) ? s.size() : semi+1;
    while (i < s.size()) {
        while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]==';'))
            ++i;
        if (i >= s.size())
            break;
        std::string::size_type eq = s.find('=', i);
        if (eq == std::string::npos)
            break;
        std::string key = trim(s.substr(i, eq-i));

        std::string val;
        i = eq+1;
        if (i < s.size() && s[i]=='"') {
            ++i;
            while (i < s.size() && s[i]!='"') { 
                val.push_back(s[i]); ++i; 
            }
            if (i < s.size() && s[i]=='"')
                ++i;
        } else {
            while (i < s.size() && s[i] != ';') {
                val.push_back(s[i]); ++i;
            }
        }
        if (key == "name")
            name = val;
        else if (key == "filename")
            filename = val;
        // advance to next ; if any
        while (i < s.size() && s[i] != ';')
            ++i;
        if (i < s.size() && s[i]==';')
            ++i;
    }
    return true;
}


/* 

bool MultipartReader::initFromContentType(const std::string& ct)

Initializes the reader by parsing the HTTP Content-Type header, expecting multipart/form-data; 
boundary=.... It validates the type prefix and extracts the boundary parameter (supports quoted forms), 
then delegates to setBoundary. Returning false signals that the body isn’t 
multipart or is malformed, allowing callers to reject the request gracefully. 
Centralizing this setup keeps boundary handling consistent and avoids accidental 
acceptance of invalid content types, which could desynchronize the parser and corrupt uploads.


*/

bool MultipartReader::initFromContentType(const std::string& ct) {
    // minimal parse: "multipart/form-data; boundary=XXXX"
    if (!starts_with(ct, "multipart/form-data")) 
        return false;
    std::string::size_type b = ct.find("boundary=");
    if (b == std::string::npos)
        return false;
    std::string val = ct.substr(b + 9);
    // strip optional quotes
    if (!val.empty() && val[0]=='"') {
        if (val.size() < 2 || val[val.size()-1] != '"')
            return false;
        val = val.substr(1, val.size()-2);
    }
    return setBoundary(val);
}


/* 

bool MultipartReader::setBoundary(const std::string& boundary)

Validates and stores the raw boundary, precomputes --boundary and 
--boundary-- strings, clears buffers and errors, and resets state to S_PREAMBLE. 
It forbids CR/LF in boundary values to prevent injection attacks and parser ambiguities. 
Precomputing marker strings speeds subsequent searches, which occur frequently while streaming data. 
Resetting state ensures reuse is safe across multiple uploads by the same object if desired. 
Without strict validation here, later parsing would be unreliable or insecure.

*/

bool MultipartReader::setBoundary(const std::string& boundary) {
    if (boundary.empty())
        return false;
    // Basic validation: boundary may not contain CR/LF
    for (std::size_t i=0;i<boundary.size();++i)
        if (boundary[i]=='\r' || boundary[i]=='\n')
            return false;
    boundary_     = boundary;
    dashBoundary_ = std::string("--") + boundary_;
    endBoundary_  = dashBoundary_ + "--";
    st_ = S_PREAMBLE;
    buf_.clear();
    err_.clear();
    return true;
}


/* 

bool MultipartReader::findLine(std::string& out)

Searches the internal buffer for the next CRLF‐terminated line. 
If found, it returns that line without CRLF and advances the buffer; 
otherwise, returns false to indicate more data is required. 
This incremental line reader underpins both the preamble/epilogue 
skipping and header-block parsing. 
By operating on the accumulated buffer, it supports non-blocking feeds where lines may arrive split across packets. 
Keeping the logic here keeps higher-level states clean and easier to reason abou

*/

bool MultipartReader::findLine(std::string& out) {
    // Look for CRLF; keep one line in out (without CRLF)
    for (std::size_t i=0;i+1<buf_.size();++i) {
        if (buf_[i]=='\r' && buf_[i+1]=='\n') {
            out.assign(buf_, 0, i);
            buf_.erase(0, i+2);
            return true;
        }
    }
    return false;
}


/* 

bool MultipartReader::startBoundaryLine(const std::string& line, bool& isFinal) const

Validates a boundary line, accepting --boundary (sets isFinal=false) and 
--boundary-- (sets isFinal=true). Any mismatch returns false. 
This precise recognition is critical to distinguish between normal part separators and the final terminator, 
which ends the multipart stream. A clean separation simplifies the main state machine: when a boundary line is confirmed, 
the machine can transition confidently into headers or finalize altogether. 
Incorrect detection would corrupt part framing or miss the end of the body.

*/

bool MultipartReader::startBoundaryLine(const std::string& line, bool& isFinal) const {
    // Valid: "--boundary" or "--boundary--"
    if (!starts_with(line, dashBoundary_))
        return false;
    if (line.size() == dashBoundary_.size()) { 
        isFinal = false; 
        return true; 
    }
    if (line.size() == endBoundary_.size() && line == endBoundary_) { 
        isFinal = true; 
        return true; 
    }
    return false;
}


/* 

std::size_t MultipartReader::feed(const char data, std::size_t n, IMultipartSink sink)**


The streaming engine. Appends incoming bytes to buf_, then advances a state machine:
S_PREAMBLE: skip until first boundary line.
S_HEADERS: read CRLF-terminated headers; lower-case keys; parse Content-Disposition; notify onPartBegin.
S_DATA: search for \r\n--boundary/\r\n--boundary--; stream chunked data to onPartData; finalize with onPartEnd.
Transition to S_DONE on final boundary.
It emits data conservatively to handle overlap across calls, returns bytes fed, 
and records errors on malformed input or sink failures. Works seamlessly with non-blocking I/O and large file uploads.


*/


std::size_t MultipartReader::feed(const char* data, std::size_t n, IMultipartSink* sink)
{
    if (!data || n==0 || !sink) 
        return 0;
    std::size_t fed = 0;
    buf_.append(data, n);
    fed = n;

    while (!buf_.empty() && st_ != S_DONE && st_ != S_ERROR) {
        if (st_ == S_PREAMBLE) {
            std::string line;
            if (!findLine(line)) 
                break; // need more
            bool finalB = false;
            if (!startBoundaryLine(line, finalB)) 
                continue; // skip preamble junk lines
            if (finalB) { 
                st_ = S_DONE; 
                break; 
            }
            st_ = S_HEADERS;
            cur_ = IMultipartSink::Part(); // reset
        }
        else if (st_ == S_HEADERS) {
            // read header lines until empty line
            std::string line;
            std::map<std::string,std::string> hdrs;
            for (;;) {
                if (!findLine(line)) 
                    return fed; // need more
                if (line.empty()) 
                    break; // end headers
                std::string::size_type c = line.find(':');
                if (c == std::string::npos) { 
                    setError("malformed part header"); 
                    return fed; }
                std::string k = trim(line.substr(0,c));
                std::string v = trim(line.substr(c+1));
                // lower-case keys (simple)
                for (std::size_t i = 0; i < k.size(); ++i) {
                    if (k[i] >= 'A' && k[i] <= 'Z') {
                        k[i] = char(k[i] - 'A' + 'a');
                    }
                }
                hdrs[k] = v;
            }
            cur_.headers = hdrs;

            std::map<std::string,std::string>::const_iterator it = hdrs.find("content-disposition");
            if (it == hdrs.end()) { 
                setError("no content-disposition"); 
                return fed; 
                }
            if (!parseContentDisposition(it->second, cur_.name, cur_.filename)) {
                setError("bad content-disposition");
                return fed;
            }
            if (!sink->onPartBegin(cur_)) { 
                setError("sink refused part"); 
                return fed; 
            }
            st_ = S_DATA;
        }
        else if (st_ == S_DATA) {
            // Search for "\r\n--boundary"
            // Keep at least (dashBoundary_.size()+4) bytes to safely search
            const std::string marker1 = std::string("\r\n") + dashBoundary_;
            const std::string marker2 = std::string("\r\n") + endBoundary_;

            // find earliest marker occurrence
            std::string::size_type m1 = buf_.find(marker1);
            std::string::size_type m2 = buf_.find(marker2);
            std::string::size_type m = std::string::npos;

            if (m1 != std::string::npos) 
				m = m1;
            if (m2 != std::string::npos && (m==std::string::npos || m2 < m))
                m = m2;
            if (m == std::string::npos) {
                // emit everything except the last (marker length) bytes, to allow overlap detection
                std::size_t safe = (buf_.size() > marker1.size()) ? (buf_.size() - marker1.size()) : 0;
                if (safe) {
                    if (!sink->onPartData(buf_.data(), safe)) { setError("sink write"); 
                        return fed; 
					}
                    buf_.erase(0, safe);
                }
                break; // need more data
            }
            // emit data up to marker
            if (m > 0) {
                if (!sink->onPartData(buf_.data(), m)) { 
                    setError("sink write"); 
                    return fed; 
                }
                buf_.erase(0, m);
            }
            // we are at "\r\n--boundary" or "\r\n--boundary--"
            // consume CRLF
            if (buf_.size() >= 2 && buf_[0]=='\r' && buf_[1]=='\n') 
                buf_.erase(0,2);

            // consume --boundary
            if (starts_with(buf_, dashBoundary_)) 
                buf_.erase(0, dashBoundary_.size());

            // final?
            bool isFinal = false;
            if (starts_with(buf_, "--")) { 
                isFinal = true; buf_.erase(0,2); 
            }

            // consume the trailing CRLF after boundary line (unless final end)
            if (!isFinal) {
                if (buf_.size() >= 2 && buf_[0]=='\r' && buf_[1]=='\n')
                    buf_.erase(0,2);
            }

            if (!sink->onPartEnd()) { 
                setError("sink close"); 
                return fed; 
            }
            if (isFinal) { 
                st_ = S_DONE; 
            }
            else { 
                st_ = S_HEADERS; cur_ = IMultipartSink::Part(); 
            }
        }
        else if (st_ == S_AFTER) { 
            st_ = S_DONE; 
        }
        else
            break; 
    }
    return fed;
}
