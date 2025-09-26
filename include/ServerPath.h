// ServerPath.h (or a small util header)
#ifndef SERVER_PATH_H
#define SERVER_PATH_H

#include <string>
#include <vector>

inline static std::string joinPaths(const std::string& base, const std::string& rel)
{
    if (!rel.empty() && rel[0] == '/')
        return rel; // already absolute (treat as absolute request path)
    if (base.empty())
        return rel;

    const bool baseEndsWithSlash = !base.empty() && base[base.size()-1] == '/';
    const bool relStartsWithSlash = !rel.empty() && rel[0] == '/';

    if (baseEndsWithSlash && relStartsWithSlash)
        return base + rel.substr(1);
    if (baseEndsWithSlash || relStartsWithSlash)
        return base + rel;
    return base + "/" + rel;
}

// Normalize a POSIX-like path. If absolute==true the result will start with '/'
// If absolute==false, it will be a relative path (no leading '/').
inline static std::string normalizePath(const std::string& path)
{
    std::vector<std::string> stack;
    bool absolute = !path.empty() && path[0] == '/';

    // Iterate segments between '/'
    std::string seg;
    for (std::string::size_type i = 0; i <= path.size(); ++i) {
        char c = (i < path.size() ? path[i] : '/'); // force flush on end
        if (c == '/') {
            if (!seg.empty()) {
                if (seg == ".") {
                    // skip
                } else if (seg == "..") {
                    if (!stack.empty()) {
                        stack.pop_back();
                    } else {
                        // For absolute paths, ignore extra ".." at root.
                        // For relative, keep a leading ".." (optional).
                        if (!absolute) stack.push_back("..");
                    }
                } else {
                    stack.push_back(seg);
                }
                seg.clear();
            }
            // else: collapse multiple '/'
        } else {
            seg.push_back(c);
        }
    }

    // Rebuild
    std::string out;
    if (absolute) out = "/";
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i > 0 || absolute) out += (i == 0 && absolute ? "" : "/");
        out += stack[i];
    }
    // Special case: absolute root or empty relative
    if (absolute && stack.empty())
        out = "/";
    if (!absolute && out.empty())
        out = "."; // or "" if you prefer

    return out;
}

// Join root + user path, normalize, and ensure result stays under root.
// Returns empty string if it escapes the root.
inline static std::string resolveUnderRoot(const std::string& root, const std::string& userPath)
{
    // 1) Build a candidate path
    std::string joined = joinPaths(root, userPath);

    // 2) Normalize *both* root and joined
    std::string normRoot   = normalizePath(root);
    std::string normJoined = normalizePath(joined);

    // Ensure normRoot ends with a single '/'
    if (normRoot.empty()) normRoot = "/";
    if (normRoot[normRoot.size()-1] != '/')
        normRoot += '/';

    // 3) Check prefix (strict directory containment)
    // Allow exactly the root itself or a child path under it.
    if (normJoined == normRoot.substr(0, normRoot.size()-1)) // exact root (without trailing '/')
        return normRoot.substr(0, normRoot.size()-1);
    if (normJoined.size() >= normRoot.size() &&
        normJoined.compare(0, normRoot.size(), normRoot) == 0)
    {
        return normJoined;
    }

    // Escaped outside root → reject
    return std::string();
}

#endif // SERVER_PATH_H
