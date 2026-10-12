/*
MEMORY_PROBE.C

Phase 1.1 of the Switch port: can a homebrew process on Horizon have the
memory the OpenCE guest needs?

The guest (port/android/include/halo_android_abi.h) is linked to fixed
addresses: the Xbox window at 0x80000000-0x88000000 and the image at
0x88000000-0x8c000000. It also needs more address space below 4 GB for its
heap and thread stacks, because it keeps every pointer in 32 bits. On
Android the host takes these with mmap(MAP_FIXED); on Horizon the kernel
decides where each kind of memory may go, so this program finds out:

1. the launch mode (applet or title takeover), the system version, and how
   much memory the process has;
2. the kernel's memory regions (svcGetInfo);
3. every mapping below 4 GB (svcQueryMemory), and the free space below 4 GB
   in each region;
4. attempts to map read/write memory at the fixed addresses, with
   svcMapMemory (heap memory into the stack region) and svcMapPhysicalMemory
   (new memory in the alias region), each checked by writing and reading
   back every page;
5. an attempt to map a pool below 4 GB elsewhere, as the guest's heap would
   need;
6. the same with svcMapProcessCodeMemory, which maps heap memory anywhere
   free in the address space region (as the homebrew loader maps each NRO),
   followed by svcSetProcessMemoryPermission: read/write for the whole
   range except one page of the image, checked by writing and reading back
   every page and by reading a file straight into it (as the game reads its
   maps); that page, which holds a function copied in before the mapping
   was made and is never made writable, read/execute, checked by calling
   the function.

The report goes to the screen and to sdmc:/switch/opence_memory_probe.txt.
Nothing is left mapped when it ends. A system call the process is not
allowed to make would end it, so each one is checked first
(envIsSyscallHinted).
*/

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_android_abi.h"

#define REPORT_PATH "sdmc:/switch/opence_memory_probe.txt"

#define LOW_LIMIT 0x100000000ULL
#define PAGE_SIZE 0x1000ULL
#define FIXED_BASE ((u64)HALO_GUEST_WINDOW_BASE)
#define FIXED_END ((u64)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE)
/* the largest pool the test of step 5 maps */
#define POOL_TEST_SIZE 0x04000000ULL

/* the system calls this program makes beyond those libnx needs to start */
#define SVC_MAP_MEMORY 0x04
#define SVC_UNMAP_MEMORY 0x05
#define SVC_MAP_PHYSICAL_MEMORY 0x2c
#define SVC_UNMAP_PHYSICAL_MEMORY 0x2d
#define SVC_SET_PROCESS_MEMORY_PERMISSION 0x73
#define SVC_MAP_PROCESS_CODE_MEMORY 0x77
#define SVC_UNMAP_PROCESS_CODE_MEMORY 0x78

/* the bytes of the file test of step 6 */
#define FILE_TEST_SIZE 0x100000ULL
/* mov w0, #0x1234; ret: the function the execution test of step 6 calls */
#define CODE_TEST_RESULT 0x1234u
static const u32 code_test_function[] = {0x52800000u | (CODE_TEST_RESULT << 5), 0xd65f03c0u};

struct region
{
	const char *name;
	const char *used_by;
	u64 base;
	u64 size;
};

enum
{
	REGION_ASLR,
	REGION_HEAP,
	REGION_ALIAS,
	REGION_STACK,
	NUMBER_OF_REGIONS
};

struct verdict
{
	bool fixed_by_map_memory;
	bool fixed_by_physical_memory;
	u64 stack_free_below_4gb;
	u64 alias_free_below_4gb;
	bool pool_mapped;
	u64 pool_size;
	u64 aslr_free_below_4gb;
	bool fixed_by_code_memory;
	bool file_read_into_fixed;
	bool executed_in_image;
	bool pool_by_code_memory;
	u64 code_memory_pool_size;
};

static FILE *report_file;
/* the path of this program (argv[0]), the file the file test reads */
static const char *program_path;
static struct region regions[NUMBER_OF_REGIONS];
static struct verdict verdict;

