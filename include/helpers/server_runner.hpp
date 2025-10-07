// include/helpers/server_runner.hpp
#pragma once

#include <catch2/catch_all.hpp>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

// --- Small utilities ---------------------------------------------------------

static inline void sr_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    ::nanosleep(&ts, 0);
}

static inline bool sr_is_regular_file(const char* p) {
    if (!p || !*p) return false;
    struct stat st;
    return (::stat(p, &st) == 0 && S_ISREG(st.st_mode));
}

static inline std::string sr_realpath_or(const char* p) {
    if (!p) return std::string();
    char buf[PATH_MAX];
    if (::realpath(p, buf)) return std::string(buf);
    if (sr_is_regular_file(p)) return std::string(p);
    return std::string();
}

static inline int sr_pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa; std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0; // ephemeral
    REQUIRE(::bind(fd, (sockaddr*)&sa, sizeof(sa)) == 0);
    socklen_t sl = sizeof(sa);
    REQUIRE(::getsockname(fd, (sockaddr*)&sa, &sl) == 0);
    int port = ntohs(sa.sin_port);
    ::close(fd);
    return port;
}

static inline bool sr_try_connect_127_0_0_1(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in sa; std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    bool ok = (::connect(fd, (sockaddr*)&sa, sizeof(sa)) == 0);
    ::close(fd);
    return ok;
}

static inline std::string sr_find_file(const std::vector<std::string>& cands) {
    for (size_t i = 0; i < cands.size(); ++i) {
        if (sr_is_regular_file(cands[i].c_str())) {
            std::string abs = sr_realpath_or(cands[i].c_str());
            if (!abs.empty()) return abs;
            return cands[i];
        }
    }
    return std::string();
}

// Replace ports in lines like:  listen 127.0.0.1:8080;
// If no such line is found, appends one at the end.
static inline std::string sr_rewrite_listen_port(const std::string& cfg, int new_port) {
    std::istringstream in(cfg);
    std::ostringstream out;
    std::string line;
    bool found_listen = false;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        size_t p = 0;
        while (p < trimmed.size() && (trimmed[p] == ' ' || trimmed[p] == '\t'))
            ++p;

        if (p < trimmed.size() &&
            trimmed.compare(p, 6, "listen") == 0 &&
            trimmed.find("127.0.0.1") != std::string::npos)
        {
            std::string indent = line.substr(0, p);
            std::ostringstream ls;
            ls << indent << "listen 127.0.0.1:" << new_port << ";";
            out << ls.str() << "\n";
            found_listen = true;
            continue;
        }
        out << line << "\n";
    }

    if (!found_listen) {
        out << "\n# injected by tests\nlisten 127.0.0.1:" << new_port << ";\n";
    }
    return out.str();
}

static inline std::string sr_write_temp_file(const std::string& contents, const char* /*suffix*/ = ".conf") {
    char tmpl[PATH_MAX];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/webserv_test_XXXXXX");
    int fd = ::mkstemp(tmpl);
    REQUIRE(fd >= 0);
    ssize_t wr = ::write(fd, contents.data(), contents.size());
    REQUIRE(wr == (ssize_t)contents.size());
    ::close(fd);
    return std::string(tmpl);
}

// --- ServerRunner ------------------------------------------------------------

struct ServerRunner {
    pid_t pid;
    int port;
    std::string bin_path;
    std::string base_conf_path;
    std::string temp_conf_path;

    ServerRunner(const std::string& conf_hint, const std::string& bin_hint)
    : pid(-1), port(0)
    {
        // Resolve binary path
        {
            const char* env_bin = std::getenv("WEBSERV_BIN");
            if (env_bin && sr_is_regular_file(env_bin)) {
                bin_path = sr_realpath_or(env_bin);
            } else if (!bin_hint.empty() && sr_is_regular_file(bin_hint.c_str())) {
                bin_path = sr_realpath_or(bin_hint.c_str());
            } else {
                std::vector<std::string> cands;
                cands.push_back("./webserv");
                cands.push_back("webserv");
                cands.push_back("build/webserv");
                cands.push_back("../webserv");
                bin_path = sr_find_file(cands);
            }
            if (bin_path.empty()) {
                INFO("[ServerRunner] Could not locate webserv binary. Set WEBSERV_BIN or place ./webserv in repo root.");
            }
            REQUIRE(!bin_path.empty());
        }

        // Resolve base config path
        {
            const char* env_conf = std::getenv("WEBSERV_CONF");
            if (env_conf && sr_is_regular_file(env_conf)) {
                base_conf_path = sr_realpath_or(env_conf);
            } else if (!conf_hint.empty() && sr_is_regular_file(conf_hint.c_str())) {
                base_conf_path = sr_realpath_or(conf_hint.c_str());
            } else {
                std::vector<std::string> cands;
                cands.push_back("extended.conf");
                cands.push_back("./extended.conf");
                cands.push_back("tests/extended.conf");
                cands.push_back("./tests/extended.conf");
                cands.push_back("config/extended.conf");
                cands.push_back("./config/extended.conf");
                base_conf_path = sr_find_file(cands);
            }
            if (base_conf_path.empty()) {
                INFO("[ServerRunner] Could not locate extended.conf. Set WEBSERV_CONF or place extended.conf in repo root.");
            }
            REQUIRE(!base_conf_path.empty());
        }

        // Read base config
        std::ifstream in(base_conf_path.c_str());
        REQUIRE(in.good());
        std::ostringstream buf; buf << in.rdbuf();
        std::string cfg = buf.str();

        // Choose a free port and rewrite listen lines
        port = sr_pick_free_port();
        std::string cfg_rewritten = sr_rewrite_listen_port(cfg, port);

        // Write temp config
        temp_conf_path = sr_write_temp_file(cfg_rewritten, ".conf");

        // Spawn server: webserv <temp_conf_path>
        pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::setsid();
            execl(bin_path.c_str(), bin_path.c_str(), temp_conf_path.c_str(), (char*)0);
            _exit(127);
        }

        // Wait until port opens (up to ~5s) or child dies
        bool up = false;
        for (int i = 0; i < 250; ++i) {
            int status = 0;
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                std::ostringstream os;
                os << "[ServerRunner] webserv exited early. status=" << status;
                if (WIFEXITED(status)) os << " (exit=" << WEXITSTATUS(status) << ")";
                if (WIFSIGNALED(status)) os << " (signal=" << WTERMSIG(status) << ")";
                FAIL(os.str());
            }
            if (sr_try_connect_127_0_0_1((uint16_t)port)) { up = true; break; }
            sr_sleep_ms(20);
        }
        if (!up) {
            std::ostringstream os;
            os << "[ServerRunner] Server did not open 127.0.0.1:" << port << " in time";
            INFO(os.str());
        }
        REQUIRE(up);
    }

    ~ServerRunner() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            bool gone = false;
            for (int i = 0; i < 40; ++i) {
                int status = 0;
                pid_t w = ::waitpid(pid, &status, WNOHANG);
                if (w == pid) { gone = true; break; }
                sr_sleep_ms(50);
            }
            if (!gone) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, 0, 0);
            }
            pid = -1;
        }
        if (!temp_conf_path.empty()) {
            ::unlink(temp_conf_path.c_str());
        }
    }

    ServerRunner(const ServerRunner&) = delete;
    ServerRunner& operator=(const ServerRunner&) = delete;
};
