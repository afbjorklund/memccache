// Copyright (C) 2002 Andrew Tridgell
// Copyright (C) 2009-2020 Joel Rosdahl
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "ccache.h"

#include <zlib.h>
#include <lz4frame.h>

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <sys/locking.h>
#include <psapi.h>
#include <tchar.h>
#endif

// Destination for conf->log_file.
static FILE *logfile;

// Buffer used for logs in conf->debug mode.
static char *debug_log_buffer;

// Allocated debug_log_buffer size.
static size_t debug_log_buffer_capacity;

// The amount of log data stored in debug_log_buffer.
static size_t debug_log_size;

#define DEBUG_LOG_BUFFER_MARGIN 1024

static bool
init_log(void)
{
	extern struct conf *conf;

	if (debug_log_buffer || logfile) {
		return true;
	}
	assert(conf);
	if (conf->debug) {
		debug_log_buffer_capacity = DEBUG_LOG_BUFFER_MARGIN;
		debug_log_buffer = x_malloc(debug_log_buffer_capacity);
		debug_log_size = 0;
	}
	if (str_eq(conf->log_file, "")) {
		return conf->debug;
	}
	logfile = fopen(conf->log_file, "a");
	if (logfile) {
#ifndef _WIN32
		set_cloexec_flag(fileno(logfile));
#endif
		return true;
	} else {
		return false;
	}
}

static void
append_to_debug_log(const char *s, size_t len)
{
	assert(debug_log_buffer);
	if (debug_log_size + len + 1 > debug_log_buffer_capacity) {
		debug_log_buffer_capacity += len + 1 + DEBUG_LOG_BUFFER_MARGIN;
		debug_log_buffer = x_realloc(debug_log_buffer, debug_log_buffer_capacity);
	}
	memcpy(debug_log_buffer + debug_log_size, s, len);
	debug_log_size += len;
}

static void
log_prefix(bool log_updated_time)
{
	static char prefix[200];
#ifdef HAVE_GETTIMEOFDAY
	if (log_updated_time) {
		char timestamp[100];
		struct tm tm;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		if (localtime_r((time_t *)&tv.tv_sec, &tm) != NULL) {
			strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm);
		} else {
			snprintf(timestamp, sizeof(timestamp), "%lu", tv.tv_sec);
		}
		snprintf(prefix, sizeof(prefix),
		         "[%s.%06d %-5d] ", timestamp, (int)tv.tv_usec, (int)getpid());
	}
#else
	snprintf(prefix, sizeof(prefix), "[%-5d] ", (int)getpid());
#endif
	if (logfile) {
		fputs(prefix, logfile);
	}
	if (debug_log_buffer) {
		append_to_debug_log(prefix, strlen(prefix));
	}
}

static long
path_max(const char *path)
{
#ifdef PATH_MAX
	(void)path;
	return PATH_MAX;
#elif defined(MAXPATHLEN)
	(void)path;
	return MAXPATHLEN;
#elif defined(_PC_PATH_MAX)
	long maxlen = pathconf(path, _PC_PATH_MAX);
	return maxlen >= 4096 ? maxlen : 4096;
#endif
}

static void warn_log_fail(void) ATTR_NORETURN;

// Warn about failure writing to the log file and then exit.
static void
warn_log_fail(void)
{
	extern struct conf *conf;

	// Note: Can't call fatal() since that would lead to recursion.
	fprintf(stderr, "ccache: error: Failed to write to %s: %s\n",
	        conf->log_file, strerror(errno));
	x_exit(EXIT_FAILURE);
}

static void
vlog(const char *format, va_list ap, bool log_updated_time)
{
	if (!init_log()) {
		return;
	}

	va_list aq;
	va_copy(aq, ap);
	log_prefix(log_updated_time);
	if (logfile) {
		int rc1 = vfprintf(logfile, format, ap);
		int rc2 = fprintf(logfile, "\n");
		if (rc1 < 0 || rc2 < 0) {
			warn_log_fail();
		}
	}
	if (debug_log_buffer) {
		char buf[8192];
		int len = vsnprintf(buf, sizeof(buf), format, aq);
		if (len >= 0) {
			append_to_debug_log(buf, MIN((size_t)len, sizeof(buf) - 1));
			append_to_debug_log("\n", 1);
		}
	}
	va_end(aq);
}

// Write a message to the log file (adding a newline) and flush.
void
cc_log(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vlog(format, ap, true);
	va_end(ap);
	if (logfile) {
		fflush(logfile);
	}
}

// Write a message to the log file (adding a newline) without flushing and with
// a reused timestamp.
void
cc_bulklog(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vlog(format, ap, false);
	va_end(ap);
}

// Log an executed command to the CCACHE_LOGFILE location.
void
cc_log_argv(const char *prefix, char **argv)
{
	if (!init_log()) {
		return;
	}

	log_prefix(true);
	if (logfile) {
		fputs(prefix, logfile);
		print_command(logfile, argv);
		int rc = fflush(logfile);
		if (rc) {
			warn_log_fail();
		}
	}
	if (debug_log_buffer) {
		append_to_debug_log(prefix, strlen(prefix));
		char *s = format_command(argv);
		append_to_debug_log(s, strlen(s));
		free(s);
	}
}

// Copy the current log memory buffer to an output file.
void
cc_dump_debug_log_buffer(const char *path)
{
	FILE *file = fopen(path, "w");
	if (file) {
		(void) fwrite(debug_log_buffer, 1, debug_log_size, file);
		fclose(file);
	} else {
		cc_log("Failed to open %s: %s", path, strerror(errno));
	}
}

// Something went badly wrong!
void
fatal(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	char msg[8192];
	vsnprintf(msg, sizeof(msg), format, ap);
	va_end(ap);

	cc_log("FATAL: %s", msg);
	fprintf(stderr, "ccache: error: %s\n", msg);

	x_exit(1);
}