/* ---------- the report */

static void report(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	if (report_file)
	{
		va_start(arguments, format);
		vfprintf(report_file, format, arguments);
		va_end(arguments);
		fflush(report_file);
	}
}

static void report_result(const char *what, Result rc)
{
	if (R_SUCCEEDED(rc))
		report("  %s: succeeded\n", what);
	else
		report("  %s: failed, 0x%x (module %u, description %u)\n", what, rc, R_MODULE(rc), R_DESCRIPTION(rc));
}

static void report_section(const char *title)
{
	report("\n== %s\n", title);
	consoleUpdate(NULL);
}

static const char *memory_type_name(u32 type)
{
	switch (type & 0xff)
	{
	case 0x00: return "free";
	case 0x01: return "io";
	case 0x02: return "static";
	case 0x03: return "code";
	case 0x04: return "code data";
	case 0x05: return "heap";
	case 0x06: return "shared";
	case 0x07: return "alias";
	case 0x08: return "module code";
	case 0x09: return "module data";
	case 0x0a: return "ipc buffer 0";
	case 0x0b: return "stack";
	case 0x0c: return "thread local";
	case 0x0d: return "transfer (iso)";
	case 0x0e: return "transfer";
	case 0x0f: return "process";
	case 0x10: return "inaccessible";
	case 0x11: return "ipc buffer 1";
	case 0x12: return "ipc buffer 3";
	case 0x13: return "kernel stack";
	case 0x14: return "code read-only";
	case 0x15: return "code writable";
	default: return "other";
	}
}

static void report_size(u64 size)
{
	report("%llu MB", (unsigned long long)(size >> 20));
}

/* ---------- 1. the process */

static void report_process(void)
{
	u32 version = hosversionGet();
	AppletType type = appletGetAppletType();
	u64 total = 0, used = 0;

	report_section("process");
	report("system version %u.%u.%u, %s\n", HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version),
		hosversionIsAtmosphere() ? "Atmosphere" : "not Atmosphere");
	report("applet type %d: %s\n", (int)type,
		type == AppletType_Application || type == AppletType_SystemApplication ? "title takeover (application)" :
		type == AppletType_LibraryApplet ? "applet mode (for example the Album)" : "other");
	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	report("memory: ");
	report_size(total);
	report(" in all, ");
	report_size(used);
	report(" in use, ");
	report_size(total - used);
	report(" free\n");
	report("system calls allowed: svcMapMemory %s, svcUnmapMemory %s, svcMapPhysicalMemory %s, svcUnmapPhysicalMemory %s\n",
		envIsSyscallHinted(SVC_MAP_MEMORY) ? "yes" : "no", envIsSyscallHinted(SVC_UNMAP_MEMORY) ? "yes" : "no",
		envIsSyscallHinted(SVC_MAP_PHYSICAL_MEMORY) ? "yes" : "no",
		envIsSyscallHinted(SVC_UNMAP_PHYSICAL_MEMORY) ? "yes" : "no");
	report("  svcMapProcessCodeMemory %s, svcUnmapProcessCodeMemory %s, svcSetProcessMemoryPermission %s\n",
		envIsSyscallHinted(SVC_MAP_PROCESS_CODE_MEMORY) ? "yes" : "no",
		envIsSyscallHinted(SVC_UNMAP_PROCESS_CODE_MEMORY) ? "yes" : "no",
		envIsSyscallHinted(SVC_SET_PROCESS_MEMORY_PERMISSION) ? "yes" : "no");
	report("own process handle: %s\n", envGetOwnProcessHandle() != INVALID_HANDLE ? "given by the loader" : "NOT given");
	report("program: %s\n", program_path ? program_path : "(unknown)");
}

/* ---------- 2. the regions */

static void read_region(int index, const char *name, const char *used_by, u32 base_info, u32 size_info)
{
	struct region *region = &regions[index];

	region->name = name;
	region->used_by = used_by;
	svcGetInfo(&region->base, base_info, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&region->size, size_info, CUR_PROCESS_HANDLE, 0);
}

