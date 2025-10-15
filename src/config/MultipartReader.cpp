
#include "MultipartReader.h"
#include <cctype>



/* 

static bool starts_with(const std::string& s, const std::string& p)

This is a tiny, allocation-free prefix check used all over the 
parser to recognize multipart sentinels such as "--<boundary>" and "--<boundary>--". 
It first ensures s is at least as long as p, then compares the first p.size() characters. 
Keeping this utility local avoids noisy inline compare(0, n, ...) 
calls throughout the state machine and makes intent explicit: “are we looking at a boundary token?” 
Because multipart framing relies on exact byte sequences (not locale or Unicode), 
a simple byte-wise compare is both correct and fast. You call it when validating a candidate boundary 
line (startBoundaryLine), when consuming the boundary text after a CRLF (feed in S_DATA), 
and when detecting the final end marker ("--" suffix). Using a single helper also reduces 
copy-paste bugs—if you ever change how you treat comparisons, you change it once. In short, 
this is a microscopic but hot-path optimization and readability win that underpins correct 
delimiter recognition without any memory churn.


*/

static bool starts_with(const std::string &s, const std::string &p)
{
	return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}


/* 

MultipartReader::MultipartReader()

The constructor initializes the streaming parser to a predictable baseline: 
state = S_PREAMBLE. In real uploads, some clients may insert leading CRLFs or 
whitespace before the first boundary line. Starting in a preamble state lets the 
reader safely skip such noise until it sees a valid "--<boundary>" line, then 
switch to header parsing. The constructor performs no allocations and touches 
no external resources; it simply sets the finite-state machine (FSM) to a safe 
initial state so you can immediately call setBoundary/initFromContentType and then 
feed() repeatedly with incoming bytes from the socket. This cheap construction 
matters in a single-threaded reactor: you may create a MultipartReader per 
request as soon as headers are parsed, even under load, without stalling. 
It also encourages reuse—if you ever pool objects, resetting to S_PREAMBLE guarantees 
the next request won’t inherit any previous buffer content or error state. 
The design keeps configuration (boundary setup) separate from streaming 
(feeding bytes), which mirrors how the HTTP layer works: parse headers first, 
then stream the bod

*/

MultipartReader::MultipartReader() : st_(S_PREAMBLE) {}



/* 

bool MultipartReader::ieq(char a, char b)

ieq is a small helper for ASCII case-insensitive character 
comparison. It folds each input to lowercase if it’s A–Z by simple arithmetic, 
then compares the bytes. Multipart header field-names are defined as ASCII 
and case-insensitive; relying on locale-aware functions like std::tolower 
can be slower and introduce surprises in non-C locales. With ieq, the code 
path is branch-light and predictable. This routine supports places where you 
normalize or check header keys—e.g., while parsing Content-Disposition and other 
part headers—without allocating temporary lowercase strings or involving locales. 
While you also normalize full keys to lowercase in S_HEADERS, having a per-char 
comparator available keeps the code flexible if you ever need case-insensitive 
scans within tokens. In short, ieq is a minimalist, dependency-free primitive 
that makes header handling robust and fast for exactly the ASCII semantics 
multipart requires.

*/

bool MultipartReader::ieq(char a, char b)
{
	if (a >= 'A' && a <= 'Z')
		a = char(a - 'A' + 'a');
	if (b >= 'A' && b <= 'Z')
		b = char(b - 'A' + 'a');
	return a == b;
}

/* 

std::string MultipartReader::trim(const std::string& s)

trim removes ASCII spaces and tabs from both ends of a string. 
It’s used during header parsing to normalize tokens like " Content-Disposition : value " 
into clean "Content-Disposition" and "value", and within parameter lists such as name="field"; 
it prevents subtle mismatches and makes map keys/values canonical before storage. 
Implementation-wise it scans indices from left and right, then returns a substring—no 
per-character erasing or realloc storms. Normalizing early simplifies the rest of the parser: 
you can split on ':', trim, lowercase the key (where needed), and proceed deterministically. 
In multipart, whitespace is often optional around separators (;, =), 
so trim avoids edge-case bugs (e.g., keys with leading tabs) 
that would otherwise break lookups like hdrs["content-disposition"]. 
Keeping it a dedicated helper keeps the main FSM readable and focused on 
state transitions rather than string housekeeping.


*/

std::string MultipartReader::trim(const std::string &s)
{
	std::string::size_type a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
		--b;
	return s.substr(a, b - a);
}