// Copy all data from fd_in to fd_out, decompressing data from fd_in if needed.
void
copy_fd(int fd_in, int fd_out)
{
	gzFile gz_in = gzdopen(dup(fd_in), "rb");
	if (!gz_in) {
		fatal("Failed to copy fd");
	}

	int n;
	char buf[READ_BUFFER_SIZE];
	while ((n = gzread(gz_in, buf, sizeof(buf))) > 0) {
		ssize_t written = 0;
		do {
			ssize_t count = write(fd_out, buf + written, n - written);
			if (count == -1) {
				if (errno != EAGAIN && errno != EINTR) {
					fatal("Failed to copy fd");
				}
			} else {
				written += count;
			}
		} while (written < n);
	}

	gzclose(gz_in);
}

#ifndef HAVE_MKSTEMP
// Cheap and nasty mkstemp replacement.
int
mkstemp(char *template)
{
#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	mktemp(template);
#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif
	return open(template, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
}
#endif

#ifndef _WIN32
static mode_t
get_umask(void)
{
	static bool mask_retrieved = false;
	static mode_t mask;
	if (!mask_retrieved) {
		mask = umask(0);
		umask(mask);
		mask_retrieved = true;
	}
	return mask;
}
#endif

static int
lz4f_copy_file(const char *src, const char *dest, int compress_level)
{
	int fd_in, fd_out;
	void *in, *out;
	size_t in_size, out_size;
	char magic[4] = { 0x04, 0x22, 0x4d, 0x18 };
	LZ4F_frameInfo_t info;
	LZ4F_preferences_t prefs;
	LZ4F_decompressionContext_t context;
	LZ4F_decompressOptions_t options;
	LZ4F_errorCode_t err;
	struct stat st;
	ssize_t n;
	size_t left, size;
	char *p, *q;
	int saved_errno = 0;
	char *tmp_name;

	/* open destination file */
	tmp_name = x_strdup(dest);
	fd_out = create_tmp_fd(&tmp_name);
	cc_log("Copying %s to %s via %s (lz4 %scompressed)",
	       src, dest, tmp_name, compress_level != 0 ? "" : "un");

	/* open source file */
	fd_in = open(src, O_RDONLY | O_BINARY);
	if (fd_in == -1) {
		saved_errno = errno;
		cc_log("open error: %s", strerror(saved_errno));
		goto error;
	}

	if (x_fstat(fd_in, &st) != 0) {
		saved_errno = errno;
		goto error;
	}

	in_size = st.st_size;
	in = x_malloc(in_size);

	/* read */
	p = in;
	left = in_size;
	do {
		n = read(fd_in, p, left);
		if (n == -1) {
			saved_errno = errno;
			free(in);
			goto error;
		}
		p += n;
		left -= n;
	} while (left > 0);

	/* decompress */
	if (in_size > 4 && memcmp(in, magic, 4) == 0) {
		err = LZ4F_createDecompressionContext(&context, LZ4F_VERSION);
		if (LZ4F_isError(err)) {
			cc_log("context error: %s", LZ4F_getErrorName(err));
			free(in);
			goto error;
		}

		q = in;
		left = in_size;

		p = in;
		size = in_size;
		err = LZ4F_getFrameInfo(context, &info, p, &size);
		if (LZ4F_isError(err)) {
			cc_log("frame error: %s", LZ4F_getErrorName(err));
			free(in);
			goto error;
		}
		q += size;
		left -= size;

		cc_log("read %ld header bytes", (long) size);

		if (info.contentSize == 0) {
			cc_log("unknown size: %ld", (long) info.contentSize);
			free(in);
			goto error;
		}

		out_size = info.contentSize;
		out = x_malloc(out_size);

		cc_log("decompressing from %ld to %ld (%.2f%%)",
		       (long) in_size, (long) out_size, (100.0 * out_size / in_size));

		p = out;
		size = out_size;
		memset(&options, 0, sizeof(options));
		do {
			size = out_size - (p - (char *) out);
			left = in_size - (q - (char *) in);

			err = LZ4F_decompress(context, p, &size, q, &left, &options);
			if (LZ4F_isError(err)) {
				cc_log("decompress error: %s", LZ4F_getErrorName(err));
				free(in);
				free(out);
				goto error;
			}
			p += size;
			q += left;
		} while (err != 0 && left > 0);

		free(in);
		in = out;
		in_size = out_size;

		err = LZ4F_freeDecompressionContext(context);
		if (LZ4F_isError(err)) {
			cc_log("context error: %s", LZ4F_getErrorName(err));
			free(in);
			goto error;
		}
	}

	/* compress */
	if (compress_level > 0) {
		memset(&prefs, 0, sizeof(prefs));
		prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
		prefs.frameInfo.contentSize = in_size;
		prefs.compressionLevel = compress_level;
		out_size = LZ4F_compressFrameBound(in_size, &prefs);
		out = x_malloc(out_size);
		err = LZ4F_compressFrame(out, out_size, in, in_size, &prefs);
		if (LZ4F_isError(err)) {
			cc_log("context error: %s", LZ4F_getErrorName(err));
			free(in);
			goto error;
		}
		size = err;
		out = x_realloc(out, size);
		out_size = size;
		cc_log("compressed from %ld to %ld (%.2f%%)",
		       (long) in_size, (long) out_size, (100.0 * out_size / in_size));
		free(in);
	} else {
		out_size = in_size;
		out = in;
	}

	/* write */
	p = out;
	left = out_size;
	do {
		n = write(fd_out, p, left);
		if (n == -1) {
			saved_errno = errno;
			free(out);
			goto error;
		}
		p += n;
		left -= n;
	} while (left > 0);

	free(out);

	if (close(fd_out) == -1) {
		saved_errno = errno;
		cc_log("close error: %s", strerror(saved_errno));
		goto error;
	}

	if (x_rename(tmp_name, dest) == -1) {
		saved_errno = errno;
		cc_log("rename error: %s", strerror(saved_errno));
		goto error;
	}

	free(tmp_name);

	return 0;

error:
	if (fd_out != -1) {
		close(fd_out);
	}
	tmp_unlink(tmp_name);
	free(tmp_name);
	errno = saved_errno;
	return -1;
}

// Copy src to dest, decompressing src if needed. compress_level != 0 decides
// whether dest will be compressed, and with which compression level. Returns 0
// on success and -1 on failure. On failure, errno represents the error.
int
copy_file(const char *src,
          const char *dest,
          const char *compress_type,
          int compress_level,
          bool via_tmp_file)
{
	int fd_out = -1;
	char *tmp_name = NULL;
	gzFile gz_in = NULL;
	gzFile gz_out = NULL;
	int saved_errno = 0;
	int level;

	if (compress_type && str_eq(compress_type, "lz4f")) {
		return lz4f_copy_file(src, dest, compress_level);
	}
	if (compress_type && compress_level != 0 && !str_eq(compress_type, "gzip")) {
		cc_log("Unknown compression type %s", compress_type);
		return -1;
	}

	// Open source file.
	int fd_in = open(src, O_RDONLY | O_BINARY);
	if (fd_in == -1) {
		saved_errno = errno;
		cc_log("open error: %s", strerror(saved_errno));
		goto error;
	}

	// Open destination file.
	if (via_tmp_file) {
		tmp_name = x_strdup(dest);
		fd_out = create_tmp_fd(&tmp_name);
		cc_log("Copying %s to %s via %s (%scompressed)",
		       src, dest, tmp_name, compress_level > 0 ? "" : "un");
	} else {
		fd_out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
		if (fd_out == -1) {
			saved_errno = errno;
			close(fd_out);
			errno = saved_errno;
			return -1;
		}
		cc_log("Copying %s to %s (%scompressed)",
		       src, dest, compress_level > 0 ? "" : "un");
	}

	gz_in = gzdopen(fd_in, "rb");
	if (!gz_in) {
		saved_errno = errno;
		cc_log("gzdopen(src) error: %s", strerror(saved_errno));
		close(fd_in);
		goto error;
	}

	if (compress_level != 0) {
		// A gzip file occupies at least 20 bytes, so it will always occupy an
		// entire filesystem block, even for empty files. Turn off compression for
		// empty files to save some space.
		struct stat st;
		if (x_fstat(fd_in, &st) != 0) {
			goto error;
		}
		if (file_size(&st) == 0) {
			compress_level = 0;
		}
	}

	if (compress_level != 0) {
		gz_out = gzdopen(dup(fd_out), "wb");
		if (!gz_out) {
			saved_errno = errno;
			cc_log("gzdopen(dest) error: %s", strerror(saved_errno));
			goto error;
		}
		level = compress_level > 0 ? compress_level : Z_DEFAULT_COMPRESSION;
		gzsetparams(gz_out, level, Z_DEFAULT_STRATEGY);
	}

	int n;
	char buf[READ_BUFFER_SIZE];
	while ((n = gzread(gz_in, buf, sizeof(buf))) > 0) {
		int written;
		if (compress_level != 0) {
			written = gzwrite(gz_out, buf, n);
		} else {
			written = 0;
			do {
				ssize_t count = write(fd_out, buf + written, n - written);
				if (count == -1 && errno != EINTR) {
					saved_errno = errno;
					break;
				}
				written += count;
			} while (written < n);
		}
		if (written != n) {
			if (compress_level != 0) {
				int errnum;
				cc_log("gzwrite error: %s (errno: %s)",
				       gzerror(gz_in, &errnum),
				       strerror(saved_errno));
			} else {
				cc_log("write error: %s", strerror(saved_errno));
			}
			goto error;
		}
	}

	// gzeof won't tell if there's an error in the trailing CRC, so we must check
	// gzerror before considering everything OK.
	int errnum;
	gzerror(gz_in, &errnum);
	if (!gzeof(gz_in) || (errnum != Z_OK && errnum != Z_STREAM_END)) {
		saved_errno = errno;
		cc_log("gzread error: %s (errno: %s)",
		       gzerror(gz_in, &errnum), strerror(saved_errno));
		gzclose(gz_in);
		if (gz_out) {
			gzclose(gz_out);
		}
		close(fd_out);
		if (via_tmp_file) {
			tmp_unlink(tmp_name);
			free(tmp_name);
		}
		return -1;
	}

	gzclose(gz_in);
	gz_in = NULL;
	if (gz_out) {
		gzclose(gz_out);
		gz_out = NULL;
	}

#ifndef _WIN32
	fchmod(fd_out, 0666 & ~get_umask());
#endif

	// The close can fail on NFS if out of space.
	if (close(fd_out) == -1) {
		saved_errno = errno;
		cc_log("close error: %s", strerror(saved_errno));
		goto error;
	}

	if (via_tmp_file) {
		if (x_rename(tmp_name, dest) == -1) {
			saved_errno = errno;
			cc_log("rename error: %s", strerror(saved_errno));
			goto error;
		}

		free(tmp_name);
	}

	return 0;

error:
	if (gz_in) {
		gzclose(gz_in);
	}
	if (gz_out) {
		gzclose(gz_out);
	}
	if (fd_out != -1) {
		close(fd_out);
	}
	if (via_tmp_file && tmp_name) {
		tmp_unlink(tmp_name);
		free(tmp_name);
	}
	errno = saved_errno;
	return -1;
}

// Write data to a fd.
int safe_write(int fd_out, const char *data, size_t length)
{
	size_t written = 0;
	do {
		int ret;
		ret = write(fd_out, data + written, length - written);
		if (ret < 0) {
			if (errno != EAGAIN && errno != EINTR) {
				return ret;
			}
		} else {
			written += ret;
		}
	} while (written < length);
	return 0;
}

// Write data to a file.
int write_file(const char *data, const char *dest, size_t length)
{
	int fd_out;
	char *tmp_name;
	int ret;
	int saved_errno = 0;

	tmp_name = x_strdup(dest);
	fd_out = create_tmp_fd(&tmp_name);
	if (fd_out < 0) {
		tmp_unlink(tmp_name);
		free(tmp_name);
		return -1;
	}

	ret = safe_write(fd_out, data, length);
	if (ret < 0) {
		saved_errno = errno;
		cc_log("write error: %s", strerror(saved_errno));
		goto error;
	}

#ifndef _WIN32
	fchmod(fd_out, 0666 & ~get_umask());
#endif

	/* the close can fail on NFS if out of space */
	if (close(fd_out) == -1) {
		saved_errno = errno;
		cc_log("close error: %s", strerror(saved_errno));
		goto error;
	}

	if (x_rename(tmp_name, dest) == -1) {
		saved_errno = errno;
		cc_log("rename error: %s", strerror(saved_errno));
		goto error;
	}

	free(tmp_name);
	return 0;

error:
	close(fd_out);
	tmp_unlink(tmp_name);
	free(tmp_name);
	errno = saved_errno;
	return -1;
}

// Run copy_file() and, if successful, delete the source file.
int
move_file(const char *src, const char *dest, const char *compress_type,
          int compress_level)
{
	int ret = copy_file(src, dest, compress_type, compress_level, true);
	if (ret != -1) {
		x_unlink(src);
	}
	return ret;
}

// Like move_file(), but assumes that src is uncompressed and that src and dest
// are on the same file system.
int
move_uncompressed_file(const char *src, const char *dest,
                       const char *compress_type,
                       int compress_level)
{
	if (compress_level > 0) {
		return move_file(src, dest, compress_type, compress_level);
	} else {
		return x_rename(src, dest);
	}
}

// test if a file is zlib or lz4 compressed
bool
file_is_compressed(const char *filename, const char **type)
{
	FILE *f = fopen(filename, "rb");
	if (!f) {
		if (type) {
			*type = NULL;
		}
		return false;
	}

	int c1 = fgetc(f);
	int c2 = fgetc(f);

	// Test if file starts with 1F8B, which is zlib's magic number.
	if ((c1 == 0x1f) && (c2 == 0x8b)) {
		if (type) {
			*type = "gzip";
		}
		fclose(f);
		return true;
	}

	int c3 = fgetc(f);
	int c4 = fgetc(f);

	// Test if file starts with 184D2204, which is lz4's magic number (LE).
	if ((c4 == 0x18) && (c3 == 0x4d) &&
	    (c2 == 0x22) && (c1 == 0x04)) {
		if (type) {
			*type = "lz4f";
		}
		fclose(f);
		return true;
	}

	/* not compressed */
	if (type) {
		*type = NULL;
	}
	fclose(f);
	return false;
}

// Make sure a directory exists.
int
create_dir(const char *dir)
{
	struct stat st;
	if (stat(dir, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			return 0;
		}
		errno = ENOTDIR;
		return 1;
	}
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		return 1;
	}
	return 0;
}