static void report_regions(void)
{
	int index;

	report_section("regions");
	read_region(REGION_ASLR, "address space", "code memory", InfoType_AslrRegionAddress, InfoType_AslrRegionSize);
	read_region(REGION_HEAP, "heap", "svcSetHeapSize", InfoType_HeapRegionAddress, InfoType_HeapRegionSize);
	read_region(REGION_ALIAS, "alias", "svcMapPhysicalMemory", InfoType_AliasRegionAddress, InfoType_AliasRegionSize);
	read_region(REGION_STACK, "stack", "svcMapMemory", InfoType_StackRegionAddress, InfoType_StackRegionSize);
	for (index = 0; index < NUMBER_OF_REGIONS; index++)
	{
		const struct region *region = &regions[index];
		u64 end = region->base + region->size;
		const char *fixed;

		if (region->base <= FIXED_BASE && end >= FIXED_END)
			fixed = "contains";
		else if (region->base < FIXED_END && end > FIXED_BASE)
			fixed = "partly covers";
		else
			fixed = "does not cover";
		report("%-13s %010llx-%010llx (", region->name, (unsigned long long)region->base, (unsigned long long)end);
		report_size(region->size);
		report(", %s): %s the fixed range\n", region->used_by, fixed);
	}
	if (hosversionAtLeast(18, 0, 0))
	{
		u64 extra = 0;

		svcGetInfo(&extra, InfoType_AliasRegionExtraSize, CUR_PROCESS_HANDLE, 0);
		report("alias region extra size: ");
		report_size(extra);
		report("\n");
	}
}

/* ---------- 3. the mappings below 4 GB */

static void report_mappings(void)
{
	u64 address = 0;

	report_section("mappings below 4 GB");
	while (address < LOW_LIMIT)
	{
		MemoryInfo info;
		u32 page_info;
		Result rc = svcQueryMemory(&info, &page_info, address);
		u64 end;

		if (R_FAILED(rc))
		{
			report_result("svcQueryMemory", rc);
			break;
		}
		end = info.addr + info.size;
		report("%010llx-%010llx %-14s %c%c%c\n", (unsigned long long)info.addr,
			(unsigned long long)(end < LOW_LIMIT ? end : LOW_LIMIT), memory_type_name(info.type),
			info.perm & Perm_R ? 'r' : '-', info.perm & Perm_W ? 'w' : '-', info.perm & Perm_X ? 'x' : '-');
		if (end <= address)
			break;
		address = end;
	}
}

/* the free space below 4 GB inside a region, leaving out the fixed range if
without_fixed; the largest free block is returned in largest_base and
largest_size */
static u64 free_below_4gb(const struct region *region, bool without_fixed, u64 *largest_base, u64 *largest_size)
{
	u64 region_end = region->base + region->size;
	u64 address = region->base;
	u64 total = 0;

	*largest_base = 0;
	*largest_size = 0;
	if (region_end > LOW_LIMIT)
		region_end = LOW_LIMIT;
	while (address < region_end)
	{
		MemoryInfo info;
		u32 page_info;
		u64 start, end;
		u64 pieces[2][2];
		int count = 0, index;

		if (R_FAILED(svcQueryMemory(&info, &page_info, address)))
			break;
		start = info.addr > region->base ? info.addr : region->base;
		end = info.addr + info.size < region_end ? info.addr + info.size : region_end;
		if ((info.type & 0xff) == 0x00 && end > start)
		{
			if (without_fixed && start < FIXED_END && end > FIXED_BASE)
			{
				if (start < FIXED_BASE)
				{
					pieces[count][0] = start;
					pieces[count++][1] = FIXED_BASE;
				}
				if (end > FIXED_END)
				{
					pieces[count][0] = FIXED_END;
					pieces[count++][1] = end;
				}
			}
			else
			{
				pieces[count][0] = start;
				pieces[count++][1] = end;
			}
			for (index = 0; index < count; index++)
			{
				u64 size = pieces[index][1] - pieces[index][0];

				total += size;
				if (size > *largest_size)
				{
					*largest_base = pieces[index][0];
					*largest_size = size;
				}
			}
		}
		if (info.addr + info.size <= address)
			break;
		address = info.addr + info.size;
	}
	return total;
}

