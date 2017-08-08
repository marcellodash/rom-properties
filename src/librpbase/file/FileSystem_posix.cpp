/***************************************************************************
 * ROM Properties Page shell extension. (librpbase)                        *
 * FileSystem_posix.cpp: File system functions. (POSIX implementation)     *
 *                                                                         *
 * Copyright (c) 2016-2017 by David Korth.                                 *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.           *
 ***************************************************************************/

#include "FileSystem.hpp"

// libunixcommon
#include "libunixcommon/userdirs.hpp"
// libromdata
#include "TextFuncs.hpp"

// One-time initialization.
#include "threads/pthread_once.h"

// C includes.
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>

// C includes. (C++ namespace)
#include <cstring>
#include <ctime>

// C++ includes.
#include <string>
using std::string;
using std::u16string;

namespace LibRpBase { namespace FileSystem {

// pthread_once() control variable.
static pthread_once_t once_control = PTHREAD_ONCE_INIT;

// Configuration directories.

// User's cache directory.
static rp_string cache_dir;
// User's configuration directory.
static rp_string config_dir;

/**
 * Recursively mkdir() subdirectories.
 *
 * The last element in the path will be ignored, so if
 * the entire pathname is a directory, a trailing slash
 * must be included.
 *
 * NOTE: Only native separators ('\\' on Windows, '/' on everything else)
 * are supported by this function.
 *
 * @param path Path to recursively mkdir. (last component is ignored)
 * @return 0 on success; negative POSIX error code on error.
 */
int rmkdir(const rp_string &path)
{
	// Linux (and most other systems) use UTF-8 natively.
	string path8 = rp_string_to_utf8(path);

	// Find all slashes and ensure the directory component exists.
	size_t slash_pos = path8.find(DIR_SEP_CHR, 0);
	if (slash_pos == 0) {
		// Root is always present.
		slash_pos = path8.find(DIR_SEP_CHR, 1);
	}

	while (slash_pos != string::npos) {
		// Temporarily NULL out this slash.
		path8[slash_pos] = 0;

		// Attempt to create this directory.
		if (::mkdir(path8.c_str(), 0777) != 0) {
			// Could not create the directory.
			// If it exists already, that's fine.
			// Otherwise, something went wrong.
			if (errno != EEXIST) {
				// Something went wrong.
				return -errno;
			}
		}

		// Put the slash back in.
		path8[slash_pos] = DIR_SEP_CHR;
		slash_pos++;

		// Find the next slash.
		slash_pos = path8.find(DIR_SEP_CHR, slash_pos);
	}

	// rmkdir() succeeded.
	return 0;
}

/**
 * Does a file exist?
 * @param pathname Pathname.
 * @param mode Mode.
 * @return 0 if the file exists with the specified mode; non-zero if not.
 */
int access(const rp_string &pathname, int mode)
{
#if defined(RP_UTF16)
	string pathname8 = rp_string_to_utf8(pathname);
	return ::access(pathname8.c_str(), mode);
#elif defined(RP_UTF8)
	return ::access(pathname.c_str(), mode);
#endif
}

/**
 * Get a file's size.
 * @param filename Filename.
 * @return Size on success; -1 on error.
 */
int64_t filesize(const rp_string &filename)
{
	struct stat buf;
#if defined(RP_UTF16)
	string filename8 = rp_string_to_utf8(filename);
	int ret = stat(filename8.c_str(), &buf);
#elif defined(RP_UTF8)
	int ret = stat(filename.c_str(), &buf);
#endif

	if (ret != 0) {
		// stat() failed.
		ret = -errno;
		if (ret == 0) {
			// Something happened...
			ret = -EINVAL;
		}

		return ret;
	}

	// Return the file size.
	return buf.st_size;
}

/**
 * Initialize the configuration directory paths.
 * Called by pthread_once().
 */
static void initConfigDirectories(void)
{
	// Use LibUnixCommon to get the XDG directories.

	// Cache directory.
	cache_dir = utf8_to_rp_string(LibUnixCommon::getCacheDirectory());
	if (!cache_dir.empty()) {
		// Add a trailing slash if necessary.
		if (cache_dir.at(cache_dir.size()-1) != '/')
			cache_dir += _RP_CHR('/');
		// Append "rom-properties".
		cache_dir += _RP("rom-properties");
	}

	// Config directory.
	config_dir = utf8_to_rp_string(LibUnixCommon::getConfigDirectory());
	if (!config_dir.empty()) {
		// Add a trailing slash if necessary.
		if (config_dir.at(config_dir.size()-1) != '/')
			config_dir += _RP_CHR('/');
		// Append "rom-properties".
		config_dir += _RP("rom-properties");
	}

	// Directories have been initialized.
}

/**
 * Get the user's cache directory.
 * This is usually one of the following:
 * - Windows XP: %APPDATA%\Local Settings\rom-properties\cache
 * - Windows Vista: %LOCALAPPDATA%\rom-properties\cache
 * - Linux: ~/.cache/rom-properties
 *
 * @return User's rom-properties cache directory, or empty string on error.
 */
const rp_string &getCacheDirectory(void)
{
	// TODO: Handle errors.
	pthread_once(&once_control, initConfigDirectories);
	return cache_dir;
}

/**
 * Get the user's rom-properties configuration directory.
 * This is usually one of the following:
 * - Windows: %APPDATA%\rom-properties
 * - Linux: ~/.config/rom-properties
 *
 * @return User's rom-properties configuration directory, or empty string on error.
 */
const rp_string &getConfigDirectory(void)
{
	// TODO: Handle errors.
	pthread_once(&once_control, initConfigDirectories);
	return config_dir;
}

/**
 * Set the modification timestamp of a file.
 * @param filename Filename.
 * @param mtime Modification time.
 * @return 0 on success; negative POSIX error code on error.
 */
int set_mtime(const rp_string &filename, time_t mtime)
{
	// FIXME: time_t is 32-bit on 32-bit Linux.
	// TODO: Add a static_warning() macro?
	// - http://stackoverflow.com/questions/8936063/does-there-exist-a-static-warning
	struct utimbuf utbuf;
	utbuf.actime = time(nullptr);
	utbuf.modtime = mtime;
	int ret = utime(rp_string_to_utf8(filename).c_str(), &utbuf);

	return (ret == 0 ? 0 : -errno);
}

/**
 * Get the modification timestamp of a file.
 * @param filename Filename.
 * @param pMtime Buffer for the modification timestamp.
 * @return 0 on success; negative POSIX error code on error.
 */
int get_mtime(const rp_string &filename, time_t *pMtime)
{
	if (!pMtime)
		return -EINVAL;

	// FIXME: time_t is 32-bit on 32-bit Linux.
	// TODO: Add a static_warning() macro?
	// - http://stackoverflow.com/questions/8936063/does-there-exist-a-static-warning
	struct stat buf;
	int ret = stat(rp_string_to_utf8(filename).c_str(), &buf);
	if (ret == 0) {
		// stat() buffer retrieved.
		*pMtime = buf.st_mtime;
	}

	return (ret == 0 ? 0 : -errno);
}

/**
 * Delete a file.
 * @param filename Filename.
 * @return 0 on success; negative POSIX error code on error.
 */
int delete_file(const rp_char *filename)
{
	if (!filename || filename[0] == 0)
		return -EINVAL;

	int ret = unlink(rp_string_to_utf8(filename).c_str());
	if (ret != 0) {
		// Error deleting the file.
		ret = -errno;
	}

	return ret;
}

/**
 * Check if the specified file is a symbolic link.
 * @return True if the file is a symbolic link; false if not.
 */
bool is_symlink(const rp_char *filename)
{
	struct stat buf;
	int ret = lstat(RP2U8_c(filename), &buf);
	if (ret != 0) {
		// lstat() failed.
		// Assume this is not a symlink.
		return false;
	}
	return !!S_ISLNK(buf.st_mode);
}

} }
