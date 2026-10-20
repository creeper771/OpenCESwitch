/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h
sends every call here), with Linux's numbers, flags, structures and error
numbers, as the guest was built for. The Android host passes most of them
to Linux; Horizon has no such calls, so each is done here with newlib (files
on the SD card) and libnx:

- files and directories: a table of the guest's file descriptors, each a
  newlib file or directory; getdents64 builds Linux's records from readdir;
- memory: host_memory.c;
- time: the system tick and the time service;
- futexes: svcWaitForAddress and svcSignalToAddress, which have the same
  meaning (requeueing wakes every waiter instead, which musl allows);
- the standard output and error streams: the host's log.

Error numbers are Linux's (LINUX_E*), whatever newlib's are.
*/

#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../../linux/src/posix.h"
#include "host_posix.h"

/* the guest's system call numbers (__NR_*) */
#include "../../android/guest/libc/arch/arm64_32/bits/syscall.h.in"

/* ---------- Linux's values */

#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_EINTR 4
#define LINUX_EIO 5
#define LINUX_EBADF 9
#define LINUX_EAGAIN 11
#define LINUX_ENOMEM 12
#define LINUX_EFAULT 14
#define LINUX_EEXIST 17
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR 21
#define LINUX_EINVAL 22
#define LINUX_EMFILE 24
#define LINUX_ENOTTY 25
#define LINUX_ESPIPE 29
#define LINUX_ENOSYS 38
#define LINUX_ETIMEDOUT 110

#define LINUX_O_ACCMODE 03
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_DIRECTORY 040000

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4

#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_DIR 4
#define LINUX_DT_REG 8

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_COMMAND_MASK 0x7f
#define LINUX_FUTEX_CLOCK_REALTIME 0x100

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_REALTIME_COARSE 5
#define LINUX_TIMER_ABSTIME 1

#define LINUX_SIGABRT 6

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* the AArch64 kernel's struct stat, which the guest reads
(port/android/guest/libc/arch/arm64_32/kstat.h) */
struct linux_stat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

struct linux_utsname
{
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

static const struct
{
	int newlib;
	int linux_value;
} error_numbers[] =
{
	{EPERM, 1}, {ENOENT, 2}, {ESRCH, 3}, {EINTR, 4}, {EIO, 5}, {ENXIO, 6}, {E2BIG, 7}, {ENOEXEC, 8},
	{EBADF, 9}, {ECHILD, 10}, {EAGAIN, 11}, {ENOMEM, 12}, {EACCES, 13}, {EFAULT, 14}, {EBUSY, 16},
	{EEXIST, 17}, {EXDEV, 18}, {ENODEV, 19}, {ENOTDIR, 20}, {EISDIR, 21}, {EINVAL, 22}, {ENFILE, 23},
	{EMFILE, 24}, {ENOTTY, 25}, {EFBIG, 27}, {ENOSPC, 28}, {ESPIPE, 29}, {EROFS, 30}, {EMLINK, 31},
	{EPIPE, 32}, {EDOM, 33}, {ERANGE, 34}, {EDEADLK, 35}, {ENAMETOOLONG, 36}, {ENOLCK, 37},
	{ENOSYS, 38}, {ENOTEMPTY, 39}, {ELOOP, 40}, {EOVERFLOW, 75}, {ENOTSOCK, 88}, {EMSGSIZE, 90},
	{EPROTONOSUPPORT, 93}, {EOPNOTSUPP, 95}, {ENOTSUP, 95}, {EAFNOSUPPORT, 97}, {EADDRINUSE, 98},
	{EADDRNOTAVAIL, 99}, {ENETUNREACH, 101}, {ECONNABORTED, 103}, {ECONNRESET, 104}, {ENOBUFS, 105},
	{EISCONN, 106}, {ENOTCONN, 107}, {ETIMEDOUT, 110}, {ECONNREFUSED, 111}, {EHOSTUNREACH, 113},
	{EALREADY, 114}, {EINPROGRESS, 115},
};

/* a newlib error number as Linux's */
int host_linux_errno(int error)
{
	size_t index;

	if (!error)
		return 0;
	for (index = 0; index < sizeof(error_numbers) / sizeof(error_numbers[0]); index++)
	{
		if (error_numbers[index].newlib == error)
			return error_numbers[index].linux_value;
	}
	return LINUX_EIO;
}

/* the guest's errno after a host function of posix_*.c */
int host_errno(void)
{
	return host_linux_errno(errno);
}

static long result_of(long value)
{
	return value < 0 ? -host_linux_errno(errno) : value;
}

/* ---------- time */

static uint64_t monotonic_nanoseconds(void)
{
	return armTicksToNs(armGetSystemTick());
}

static int64_t realtime_nanoseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return (int64_t)monotonic_nanoseconds();
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int64_t clock_nanoseconds(int clock)
{
	return clock == LINUX_CLOCK_REALTIME || clock == LINUX_CLOCK_REALTIME_COARSE ? realtime_nanoseconds() :
		(int64_t)monotonic_nanoseconds();
}

/* a guest timespec in nanoseconds; -1 if address is 0 */
static int64_t timespec_in(uint64_t address)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return -1;
	return (int64_t)value->seconds * 1000000000LL + value->nanoseconds;
}