static void report_free_space(void)
{
	int index;

	report_section("free space below 4 GB");
	for (index = 0; index < NUMBER_OF_REGIONS; index++)
	{
		u64 largest_base, largest_size;
		u64 total = free_below_4gb(&regions[index], false, &largest_base, &largest_size);

		report("%-13s ", regions[index].name);
		report_size(total);
		report(" free");
		if (largest_size)
		{
			report(", the largest block ");
			report_size(largest_size);
			report(" at %010llx", (unsigned long long)largest_base);
		}
		report("\n");
		if (index == REGION_STACK)
			verdict.stack_free_below_4gb = total;
		else if (index == REGION_ALIAS)
			verdict.alias_free_below_4gb = total;
		else if (index == REGION_ASLR)
			verdict.aslr_free_below_4gb = total;
	}
}

/* ---------- 4. and 5. mapping */

/* writes a pattern to every page of a mapping and reads it back */
static bool check_pages(u64 address, u64 size)
{
	u64 offset;

	for (offset = 0; offset < size; offset += PAGE_SIZE)
		*(volatile u64 *)(address + offset) = (address + offset) ^ 0x4f50454e4345ULL;
	for (offset = 0; offset < size; offset += PAGE_SIZE)
		if (*(volatile u64 *)(address + offset) != ((address + offset) ^ 0x4f50454e4345ULL))
			return false;
	return true;
}

static bool is_free(u64 address, u64 size)
{
	MemoryInfo info;
	u32 page_info;

	if (R_FAILED(svcQueryMemory(&info, &page_info, address)))
		return false;
	return (info.type & 0xff) == 0x00 && info.addr + info.size >= address + size;
}

/* svcMapMemory: heap memory mapped again at address (in the stack region) */
static bool try_map_memory(const char *what, u64 address, u64 size)
{
	void *source;
	Result rc;
	bool checked;

	report("svcMapMemory, %s, %010llx-%010llx:\n", what, (unsigned long long)address, (unsigned long long)(address + size));
	if (!envIsSyscallHinted(SVC_MAP_MEMORY) || !envIsSyscallHinted(SVC_UNMAP_MEMORY))
	{
		report("  not allowed for this process: skipped\n");
		return false;
	}
	if (!is_free(address, size))
	{
		report("  the range is not free: skipped\n");
		return false;
	}
	source = aligned_alloc(PAGE_SIZE, size);
	if (!source)
	{
		report("  no heap memory of that size: skipped\n");
		return false;
	}
	virtmemLock();
	rc = svcMapMemory((void *)address, source, size);
	virtmemUnlock();
	report_result("map", rc);
	if (R_FAILED(rc))
	{
		free(source);
		return false;
	}
	checked = check_pages(address, size);
	report("  every page %s\n", checked ? "reads back what was written" : "DOES NOT read back what was written");
	report_result("unmap", svcUnmapMemory((void *)address, source, size));
	free(source);
	return checked;
}

/* svcMapPhysicalMemory: new memory at address (in the alias region) */
static bool try_map_physical_memory(const char *what, u64 address, u64 size)
{
	Result rc;
	bool checked;

	report("svcMapPhysicalMemory, %s, %010llx-%010llx:\n", what, (unsigned long long)address,
		(unsigned long long)(address + size));
	if (!envIsSyscallHinted(SVC_MAP_PHYSICAL_MEMORY) || !envIsSyscallHinted(SVC_UNMAP_PHYSICAL_MEMORY))
	{
		report("  not allowed for this process: skipped\n");
		return false;
	}
	if (!is_free(address, size))
	{
		report("  the range is not free: skipped\n");
		return false;
	}
	virtmemLock();
	rc = svcMapPhysicalMemory((void *)address, size);
	virtmemUnlock();
	report_result("map", rc);
	if (R_FAILED(rc))
		return false;
	checked = check_pages(address, size);
	report("  every page %s\n", checked ? "reads back what was written" : "DOES NOT read back what was written");
	report_result("unmap", svcUnmapPhysicalMemory((void *)address, size));
	return checked;
}

