/* --- ServerConfig.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "ServerConfig.h"
#include <fstream>
#include <cctype>
#include <sstream>
#include <fstream>

// ---------------- Lexer (yours, kept) ----------------
std::vector<Token> Lexer::lex(const std::string& src){
    std::vector<Token> out;
    size_t i = 0, n = src.size();
    size_t line = 1, col = 1;

    while(i < n){
        char c = src[i];

        // Comments
        if(c == '#'){
            while (i < n && src[i] != '\n'){ ++i; ++col; }
            continue;
        }
        // Whitespace
        if (c == ' ' || c == '\t' || c == '\r'){ ++i; ++col; continue; }
        if (c == '\n') { ++i; ++line; col = 1; continue; }

        // Single-char symbols
        if(c == '{' || c == '}' || c == ';'){
            Token t; t.text = std::string(1, c); t.line = line; t.col = col;
            out.push_back(t);
            ++i; ++col;
            continue;
        }

        // Word token
        size_t start = i, startCol = col;
        while(i < n){
            char ch = src[i];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'
             || ch == '{' || ch == '}' || ch == ';' || ch == '#')
                break;
            ++i; ++col;
        }
        if (i > start) {
            Token t; t.text = src.substr(start, i - start); t.line = line; t.col = startCol;
            out.push_back(t);
        }
    }
    return out;
}

static std::string readWholeFile(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string upper(const std::string& s) {
    std::string r(s);
    for (size_t i=0;i<r.size();++i) r[i] = static_cast<char>(std::toupper(r[i]));
    return r;
}

static size_t parseSizeBytes(const std::string& s) {
    if (s.empty()) throw std::runtime_error("empty size");
    std::string num; char suf = 0;
    for (size_t i=0;i<s.size();++i) {
        if (std::isdigit(s[i])) num += s[i];
        else { suf = static_cast<char>(std::tolower(s[i])); if (i != s.size()-1) 
            throw std::runtime_error("invalid size '"+s+"'"); 
        }
    }
    if (num.empty()) throw std::runtime_error("invalid size '"+s+"'");
    std::istringstream is(num);
    unsigned long base = 0; is >> base;
    if (!is)
        throw std::runtime_error("invalid number '"+s+"'");
    if (suf=='k') base *= 1024ul;
    else if (suf=='m') 
        base *= 1024ul*1024ul;
    else if (suf=='b' || suf==0) 
        { /* bytes */ }
    else
        throw std::runtime_error("invalid size suffix in '"+s+"'");
    return static_cast<size_t>(base);
}

static Listen parseListenOne(const std::string& s) {
    Listen L;
    L.host = "";
    L.port = 0;
    std::string host, portstr;
    size_t pos = s.find(':');
    if (pos == std::string::npos) {
        portstr = s; 
    }
    else{
        host = s.substr(0,pos); portstr = s.substr(pos+1);
    }
    if (portstr.empty())
        throw std::runtime_error("missing port in listen '"+s+"'");
    std::istringstream is(portstr);
    unsigned long p=0; is >> p;
    if (!is || p < 1 || p > 65535)
        throw std::runtime_error("invalid port in listen '"+s+"'");
    L.host = host;
    L.port = static_cast<unsigned short>(p);
    return L;
}

static std::runtime_error parseErr(const Token& t, const std::string& msg) {
    std::ostringstream os; os << msg << " at line " << t.line << ", col " << t.col;
    return std::runtime_error(os.str());
}

// ---------------- Parser ----------------
namespace {

class Parser {
public:
    Parser(const std::vector<Token>& toks, std::vector<ServerBlock>& out)
    : t(toks), i(0), outServers(out) {}

    void parse() {
        outServers.clear();
        while (!atEnd()) outServers.push_back(parseServerBlock());
        if (outServers.empty())
            throw std::runtime_error("no server blocks found");
        for (size_t s=0;s<outServers.size();++s)
            validateServer(outServers[s]);
    }

private:
    const std::vector<Token>& t;
    size_t i;
    std::vector<ServerBlock>& outServers;

    bool atEnd() const {
            return i >= t.size();
        }
    const Token& cur() const {
        if (atEnd()) return t[t.size()-1];
        return t[i]; 
    }
    bool is(const char* s) const {
        return !atEnd() && t[i].text == s;
    }

    bool match(const char* s) {
        if (is(s)) { 
            ++i;
            return true; 
        }
        return false;
    }
    const Token& expect(const char* s, const char* what) {
        if (!match(s))
            throw parseErr(cur(), std::string("expected '")+s+"' for "+what);
        return t[i-1];
    }
    const Token& takeWord(const char* what) {
        if (atEnd())
            throw std::runtime_error(std::string("unexpected EOF while reading ")+what);
        const Token& k = t[i];
        if (k.text=="{" || k.text=="}" || k.text==";")
            throw parseErr(k, std::string("unexpected '")+k.text+"' while reading "+what);
        ++i;
        return k;
    }

    ServerBlock parseServerBlock() {
        const Token& k = cur();
        if (k.text != "server")
            throw parseErr(k, "expected 'server'");
        ++i;
        expect("{","server block");
        ServerBlock sb;

        while (!atEnd() && !is("}")) {
            if (is("location")) {
                sb.locations.push_back(parseLocationBlock());
            } else {
                parseServerDirective(sb);
            }
        }
        expect("}","end of server block");
        return sb;
    }