// Create directories leading to path. Returns 0 on success, otherwise -1.
int
create_parent_dirs(const char *path)
{
	int res;
	char *parent = dirname(path);
	struct stat st;
	if (stat(parent, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			res = 0;
		} else {
			res = -1;
			errno = ENOTDIR;
		}
	} else {
		res = create_parent_dirs(parent);
		if (res == 0) {
			res = mkdir(parent, 0777);
			// Have to handle the condition of the directory already existing because
			// the file system could have changed in between calling stat and
			// actually creating the directory. This can happen when there are
			// multiple instances of ccache running and trying to create the same
			// directory chain, which usually is the case when the cache root does
			// not initially exist. As long as one of the processes creates the
			// directories then our condition is satisfied and we avoid a race
			// condition.
			if (res != 0 && errno == EEXIST) {
				res = 0;
			}
		} else {
			res = -1;
		}
	}
	free(parent);
	return res;
}

// Return a static string with the current hostname.
const char *
get_hostname(void)
{
	static char hostname[260] = "";

	if (hostname[0]) {
		return hostname;
	}

	strcpy(hostname, "unknown");
#if HAVE_GETHOSTNAME
	gethostname(hostname, sizeof(hostname) - 1);
#elif defined(_WIN32)
	const char *computer_name = getenv("COMPUTERNAME");
	if (computer_name) {
		snprintf(hostname, sizeof(hostname), "%s", computer_name);
		return hostname;
	}

	WORD w_version_requested = MAKEWORD(2, 2);
	WSADATA wsa_data;
	int err = WSAStartup(w_version_requested, &wsa_data);
	if (err != 0) {
		// Tell the user that we could not find a usable Winsock DLL.
		cc_log("WSAStartup failed with error: %d", err);
		return hostname;
	}

	if (LOBYTE(wsa_data.wVersion) != 2 || HIBYTE(wsa_data.wVersion) != 2) {
		// Tell the user that we could not find a usable WinSock DLL.
		cc_log("Could not find a usable version of Winsock.dll");
		WSACleanup();
		return hostname;
	}

	int result = gethostname(hostname, sizeof(hostname) - 1);
	if (result != 0) {
		LPVOID lp_msg_buf;
		DWORD dw = WSAGetLastError();

		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR) &lp_msg_buf, 0, NULL);

		LPVOID lp_display_buf = (LPVOID) LocalAlloc(
			LMEM_ZEROINIT,
			(lstrlen((LPCTSTR) lp_msg_buf) + lstrlen((LPCTSTR) __FILE__) + 200)
			* sizeof(TCHAR));
		_snprintf((LPTSTR) lp_display_buf,
		          LocalSize(lp_display_buf) / sizeof(TCHAR),
		          TEXT("%s failed with error %lu: %s"), __FILE__, dw,
		          (const char *)lp_msg_buf);

		cc_log("can't get hostname OS returned error: %s", (char *)lp_display_buf);

		LocalFree(lp_msg_buf);
		LocalFree(lp_display_buf);
	}
	WSACleanup();
