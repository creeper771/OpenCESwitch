/*
HOST_MEMORY.C

The guest's memory, all below 4 GB.

Horizon places a homebrew process's heap, alias and stack regions above
4 GB, so neither the heap nor svcMapMemory can give the guest its memory.
svcMapProcessCodeMemory can: it maps heap memory again anywhere free in the
address space region, which covers the low 4 GB, as the homebrew loader
maps each NRO. port/switch/feasibility/memory_probe checked it on a Switch.
Every guest mapping is therefore a range of heap memory mapped again:

- fixed ranges: the Xbox window at 0x80000000 (mapped at start-up), the
  image (host_loader.c), and any free range the guest asks for at a fixed
  address (the Custom Edition tag cache, port/linux/src/xbox_memory.c);
- pools: 64 MB ranges (or larger, for a larger request) in free gaps below
  4 GB, kept out of the fixed addresses the guest uses, from which the
  guest's other mappings (musl's malloc, thread stacks) take pages.

A mapping is never returned to the heap: the guest's munmap frees pool
pages for reuse, and the guest's fixed requests over a mapped range only
zero it, as fresh anonymous pages would be. mprotect has no effect, and
page faults cannot be caught (there are no signals), so the textures' write
tracking compares page hashes (host_watch_hash.c), as the Android host does
where it cannot catch faults.
*/

#include "host.h"
#include "../../android/host/host_watch_hash.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Linux's mmap flags and protections, as the guest passes them */
#define LINUX_PROT_NONE 0
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

#define POOL_SIZE 0x04000000ULL
#define POOL_ALIGNMENT 0x10000ULL
#define MAXIMUM_MAPPINGS 256

/* the Custom Edition tag cache's address and size
(port/linux/game/cache_file_formats.h) */
#define CUSTOM_EDITION_TAG_CACHE_ADDRESS 0x40440000ULL
#define CUSTOM_EDITION_TAG_CACHE_BYTES 0x01700000ULL

struct mapping
{
	uint64_t base;
	uint64_t size;
	void *source;
	/* pools only: a byte per page, nonzero when the guest has it */
	uint8_t *pages;
	uint64_t free_pages;
};

/* the ranges the guest asks for at fixed addresses, which pools keep out of */
static const struct
{
	uint64_t base;
	uint64_t size;
} fixed_ranges[] =
{
	{CUSTOM_EDITION_TAG_CACHE_ADDRESS, CUSTOM_EDITION_TAG_CACHE_BYTES},
	{HALO_GUEST_WINDOW_BASE, (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE - HALO_GUEST_WINDOW_BASE},
};

static struct mapping mappings[MAXIMUM_MAPPINGS];
static int mapping_count;
static Mutex memory_lock;
static Handle process = INVALID_HANDLE;
static uint64_t search_start;

static uint64_t round_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int overlaps(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size)
{
	return a < b + b_size && b < a + a_size;
}

/* ---------- mapping heap memory below 4 GB */

static int range_is_free(uint64_t address, uint64_t size)
{
	MemoryInfo info;
	u32 page_info;

	if (R_FAILED(svcQueryMemory(&info, &page_info, address)))
		return 0;
	return (info.type & 0xff) == MemType_Unmapped && info.addr + info.size >= address + size;
}

/* a free range of size bytes below 4 GB, outside the fixed ranges; 0 if none */
static uint64_t find_free(uint64_t size)
{
	uint64_t address = search_start;

	while (address < HOST_LOW_LIMIT)
	{
		MemoryInfo info;
		u32 page_info;
		uint64_t end;

		if (R_FAILED(svcQueryMemory(&info, &page_info, address)))
			return 0;
		end = info.addr + info.size;
		if (end > HOST_LOW_LIMIT)
			end = HOST_LOW_LIMIT;
		if ((info.type & 0xff) == MemType_Unmapped)
		{
			uint64_t candidate = round_up(address, POOL_ALIGNMENT);
			size_t index;
			int moved = 1;

			while (moved)
			{
				moved = 0;
				for (index = 0; index < sizeof(fixed_ranges) / sizeof(fixed_ranges[0]); index++)
				{
					if (overlaps(candidate, size, fixed_ranges[index].base, fixed_ranges[index].size))
					{
						candidate = round_up(fixed_ranges[index].base + fixed_ranges[index].size, POOL_ALIGNMENT);
						moved = 1;
					}
				}
			}
			if (candidate + size <= end)
				return candidate;
		}
		if (info.addr + info.size <= address)
			return 0;
		address = info.addr + info.size;
	}
	return 0;
}

static struct mapping *add_mapping(uint64_t address, uint64_t size, void *source, int read_write, const char *what)
{
	struct mapping *mapping;
	int allocated = 0;
	Result rc;

	if (mapping_count == MAXIMUM_MAPPINGS)
	{
		host_logf(HOST_LOG_ERROR, "too many guest mappings for %s", what);
		return NULL;
	}
	if (!source)
	{
		source = aligned_alloc(HOST_PAGE, size);
		if (!source)
		{
			host_logf(HOST_LOG_ERROR, "no heap memory for %s (%llu MB)", what, (unsigned long long)(size >> 20));
			return NULL;
		}
		allocated = 1;
	}
	virtmemLock();
	rc = svcMapProcessCodeMemory(process, address, (uint64_t)(uintptr_t)source, size);
	virtmemUnlock();
	if (R_SUCCEEDED(rc) && read_write)
		rc = svcSetProcessMemoryPermission(process, address, size, Perm_Rw);
	if (R_FAILED(rc))
	{
		host_logf(HOST_LOG_ERROR, "cannot map %s at %08llx-%08llx: 0x%x", what, (unsigned long long)address,
			(unsigned long long)(address + size), rc);
		if (allocated)
			free(source);
		return NULL;
	}
	mapping = &mappings[mapping_count++];
	memset(mapping, 0, sizeof(*mapping));
	mapping->base = address;
	mapping->size = size;
	mapping->source = source;
	host_logf(HOST_LOG_INFO, "mapped %s at %08llx-%08llx", what, (unsigned long long)address,
		(unsigned long long)(address + size));
	return mapping;
}

static struct mapping *mapping_of(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < mapping_count; index++)
	{
		struct mapping *mapping = &mappings[index];

		if (address >= mapping->base && address + size <= mapping->base + mapping->size)
			return mapping;
	}
	return NULL;
}

