#ifndef UTIL_H
# define UTIL_H

# include <string>
# include <vector>
# include <unistd.h>
# include <limits.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <dirent.h>
# include <cstring>
# include <errno.h>

class Util
{
	public:
		static bool realpath(const char *path, char *resolved);
		static char *getcwd(char *buf, size_t size);
};

#endif