static void try_fixed_range(void)
{
	report_section("the fixed addresses");
	verdict.fixed_by_map_memory = try_map_memory("window and image", FIXED_BASE, FIXED_END - FIXED_BASE);
	if (!verdict.fixed_by_map_memory)
	{
		/* separately, in case only the size was the problem */
		try_map_memory("window", HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE);
		try_map_memory("image", HALO_GUEST_IMAGE_BASE, HALO_GUEST_IMAGE_RESERVE);
	}
	consoleUpdate(NULL);
	verdict.fixed_by_physical_memory =
		try_map_physical_memory("window and image", FIXED_BASE, FIXED_END - FIXED_BASE);
	consoleUpdate(NULL);
}

static void try_pool(void)
{
	u64 base, size;

	report_section("a pool below 4 GB");
	free_below_4gb(&regions[REGION_STACK], true, &base, &size);
	if (!size)
	{
		report("the stack region has no free space below 4 GB\n");
		return;
	}
	if (size > POOL_TEST_SIZE)
		size = POOL_TEST_SIZE;
	verdict.pool_mapped = try_map_memory("pool", base, size);
	if (verdict.pool_mapped)
		verdict.pool_size = size;
}

/* ---------- 6. code mapping in the address space region */

/* reads the start of this program's file straight into buffer (no stdio
buffer between, so the file system writes into it as it would into the
guest's memory) and compares it with a read into ordinary heap memory */
static bool check_file_read(void *buffer)
{
	FILE *file;
	void *expected;
	size_t read, expected_read;
	bool same;

	if (!program_path)
	{
		report("  file read: no program path: skipped\n");
		return false;
	}
	expected = malloc(FILE_TEST_SIZE);
	file = fopen(program_path, "rb");
	if (!expected || !file)
	{
		report("  file read: %s could not be opened: skipped\n", program_path);
		if (file)
			fclose(file);
		free(expected);
		return false;
	}
	setvbuf(file, NULL, _IONBF, 0);
	read = fread(buffer, 1, FILE_TEST_SIZE, file);
	rewind(file);
	expected_read = fread(expected, 1, FILE_TEST_SIZE, file);
	fclose(file);
	same = read > 0 && read == expected_read && memcmp(buffer, expected, read) == 0;
	report("  file read: %zu bytes, %s\n", read, same ? "the same as a read into heap memory" : "DIFFERENT or failed");
	free(expected);
	return same;
}

/* copies the test function into the heap memory behind a mapping, at offset,
before it is mapped, as the homebrew loader copies an NRO: once the kernel
has made a page of such a mapping writable it stays data and cannot be made
executable (the first runs got 0xd401), so code pages must go straight from
the mapping to read/execute */
static void place_test_function(void *source, u64 offset)
{
	memcpy((char *)source + offset, code_test_function, sizeof(code_test_function));
	armDCacheFlush((char *)source + offset, sizeof(code_test_function));
}

/* makes the page at address read/execute and calls the function placed there */
static bool check_execution(Handle process, u64 address)
{
	u32 (*function)(void) = (u32 (*)(void))address;
	MemoryInfo info;
	u32 page_info;
	Result rc;
	u32 result;

	rc = svcSetProcessMemoryPermission(process, address, PAGE_SIZE, Perm_Rx);
	report_result("read/execute permission on the code page, never made writable", rc);
	if (R_FAILED(rc))
		return false;
	if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, address)))
		report("  the code page is mapped as %s (type 0x%x)\n", memory_type_name(info.type), info.type);
	armICacheInvalidate((void *)address, PAGE_SIZE);
	result = function();
	report("  the function placed there returned 0x%x: %s\n", result, result == CODE_TEST_RESULT ? "it ran" : "WRONG");
	return result == CODE_TEST_RESULT;
}

