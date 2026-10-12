/*
HOST_LOADER.C

Loads the guest image: the Android port's statically linked AArch64 ELF
executable (ILP32 code, built by `ninja android`), linked at 0x88000000. It
is loaded as the homebrew loader loads an NRO: built whole in heap memory
(segments copied, .bss zeroed, the import table filled), then mapped at
its address with svcMapProcessCodeMemory, and only then given each page's
permission: read/execute for code, read/write for data, read-only for the
rest. Horizon lets a page of such a mapping become executable only if it
was never made writable, so no page is written after the mapping is made
except data pages.
*/

#include "host.h"

#include <elf.h>
#include <stdlib.h>
#include <string.h>

struct host_guest_image host_image;

enum
{
	PAGE_READ,
	PAGE_READ_WRITE,
	PAGE_READ_EXECUTE
};

static uint32_t permission_of(uint8_t kind)
{
	return kind == PAGE_READ_EXECUTE ? Perm_Rx : kind == PAGE_READ_WRITE ? Perm_Rw : Perm_R;
}

int host_load_image(const void *file, size_t size)
{
	const Elf64_Ehdr *elf = file;
	const Elf64_Phdr *segments;
	uint64_t low = ~0ULL, high = 0, image_size, page, run_start;
	const struct halo_guest_header *header;
	uint8_t *image, *kinds;
	uint64_t *table;
	const char *name;
	uint32_t count, index;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_ident[EI_CLASS] != ELFCLASS64 ||
		elf->e_machine != EM_AARCH64 || elf->e_type != ET_EXEC ||
		elf->e_phoff + (uint64_t)elf->e_phnum * sizeof(Elf64_Phdr) > size)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	segments = (const Elf64_Phdr *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		if (segments[index].p_type != PT_LOAD)
			continue;
		if (segments[index].p_vaddr < low)
			low = segments[index].p_vaddr;
		if (segments[index].p_vaddr + segments[index].p_memsz > high)
			high = segments[index].p_vaddr + segments[index].p_memsz;
	}
	low &= ~(HOST_PAGE - 1);
	high = (high + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
	if (low != HALO_GUEST_IMAGE_BASE || high > (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	image_size = high - low;

	/* the image, whole, in heap memory */
	image = aligned_alloc(HOST_PAGE, image_size);
	kinds = calloc(image_size / HOST_PAGE, 1);
	if (!image || !kinds)
	{
		host_logf(HOST_LOG_ERROR, "no memory to load the guest image");
		free(image);
		free(kinds);
		return -1;
	}
	memset(image, 0, image_size);
	for (index = 0; index < elf->e_phnum; index++)
	{
		const Elf64_Phdr *segment = &segments[index];
		uint64_t first, last;
		uint8_t kind;

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size || segment->p_filesz > segment->p_memsz)
		{
			host_logf(HOST_LOG_ERROR, "a segment of the guest image is outside the file");
			free(image);
			free(kinds);
			return -1;
		}
		memcpy(image + (segment->p_vaddr - low), (const char *)file + segment->p_offset, segment->p_filesz);
		if ((segment->p_flags & PF_X) && (segment->p_flags & PF_W))
		{
			host_logf(HOST_LOG_ERROR, "a segment of the guest image is writable and executable");
			free(image);
			free(kinds);
			return -1;
		}
		kind = (segment->p_flags & PF_X) ? PAGE_READ_EXECUTE : (segment->p_flags & PF_W) ? PAGE_READ_WRITE : PAGE_READ;
		first = (segment->p_vaddr - low) / HOST_PAGE;
		last = (segment->p_vaddr + segment->p_memsz - 1 - low) / HOST_PAGE;
		for (page = first; page <= last; page++)
		{
			/* a page shared by two segments takes the wider permission */
			if (kinds[page] != PAGE_READ && kind != PAGE_READ && kinds[page] != kind)
			{
				host_logf(HOST_LOG_ERROR, "a page of the guest image is both code and data");
				free(image);
				free(kinds);
				return -1;
			}
			if (kind != PAGE_READ)
				kinds[page] = kind;
		}
	}

	header = (const struct halo_guest_header *)image;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		free(image);
		free(kinds);
		return -1;
	}
	if (header->import_table < low || header->import_names < low || header->import_count < low ||
		header->import_count + 4 > high)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's import table is outside it");
		free(image);
		free(kinds);
		return -1;
	}
	table = (uint64_t *)(image + (header->import_table - low));
	name = (const char *)(image + (header->import_names - low));
	count = *(const uint32_t *)(image + (header->import_count - low));
	if (header->import_table + (uint64_t)count * sizeof(uint64_t) > high)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's import table is outside it");
		free(image);
		free(kinds);
		return -1;
	}
	for (index = 0; index < count; index++)
	{
		table[index] = (uint64_t)(uintptr_t)host_resolve_import(name, index);
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%u not implemented yet)",
		(unsigned long long)low, (unsigned long long)high, count, host_missing_import_count());

	/* mapped at its address, then each run of pages given its permission */
	armDCacheFlush(image, image_size);
	if (host_map_fixed(low, image_size, image, 0, "the guest image") != 0)
	{
		free(image);
		free(kinds);
		return -1;
	}
	run_start = 0;
	for (page = 1; page <= image_size / HOST_PAGE; page++)
	{
		if (page == image_size / HOST_PAGE || kinds[page] != kinds[run_start])
		{
			if (host_set_permission(low + run_start * HOST_PAGE, (page - run_start) * HOST_PAGE,
				permission_of(kinds[run_start])) != 0)
			{
				free(kinds);
				return -1;
			}
			run_start = page;
		}
	}
	free(kinds);
	armICacheInvalidate((void *)(uintptr_t)low, image_size);

	host_image.header = (const struct halo_guest_header *)(uintptr_t)low;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;
	return 0;
}
