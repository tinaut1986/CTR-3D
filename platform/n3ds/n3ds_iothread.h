#ifndef N3DS_IOTHREAD_H
#define N3DS_IOTHREAD_H

/*
 * One background I/O job at a time, run by a thread on the game's own core
 * just below its priority. Its work is an SD read: the thread spends it
 * blocked in the FS IPC, so the game thread keeps the CPU, and the read's
 * copy-out runs in the time the game spends waiting for the vblank or the
 * render worker. Used for the XA music read-ahead (native_audio.c): on an Old
 * 3DS the stream is refilled on the game thread, and a 64-sector span read
 * there held a race frame for ~28 ms.
 *
 * Plain C (no libctru types): the game TU calls it. The job function must not
 * touch anything the game thread changes while it runs; it gets its own
 * buffers and a copy of what it reads.
 */

/* Queues fn(arg). 0 when a job is still running or the thread could not be
 * started (the caller then does the work itself). */
int N3DS_IoJobSubmit(void (*fn)(void *), void *arg);

/* 1 while a submitted job has not finished. */
int N3DS_IoJobBusy(void);

/* Blocks until no job is in flight. */
void N3DS_IoJobWait(void);

#endif