static void timespec_out(uint64_t address, int64_t nanoseconds)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)(nanoseconds / 1000000000LL);
	result->nanoseconds = (int32_t)(nanoseconds % 1000000000LL);
}

static void sleep_nanoseconds(int64_t nanoseconds)
{
	if (nanoseconds > 0)
	{
		svcSleepThread(nanoseconds);
		HOST_ADD(sleep_time, (uint64_t)nanoseconds);
	}
}

/* ---------- futexes */

static long futex_result(Result rc)
{
	static int reported;

	if (R_SUCCEEDED(rc))
		return 0;
	if (R_DESCRIPTION(rc) == KernelError_TimedOut)
		return -LINUX_ETIMEDOUT;
	if (R_DESCRIPTION(rc) == KernelError_InvalidState)
		return -LINUX_EAGAIN;
	if (!reported)
	{
		reported = 1;
		host_logf(HOST_LOG_WARN, "a futex wait failed: 0x%x", rc);
	}
	return -LINUX_EINTR;
}

static long futex_wait(void *word, uint32_t value, int64_t nanoseconds)
{
	uint64_t start = host_now();
	long result;

	host_activity.last_wait_address = (uint32_t)(uintptr_t)word;
	__atomic_add_fetch(&host_activity.waiting, 1, __ATOMIC_RELAXED);
	result = futex_result(svcWaitForAddress(word, ArbitrationType_WaitIfEqual, (int32_t)value, nanoseconds));
	__atomic_sub_fetch(&host_activity.waiting, 1, __ATOMIC_RELAXED);
	HOST_ADD(wait_time, host_now() - start);
	return result;
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2,
	uint32_t value3)
{
	void *word = GUEST(void *, address);
	int command = operation & LINUX_FUTEX_COMMAND_MASK;
	int64_t nanoseconds;

	(void)address2;
	if (address & 3)
		return -LINUX_EINVAL;
	switch (command)
	{
	case LINUX_FUTEX_WAIT:
		nanoseconds = timespec_in(timeout);
		return futex_wait(word, value, nanoseconds);
	case LINUX_FUTEX_WAIT_BITSET:
		/* an absolute timeout */
		nanoseconds = timespec_in(timeout);
		if (nanoseconds >= 0)
		{
			nanoseconds -= (operation & LINUX_FUTEX_CLOCK_REALTIME) ? realtime_nanoseconds() :
				(int64_t)monotonic_nanoseconds();
			if (nanoseconds <= 0)
				return -LINUX_ETIMEDOUT;
		}
		return futex_wait(word, value, nanoseconds);
	case LINUX_FUTEX_WAKE:
	case LINUX_FUTEX_WAKE_BITSET:
		svcSignalToAddress(word, SignalType_Signal, 0, (int32_t)value <= 0 ? -1 : (int32_t)value);
		return 0;
	case LINUX_FUTEX_CMP_REQUEUE:
		if (*(volatile uint32_t *)word != value3)
			return -LINUX_EAGAIN;
		/* fall through */
	case LINUX_FUTEX_REQUEUE:
		/* every waiter wakes, and waits again where it would have been
		moved to */
		svcSignalToAddress(word, SignalType_Signal, 0, -1);
		return 0;
	default:
		return -LINUX_ENOSYS;
	}
}

