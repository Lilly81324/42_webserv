#ifndef UTIL_H
# define UTIL_H

# include <string>
# include <vector>
# include <unistd.h>   // getcwd, access
# include <limits.h>   // PATH_MAX
# include <sys/stat.h> // stat
# include <cstring>

class Util
{
	public:
		static bool realpath(const char *path, char *resolved);
};

#endif