#endif

	hostname[sizeof(hostname) - 1] = 0;
	return hostname;
}

// Return a string to be passed to mkstemp to create a temporary file. Also
// tries to cope with NFS by adding the local hostname.
const char *
tmp_string(void)
{
	static char *ret;
	if (!ret) {
		ret = format("%s.%u.XXXXXX", get_hostname(), (unsigned)getpid());
	}
	return ret;
}

// Return the hash result as a hex string. Size -1 means don't include size
// suffix. Caller frees.
char *
format_hash_as_string(const unsigned char *hash, int size)
{
	int i;
	char *ret = x_malloc(53);
	for (i = 0; i < 16; i++) {
		sprintf(&ret[i*2], "%02x", (unsigned) hash[i]);
	}
	if (size >= 0) {
		sprintf(&ret[i*2], "-%d", size);
	}
	return ret;
}

static char const CACHEDIR_TAG[] =
	"Signature: 8a477f597d28d172789f06886806bc55\n"
	"# This file is a cache directory tag created by ccache.\n"
	"# For information about cache directory tags, see:\n"
	"#\thttp://www.brynosaurus.com/cachedir/\n";

int
create_cachedirtag(const char *dir)
{
	char *filename = format("%s/CACHEDIR.TAG", dir);
	struct stat st;
	if (stat(filename, &st) == 0) {
		if (S_ISREG(st.st_mode)) {
			goto success;
		}
		errno = EEXIST;
		goto error;
	}
	if (create_dir(dir)) {
		goto error;
	}
	FILE *f = fopen(filename, "w");
	if (!f) {
		goto error;
	}
	if (fwrite(CACHEDIR_TAG, sizeof(CACHEDIR_TAG)-1, 1, f) != 1) {
		fclose(f);
		goto error;
	}
	if (fclose(f)) {
		goto error;
	}
success:
	free(filename);
	return 0;
error:
	free(filename);
	return 1;
}