/* ---------- standard output and error */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static Mutex log_stream_lock;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	mutexLock(&log_stream_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_logf(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, "%s", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	mutexUnlock(&log_stream_lock);
}

/* ---------- the guest's file descriptors */

enum
{
	FILE_NONE,
	FILE_STREAM,
	FILE_FILE,
	FILE_DIRECTORY
};

#define MAXIMUM_FILES 256

struct guest_file
{
	int kind;
	int fd;
	DIR *directory;
	char *path;
	int flags;
	/* directories: the entry that did not fit in the last getdents64 */
	int has_pending;
	struct dirent pending;
	int64_t position;
	/* a cache file's memory (host_file_memory.c), or NULL */
	struct file_memory *memory;
};

static struct guest_file files[MAXIMUM_FILES] =
{
	{FILE_STREAM, 0, NULL, NULL, 0, 0, {0}, 0, NULL},
	{FILE_STREAM, 1, NULL, NULL, 1, 0, {0}, 0, NULL},
	{FILE_STREAM, 2, NULL, NULL, 1, 0, {0}, 0, NULL},
};
static Mutex files_lock;

static struct guest_file *file_of(long long fd)
{
	if (fd < 0 || fd >= MAXIMUM_FILES || files[fd].kind == FILE_NONE)
		return NULL;
	return &files[fd];
}

static int file_new(void)
{
	int fd;

	for (fd = 3; fd < MAXIMUM_FILES; fd++)
	{
		if (files[fd].kind == FILE_NONE)
			return fd;
	}
	return -1;
}

/* a guest path, relative to the directory dirfd names; NULL on failure,
with *error set (a Linux error number) */
static char *path_at(long long dirfd, uint64_t address, int *error)
{
	const char *path = GUEST(const char *, address);
	struct guest_file *directory;
	char *result;

	if (!address)
	{
		*error = LINUX_EFAULT;
		return NULL;
	}
	if (path[0] == '/' || dirfd == LINUX_AT_FDCWD)
		return strdup(path);
	directory = file_of(dirfd);
	if (!directory || directory->kind != FILE_DIRECTORY)
	{
		*error = directory ? LINUX_ENOTDIR : LINUX_EBADF;
		return NULL;
	}
	result = malloc(strlen(directory->path) + strlen(path) + 2);
	if (result)
		sprintf(result, "%s/%s", directory->path, path);
	return result;
}

static void stat_out(uint64_t address, const struct stat *information)
{
	struct linux_stat *result = GUEST(struct linux_stat *, address);

	memset(result, 0, sizeof(*result));
	result->st_ino = (uint64_t)information->st_ino;
	/* (newlib's file type bits are Linux's) */
	result->st_mode = (uint32_t)information->st_mode;
	result->st_nlink = 1;
	result->st_size = (int64_t)information->st_size;
	result->st_blksize = 512;
	result->st_blocks = (result->st_size + 511) / 512;
	result->st_atime_sec = (int64_t)information->st_atime;
	result->st_mtime_sec = (int64_t)information->st_mtime;
	result->st_ctime_sec = (int64_t)information->st_ctime;
}

static long guest_openat(long long dirfd, uint64_t address, int flags, int mode)
{
	int error = LINUX_ENOMEM;
	char *path = path_at(dirfd, address, &error);
	struct stat information;
	int is_directory, fd, host_flags;

	if (!path)
		return -error;
	is_directory = stat(path, &information) == 0 && S_ISDIR(information.st_mode);
	if ((flags & LINUX_O_DIRECTORY) && !is_directory)
	{
		free(path);
		return -(errno == ENOENT ? LINUX_ENOENT : LINUX_ENOTDIR);
	}
	mutexLock(&files_lock);
	fd = file_new();
	if (fd < 0)
	{
		mutexUnlock(&files_lock);
		free(path);
		return -LINUX_EMFILE;
	}
	if (is_directory)
	{
		if ((flags & LINUX_O_ACCMODE) != 0)
		{
			mutexUnlock(&files_lock);
			free(path);
			return -LINUX_EISDIR;
		}
		files[fd].directory = opendir(path);
		if (!files[fd].directory)
		{
			error = host_linux_errno(errno);
			mutexUnlock(&files_lock);
			free(path);
			return -error;
		}
		files[fd].kind = FILE_DIRECTORY;
	}
	else
	{
		host_flags = (flags & LINUX_O_ACCMODE) == 2 ? O_RDWR : (flags & LINUX_O_ACCMODE) == 1 ? O_WRONLY : O_RDONLY;
		if (flags & LINUX_O_CREAT)
			host_flags |= O_CREAT;
		if (flags & LINUX_O_EXCL)
			host_flags |= O_EXCL;
		if (flags & LINUX_O_TRUNC)
			host_flags |= O_TRUNC;
		if (flags & LINUX_O_APPEND)
			host_flags |= O_APPEND;
		files[fd].fd = open(path, host_flags, mode);
		if (files[fd].fd < 0)
		{
			error = host_linux_errno(errno);
			mutexUnlock(&files_lock);
			free(path);
			return -error;
		}
		files[fd].kind = FILE_FILE;
		files[fd].memory = host_file_memory_open(path, files[fd].fd);
	}
	files[fd].path = path;
	files[fd].flags = flags;
	files[fd].has_pending = 0;
	files[fd].position = 0;
	mutexUnlock(&files_lock);
	return fd;
}

static long guest_close(long long fd)
{
	struct guest_file *file;
	long result = 0;

	mutexLock(&files_lock);
	file = file_of(fd);
	if (!file || file->kind == FILE_STREAM)
	{
		mutexUnlock(&files_lock);
		return file ? 0 : -LINUX_EBADF;
	}
	if (file->kind == FILE_FILE)
	{
		host_file_memory_close(file->memory);
		result = result_of(close(file->fd));
	}
	else
		closedir(file->directory);
	free(file->path);
	memset(file, 0, sizeof(*file));
	mutexUnlock(&files_lock);
	return result;
}

static long guest_read(long long fd, void *buffer, size_t size)
{
	struct guest_file *file = file_of(fd);

	if (!file)
		return -LINUX_EBADF;
	if (file->kind == FILE_STREAM)
		return 0;
	if (file->kind == FILE_DIRECTORY)
		return -LINUX_EISDIR;
	{
		uint64_t start = host_now();
		long result;

		/* (in memory: copied, and the position moved past it) */
		if (file->memory)
		{
			off_t position = lseek(file->fd, 0, SEEK_CUR);

			if (position >= 0 && host_file_memory_read(file->memory, buffer, size, (uint64_t)position) == size)
			{
				lseek(file->fd, position + (off_t)size, SEEK_SET);
				return (long)size;
			}
		}
		result = result_of(read(file->fd, buffer, size));
		HOST_ADD(read_time, host_now() - start);
		if (result > 0)
			HOST_ADD(read_bytes, (uint64_t)result);
		return result;
	}
}

static long guest_write(long long fd, const void *buffer, size_t size)
{
	struct guest_file *file = file_of(fd);

	if (!file)
		return -LINUX_EBADF;
	if (file->kind == FILE_STREAM)
	{
		if (fd == 0)
			return -LINUX_EBADF;
		log_bytes((int)fd, buffer, size);
		return (long)size;
	}
	if (file->kind == FILE_DIRECTORY)
		return -LINUX_EISDIR;
	{
		off_t position = file->memory ? lseek(file->fd, 0, SEEK_CUR) : 0;
		long result = result_of(write(file->fd, buffer, size));

		if (file->memory && result > 0 && position >= 0)
			host_file_memory_wrote(file->memory, buffer, (size_t)result, (uint64_t)position);
		return result;
	}
}

static long guest_seek(long long fd, int64_t offset, int whence)
{
	struct guest_file *file = file_of(fd);

	if (!file)
		return -LINUX_EBADF;
	if (file->kind == FILE_STREAM)
		return -LINUX_ESPIPE;
	if (file->kind == FILE_DIRECTORY)
	{
		if (offset != 0 || whence != SEEK_SET)
			return -LINUX_EINVAL;
		rewinddir(file->directory);
		file->has_pending = 0;
		file->position = 0;
		return 0;
	}
	return result_of((long)lseek(file->fd, (off_t)offset, whence));
}

/* reads or writes at an offset, keeping the file's position */
static long guest_positional(long long fd, void *buffer, size_t size, int64_t offset, int writing)
{
	struct guest_file *file = file_of(fd);
	off_t position;
	long result;

	if (!file || file->kind != FILE_FILE)
		return file ? -LINUX_ESPIPE : -LINUX_EBADF;
	position = lseek(file->fd, 0, SEEK_CUR);
	if (position < 0 || lseek(file->fd, (off_t)offset, SEEK_SET) < 0)
		return -host_linux_errno(errno);
	{
		uint64_t start = host_now();

		if (!writing && file->memory &&
			host_file_memory_read(file->memory, buffer, size, (uint64_t)offset) == size)
		{
			lseek(file->fd, position, SEEK_SET);
			return (long)size;
		}
		result = writing ? result_of(write(file->fd, buffer, size)) : result_of(read(file->fd, buffer, size));
		if (writing && file->memory && result > 0)
			host_file_memory_wrote(file->memory, buffer, (size_t)result, (uint64_t)offset);
		if (!writing)
		{
			HOST_ADD(read_time, host_now() - start);
			if (result > 0)
				HOST_ADD(read_bytes, (uint64_t)result);
		}
	}
	lseek(file->fd, position, SEEK_SET);
	return result;
}

static long guest_vector(long long fd, uint64_t vector, int count, int64_t offset, int positional, int writing)
{
	const struct guest_iovec *guest_vectors = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -LINUX_EINVAL;
	for (index = 0; index < count; index++)
	{
		void *base = GUEST(void *, guest_vectors[index].base);
		size_t length = guest_vectors[index].length;
		long result;

		if (!length)
			continue;
		if (positional)
			result = guest_positional(fd, base, length, offset + total, writing);
		else if (writing)
			result = guest_write(fd, base, length);
		else
			result = guest_read(fd, base, length);
		if (result < 0)
			return total ? total : result;
		total += result;
		if ((size_t)result < length)
			break;
	}
	return total;
}

static long guest_fstat(long long fd, uint64_t address)
{
	struct guest_file *file = file_of(fd);
	struct stat information;

	if (!file)
		return -LINUX_EBADF;
	memset(&information, 0, sizeof(information));
	if (file->kind == FILE_STREAM)
	{
		information.st_mode = S_IFCHR | 0620;
	}
	else if (file->kind == FILE_DIRECTORY)
	{
		if (stat(file->path, &information) != 0)
			return -host_linux_errno(errno);
	}
	else if (fstat(file->fd, &information) != 0)
	{
		return -host_linux_errno(errno);
	}
	stat_out(address, &information);
	return 0;
}

static long guest_fstatat(long long dirfd, uint64_t path_address, uint64_t address, int flags)
{
	int error = LINUX_ENOMEM;
	struct stat information;
	char *path;
	long result;

	if ((flags & LINUX_AT_EMPTY_PATH) && path_address && !*GUEST(const char *, path_address))
		return guest_fstat(dirfd, address);
	path = path_at(dirfd, path_address, &error);
	if (!path)
		return -error;
	memset(&information, 0, sizeof(information));
	result = result_of(stat(path, &information));
	free(path);
	if (result == 0)
		stat_out(address, &information);
	return result;
}

/* Linux's struct linux_dirent64 records from readdir */
static long guest_getdents(long long fd, uint64_t address, uint32_t size)
{
	struct guest_file *file = file_of(fd);
	uint8_t *buffer = GUEST(uint8_t *, address);
	uint32_t used = 0;

	if (!file)
		return -LINUX_EBADF;
	if (file->kind != FILE_DIRECTORY)
		return -LINUX_ENOTDIR;
	for (;;)
	{
		struct dirent *entry;
		size_t name_length, record;
		uint8_t *out;
		uint64_t inode;
		int64_t next;

		if (file->has_pending)
		{
			entry = &file->pending;
		}
		else
		{
			errno = 0;
			entry = readdir(file->directory);
			if (!entry)
				break;
		}
		name_length = strlen(entry->d_name);
		record = (19 + name_length + 1 + 7) & ~(size_t)7;
		if (used + record > size)
		{
			if (!file->has_pending)
			{
				file->pending = *entry;
				file->has_pending = 1;
			}
			if (!used)
				return -LINUX_EINVAL;
			return used;
		}
		file->has_pending = 0;
		out = buffer + used;
		inode = (uint64_t)file->position + 1;
		next = file->position + 1;
		memcpy(out, &inode, 8);
		memcpy(out + 8, &next, 8);
		out[16] = (uint8_t)record;
		out[17] = (uint8_t)(record >> 8);
		out[18] = entry->d_type == DT_DIR ? LINUX_DT_DIR : entry->d_type == DT_REG ? LINUX_DT_REG : LINUX_DT_UNKNOWN;
		memcpy(out + 19, entry->d_name, name_length + 1);
		memset(out + 19 + name_length + 1, 0, record - (19 + name_length + 1));
		used += (uint32_t)record;
		file->position++;
	}
	return used;
}

/* (a file deleted: the memory of it too, host_file_memory.c) */
static int unlink_forgetting(const char *path)
{
	host_file_memory_forget(path);
	return unlink(path);
}

static long path_call(long long dirfd, uint64_t address, int (*call)(const char *))
{
	int error = LINUX_ENOMEM;
	char *path = path_at(dirfd, address, &error);
	long result;

	if (!path)
		return -error;
	result = result_of(call(path));
	free(path);
	return result;
}

static int exists(const char *path)
{
	struct stat information;

	return stat(path, &information);
}

static int make_directory(const char *path)
{
	return mkdir(path, 0777);
}

static long guest_renameat(long long old_dirfd, uint64_t old_address, long long new_dirfd, uint64_t new_address)
{
	int error = LINUX_ENOMEM;
	char *old_path = path_at(old_dirfd, old_address, &error);
	char *new_path = old_path ? path_at(new_dirfd, new_address, &error) : NULL;
	long result;

	if (!old_path || !new_path)
	{
		free(old_path);
		return -error;
	}
	host_file_memory_forget(old_path);
	host_file_memory_forget(new_path);
	/* Linux replaces an existing file; the SD card's file system does not */
	if (rename(old_path, new_path) == 0)
		result = 0;
	else if (errno == EEXIST && unlink(new_path) == 0)
		result = result_of(rename(old_path, new_path));
	else
		result = -host_linux_errno(errno);
	free(old_path);
	free(new_path);
	return result;
}

static long guest_getcwd(uint64_t address, uint32_t size)
{
	char path[1024];
	const char *shown;
	size_t length;

	if (!getcwd(path, sizeof(path)))
		return -host_linux_errno(errno);
	/* without the device ("sdmc:"), which the guest's paths leave out */
	shown = strchr(path, ':') ? strchr(path, ':') + 1 : path;
	length = strlen(shown) + 1;
	if (length > size)
		return -34; /* ERANGE */
	memcpy(GUEST(char *, address), shown, length);
	return (long)length;
}

/* the shutdown: the guest's files and directories are closed */
void host_files_close(void)
{
	int fd;

	host_file_memory_stop();
	mutexLock(&files_lock);
	for (fd = 3; fd < MAXIMUM_FILES; fd++)
	{
		if (files[fd].kind == FILE_FILE)
			close(files[fd].fd);
		else if (files[fd].kind == FILE_DIRECTORY)
			closedir(files[fd].directory);
		free(files[fd].path);
		memset(&files[fd], 0, sizeof(files[fd]));
	}
	mutexUnlock(&files_lock);
}

/* ---------- posix_files.c's functions that take a descriptor: the guest's
descriptors are this file's, not newlib's */

static int newlib_descriptor(int descriptor)
{
	struct guest_file *file = file_of(descriptor);

	if (!file || file->kind != FILE_FILE)
	{
		errno = file ? EINVAL : EBADF;
		return -1;
	}
	return file->fd;
}

int host_posix_fstat(int descriptor, struct posix_file_information *information)
{
	int fd = newlib_descriptor(descriptor);

	return fd < 0 ? -1 : posix_fstat(fd, information);
}

int host_posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	int fd = newlib_descriptor(descriptor);

	return fd < 0 ? -1 : posix_seek(fd, offset_low, offset_high, whence, position_low, position_high);
}

