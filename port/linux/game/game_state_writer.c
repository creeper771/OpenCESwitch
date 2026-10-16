/*
GAME_STATE_WRITER.C

The checkpoint's saved game file (z:\savegame.bin), written on a thread of
its own. A checkpoint wrote the whole game state, 20 MB with the port's
capacities (halo_port_capacity.h), on the main thread, which stopped the
game for half a second or more on a Switch's SD card; a revert read it all
back. The state is copied instead (a few milliseconds), and the copy is
written while the game goes on, in pieces with a pause after each, so that
the game's own reads of its maps (the textures of the area a checkpoint is
often at the start of) are not kept waiting behind it: written at once, it
had the card for a second; a revert takes the copy,
which is what the file holds, without reading the file. Called from
game_state_xbox.c.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include <xtl.h>
#include "game_state_writer.h"

#include <stdlib.h>
#include <string.h>

/* each write to the file, and the pause after it, in which the game's reads
get their turn (20 MB in about two seconds) */
#define WRITE_PIECE_SIZE 0x40000UL
#define WRITE_PAUSE_MILLISECONDS 10

static struct
{
	boolean started;
	HANDLE thread;
	/* signalled when there is a copy to write (auto reset), and while the
	writer is idle (manual reset) */
	HANDLE work;
	HANDLE idle;
	HANDLE file;
	void *image;
	unsigned long image_size;
	boolean image_valid;
	volatile boolean failed;
} writer;

static DWORD WINAPI game_state_writer_main(
	LPVOID parameter)
{
	(void)parameter;
	for (;;)
	{
		const char *bytes;
		unsigned long left, written;
		boolean succeeded;

		WaitForSingleObject(writer.work, INFINITE);
		bytes = writer.image;
		left = writer.image_size;
		succeeded = SetFilePointer(writer.file, 0, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
		while (succeeded && left)
		{
			unsigned long piece = left < WRITE_PIECE_SIZE ? left : WRITE_PIECE_SIZE;

			succeeded = WriteFile(writer.file, bytes, piece, &written, NULL) && written == piece;
			bytes += piece;
			left -= piece;
			if (left)
				Sleep(WRITE_PAUSE_MILLISECONDS);
		}
		writer.failed = !succeeded;
		SetEvent(writer.idle);
	}
}

static boolean game_state_writer_start(
	unsigned long size)
{
	if (writer.started)
		return writer.thread != NULL && writer.image_size == size;
	writer.started = TRUE;
	writer.image = malloc(size);
	writer.work = CreateEventA(NULL, FALSE, FALSE, NULL);
	writer.idle = CreateEventA(NULL, TRUE, TRUE, NULL);
	if (writer.image && writer.work && writer.idle)
		writer.thread = CreateThread(NULL, 0, game_state_writer_main, NULL, 0, NULL);
	if (!writer.thread)
	{
		error(_error_silent, "saved games: no writer thread, so checkpoints are written as the game waits");
		free(writer.image);
		writer.image = NULL;
		return FALSE;
	}
	writer.image_size = size;
	return TRUE;
}

boolean game_state_writer_write(
	HANDLE file,
	const void *buffer,
	unsigned long size)
{
	if (!game_state_writer_start(size))
		return FALSE;
	game_state_writer_wait();
	memcpy(writer.image, buffer, size);
	writer.image_valid = TRUE;
	writer.file = file;
	ResetEvent(writer.idle);
	SetEvent(writer.work);
	return TRUE;
}

void game_state_writer_wait(
	void)
{
	if (!writer.thread)
		return;
	WaitForSingleObject(writer.idle, INFINITE);
	if (writer.failed)
	{
		/* (a revert still takes the copy; the file is only this session's) */
		error(_error_silent, "saved games: couldn't write saved game file (#%d)", GetLastError());
		writer.failed = FALSE;
	}
}

const void *game_state_writer_image(
	unsigned long size)
{
	return writer.image_valid && writer.image_size == size ? writer.image : NULL;
}

void game_state_writer_forget(
	void)
{
	writer.image_valid = FALSE;
}