// Construct a string according to a format. Caller frees.
char *
format(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	char *ptr = NULL;
	if (vasprintf(&ptr, format, ap) == -1) {
		fatal("Out of memory in format");
	}
	va_end(ap);

	return ptr;
}

// This is like strdup() but dies if the malloc fails.
char *
x_strdup(const char *s)
{
	char *ret = strdup(s);
	if (!ret) {
		fatal("Out of memory in x_strdup");
	}
	return ret;
}

// This is like strndup() but dies if the malloc fails.
char *
x_strndup(const char *s, size_t n)
{
#ifndef HAVE_STRNDUP
	if (!s) {
		return NULL;
	}
	size_t m = 0;
	while (m < n && s[m]) {
		m++;
	}
	char *ret = malloc(m + 1);
	if (ret) {
		memcpy(ret, s, m);
		ret[m] = '\0';
	}
#else
	char *ret = strndup(s, n);
#endif
	if (!ret) {
		fatal("x_strndup: Could not allocate %lu bytes", (unsigned long)n);
	}
	return ret;
}

// This is like malloc() but dies if the malloc fails.
void *
x_malloc(size_t size)
{
	if (size == 0) {
		// malloc() may return NULL if size is zero, so always do this to make sure
		// that the code handles it regardless of platform.
		return NULL;
	}
	void *ret = malloc(size);
	if (!ret) {
		fatal("x_malloc: Could not allocate %lu bytes", (unsigned long)size);
	}
	return ret;
}

// This is like calloc() but dies if the allocation fails.
void *
x_calloc(size_t nmemb, size_t size)
{
	if (nmemb * size == 0) {
		// calloc() may return NULL if nmemb or size is 0, so always do this to
		// make sure that the code handles it regardless of platform.
		return NULL;
	}
	void *ret = calloc(nmemb, size);
	if (!ret) {
		fatal("x_calloc: Could not allocate %lu bytes", (unsigned long)size);
	}
	return ret;
}

// This is like realloc() but dies if the malloc fails.
void *
x_realloc(void *ptr, size_t size)
{
	if (!ptr) {
		return x_malloc(size);
	}
	void *p2 = realloc(ptr, size);
	if (!p2) {
		fatal("x_realloc: Could not allocate %lu bytes", (unsigned long)size);
	}
	return p2;
}

// This is like setenv.
void x_setenv(const char *name, const char *value)
{
#ifdef HAVE_SETENV
	setenv(name, value, true);
#else
	putenv(format("%s=%s", name, value)); // Leak to environment.
#endif
}

// This is like unsetenv.
void x_unsetenv(const char *name)
{
#ifdef HAVE_UNSETENV
	unsetenv(name);
#else
	putenv(x_strdup(name)); // Leak to environment.
#endif
}

// Like fstat() but also call cc_log on failure.
int
x_fstat(int fd, struct stat *buf)
{
	int result = fstat(fd, buf);
	if (result != 0) {
		cc_log("Failed to fstat fd %d: %s", fd, strerror(errno));
	}
	return result;
}

// Like lstat() but also call cc_log on failure.
int
x_lstat(const char *pathname, struct stat *buf)
{
	int result = lstat(pathname, buf);
	if (result != 0) {
		cc_log("Failed to lstat %s: %s", pathname, strerror(errno));
	}
	return result;
}

// Like stat() but also call cc_log on failure.
int
x_stat(const char *pathname, struct stat *buf)
{
	int result = stat(pathname, buf);
	if (result != 0) {
		cc_log("Failed to stat %s: %s", pathname, strerror(errno));
	}
	return result;
}

// Construct a string according to the format and store it in *ptr. The
// original *ptr is then freed.
void
reformat(char **ptr, const char *format, ...)
{
	char *saved = *ptr;
	*ptr = NULL;

	va_list ap;
	va_start(ap, format);
	if (vasprintf(ptr, format, ap) == -1) {
		fatal("Out of memory in reformat");
	}
	va_end(ap);

	if (saved) {
		free(saved);
	}
}

// Recursive directory traversal. fn() is called on all entries in the tree.
void
traverse(const char *dir, void (*fn)(const char *, struct stat *))
{
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}

	struct dirent *de;
	while ((de = readdir(d))) {
		if (str_eq(de->d_name, ".")) {
			continue;
		}
		if (str_eq(de->d_name, "..")) {
			continue;
		}

		if (strlen(de->d_name) == 0) {
			continue;
		}

		char *fname = format("%s/%s", dir, de->d_name);
		struct stat st;
		if (lstat(fname, &st)) {
			if (errno != ENOENT && errno != ESTALE) {
				fatal("lstat %s failed: %s", fname, strerror(errno));
			}
			free(fname);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			traverse(fname, fn);
		}

		fn(fname, &st);
		free(fname);
	}

	closedir(d);
}


// Return the base name of a file - caller frees.
char *
basename(const char *path)
{
	char *p = strrchr(path, '/');
	if (p) {
		path = p + 1;
	}
#ifdef _WIN32
	p = strrchr(path, '\\');
	if (p) {
		path = p + 1;
	}
#endif

	return x_strdup(path);
}

// Return the dir name of a file - caller frees.
char *
dirname(const char *path)
{
	char *s = x_strdup(path);
	char *p = strrchr(s, '/');
#ifdef _WIN32
	char *p2 = strrchr(s, '\\');
	if (!p || (p2 && p < p2)) {
		p = p2;
	}
#endif
	if (!p) {
		free(s);
		s = x_strdup(".");
	} else if (p == s) {
		*(p + 1) = 0;
	} else {
		*p = 0;
	}
	return s;
}