int host_posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	int fd = newlib_descriptor(descriptor);

	return fd < 0 ? -1 : posix_truncate(fd, size_low, size_high);
}

/* utimensat for posix_files.c (host_posix.h), on utimes */
int host_posix_utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
	struct timeval values[2];
	struct stat information;
	int index;

	(void)directory;
	(void)flags;
	if (times[0].tv_nsec == UTIME_OMIT && times[1].tv_nsec == UTIME_OMIT)
		return 0;
	if (stat(path, &information) != 0)
		return -1;
	values[0].tv_sec = information.st_atime;
	values[1].tv_sec = information.st_mtime;
	values[0].tv_usec = values[1].tv_usec = 0;
	for (index = 0; index < 2; index++)
	{
		if (times[index].tv_nsec == UTIME_NOW)
		{
			values[index].tv_sec = time(NULL);
		}
		else if (times[index].tv_nsec != UTIME_OMIT)
		{
			values[index].tv_sec = times[index].tv_sec;
			values[index].tv_usec = times[index].tv_nsec / 1000;
		}
	}
	return utimes(path, values);
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	__atomic_add_fetch(&host_activity.system_calls, 1, __ATOMIC_RELAXED);
	host_profile_syscall(number);
	switch (number)
	{
	case __NR_read:
		return guest_read(a, GUEST(void *, b), (size_t)(uint32_t)c);
	case __NR_write:
		return guest_write(a, GUEST(const void *, b), (size_t)(uint32_t)c);
	case __NR_readv:
		return guest_vector(a, (uint64_t)b, (int)c, 0, 0, 0);
	case __NR_writev:
		return guest_vector(a, (uint64_t)b, (int)c, 0, 0, 1);
	case __NR_preadv:
		return guest_vector(a, (uint64_t)b, (int)c, d, 1, 0);
	case __NR_pwritev:
		return guest_vector(a, (uint64_t)b, (int)c, d, 1, 1);
	case __NR_pread64:
		return guest_positional(a, GUEST(void *, b), (size_t)(uint32_t)c, d, 0);
	case __NR_pwrite64:
		return guest_positional(a, GUEST(void *, b), (size_t)(uint32_t)c, d, 1);
	case __NR_openat:
		return guest_openat(a, (uint64_t)b, (int)c, (int)d);
	case __NR_close:
		return guest_close(a);
	case __NR_lseek:
		return guest_seek(a, b, (int)c);
	case __NR_fstat:
		return guest_fstat(a, (uint64_t)b);
	case __NR_newfstatat:
		return guest_fstatat(a, (uint64_t)b, (uint64_t)c, (int)d);
	case __NR_getdents64:
		return guest_getdents(a, (uint64_t)b, (uint32_t)c);
	case __NR_faccessat:
		return path_call(a, (uint64_t)b, exists);
	case __NR_mkdirat:
		return path_call(a, (uint64_t)b, make_directory);
	case __NR_unlinkat:
		return path_call(a, (uint64_t)b, ((int)c & LINUX_AT_REMOVEDIR) ? rmdir : unlink_forgetting);
	case __NR_renameat:
	case __NR_renameat2:
		return guest_renameat(a, (uint64_t)b, c, (uint64_t)d);
	case __NR_chdir:
	{
		int error = LINUX_ENOMEM;
		char *path = path_at(LINUX_AT_FDCWD, (uint64_t)a, &error);
		long result;

		if (!path)
			return -error;
		result = result_of(chdir(path));
		free(path);
		return result;
	}
	case __NR_getcwd:
		return guest_getcwd((uint64_t)a, (uint32_t)b);
	case __NR_ftruncate:
	{
		struct guest_file *file = file_of(a);

		if (!file || file->kind != FILE_FILE)
			return file ? -LINUX_EINVAL : -LINUX_EBADF;
		if (file->memory)
		{
			host_file_memory_forget(file->path);
			host_file_memory_close(file->memory);
			file->memory = NULL;
		}
		return result_of(ftruncate(file->fd, (off_t)b));
	}
	case __NR_fsync:
	case __NR_fdatasync:
	{
		struct guest_file *file = file_of(a);

		if (!file)
			return -LINUX_EBADF;
		return file->kind == FILE_FILE ? result_of(fsync(file->fd)) : 0;
	}
	case __NR_fcntl:
	{
		struct guest_file *file = file_of(a);

		if (!file)
			return -LINUX_EBADF;
		switch ((int)b)
		{
		case LINUX_F_GETFD:
		case LINUX_F_SETFD:
		case LINUX_F_SETFL:
			return 0;
		case LINUX_F_GETFL:
			return file->flags;
		default:
			return -LINUX_EINVAL;
		}
	}
	case __NR_ioctl:
		return -LINUX_ENOTTY;
	case __NR_utimensat:
	case __NR_fchmod:
	case __NR_fchmodat:
	case __NR_flock:
		return 0;
	case __NR_umask:
		return 022;
	case __NR_readlinkat:
		return -LINUX_EINVAL;

	case __NR_clock_gettime:
		timespec_out((uint64_t)b, clock_nanoseconds((int)a));
		return 0;
	case __NR_clock_getres:
		timespec_out((uint64_t)b, 52);
		return 0;
	case __NR_gettimeofday:
	{
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);
		int64_t now = realtime_nanoseconds();

		if (result)
		{
			result->seconds = (int32_t)(now / 1000000000LL);
			result->nanoseconds = (int32_t)((now % 1000000000LL) / 1000);
		}
		return 0;
	}
	case __NR_nanosleep:
	{
		int64_t request = timespec_in((uint64_t)a);

		if (request < 0)
			return -LINUX_EFAULT;
		sleep_nanoseconds(request);
		return 0;
	}
	case __NR_clock_nanosleep:
	{
		int64_t request = timespec_in((uint64_t)c);

		if (request < 0)
			return -LINUX_EFAULT;
		if ((int)b & LINUX_TIMER_ABSTIME)
			request -= clock_nanoseconds((int)a);
		sleep_nanoseconds(request);
		return 0;
	}
	case __NR_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case __NR_sched_yield:
		svcSleepThread(YieldType_WithCoreMigration);
		return 0;

	case __NR_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case __NR_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case __NR_mprotect:
	case __NR_madvise:
		return 0;
	case __NR_mremap:
	case __NR_brk:
		/* musl then falls back to mmap and copying */
		return -LINUX_ENOMEM;

	case __NR_exit:
	case __NR_exit_group:
		host_exit((int)a);

	case __NR_set_tid_address:
	case __NR_gettid:
		return (long long)host_thread_id();
	case __NR_getpid:
		return 1;
	case __NR_getppid:
	case __NR_getuid:
	case __NR_geteuid:
	case __NR_getgid:
	case __NR_getegid:
		return 0;
	case __NR_rt_sigprocmask:
		if (c && (uint32_t)d <= 128)
			memset(GUEST(void *, c), 0, (size_t)(uint32_t)d);
		return 0;
	case __NR_rt_sigaction:
	case __NR_sigaltstack:
		return 0;
	case __NR_kill:
	case __NR_tkill:
	case __NR_tgkill:
	{
		int signal_number = (int)(number == __NR_tgkill ? c : b);

		if (!signal_number)
			return 0;
		if (signal_number == LINUX_SIGABRT)
			host_abort("the guest called abort()");
		host_logf(HOST_LOG_WARN, "the guest sent signal %d, which is ignored", signal_number);
		return 0;
	}
	case __NR_getrandom:
		randomGet(GUEST(void *, a), (size_t)(uint32_t)b);
		return (uint32_t)b;
	case __NR_uname:
	{
		struct linux_utsname *name = GUEST(struct linux_utsname *, a);

		memset(name, 0, sizeof(*name));
		strcpy(name->sysname, "Horizon");
		strcpy(name->nodename, "switch");
		strcpy(name->release, "1");
		strcpy(name->version, "1");
		strcpy(name->machine, "aarch64");
		return 0;
	}

	case __NR_sysinfo:
	case __NR_statx:
	case __NR_statfs:
	case __NR_fstatfs:
	case __NR_prlimit64:
	case __NR_getrlimit:
	case __NR_membarrier:
	case __NR_dup:
	case __NR_dup3:
	case __NR_pipe2:
	case __NR_ppoll:
	case __NR_pselect6:
	case __NR_clone:
	case __NR_execve:
		return -LINUX_ENOSYS;

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -LINUX_ENOSYS;
	}
}
