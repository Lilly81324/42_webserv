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
		if (Util::getcwd(cwd, sizeof(cwd)) == NULL)
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

/*
* pseudo_getcwd: replacement for getcwd using only allowed syscalls.
* - buf: caller buffer (must be non-NULL)
* - size: size of caller buffer (e.g. PATH_MAX)
* Returns buf on success, NULL on failure (errno set).
*
* Notes:
* - Produces a physical absolute path (resolves .. and . by walking the
*   filesystem), does NOT attempt to preserve logical symlink components.
* - Uses stat/chdir/opendir/readdir/closedir which are allowed in your list.
*/
char *Util::getcwd(char *buf, size_t size)
{
	if (!buf || size == 0)
	{
		errno = EINVAL;
		return NULL;
	}

	std::vector<std::string> parts;
	struct stat cur_stat;

	// Get stat of current directory
	if (stat(".", &cur_stat) != 0)
		return NULL;

	while (1)
	{
		struct stat parent_stat;
		if (stat("..", &parent_stat) != 0)
			return NULL;

		// If current == parent, we are at filesystem root
		if (cur_stat.st_dev == parent_stat.st_dev && cur_stat.st_ino == parent_stat.st_ino)
		{
			break;
		}

		// Move up to parent
		if (chdir("..") != 0)
			return NULL;

		// Now we are in parent; scan entries to find name matching previous directory
		DIR *d = opendir(".");
		if (!d)
			return NULL;

		struct dirent *ent;
		bool found = false;
		while ((ent = readdir(d)) != NULL)
		{
			// skip . and ..
			if (ent->d_name[0] == '.'
				&& (ent->d_name[1] == '\0'
					|| (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
				continue;

			struct stat ent_st;
			if (stat(ent->d_name, &ent_st) != 0)
				continue; // ignore entries we can't stat

			if (ent_st.st_ino == cur_stat.st_ino && ent_st.st_dev == cur_stat.st_dev)
			{
				parts.push_back(std::string(ent->d_name));
				found = true;
				break;
			}
		}
		closedir(d);

		if (!found)
		{
			// Unexpected: cannot find the directory entry that matches previous inode
			errno = ENOENT;
			return NULL;
		}

		// Update cur_stat to parent's stat for next iteration
		if (stat(".", &cur_stat) != 0)
			return NULL;
	}

	// We are now at root. Build path string.
	std::string result;
	if (parts.empty())
	{
		// we're in "/"
		result = "/";
	}
	else
	{
		// join reversed parts
		for (std::vector<std::string>::reverse_iterator it = parts.rbegin(); it != parts.rend(); ++it)
		{
			result += '/';
			result += *it;
		}
	}

	// Ensure buffer is big enough (including terminating NUL)
	if (result.size() + 1 > size)
	{
		errno = ERANGE;
		return NULL;
	}

	// Copy into caller buffer
	std::memcpy(buf, result.c_str(), result.size() + 1);

	// Try to restore original cwd by chdir-ing back to the constructed path.
	// If this fails, we still return success because we have the path; but
	// to be conservative we attempt restoration and return error if it fails.
	if (chdir(buf) != 0)
	{
		// If we can't restore, that's bad for caller expectations;
		// set errno but return NULL (caller still has no cwd restored).
		return NULL;
	}

	return buf;
}