    void parseServerDirective(ServerBlock& sb) {
        const Token& k = cur();
        if (k.text == "listen") {
            ++i;
            const Token& v = takeWord("listen value");
            Listen L = parseListenOne(v.text);
            std::ostringstream hp; hp << (L.host) << ":" << L.port;
            for (size_t j=0;j<sb.listens.size();++j) {
                std::ostringstream hp2; hp2 << sb.listens[j].host << ":" << sb.listens[j].port;
                if (hp.str() == hp2.str())
                    throw parseErr(v, "duplicate listen '"+hp.str()+"'");
            }
            sb.listens.push_back(L);
            expect(";","listen");
        } else if (k.text == "root") {
            ++i;
            const Token& p = takeWord("root path");
            sb.root = p.text;
            expect(";","root");
        } else if (k.text == "index") {
            ++i;
            while (!is(";")) {
                const Token& f = takeWord("index filename");
                sb.index.push_back(f.text);
            }
            expect(";","index");
        } else if (k.text == "error_page") {
            ++i;
            const Token& codeTok = takeWord("error code");
            std::istringstream is(codeTok.text);
            int code = 0; is >> code;
            if (!is || code < 100 || code > 599)
                throw parseErr(codeTok, "invalid error code");
            const Token& pathTok = takeWord("error page path");
            sb.error_pages[code] = pathTok.text;
            expect(";","error_page");
        } else if (k.text == "client_max_body_size") {
            ++i;
            const Token& sizeTok = takeWord("client_max_body_size");
            try {
                sb.client_max_body_size = parseSizeBytes(sizeTok.text);
            }
            catch (const std::exception& e) {
                throw parseErr(sizeTok, e.what()); 
            }
            expect(";","client_max_body_size");
        } else {
            throw parseErr(k, std::string("unknown server directive '")+k.text+"'");
        }
    }

    Location parseLocationBlock() {
        const Token& kw = expect("location","location");
        (void)kw;
        const Token& pathTok = takeWord("location path");
        expect("{","location block");
        Location loc;
        loc.path = pathTok.text;

        while (!atEnd() && !is("}")) {
            parseLocationDirective(loc);
        }
        expect("}","end of location block");
        if (loc.methods.empty()) {
            loc.methods.insert("GET"); loc.methods.insert("POST"); loc.methods.insert("DELETE");
        }
        return loc;
    }

    void parseLocationDirective(Location& loc) {
        const Token& k = cur();
        if (k.text == "methods") {
            ++i;
            while (!is(";")) {
                const Token& m = takeWord("HTTP method");
                std::string M = upper(m.text);
                if (M!="GET" && M!="POST" && M!="DELETE")
                    throw parseErr(m, "unsupported method '"+m.text+"'");
                loc.methods.insert(M);
            }
            expect(";","methods");
        } else if (k.text == "root") {
            ++i; const Token& p = takeWord("root path");
            loc.root = p.text; expect(";","root");
        } else if (k.text == "index") {
            ++i; while (!is(";")) {
                const Token& f = takeWord("index filename");
                loc.index.push_back(f.text);
            }
            expect(";","index");
        } else if (k.text == "autoindex") {
            ++i; const Token& v = takeWord("autoindex value");
            std::string val = upper(v.text);
            if (val=="ON")
                loc.autoindex = true;
            else if (val=="OFF")
                loc.autoindex = false;
            else
                throw parseErr(v, "autoindex expects 'on' or 'off'");
            expect(";","autoindex");
        } else if (k.text == "upload_store") {
            ++i; const Token& p = takeWord("upload_store path");
            loc.upload_store = p.text; expect(";","upload_store");
        } else if (k.text == "cgi") {
            ++i;
            const Token& ext = takeWord("cgi extension");
            const Token& interp = takeWord("cgi interpreter");
            if (ext.text.empty() || ext.text[0] != '.')
                throw parseErr(ext, "cgi extension must start with '.'");
            loc.cgi[ext.text] = interp.text;
            expect(";","cgi");
        } else if (k.text == "return") {
            ++i;
            const Token& codeTok = takeWord("return code");
            std::istringstream is(codeTok.text);
            int code=0; is >> code;
            if (!is || code < 300 || code > 399)
                throw parseErr(codeTok, "return expects 3xx code");
            const Token& tgt = takeWord("return target");
            loc.redirect_code = code;
            loc.redirect_target = tgt.text;
            expect(";","return");
        } else {
            throw parseErr(k, std::string("unknown location directive '")+k.text+"'");
        }
    }

    static void validateServer(ServerBlock& sb) {
        if (sb.listens.empty())
            throw std::runtime_error("server missing 'listen' directive");
       
        if (sb.client_max_body_size == 0)
            sb.client_max_body_size = 1024ul*1024ul;
    }
};

}

// ---------------- ServerConfig API ----------------


ServerConfig::ServerConfig(){}
ServerConfig::~ServerConfig(){}

bool ServerConfig::canOpen(const char *path) const{
    std::ifstream f(path);
    return f.good();
}

void ServerConfig::parseFile(const std::string& path) {
    std::string src = readWholeFile(path);
    Lexer lx;
    std::vector<Token> toks = lx.lex(src);
    Parser p(toks, _servers);
    p.parse();
}

const std::vector<ServerBlock>& ServerConfig::servers() const {
    return _servers;
}