// Return the file extension (including the dot) of a path as a pointer into
// path. If path has no file extension, the empty string and the end of path is
// returned.
const char *
get_extension(const char *path)
{
	size_t len = strlen(path);
	for (const char *p = &path[len - 1]; p >= path; --p) {
		if (*p == '.') {
			return p;
		}
		if (*p == '/') {
			break;
		}
	}
	return &path[len];
}

// Return a string containing the given path without the filename extension.
// Caller frees.
char *
remove_extension(const char *path)
{
	return x_strndup(path, strlen(path) - strlen(get_extension(path)));
}

// Return size on disk of a file.
size_t
file_size(struct stat *st)
{
#ifdef _WIN32
	return (st->st_size + 1023) & ~1023;
#else
	return st->st_blocks * 512;
#endif
}

// Format a size as a human-readable string. Caller frees.
char *
format_human_readable_size(uint64_t v)
{
	char *s;
	if (v >= 1000*1000*1000) {
		s = format("%.1f GB", v/((double)(1000*1000*1000)));
	} else if (v >= 1000*1000) {
		s = format("%.1f MB", v/((double)(1000*1000)));
	} else {
		s = format("%.1f kB", v/((double)(1000)));
	}
	return s;
}

// Format a size as a parsable string. Caller frees.
char *
format_parsable_size_with_suffix(uint64_t size)
{
	char *s;
	if (size >= 1000*1000*1000) {
		s = format("%.1fG", size / ((double)(1000*1000*1000)));
	} else if (size >= 1000*1000) {
		s = format("%.1fM", size / ((double)(1000*1000)));
	} else if (size >= 1000) {
		s = format("%.1fk", size / ((double)(1000)));
	} else {
		s = format("%u", (unsigned)size);
	}
	return s;
}

// Parse a "size value", i.e. a string that can end in k, M, G, T (10-based
// suffixes) or Ki, Mi, Gi, Ti (2-based suffixes). For backward compatibility,
// K is also recognized as a synonym of k.
bool
parse_size_with_suffix(const char *str, uint64_t *size)
{
	errno = 0;

	char *p;
	double x = strtod(str, &p);
	if (errno != 0 || x < 0 || p == str || *str == '\0') {
		return false;
	}

	while (isspace(*p)) {
		++p;
	}

	if (*p != '\0') {
		unsigned multiplier = *(p+1) == 'i' ? 1024 : 1000;
		switch (*p) {
		case 'T':
			x *= multiplier;
		// Fallthrough.
		case 'G':
			x *= multiplier;
		// Fallthrough.
		case 'M':
			x *= multiplier;
		// Fallthrough.
		case 'K':
		case 'k':
			x *= multiplier;
			break;
		default:
			return false;
		}
	} else {
		// Default suffix: G.
		x *= 1000 * 1000 * 1000;
	}
	*size = (uint64_t)x;
	return true;
}

// A sane realpath() function, trying to cope with stupid path limits and a
// broken API. Caller frees.
char *
x_realpath(const char *path)
{
	long maxlen = path_max(path);
	char *ret = x_malloc(maxlen);
	char *p;

#if HAVE_REALPATH
	p = realpath(path, ret);
#elif defined(_WIN32)
	if (path[0] == '/') {
		path++;  // Skip leading slash.
	}
	HANDLE path_handle = CreateFile(
		path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE != path_handle) {
		bool ok = GetFinalPathNameByHandle(
			path_handle, ret, maxlen, FILE_NAME_NORMALIZED) != 0;
		CloseHandle(path_handle);
		if (!ok) {
			free(ret);
			return x_strdup(path);
		}
		p = ret + 4; // Strip \\?\ from the file name.
	} else {
		snprintf(ret, maxlen, "%s", path);
		p = ret;
	}
#else
	// Yes, there are such systems. This replacement relies on the fact that when
	// we call x_realpath we only care about symlinks.
	{
		int len = readlink(path, ret, maxlen-1);
		if (len == -1) {
			free(ret);
			return NULL;
		}
		ret[len] = 0;
		p = ret;
	}
#endif
	if (p) {
		p = x_strdup(p);
		free(ret);
		return p;
	}
	free(ret);
	return NULL;
}

// A getcwd that will returns an allocated buffer.
char *
gnu_getcwd(void)
{
	unsigned size = 128;

	while (true) {
		char *buffer = (char *)x_malloc(size);
		if (getcwd(buffer, size) == buffer) {
			return buffer;
		}
		free(buffer);
		if (errno != ERANGE) {
			cc_log("getcwd error: %d (%s)", errno, strerror(errno));
			return NULL;
		}
		size *= 2;
	}
}

#ifndef HAVE_LOCALTIME_R
// localtime_r replacement.
struct tm *
localtime_r(const time_t *timep, struct tm *result)
{
	struct tm *tm = localtime(timep);
	if (tm) {
		*result = *tm;
		return result;
	} else {
		memset(result, 0, sizeof(*result));
		return NULL;
	}
}
#endif

#ifndef HAVE_STRTOK_R
// strtok_r replacement.
char *
strtok_r(char *str, const char *delim, char **saveptr)
{
	if (!str) {
		str = *saveptr;
	}
	int len = strlen(str);
	char *ret = strtok(str, delim);
	if (ret) {
		char *save = ret;
		while (*save++) {
			// Do nothing.
		}
		if ((len + 1) == (intptr_t) (save - str)) {
			save--;
		}
		*saveptr = save;
	}
	return ret;
}
#endif

// Create an empty temporary file. *fname will be reallocated and set to the
// resulting filename. Returns an open file descriptor to the file.
int
create_tmp_fd(char **fname)
{
	char *template = format("%s.%s", *fname, tmp_string());
	int fd = mkstemp(template);
	if (fd == -1 && errno == ENOENT) {
		if (create_parent_dirs(*fname) != 0) {
			fatal("Failed to create directory %s: %s",
			      dirname(*fname), strerror(errno));
		}
		reformat(&template, "%s.%s", *fname, tmp_string());
		fd = mkstemp(template);
	}
	if (fd == -1) {
		fatal("Failed to create temporary file for %s: %s",
		      *fname, strerror(errno));
	}
	set_cloexec_flag(fd);

#ifndef _WIN32
	fchmod(fd, 0666 & ~get_umask());
#endif

	free(*fname);
	*fname = template;
	return fd;
}