/* makes a mapping read/write, except the page at skip if skip is not 0 */
static Result set_read_write(Handle process, u64 address, u64 size, u64 skip)
{
	Result rc = 0;

	if (!skip)
		return svcSetProcessMemoryPermission(process, address, size, Perm_Rw);
	if (skip > address)
		rc = svcSetProcessMemoryPermission(process, address, skip - address, Perm_Rw);
	if (R_SUCCEEDED(rc) && skip + PAGE_SIZE < address + size)
		rc = svcSetProcessMemoryPermission(process, skip + PAGE_SIZE, address + size - skip - PAGE_SIZE, Perm_Rw);
	return rc;
}

/* check_pages, except the page at skip if skip is not 0 */
static bool check_pages_except(u64 address, u64 size, u64 skip)
{
	if (!skip)
		return check_pages(address, size);
	return (skip == address || check_pages(address, skip - address)) &&
		(skip + PAGE_SIZE >= address + size || check_pages(skip + PAGE_SIZE, address + size - skip - PAGE_SIZE));
}

/* svcMapProcessCodeMemory: heap memory mapped again at address (in the
address space region). execute_at, if not 0, is a page that gets the test
function and becomes read/execute; the rest becomes read/write. Sets
*file_read and *executed as those tests go. */
static bool try_code_memory(const char *what, u64 address, u64 size, u64 execute_at, bool *file_read, bool *executed)
{
	Handle process = envGetOwnProcessHandle();
	void *source;
	Result rc;
	bool checked;

	report("svcMapProcessCodeMemory, %s, %010llx-%010llx:\n", what, (unsigned long long)address,
		(unsigned long long)(address + size));
	if (!envIsSyscallHinted(SVC_MAP_PROCESS_CODE_MEMORY) || !envIsSyscallHinted(SVC_UNMAP_PROCESS_CODE_MEMORY) ||
		!envIsSyscallHinted(SVC_SET_PROCESS_MEMORY_PERMISSION))
	{
		report("  not allowed for this process: skipped\n");
		return false;
	}
	if (process == INVALID_HANDLE)
	{
		report("  the loader gave no handle to this process: skipped\n");
		return false;
	}
	if (!is_free(address, size))
	{
		report("  the range is not free: skipped\n");
		return false;
	}
	source = aligned_alloc(PAGE_SIZE, size);
	if (!source)
	{
		report("  no heap memory of that size: skipped\n");
		return false;
	}
	if (execute_at)
		place_test_function(source, execute_at - address);
	virtmemLock();
	rc = svcMapProcessCodeMemory(process, address, (u64)source, size);
	virtmemUnlock();
	report_result("map", rc);
	if (R_FAILED(rc))
	{
		free(source);
		return false;
	}
	if (execute_at && executed)
		*executed = check_execution(process, execute_at);
	rc = set_read_write(process, address, size, execute_at);
	report_result(execute_at ? "read/write permission on the other pages" : "read/write permission", rc);
	checked = R_SUCCEEDED(rc) && check_pages_except(address, size, execute_at);
	if (R_SUCCEEDED(rc))
	{
		MemoryInfo info;
		u32 page_info;

		report("  every read/write page %s\n", checked ? "reads back what was written" : "DOES NOT read back what was written");
		if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, address)))
			report("  mapped as %s (type 0x%x), attributes 0x%x\n", memory_type_name(info.type), info.type, info.attr);
		if (checked && file_read)
			*file_read = check_file_read((void *)address);
	}
	/* the whole mapping read/write (data) again, so it is of one kind when
	it is unmapped */
	if (execute_at)
		report_result("the code page read/write again, for unmapping",
			svcSetProcessMemoryPermission(process, execute_at, PAGE_SIZE, Perm_Rw));
	virtmemLock();
	report_result("unmap", svcUnmapProcessCodeMemory(process, address, (u64)source, size));
	virtmemUnlock();
	free(source);
	return checked;
}

