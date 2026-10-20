/*
HALO_CRC32.C

The game's CRC-32 (source/memory/crc.c: reflected, polynomial 0xEDB88320,
from 0xFFFFFFFF and without a final inversion) through the port's zlib,
which takes several bytes a step where the game's loop takes one: the
saved game the menus check and a level's start reads is 20 MB with the
port's capacities, and its checksum stopped the menus for a moment on a
Switch. zlib's crc32 inverts the value before and after, so the game's is
its inverse of the inverse.
*/

#include "zlib_prefixed.h"

unsigned long halo_crc32_update(unsigned long crc, const void *buffer, long size)
{
	const unsigned char *bytes = buffer;

	while (size > 0)
	{
		unsigned int piece = size > 0x40000000L ? 0x40000000U : (unsigned int)size;

		crc = ~(unsigned long)crc32(~crc & 0xffffffffUL, bytes, piece) & 0xffffffffUL;
		bytes += piece;
		size -= (long)piece;
	}
	return crc;
}
