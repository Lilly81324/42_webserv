#include "Util.h"

// Simplified realpath replacement
// - Resolves "." and ".."
// - Converts relative to absolute
// - Checks existence with stat()
// - Does NOT resolve symbolic links
bool Util::realpath(const char *path, char *resolved)
{
    if (!path || !resolved)
        return false;

    std::string abs;
    if (path[0] == '/')
    {
        // Already absolute
        abs = path;
    }
    else
    {
        // Convert relative path to absolute
        char cwd[PATH_MAX];
        if (::getcwd(cwd, sizeof(cwd)) == NULL)
            return false;
        abs = std::string(cwd) + "/" + path;
    }

    // Split into components
    std::vector<std::string> parts;
    std::string token;
    for (size_t i = 0; i <= abs.size(); ++i)
    {
        if (i == abs.size() || abs[i] == '/')
        {
            if (!token.empty())
            {
                if (token == ".")
                {
                    // ignore
                }
                else if (token == "..")
                {
                    if (!parts.empty())
                        parts.pop_back();
                }
                else
                {
                    parts.push_back(token);
                }
                token.clear();
            }
        }
        else
        {
            token.push_back(abs[i]);
        }
    }

    // Rebuild canonical path
    std::string result = "/";
    for (size_t i = 0; i < parts.size(); ++i)
    {
        result += parts[i];
        if (i + 1 < parts.size())
            result += "/";
    }

    // Verify existence
    struct stat st;
    if (::stat(result.c_str(), &st) != 0)
    {
        return false; // path doesn't exist
    }

    // Copy into caller buffer
    if (result.size() >= PATH_MAX)
        return false; // too long
    std::strcpy(resolved, result.c_str());
    return true;
}