static int touches_mapping(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < mapping_count; index++)
	{
		if (overlaps(address, size, mappings[index].base, mappings[index].size))
			return 1;
	}
	return 0;
}

int host_map_fixed(uint64_t address, uint64_t size, void *source, int read_write, const char *what)
{
	struct mapping *mapping;

	mutexLock(&memory_lock);
	mapping = add_mapping(address, size, source, read_write, what);
	mutexUnlock(&memory_lock);
	return mapping ? 0 : -1;
}

int host_set_permission(uint64_t address, uint64_t size, uint32_t permission)
{
	Result rc = svcSetProcessMemoryPermission(process, address, size, permission);

	if (R_FAILED(rc))
		host_logf(HOST_LOG_ERROR, "cannot set permission %u on %08llx-%08llx: 0x%x", permission,
			(unsigned long long)address, (unsigned long long)(address + size), rc);
	return R_SUCCEEDED(rc) ? 0 : -1;
}

/* ---------- pools */

static void *pool_take(struct mapping *pool, uint64_t count)
{
	uint64_t total = pool->size / HOST_PAGE;
	uint64_t page, run = 0;

	if (pool->free_pages < count)
		return NULL;
	for (page = 0; page < total; page++)
	{
		if (pool->pages[page])
		{
			run = 0;
			continue;
		}
		if (++run == count)
		{
			uint64_t first = page + 1 - count;

			memset(pool->pages + first, 1, count);
			pool->free_pages -= count;
			return (void *)(uintptr_t)(pool->base + first * HOST_PAGE);
		}
	}
	return NULL;
}

/* pages of a pool, zeroed, as fresh anonymous memory is */
static void *pages_take(uint64_t size)
{
	uint64_t count = size / HOST_PAGE;
	uint64_t pool_size, address;
	struct mapping *pool;
	void *result = NULL;
	int index;

	mutexLock(&memory_lock);
	for (index = 0; index < mapping_count && !result; index++)
	{
		if (mappings[index].pages)
			result = pool_take(&mappings[index], count);
	}
	if (!result)
	{
		pool_size = size > POOL_SIZE ? round_up(size, POOL_SIZE) : POOL_SIZE;
		address = find_free(pool_size);
		pool = address ? add_mapping(address, pool_size, NULL, 1, "a pool") : NULL;
		if (pool)
		{
			pool->pages = calloc(pool_size / HOST_PAGE, 1);
			pool->free_pages = pool_size / HOST_PAGE;
			if (pool->pages)
				result = pool_take(pool, count);
		}
	}
	mutexUnlock(&memory_lock);
	if (result)
		memset(result, 0, size);
	return result;
}

static void pages_give_back(uint64_t address, uint64_t size)
{
	struct mapping *pool;

	mutexLock(&memory_lock);
	pool = mapping_of(address, size);
	if (pool && pool->pages)
	{
		uint64_t first = (address - pool->base) / HOST_PAGE;
		uint64_t count = size / HOST_PAGE, page;

		for (page = first; page < first + count; page++)
		{
			if (pool->pages[page])
			{
				pool->pages[page] = 0;
				pool->free_pages++;
			}
		}
	}
	mutexUnlock(&memory_lock);
}

void *host_low_map(uint64_t size)
{
	return pages_take(round_up(size, HOST_PAGE));
}

void host_low_unmap(void *address, uint64_t size)
{
	pages_give_back((uint64_t)(uintptr_t)address, round_up(size, HOST_PAGE));
}

