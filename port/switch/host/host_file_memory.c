/*
HOST_FILE_MEMORY.C

The level being played, kept in memory. The game streams a level's textures,
sounds and geometry from its decompressed copy in save/z (cache000.map and
the others, up to 291 MB each) as it first draws them, and its main thread
waits while its cache thread reads them: on a Switch's SD card, about 60 MB a
second, that was most of the stutter as a level began, whatever the CPU's
clock.

The game opens every cache file as it starts and keeps them open, so a file
is kept in memory once the game reads from it (ACTIVATE_BYTES: the level
being played's), and the one read from least recently is let go when more
than MAXIMUM_KEPT would be. A thread of the host's (filler) reads it in, as
much as its header says its map is, in pieces, politely: a piece waits while
the game has just read, so that the game's own reads go first. The game's
reads of what is in memory are copies (host_file_memory_read); the rest
still go to the card.

The game writes a cache file as it puts a map into it: each write is copied
into memory too (host_file_memory_wrote), and a piece the filler read while
a write was made is read again, so that memory never holds what the file no
longer does. A file deleted, renamed or cut short is let go
(host_file_memory_forget).
*/

#include "host.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXIMUM_FILES 8
#define MAXIMUM_KEPT 2
#define ACTIVATE_BYTES (8u << 20)
/* (of a cache file's header: its map's length, little-endian, at 8) */
#define HEADER_LENGTH_OFFSET 8
#define MINIMUM_SIZE (16u << 20)
#define MAXIMUM_SIZE (300u << 20)
#define PIECE_SIZE (1u << 20)
/* how long a piece waits after the game's last read, and how often it looks */
#define QUIET_NANOSECONDS 20000000ULL
#define WAIT_NANOSECONDS 5000000ULL
#define FILLER_PRIORITY 0x3f
#define FILLER_CORE 2
#define FILLER_STACK (64 * 1024)

struct file_memory
{
	char *path;
	uint64_t size;
	int opens;
	/* the file was deleted, renamed or cut: no longer kept, freed with its
	last close */
	int forgotten;
	/* the game's reads of it, and when it last read */
	uint64_t read_bytes;
	uint64_t last_read;
	/* kept: its bytes, how much of them is the file's (it only grows), the
	writes made, and its filler. The lock is held for every copy in or out */
	Mutex lock;
	unsigned char *bytes;
	volatile uint64_t filled;
	volatile uint32_t writes;
	volatile int quit;
	int kept;
	Thread filler;
	int filler_started;
};

static struct file_memory *memories[MAXIMUM_FILES];
static Mutex memories_lock;
static volatile uint64_t last_game_read;

/* whether a file the guest opens is one of the game's cache files */
static int is_cache_file(const char *path)
{
	const char *name = strrchr(path, '/');
	size_t length;

	if (!name || !strstr(path, "/save/z/"))
		return 0;
	name++;
	length = strlen(name);
	return length > 9 && !strncmp(name, "cache", 5) && !strcmp(name + length - 4, ".map");
}

static void filler(void *context)
{
	struct file_memory *memory = context;
	int fd = open(memory->path, O_RDONLY);
	unsigned char *piece = malloc(PIECE_SIZE);
	uint64_t started = host_now();

	while (fd >= 0 && piece && !memory->quit && memory->filled < memory->size)
	{
		uint64_t offset = memory->filled;
		uint32_t length = memory->size - offset < PIECE_SIZE ? (uint32_t)(memory->size - offset) : PIECE_SIZE;
		uint32_t writes = memory->writes;

		/* (the game's reads first) */
		while (!memory->quit && host_now() - last_game_read < QUIET_NANOSECONDS)
			svcSleepThread((int64_t)WAIT_NANOSECONDS);
		if (memory->quit)
			break;
		if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset || read(fd, piece, length) != (ssize_t)length)
		{
			host_logf(HOST_LOG_WARN, "files: %s cannot be read at %llu MB: not kept further", memory->path,
				(unsigned long long)(offset >> 20));
			break;
		}
		mutexLock(&memory->lock);
		if (memory->writes == writes && !memory->quit)
		{
			memcpy(memory->bytes + offset, piece, length);
			__atomic_store_n(&memory->filled, offset + length, __ATOMIC_RELEASE);
		}
		mutexUnlock(&memory->lock);
	}
	if (memory->size && memory->filled == memory->size)
	{
		host_logf(HOST_LOG_INFO, "files: %s in memory (%llu MB, %.1f s)", memory->path,
			(unsigned long long)(memory->size >> 20), (double)(host_now() - started) / 1e9);
	}
	free(piece);
	if (fd >= 0)
		close(fd);
}