/* 

bool MultipartReader::parseContentDisposition(const std::string& v, 
std::string& name, std::string& filename)
Parses a single Content-Disposition header value, 
extracting the form field name and optional filename. 
First it insists the type is "form-data" (after trimming), 
because other dispositions (e.g., attachment) are not part form 
fields in multipart/form-data. Then it walks the parameter list, 
splitting on ;, trimming each token, and parsing key=value pairs. 
Values may be quoted—"file.ext"—or unquoted; the code handles both. 
It specifically fills name and filename outputs if present. 
This function is crucial because it tells the sink what to do with the 
part: a missing or malformed name means you cannot route the field; a present 
filename indicates a file upload and influences storage decisions (e.g., 
write stream to disk vs. memory). Centralizing this parser avoids duplicating 
fragile string logic across the FSM and lets you enforce conservative correctness—if 
the header is malformed, you can fail early with a clear error 
(setError("bad content-disposition")). Overall, it’s the gatekeeper that 
translates raw header text into structured semantics for each part.


*/

bool MultipartReader::parseContentDisposition(const std::string &v,
											  std::string &name, std::string &filename)
{
	// Expect: form-data; name="field"; filename="file"
	name.clear();
	filename.clear();
	std::string s = v;
	// find first semicolon, skip type token (must be form-data)
	std::string::size_type semi = s.find(';');
	std::string type = trim(semi == std::string::npos ? s : s.substr(0, semi));
	if (type != "form-data")
		return false;

	std::string::size_type i = (semi == std::string::npos) ? s.size() : semi + 1;
	while (i < s.size())
	{
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ';'))
			++i;
		if (i >= s.size())
			break;

		std::string::size_type eq = s.find('=', i);
		if (eq == std::string::npos)
			break;
		std::string key = trim(s.substr(i, eq - i));

		std::string val;
		i = eq + 1;
		if (i < s.size() && s[i] == '"')
		{
			++i;
			while (i < s.size() && s[i] != '"')
			{
				val.push_back(s[i]);
				++i;
			}
			if (i < s.size() && s[i] == '"')
				++i;
		}
		else
		{
			while (i < s.size() && s[i] != ';')
			{
				val.push_back(s[i]);
				++i;
			}
		}
		if (key == "name")
			name = val;
		else if (key == "filename")
			filename = val;
		// advance to next ; if any
		while (i < s.size() && s[i] != ';')
			++i;
		if (i < s.size() && s[i] == ';')
			++i;
	}
	return true;
}


/* 

bool MultipartReader::initFromContentType(const std::string& ct)

This method validates the HTTP Content-Type header for an 
incoming request body and extracts the boundary parameter. 
It requires a prefix of "multipart/form-data" and then searches for 
boundary=. If a quoted boundary is provided (e.g., boundary="abc123"), 
it removes the quotes. If either the type or a non-empty boundary is missing, 
it returns false, allowing upper layers to reject the request with 415 or 400. 
On success it calls setBoundary to store the raw delimiter and precompute tokens. 
Keeping this logic separate from the streaming FSM lets your higher-level header 
processor decide which body reader to construct (multipart vs. fixed length vs. chunked) 
and only instantiate MultipartReader when the headers make sense. 
It also prevents desynchronization: without a boundary, the parser could accidentally 
treat content bytes as header lines and corrupt its state. In other words, 
initFromContentType is the first line of defense and configuration for safe multipart streaming.


*/

bool MultipartReader::initFromContentType(const std::string &ct)
{
	// minimal parse: "multipart/form-data; boundary=XXXX"
	if (!starts_with(ct, "multipart/form-data"))
		return false;
	std::string::size_type b = ct.find("boundary=");
	if (b == std::string::npos)
		return false;
	std::string val = ct.substr(b + 9);
	// strip optional quotes
	if (!val.empty() && val[0] == '"')
	{
		if (val.size() < 2 || val[val.size() - 1] != '"')
			return false;
		val = val.substr(1, val.size() - 2);
	}
	return setBoundary(val);
}



/* 

bool MultipartReader::setBoundary(const std::string& boundary)

Stores and validates the raw boundary string. It rejects empty boundaries and boundaries 
containing CR or LF to avoid ambiguous parsing/injection attacks. Then it precomputes 
two key strings: dashBoundary_ = "--" + boundary and endBoundary_ = dashBoundary_ + "--". 
These are the exact tokens that will appear on lines separating parts and terminating the body, 
respectively. It also resets the parser to S_PREAMBLE, clears the accumulation buffer buf_, 
and empties err_, preparing the object for a fresh stream. 
Precomputation matters: during streaming you will compare against these tokens frequently; 
having them ready as full strings avoids re-allocating them in the hot path. 
Consolidating boundary initialization in one function keeps the rest of the code 
free from ad-hoc validation and guarantees the FSM starts from a clean 
slate each time you (re)configure the reader. Returns true to indicate it’s safe to start feeding bytes.

*/

