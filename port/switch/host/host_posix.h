/*
HOST_POSIX.H

Forced into port/linux/src/posix_files.c when the Switch host compiles it
(its file functions are the guest's hostposix_* imports, as on Android):
newlib declares utimensat without implementing it and leaves out
UTIME_OMIT, so the host gives its own (host_posix_utimensat,
host_syscall.c, on utimes).
*/

#ifndef SWITCH_HOST_POSIX_H
#define SWITCH_HOST_POSIX_H

#include <sys/stat.h>
#include <time.h>

#ifndef UTIME_NOW
#define UTIME_NOW -2L
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT -1L
#endif

int host_posix_utimensat(int directory, const char *path, const struct timespec times[2], int flags);

#define utimensat host_posix_utimensat

#endif