static void try_code_mapping(void)
{
	u64 base, size;

	report_section("code mapping in the address space region");
	verdict.fixed_by_code_memory = try_code_memory("window and image", FIXED_BASE, FIXED_END - FIXED_BASE,
		HALO_GUEST_IMAGE_BASE, &verdict.file_read_into_fixed, &verdict.executed_in_image);
	consoleUpdate(NULL);
	free_below_4gb(&regions[REGION_ASLR], true, &base, &size);
	if (!size)
	{
		report("the address space region has no free space below 4 GB outside the fixed range\n");
		return;
	}
	if (size > POOL_TEST_SIZE)
		size = POOL_TEST_SIZE;
	verdict.pool_by_code_memory = try_code_memory("pool", base, size, 0, NULL, NULL);
	if (verdict.pool_by_code_memory)
		verdict.code_memory_pool_size = size;
	consoleUpdate(NULL);
}

/* ---------- the verdict */

static void report_verdict(void)
{
	report_section("verdict");
	if (verdict.fixed_by_map_memory || verdict.fixed_by_physical_memory || verdict.fixed_by_code_memory)
		report("FIXED ADDRESSES: yes, with %s\n",
			verdict.fixed_by_map_memory ? "svcMapMemory (heap memory in the stack region)" :
			verdict.fixed_by_physical_memory ? "svcMapPhysicalMemory (the alias region)" :
			"svcMapProcessCodeMemory (heap memory in the address space region)");
	else
		report("FIXED ADDRESSES: NO, 0x%08llx-0x%08llx could not be mapped read/write in this launch mode\n",
			(unsigned long long)FIXED_BASE, (unsigned long long)FIXED_END);
	report("POOLS BELOW 4 GB: ");
	report_size(verdict.stack_free_below_4gb);
	report(" free in the stack region, ");
	report_size(verdict.alias_free_below_4gb);
	report(" in the alias region; ");
	if (verdict.pool_mapped)
	{
		report("a ");
		report_size(verdict.pool_size);
		report(" pool was mapped and checked\n");
	}
	else
		report("no pool could be mapped\n");
	report("CODE MAPPING: ");
	report_size(verdict.aslr_free_below_4gb);
	report(" free below 4 GB in the address space region; the fixed range %s",
		verdict.fixed_by_code_memory ? "mapped read/write" : "NOT mapped");
	if (verdict.fixed_by_code_memory)
		report(", a file read into it %s, code in the image %s", verdict.file_read_into_fixed ? "worked" : "FAILED",
			verdict.executed_in_image ? "ran" : "DID NOT run");
	if (verdict.pool_by_code_memory)
	{
		report("; a ");
		report_size(verdict.code_memory_pool_size);
		report(" pool was mapped and checked\n");
	}
	else
		report("; no pool could be mapped\n");
	report("Run this both from the Album (applet mode) and by holding R while starting a game (title takeover).\n");
}

int main(int argc, char **argv)
{
	PadState pad;

	if (argc > 0)
		program_path = argv[0];
	consoleInit(NULL);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	report_file = fopen(REPORT_PATH, "w");

	report("OpenCE memory probe: the guest needs 0x%08llx-0x%08llx read/write, and more below 4 GB\n",
		(unsigned long long)FIXED_BASE, (unsigned long long)FIXED_END);
	report_process();
	report_regions();
	report_mappings();
	report_free_space();
	try_fixed_range();
	try_pool();
	try_code_mapping();
	report_verdict();

	if (report_file)
	{
		fclose(report_file);
		report_file = NULL;
		printf("\nThe report is in %s.\n", REPORT_PATH);
	}
	else
		printf("\n%s could not be written.\n", REPORT_PATH);
	printf("Press + to exit.\n");
	while (appletMainLoop())
	{
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		consoleUpdate(NULL);
	}
	consoleExit(NULL);
	return 0;
}