bool MultipartReader::setBoundary(const std::string &boundary)
{
	if (boundary.empty())
		return false;
	// Basic validation: boundary may not contain CR/LF
	for (std::size_t i = 0; i < boundary.size(); ++i)
		if (boundary[i] == '\r' || boundary[i] == '\n')
			return false;
	boundary_ = boundary;
	dashBoundary_ = std::string("--") + boundary_;
	endBoundary_ = dashBoundary_ + "--";
	st_ = S_PREAMBLE;
	buf_.clear();
	err_.clear();
	return true;
}


/* 

bool MultipartReader::findLine(std::string& out)

Incrementally scans the internal buffer buf_ for a CRLF terminator ("\r\n"). 
If found, it copies the line without CRLF into out and erases the consumed prefix 
(including the CRLF) from buf_, then returns true. If not found, 
it returns false so the caller can append more bytes via feed. 
This is the core building block for line-oriented stages of the FSM: 
skipping the preamble until a boundary line, collecting part headers up to an empty 
line, and consuming the boundary line itself in the S_DATA transition. 
Because network reads can split lines across packets, 
findLine must tolerate partial lines; by keeping data in buf_ until a full 
CRLF arrives, it remains safe and non-blocking. Centralizing the 
CRLF extraction also reduces subtle off-by-one mistakes and keeps the 
main state transitions readable. In short, it’s a minimal, 
robust streaming line reader tuned for HTTP multipart needs.

*/

bool MultipartReader::findLine(std::string &out)
{
	// Look for CRLF; keep one line in out (without CRLF)
	for (std::size_t i = 0; i + 1 < buf_.size(); ++i)
	{
		if (buf_[i] == '\r' && buf_[i + 1] == '\n')
		{
			out.assign(buf_, 0, i);
			buf_.erase(0, i + 2);
			return true;
		}
	}
	return false;
}

/* 

bool MultipartReader::startBoundaryLine(const std::string& line, bool& isFinal) const

Validates that a candidate line is either a regular boundary ("--<boundary>") or the 
final boundary ("--<boundary>--"). It first checks the fixed prefix using dashBoundary_; 
if it matches exactly, it marks isFinal=false. If the line equals endBoundary_, 
it marks isFinal=true. Any other length/extra characters produce false. This strictness is important: 
the parser must know precisely whether to transition into S_HEADERS for the next part or finish in S_DONE. 
Accepting near-matches could merge adjacent parts or truncate a file field. 
The function doesn’t strip CRLF—that’s already handled by findLine. 
By isolating this logic, your S_PREAMBLE code can cleanly “skip junk lines until a valid 
boundary appears,” and your S_DATA code can confidently consume the marker 
and then either stop (final) or parse the next part’s headers. 
It’s a small gate that preserves the integrity of the entire stream

*/

bool MultipartReader::startBoundaryLine(const std::string &line, bool &isFinal) const
{
	// Valid: "--boundary" or "--boundary--"
	if (!starts_with(line, dashBoundary_))
		return false;
	if (line.size() == dashBoundary_.size())
	{
		isFinal = false;
		return true;
	}
	if (line.size() == endBoundary_.size() && line == endBoundary_)
	{
		isFinal = true;
		return true;
	}
	return false;
}


/* 

std::size_t MultipartReader::feed(const char* data, std::size_t n, IMultipartSink* sink)
This is the streaming engine. It appends incoming bytes to buf_, 
reports fed = n, and then drives the FSM until it needs more data, reaches S_DONE, or hits S_ERROR.
S_PREAMBLE: Repeatedly call findLine. Non-matching lines are ignored; a valid boundary 
line switches to S_HEADERS. If the final boundary appears immediately, the stream ends (S_DONE).
S_HEADERS: Collect header lines until an empty line. Keys are lower-cased; 
a content-disposition header is required. It is parsed via parseContentDisposition to 
extract name/filename. Then sink->onPartBegin(cur_) is invoked; if the sink refuses, 
parsing stops with an error. Transition to S_DATA.
S_DATA: Search for the earliest occurrence of "\r\n--<boundary>" or "\r\n--<boundary>--". 
If none is present yet, emit a safe prefix (buf_.size() - markerLen) to sink->onPartData 
and keep the overlap to catch boundary splits. If a marker is found, emit data up to the marker, 
consume CRLF and the boundary token, detect final via trailing "--", 
optionally consume trailing CRLF, call sink->onPartEnd(), 
and then either S_DONE (final) or S_HEADERS for the next part.
Errors call setError(...) and keep fed as the number of bytes 
accepted into the buffer, allowing the caller to advance network buffers consistently. 
The function never blocks or inspects errno; it’s designed for non-blocking use right after POLLIN. 
The IMultipartSink abstraction lets you route data to memory or 
files without entangling I/O policy with parsing.

*/