/* ---------- the guest's mmap and munmap */

static long guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t start = host_now();
	long result = guest_mmap(address, size, protection, flags, fd, offset);

	HOST_ADD(mapping_time, host_now() - start);
	return result;
}

static long guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size, HOST_PAGE);
	struct mapping *mapping;
	void *result;

	(void)offset;
	if (!length)
		return -EINVAL;
	if (!(flags & LINUX_MAP_ANONYMOUS) || fd != -1)
	{
		host_logf(HOST_LOG_WARN, "the guest mapped a file, which is not supported");
		return -ENODEV;
	}
	if (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE))
	{
		if ((address & (HOST_PAGE - 1)) || address + length > HOST_LOW_LIMIT)
			return -EINVAL;
		mutexLock(&memory_lock);
		mapping = mapping_of(address, length);
		if (mapping)
		{
			/* inside memory the host mapped: fresh pages over it */
			if (mapping->pages)
			{
				uint64_t first = (address - mapping->base) / HOST_PAGE, page;

				for (page = first; page < first + length / HOST_PAGE; page++)
				{
					if (!mapping->pages[page])
					{
						mapping->pages[page] = 1;
						mapping->free_pages--;
					}
				}
			}
			mutexUnlock(&memory_lock);
			if (protection != LINUX_PROT_NONE)
				memset((void *)(uintptr_t)address, 0, length);
			return (long)address;
		}
		if (touches_mapping(address, length) || !range_is_free(address, length))
		{
			mutexUnlock(&memory_lock);
			return (flags & LINUX_MAP_FIXED_NOREPLACE) ? -EEXIST : -ENOMEM;
		}
		mapping = add_mapping(address, length, NULL, 1, "a fixed range the guest asked for");
		mutexUnlock(&memory_lock);
		if (!mapping)
			return -ENOMEM;
		memset((void *)(uintptr_t)address, 0, length);
		return (long)address;
	}
	result = pages_take(length);
	if (!result)
		return -ENOMEM;
	return (long)(uintptr_t)result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	if ((address & (HOST_PAGE - 1)) || !size)
		return -EINVAL;
	pages_give_back(address, round_up(size, HOST_PAGE));
	return 0;
}

/* ---------- start-up */

int host_memory_initialize(void)
{
	uint64_t region = 0;

	mutexInit(&memory_lock);
	process = envGetOwnProcessHandle();
	if (process == INVALID_HANDLE)
	{
		host_logf(HOST_LOG_ERROR, "the homebrew loader gave no handle to this process");
		return -1;
	}
	svcGetInfo(&region, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
	search_start = region > 0x10000000ULL ? region : 0x10000000ULL;
	/* the Xbox window, which the guest's xbox_memory.c asks for at its
	fixed address, page by page */
	return host_map_fixed(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, NULL, 1, "the Xbox window");
}

/* ---------- the textures' write tracking, by page hashes */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / HOST_PAGE)

static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
static uint8_t page_hash_watched[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static uint32_t page_hashed_frame[WATCH_PAGE_COUNT];
static struct watch_hash page_hashes =
{
	.base = (const uint8_t *)(uintptr_t)HALO_GUEST_WINDOW_BASE,
	.page_count = WATCH_PAGE_COUNT,
	.watched = page_hash_watched,
	.hash = page_hash,
	.hashed_frame = page_hashed_frame,
	.generation = page_generation,
	.current_generation = &current_generation,
	.frame = 1,
};

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

/* the pages first to last of a range, clipped to the window; 0 if the range
does not start in it */
static int window_pages(uint32_t address, uint32_t size, uint32_t *first, uint32_t *last)
{
	uint64_t end;

	if (!size || !in_window(address))
		return 0;
	end = (uint64_t)address + size - 1;
	*first = (uint32_t)((address - HALO_GUEST_WINDOW_BASE) / HOST_PAGE);
	*last = in_window(end) ? (uint32_t)((end - HALO_GUEST_WINDOW_BASE) / HOST_PAGE) : WATCH_PAGE_COUNT - 1;
	return 1;
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint32_t first, last;

	if (watch_active && window_pages(address, size, &first, &last))
		watch_hash_protect(&page_hashes, first, last);
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint32_t first, last;

	if (!window_pages(address, size, &first, &last))
		return 0;
	return watch_hash_generation(&page_hashes, first, last);
}

/* nothing: a write shows in the next hash, and there is no protection to
fault on (as on Android under hashing) */
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	(void)address;
	(void)size;
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint32_t first, last;

	if (window_pages(address, size, &first, &last))
		watch_hash_forget(&page_hashes, first, last);
}

void host_memory_watch_begin_frame(void)
{
	__atomic_add_fetch(&host_activity.frames, 1, __ATOMIC_RELAXED);
	watch_hash_begin_frame(&page_hashes);
}
