#pragma once
/* Remote control of the running game for debug builds (CTR_INTERNAL), carried as
 * SITE commands over the in-game FTP server (one port, one switch), the same
 * design as ../zelda-tmc-3ds/platform/3ds/source/debug_link_3ds.h:
 *
 *   SITE SHOT                 debug/top.bmp (400x240) and debug/bottom.bmp (320x240)
 *                             in the data dir (sdmc:/3ds/ctr_native/debug/)
 *   SITE STATE                one line: frames, fps, level, game mode, load stage, memory
 *   SITE DUMPREQ <word>       what dropping <word> in sdmc:/dumpreq.txt does, now
 *   SITE KEYS <A+B+LEFT|NONE> [frames]   hold 3DS buttons for N frames (default 1)
 *   SITE TOUCH <x> <y>        tap the bottom screen
 *   SITE MARK [text]          a marker line in the SD log, flushed
 *   SITE SHOTR                debug/top-right.bmp, the right eye (3D slider up)
 *   SITE PEEK <hex addr> <n>  hex of up to 256 bytes of this process's memory
 *   SITE POKE <hex addr> <hex bytes>   write them (main thread, between frames)
 *
 * Anything that touches the game runs on the main thread (N3DS_DebugLinkPoll,
 * once per presented frame); the FTP thread waits for its result line. */

#ifdef CTR_INTERNAL
/* FTP thread. `verb` is upper case. 1 and a whole reply line (code included)
 * in `reply` when the verb is ours, 0 otherwise. */
int N3DS_DebugLinkCommand(const char *verb, const char *args, char *reply, unsigned cap);

/* Debug tab CAPTURE button: debug/capture-NNNN-* on the next presented frame (n3ds_debuglink.c). */
void N3DS_DebugLinkRequestCapture(void);

/* Main thread, once per presented frame: runs the queued request, if any. */
void N3DS_DebugLinkPoll(void);

/* Main thread, from the input poll: buttons to add to what the pad reports
 * (libctru KEY_* bits), counting a KEYS request down one frame per call. */
unsigned N3DS_DebugLinkKeys(void);

/* Main thread, from the touch poll: 1 and a position when a TOUCH is due. */
int N3DS_DebugLinkTouch(int *x, int *y);
#endif