// Create an empty temporary file. *fname will be reallocated and set to the
// resulting filename. Returns an open FILE*.
FILE *
create_tmp_file(char **fname, const char *mode)
{
	FILE *file = fdopen(create_tmp_fd(fname), mode);
	if (!file) {
		fatal("Failed to create file %s: %s", *fname, strerror(errno));
	}
	return file;
}

// Return current user's home directory, or NULL if it can't be determined.
const char *
get_home_directory(void)
{
	const char *p = getenv("HOME");
	if (p) {
		return p;
	}
#ifdef _WIN32
	p = getenv("APPDATA");
	if (p) {
		return p;
	}
#endif
#ifdef HAVE_GETPWUID
	{
		struct passwd *pwd = getpwuid(getuid());
		if (pwd) {
			return pwd->pw_dir;
		}
	}
#endif
	return NULL;
}

// Get the current directory by reading $PWD. If $PWD isn't sane, gnu_getcwd()
// is used. Caller frees.
char *
get_cwd(void)
{
	struct stat st_pwd;
	struct stat st_cwd;

	char *cwd = gnu_getcwd();
	if (!cwd) {
		return NULL;
	}
	char *pwd = getenv("PWD");
	if (!pwd) {
		return cwd;
	}
	if (stat(pwd, &st_pwd) != 0) {
		return cwd;
	}
	if (stat(cwd, &st_cwd) != 0) {
		return cwd;
	}
	if (st_pwd.st_dev == st_cwd.st_dev && st_pwd.st_ino == st_cwd.st_ino) {
		free(cwd);
		return x_strdup(pwd);
	} else {
		return cwd;
	}
}

// Check whether s1 and s2 have the same executable name.
bool
same_executable_name(const char *s1, const char *s2)
{
#ifdef _WIN32
	bool eq = strcasecmp(s1, s2) == 0;
	if (!eq) {
		char *tmp = format("%s.exe", s2);
		eq = strcasecmp(s1, tmp) == 0;
		free(tmp);
	}
	return eq;
#else
	return str_eq(s1, s2);
#endif
}

// Compute the length of the longest directory path that is common to two
// paths. s1 is assumed to be the path to a directory.
size_t
common_dir_prefix_length(const char *s1, const char *s2)
{
	const char *p1 = s1;
	const char *p2 = s2;

	while (*p1 && *p2 && *p1 == *p2) {
		++p1;
		++p2;
	}
	while ((*p1 && *p1 != '/') || (*p2 && *p2 != '/')) {
		p1--;
		p2--;
	}
	if (!*p1 && !*p2 && p2 == s2 + 1) {
		// Special case for s1 and s2 both being "/".
		return 0;
	}
	return p1 - s1;
}

// Compute a relative path from from (an absolute path to a directory) to to (a
// path). Assumes that both from and to are well-formed and canonical. Caller
// frees.
char *
get_relative_path(const char *from, const char *to)
{
	size_t common_prefix_len;
	char *result;

	assert(from && is_absolute_path(from));
	assert(to);

	if (!*to || !is_absolute_path(to)) {
		return x_strdup(to);
	}

#ifdef _WIN32
	// Paths can be escaped by a slash for use with -isystem.
	if (from[0] == '/') {
		from++;
	}
	if (to[0] == '/') {
		to++;
	}
	// Both paths are absolute, drop the drive letters.
	assert(from[0] == to[0]); // Assume the same drive letter.
	from += 2;
	to += 2;
#endif

	result = x_strdup("");
	common_prefix_len = common_dir_prefix_length(from, to);
	if (common_prefix_len > 0 || !str_eq(from, "/")) {
		const char *p;
		for (p = from + common_prefix_len; *p; p++) {
			if (*p == '/') {
				reformat(&result, "../%s", result);
			}
		}
	}
	if (strlen(to) > common_prefix_len) {
		reformat(&result, "%s%s", result, to + common_prefix_len + 1);
	}
	for (int i = strlen(result) - 1; i >= 0 && result[i] == '/'; i--) {
		result[i] = '\0';
	}
	if (str_eq(result, "")) {
		free(result);
		result = x_strdup(".");
	}
	return result;
}

// Return whether path is absolute.
bool
is_absolute_path(const char *path)
{
#ifdef _WIN32
	return path[0] && path[1] == ':';
#else
	return path[0] == '/';
#endif
}

// Return whether the argument is a full path.
bool
is_full_path(const char *path)
{
	if (strchr(path, '/')) {
		return true;
	}
#ifdef _WIN32
	if (strchr(path, '\\')) {
		return true;
	}
#endif
	return false;
}

bool is_symlink(const char *path)
{
#ifdef _WIN32
	(void)path;
	return false;
#else
	struct stat st;
	return x_lstat(path, &st) == 0 && ((st.st_mode & S_IFMT) == S_IFLNK);
#endif
}

// Update the modification time of a file in the cache to save it from LRU
// cleanup.
void
update_mtime(const char *path)
{
#ifdef HAVE_UTIMES
	utimes(path, NULL);
#else
	utime(path, NULL);
#endif
}

// If exit() already has been called, call _exit(), otherwise exit(). This is
// used to avoid calling exit() inside an atexit handler.
void
x_exit(int status)
{
	static bool first_time = true;
	if (first_time) {
		first_time = false;
		exit(status);
	} else {
		_exit(status);
	}
}

// Rename oldpath to newpath (deleting newpath).
int
x_rename(const char *oldpath, const char *newpath)
{
#ifndef _WIN32
	return rename(oldpath, newpath);
#else
	// Windows' rename() refuses to overwrite an existing file.
	// If the function succeeds, the return value is nonzero.
	if (MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING) == 0) {
		LPVOID lp_msg_buf;
		DWORD dw = GetLastError();
		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, dw,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &lp_msg_buf,
			0,
			NULL);

		LPVOID lp_display_buf = (LPVOID) LocalAlloc(
			LMEM_ZEROINIT,
			(lstrlen((LPCTSTR) lp_msg_buf) + lstrlen((LPCTSTR) __FILE__) + 40)
			* sizeof(TCHAR));
		_snprintf((LPTSTR) lp_display_buf,
		          LocalSize(lp_display_buf) / sizeof(TCHAR),
		          TEXT("%s failed with error %lu: %s"), __FILE__, dw,
		          (const char *)lp_msg_buf);

		cc_log("can't rename file %s to %s OS returned error: %s",
		       oldpath, newpath, (char *) lp_display_buf);

		LocalFree(lp_msg_buf);
		LocalFree(lp_display_buf);
		return -1;
	} else {
		return 0;
	}