std::size_t MultipartReader::feed(const char *data, std::size_t n, IMultipartSink *sink)
{
	if (!data || n == 0 || !sink)
		return 0;
	std::size_t fed = 0;
	buf_.append(data, n);
	fed = n;

	while (!buf_.empty() && st_ != S_DONE && st_ != S_ERROR)
	{
		if (st_ == S_PREAMBLE)
		{
			std::string line;
			if (!findLine(line))
				break; // need more
			bool finalB = false;
			if (!startBoundaryLine(line, finalB))
				continue; // skip preamble junk lines
			if (finalB)
			{
				st_ = S_DONE;
				break;
			}
			st_ = S_HEADERS;
			cur_ = IMultipartSink::Part(); // reset
		}
		else if (st_ == S_HEADERS)
		{
			// read header lines until empty line
			std::string line;
			std::map<std::string, std::string> hdrs;
			for (;;)
			{
				if (!findLine(line))
					return fed; // need more
				if (line.empty())
					break; // end headers
				std::string::size_type c = line.find(':');
				if (c == std::string::npos)
				{
					setError("malformed part header");
					return fed;
				}
				std::string k = trim(line.substr(0, c));
				std::string v = trim(line.substr(c + 1));
				// lower-case keys (simple)
				for (std::size_t i = 0; i < k.size(); ++i)
					if (k[i] >= 'A' && k[i] <= 'Z')
						k[i] = char(k[i] - 'A' + 'a');
				hdrs[k] = v;
			}
			cur_.headers = hdrs;

			std::map<std::string, std::string>::const_iterator it = hdrs.find("content-disposition");
			if (it == hdrs.end())
			{
				setError("no content-disposition");
				return fed;
			}
			if (!parseContentDisposition(it->second, cur_.name, cur_.filename))
			{
				setError("bad content-disposition");
				return fed;
			}
			if (!sink->onPartBegin(cur_))
			{
				setError("sink refused part");
				return fed;
			}
			st_ = S_DATA;
		}
		else if (st_ == S_DATA)
		{
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
			if (m2 != std::string::npos && (m == std::string::npos || m2 < m))
				m = m2;

			if (m == std::string::npos)
			{
				// emit everything except the last (marker length) bytes, to allow overlap detection
				std::size_t safe = (buf_.size() > marker1.size()) ? (buf_.size() - marker1.size()) : 0;
				if (safe)
				{
					if (!sink->onPartData(buf_.data(), safe))
					{
						setError("sink write");
						return fed;
					}
					buf_.erase(0, safe);
				}
				break; // need more data
			}

			// emit data up to marker
			if (m > 0)
			{
				if (!sink->onPartData(buf_.data(), m))
				{
					setError("sink write");
					return fed;
				}
				buf_.erase(0, m);
			}

			// we are at "\r\n--boundary" or "\r\n--boundary--"
			// consume CRLF
			if (buf_.size() >= 2 && buf_[0] == '\r' && buf_[1] == '\n')
				buf_.erase(0, 2);

			// consume --boundary
			if (starts_with(buf_, dashBoundary_))
				buf_.erase(0, dashBoundary_.size());

			// final?
			bool isFinal = false;
			if (starts_with(buf_, "--"))
			{
				isFinal = true;
				buf_.erase(0, 2);
			}

			// consume the trailing CRLF after boundary line (unless final end)
			if (!isFinal)
			{
				if (buf_.size() >= 2 && buf_[0] == '\r' && buf_[1] == '\n')
					buf_.erase(0, 2);
			}

			if (!sink->onPartEnd())
			{
				setError("sink close");
				return fed;
			}
			if (isFinal)
			{
				st_ = S_DONE;
			}
			else
			{
				st_ = S_HEADERS;
				cur_ = IMultipartSink::Part();
			}
		}
		else if (st_ == S_AFTER)
		{
			st_ = S_DONE;
		}
		else
		{
			break;
		}
	}
	return fed;
}
