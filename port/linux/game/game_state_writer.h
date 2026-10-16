/*
GAME_STATE_WRITER.H

The checkpoint's saved game file, written on a thread of its own
(game_state_writer.c).
*/

#ifndef GAME_STATE_WRITER_H
#define GAME_STATE_WRITER_H

/* copies the game state and writes the copy to the file from the start, on
the writer's thread; FALSE if there is no writer (the caller writes it) */
boolean game_state_writer_write(HANDLE file, const void *buffer, unsigned long size);
/* returns once the last write is done */
void game_state_writer_wait(void);
/* the copy last written, if it is of that size and still the file's, else
NULL: what the file holds, without reading it */
const void *game_state_writer_image(unsigned long size);
/* after a wait: the copy is no longer the file's (it was closed or opened
again) */
void game_state_writer_forget(void);

#endif