/* lets a kept file's bytes go; memories_lock held */
static void memory_unkeep(struct file_memory *memory)
{
	if (!memory->kept)
		return;
	memory->quit = 1;
	if (memory->filler_started)
	{
		threadWaitForExit(&memory->filler);
		threadClose(&memory->filler);
		memory->filler_started = 0;
	}
	mutexLock(&memory->lock);
	__atomic_store_n(&memory->filled, 0, __ATOMIC_RELEASE);
	free(memory->bytes);
	memory->bytes = NULL;
	memory->kept = 0;
	mutexUnlock(&memory->lock);
}

static void memory_free(struct file_memory *memory)
{
	memory_unkeep(memory);
	free(memory->path);
	free(memory);
}

/* keeps the file: its bytes, and its filler started; memories_lock held */
static void memory_keep(struct file_memory *memory)
{
	struct file_memory *oldest = NULL;
	int index, kept = 0;

	for (index = 0; index < MAXIMUM_FILES; index++)
	{
		if (memories[index] && memories[index]->kept)
		{
			kept++;
			if (!oldest || memories[index]->last_read < oldest->last_read)
				oldest = memories[index];
		}
	}
	if (kept >= MAXIMUM_KEPT && oldest)
	{
		host_logf(HOST_LOG_INFO, "files: %s no longer kept", oldest->path);
		memory_unkeep(oldest);
	}
	memory->bytes = malloc(memory->size);
	if (!memory->bytes)
	{
		host_logf(HOST_LOG_WARN, "files: no memory to keep %s (%llu MB)", memory->path,
			(unsigned long long)(memory->size >> 20));
		memory->size = 0;
		return;
	}
	memory->kept = 1;
	memory->quit = 0;
	memory->filled = 0;
	memory->filler_started = R_SUCCEEDED(threadCreate(&memory->filler, filler, memory, NULL, FILLER_STACK,
		FILLER_PRIORITY, FILLER_CORE)) && R_SUCCEEDED(threadStart(&memory->filler));
	host_logf(HOST_LOG_INFO, "files: keeping %s in memory (%llu MB)", memory->path,
		(unsigned long long)(memory->size >> 20));
}

/* the guest opened path (host_syscall.c, its fd one of the host's): its
record, if it is a cache file, else NULL */
struct file_memory *host_file_memory_open(const char *path, int fd)
{
	struct file_memory *memory = NULL;
	unsigned char header[HEADER_LENGTH_OFFSET + 4];
	uint64_t size;
	off_t end;
	int index, free_slot = -1;

	if (!is_cache_file(path))
		return NULL;
	mutexLock(&memories_lock);
	for (index = 0; index < MAXIMUM_FILES; index++)
	{
		if (memories[index] && !strcmp(memories[index]->path, path))
		{
			memory = memories[index];
			memory->opens++;
			mutexUnlock(&memories_lock);
			return memory;
		}
		if (!memories[index])
			free_slot = index;
	}
	/* (the map's length, from its header; the file's if that is not one) */
	end = lseek(fd, 0, SEEK_END);
	if (free_slot < 0 || end <= 0 || lseek(fd, 0, SEEK_SET) != 0 ||
		read(fd, header, sizeof(header)) != (ssize_t)sizeof(header))
	{
		lseek(fd, 0, SEEK_SET);
		mutexUnlock(&memories_lock);
		return NULL;
	}
	/* (the guest's handle as it was opened: at its start) */
	lseek(fd, 0, SEEK_SET);
	size = (uint64_t)header[HEADER_LENGTH_OFFSET] | (uint64_t)header[HEADER_LENGTH_OFFSET + 1] << 8 |
		(uint64_t)header[HEADER_LENGTH_OFFSET + 2] << 16 | (uint64_t)header[HEADER_LENGTH_OFFSET + 3] << 24;
	if (!size || size > (uint64_t)end)
		size = (uint64_t)end;
	memory = calloc(1, sizeof(*memory));
	if (memory)
		memory->path = strdup(path);
	if (!memory || !memory->path)
	{
		free(memory);
		mutexUnlock(&memories_lock);
		return NULL;
	}
	/* (one too small or too large is never kept) */
	memory->size = size >= MINIMUM_SIZE && size <= MAXIMUM_SIZE ? size : 0;
	memory->opens = 1;
	mutexInit(&memory->lock);
	memories[free_slot] = memory;
	mutexUnlock(&memories_lock);
	return memory;
}