#endif
}

// Remove path, NFS hazardous. Use only for temporary files that will not exist
// on other systems. That is, the path should include tmp_string().
int
tmp_unlink(const char *path)
{
	cc_log("Unlink %s", path);
	int rc = unlink(path);
	if (rc) {
		cc_log("Unlink failed: %s", strerror(errno));
	}
	return rc;
}

static int
do_x_unlink(const char *path, bool log_failure)
{
	int saved_errno = 0;

	// If path is on an NFS share, unlink isn't atomic, so we rename to a temp
	// file. We don't care if the temp file is trashed, so it's always safe to
	// unlink it first.
	char *tmp_name = format("%s.ccache.rm.tmp", path);

	int result = 0;
	if (x_rename(path, tmp_name) == -1) {
		result = -1;
		saved_errno = errno;
		goto out;
	}
	if (unlink(tmp_name) == -1) {
		// If it was released in a race, that's OK.
		if (errno != ENOENT && errno != ESTALE) {
			result = -1;
			saved_errno = errno;
		}
	}

out:
	if (result == 0 || log_failure) {
		cc_log("Unlink %s via %s", path, tmp_name);
		if (result != 0 && log_failure) {
			cc_log("x_unlink failed: %s", strerror(saved_errno));
		}
	}
	free(tmp_name);
	errno = saved_errno;
	return result;
}

// Remove path, NFS safe, log both successes and failures.
int
x_unlink(const char *path)
{
	return do_x_unlink(path, true);
}

// Remove path, NFS safe, only log successes.
int
x_try_unlink(const char *path)
{
	return do_x_unlink(path, false);
}

#ifndef _WIN32
// Like readlink() but returns the string or NULL on failure. Caller frees.
char *
x_readlink(const char *path)
{
	long maxlen = path_max(path);
	char *buf = x_malloc(maxlen);
	ssize_t len = readlink(path, buf, maxlen-1);
	if (len == -1) {
		free(buf);
		return NULL;
	}
	buf[len] = 0;
	return buf;
}
#endif

// Reads the content of a file. Size hint 0 means no hint. Returns true on
// success, otherwise false.
bool
read_file(const char *path, size_t size_hint, char **data, size_t *size)
{
	if (size_hint == 0) {
		struct stat st;
		if (x_stat(path, &st) == 0) {
			size_hint = st.st_size;
		}
	}
	size_hint = (size_hint < 1024) ? 1024 : size_hint;

	int fd = open(path, O_RDONLY | O_BINARY);
	if (fd == -1) {
		return false;
	}
	size_t allocated = size_hint;
	*data = x_malloc(allocated);
	int ret;
	size_t pos = 0;
	while (true) {
		if (pos > allocated / 2) {
			allocated *= 2;
			*data = x_realloc(*data, allocated);
		}
		ret = read(fd, *data + pos, allocated - pos);
		if (ret == 0 || (ret == -1 && errno != EINTR)) {
			break;
		}
		if (ret > 0) {
			pos += ret;
		}
	}
	close(fd);
	if (ret == -1) {
		cc_log("Failed reading %s", path);
		free(*data);
		*data = NULL;
		return false;
	}

	*size = pos;
	return true;
}

// Return the content (with NUL termination) of a text file, or NULL on error.
// Caller frees. Size hint 0 means no hint.
char *
read_text_file(const char *path, size_t size_hint)
{
	size_t size;
	char *data;
	if (read_file(path, size_hint, &data, &size)) {
		data = x_realloc(data, size + 1);
		data[size] = '\0';
		return data;
	} else {
		return NULL;
	}
}

static bool
expand_variable(const char **str, char **result, char **errmsg)
{
	assert(**str == '$');

	bool curly;
	const char *p = *str + 1;
	if (*p == '{') {
		curly = true;
		++p;
	} else {
		curly = false;
	}

	const char *q = p;
	while (isalnum(*q) || *q == '_') {
		++q;
	}
	if (curly) {
		if (*q != '}') {
			*errmsg = format("syntax error: missing '}' after \"%s\"", p);
			return false;
		}
	}

	if (q == p) {
		// Special case: don't consider a single $ the start of a variable.
		reformat(result, "%s$", *result);
		return true;
	}

	char *name = x_strndup(p, q - p);
	const char *value = getenv(name);
	if (!value) {
		*errmsg = format("environment variable \"%s\" not set", name);
		free(name);
		return false;
	}
	reformat(result, "%s%s", *result, value);
	if (!curly) {
		--q;
	}
	*str = q;
	free(name);
	return true;
}

// Substitute all instances of $VAR or ${VAR}, where VAR is an environment
// variable, in a string. Caller frees. If one of the environment variables
// doesn't exist, NULL will be returned and *errmsg will be an appropriate
// error message (caller frees).
char *
subst_env_in_string(const char *str, char **errmsg)
{
	assert(errmsg);
	*errmsg = NULL;

	char *result = x_strdup("");
	const char *p = str; // Interval start.
	const char *q = str; // Interval end.
	for (q = str; *q; ++q) {
		if (*q == '$') {
			reformat(&result, "%s%.*s", result, (int)(q - p), p);
			if (!expand_variable(&q, &result, errmsg)) {
				free(result);
				return NULL;
			}
			p = q + 1;
		}
	}
	reformat(&result, "%s%.*s", result, (int)(q - p), p);
	return result;
}

void
set_cloexec_flag(int fd)
{
#ifndef _WIN32
	int flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
#else
	(void)fd;
#endif
}

double time_seconds(void)
{
#ifdef HAVE_GETTIMEOFDAY
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#else
	return (double)time(NULL);
#endif
}