/* the guest closed one of its opens (the record stays, to be found again) */
void host_file_memory_close(struct file_memory *memory)
{
	if (!memory)
		return;
	mutexLock(&memories_lock);
	if (memory->opens > 0)
		memory->opens--;
	if (memory->forgotten && !memory->opens)
		memory_free(memory);
	mutexUnlock(&memories_lock);
}

/* path was deleted, renamed (from or to) or cut short: its memory is no
longer the file's */
void host_file_memory_forget(const char *path)
{
	int index;

	mutexLock(&memories_lock);
	for (index = 0; index < MAXIMUM_FILES; index++)
	{
		struct file_memory *memory = memories[index];

		if (memory && !strcmp(memory->path, path))
		{
			memories[index] = NULL;
			memory_unkeep(memory);
			if (memory->opens)
				memory->forgotten = 1;
			else
				memory_free(memory);
		}
	}
	mutexUnlock(&memories_lock);
}

/* a read of size bytes at offset: copied if memory has them all (returns
size), else 0, for the caller to read the file. The game's reads of a file
make it kept. */
size_t host_file_memory_read(struct file_memory *memory, void *buffer, size_t size, uint64_t offset)
{
	size_t copied = 0;

	last_game_read = host_now();
	if (!memory || !size)
		return 0;
	if (!memory->kept && memory->size && !memory->forgotten)
	{
		memory->read_bytes += size;
		if (memory->read_bytes >= ACTIVATE_BYTES)
		{
			mutexLock(&memories_lock);
			if (!memory->kept && memory->size && !memory->forgotten)
				memory_keep(memory);
			mutexUnlock(&memories_lock);
		}
	}
	memory->last_read = last_game_read;
	if (offset + size > __atomic_load_n(&memory->filled, __ATOMIC_ACQUIRE))
		return 0;
	mutexLock(&memory->lock);
	if (memory->bytes && offset + size <= memory->filled)
	{
		memcpy(buffer, memory->bytes + offset, size);
		copied = size;
	}
	mutexUnlock(&memory->lock);
	return copied;
}

/* the guest wrote size bytes at offset: memory is kept the file's */
void host_file_memory_wrote(struct file_memory *memory, const void *buffer, size_t size, uint64_t offset)
{
	uint64_t end;

	if (!memory || !size)
		return;
	mutexLock(&memory->lock);
	memory->writes++;
	end = offset + size < memory->size ? offset + size : memory->size;
	if (memory->bytes && offset < end)
		memcpy(memory->bytes + offset, buffer, (size_t)(end - offset));
	mutexUnlock(&memory->lock);
}

/* the shutdown: the fillers stop */
void host_file_memory_stop(void)
{
	int index;

	mutexLock(&memories_lock);
	for (index = 0; index < MAXIMUM_FILES; index++)
	{
		if (memories[index])
			memories[index]->quit = 1;
	}
	for (index = 0; index < MAXIMUM_FILES; index++)
	{
		if (memories[index] && memories[index]->filler_started)
		{
			threadWaitForExit(&memories[index]->filler);
			threadClose(&memories[index]->filler);
			memories[index]->filler_started = 0;
		}
	}
	mutexUnlock(&memories_lock);
}
