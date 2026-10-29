/*
 * In-game FTP server for the 3DS build. See n3ds_ftp.h for the commands, the
 * two modes (full on debug builds, install-only on production ones) and the CIA
 * install convention. Same behaviour as the author's zelda-tmc-3ds
 * ftp_server_3ds.c / cia_install_3ds.c: port 5000, progress, a 10 s restart
 * countdown, the running title's ID checked.
 *
 * Threading, the part that matters for the game:
 *   - A worker thread of its own, at the LOWEST priority (0x3F), on core 3.
 *     Core 3 holds only the audio worker, which runs at main-1 and sleeps most
 *     of every frame; at 0x3F the server only ever gets the gaps, so it cannot
 *     delay audio. Core 2 (the render worker, main priority) is the fallback if
 *     core 3 is refused. Core 1 belongs to the system (a thread there hangs the
 *     console when the Rosalina overlay opens, and reserving app time on it is
 *     what wedges the menu-leave: n3ds_audio_thread.c), so it is never used.
 *   - Old 3DS / 2DS (and the Old-3DS simulation): no cores 2/3, so the thread
 *     goes on core 0 behind the game, still at 0x3F (as zelda-tmc-3ds and
 *     legend-of-doom-3ds do there without a core-1 share). The kernel schedules
 *     a core strictly by priority, so it runs only while the main thread
 *     (0x30) is blocked -- waiting for the GPU or the vblank -- and the main
 *     thread preempts it the moment it wakes. Its own cost is a zero-timeout
 *     poll() IPC every 50 ms when idle (10 ms during a transfer) and every 1 s
 *     while there is no Wi-Fi; SD writes and the CIA install are blocking IPC,
 *     so they wait in the kernel, not on the CPU. A frame that never blocks
 *     starves the server (slow transfers), never the game.
 *   - Polling from the main loop was rejected: an SD read or a CIA install of
 *     tens of milliseconds would land inside a frame.
 *   - libctru funnels every socket call of the process through one SOC session,
 *     and a blocking poll() would hold up other threads' socket calls (the
 *     3dslink stream). So the server never blocks in the kernel on a socket:
 *     it polls with a zero timeout and naps in between (WaitFd).
 *   - Waiting for Wi-Fi retries every second (bind failures every 2 s) until
 *     the network is up, so starting the game before joining Wi-Fi is fine.
 *   - With the server off nothing exists: no socket buffer, no thread.
 */
#include <3ds.h>

#include "n3ds_debuglink.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n3ds_backend.h"
#include "n3ds_ciainstall.h"
#include "n3ds_ftp.h"

#define N3DS_FTP_DEV_FLAG N3DS_APP_DIR "/dev-ftp.txt"
#define N3DS_FTP_UPDATE_DIR "/3ds/ctr_native/update" /* SD path, the only one install-only mode reaches */
#define N3DS_FTP_PORT 5000
#define N3DS_FTP_PASV_FIRST (N3DS_FTP_PORT + 1)
#define N3DS_FTP_PASV_COUNT 64
#define N3DS_FTP_PATH_MAX 512
#define N3DS_FTP_LINE_MAX 600
#define N3DS_FTP_IO_SIZE (64 * 1024)
#define N3DS_FTP_SEND_CHUNK (8 * 1024)
#define N3DS_FTP_NAP_MS 10
#define N3DS_FTP_IDLE_NAP_MS 50
#define N3DS_FTP_DATA_ACCEPT_MS 15000
#define N3DS_FTP_IDLE_MS (10 * 60 * 1000)
#define N3DS_FTP_STACK_SIZE (64 * 1024)
#define N3DS_FTP_PRIORITY 0x3F
#define N3DS_FTP_RELAUNCH_DELAY_MS 10000 /* the same wait the author measured safe on the loader */

static Thread s_ftpThread = NULL;
static volatile int s_ftpRun = 0;
static int s_ftpStuck = 0; /* the thread did not end at exit */
static int s_ftpListen = -1;
static uint32_t s_ftpIp = 0;
static volatile unsigned s_ftpWakeGen = 0;    /* bumped by the APT hook on sleep/wake */
static volatile unsigned s_ftpSessionGen = 0; /* generation the current listen belongs to */
static unsigned char *s_ftpIo = NULL;
static unsigned s_ftpNextPasv = 0;
static int s_ftpAtExitSet = 0;

/* Set by a successful SITE INSTALL: osGetTime() after which the main loop quits. */
static volatile unsigned long long s_ftpRelaunchAt = 0;
static int s_ftpRelaunchArmed = 0;
/* Install-only mode (production): only the update dir is reachable, SITE INSTALL
 * is the only SITE command. After a CANCEL the server never starts again until
 * the game is relaunched. */
static volatile int s_ftpInstallOnly = 0;
static int s_ftpCancelled = 0;
/* For the UI: 0 idle, 1 a .cia is arriving, 2 installing, 3 restart countdown;
 * done/total are bytes (ms for 3). Total is 0 when the client did not announce
 * the size with ALLO. */
static volatile int s_ftpPhase = 0;
static const char *volatile s_ftpNotice = NULL;
static volatile unsigned long long s_ftpDone = 0, s_ftpTotal = 0;
static unsigned long long s_ftpAllo = 0;

typedef struct N3DS_FtpSession
{
	int ctrl;
	int pasv; /* listening data socket after PASV/EPSV, -1 when none */
	char cwd[N3DS_FTP_PATH_MAX];
	char rnfr[N3DS_FTP_PATH_MAX];
	char in[N3DS_FTP_LINE_MAX * 2];
	unsigned inUsed;
	unsigned long long rest; /* REST offset for the next RETR/STOR, 0 = none */
} N3DS_FtpSession;

static N3DS_FtpSession s_session = {.ctrl = -1, .pasv = -1};

/* ---- sockets ---- */

static void N3DS_FtpCloseSocket(int *fd)
{
	if (*fd >= 0)
	{
		shutdown(*fd, SHUT_RDWR);
		close(*fd);
		*fd = -1;
	}
}

/* Waits until fd is readable (or writable); false on timeout, error or stop.
 * Zero-timeout poll plus a nap: see the file comment. */
static int N3DS_FtpWaitFd(int fd, short events, int timeoutMs)
{
	int waited = 0;

	while (s_ftpRun && (s_ftpWakeGen == s_ftpSessionGen))
	{
		struct pollfd p;
		int r;

		p.fd = fd;
		p.events = events;
		p.revents = 0;
		r = poll(&p, 1, 0);
		if (r > 0)
		{
			return (p.revents & (events | POLLHUP | POLLERR | POLLNVAL)) != 0;
		}
		if (r < 0)
		{
			return 0;
		}
		svcSleepThread((s64)N3DS_FTP_NAP_MS * 1000000);
		waited += N3DS_FTP_NAP_MS;
		if ((timeoutMs > 0) && (waited >= timeoutMs))
		{
			return 0;
		}
	}
	return 0;
}

static int N3DS_FtpSendAll(int fd, const char *data, size_t n)
{
	while (n > 0)
	{
		ssize_t w;

		if (!N3DS_FtpWaitFd(fd, POLLOUT, 30000))
		{
			return 0;
		}
		/* Small pieces keep each call inside the SOC session short. */
		w = send(fd, data, (n > N3DS_FTP_SEND_CHUNK) ? N3DS_FTP_SEND_CHUNK : n, 0);
		if (w <= 0)
		{
			return 0;
		}
		data += w;
		n -= (size_t)w;
	}
	return 1;
}

static int N3DS_FtpReply(N3DS_FtpSession *s, const char *fmt, ...)
{
	char line[N3DS_FTP_LINE_MAX];
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprintf(line, sizeof(line) - 2, fmt, args);
	va_end(args);
	if (n < 0)
	{
		return 0;
	}
	if (n > (int)sizeof(line) - 3)
	{
		n = (int)sizeof(line) - 3;
	}
	line[n++] = '\r';
	line[n++] = '\n';
	return N3DS_FtpSendAll(s->ctrl, line, (size_t)n);
}

/* One command line, CR/LF stripped. False when the client left or we stop. */
static int N3DS_FtpReadLine(N3DS_FtpSession *s, char *out, unsigned cap)
{
	for (;;)
	{
		unsigned i;
		ssize_t r;

		for (i = 0; i < s->inUsed; i++)
		{
			if (s->in[i] == '\n')
			{
				unsigned n = i;

				if ((n > 0) && (s->in[n - 1] == '\r'))
				{
					n--;
				}
				if (n >= cap)
				{
					n = cap - 1;
				}
				memcpy(out, s->in, n);
				out[n] = 0;
				memmove(s->in, s->in + i + 1, s->inUsed - i - 1);
				s->inUsed -= i + 1;
				return 1;
			}
		}
		if (s->inUsed >= sizeof(s->in) - 1)
		{
			s->inUsed = 0; /* overlong line: drop it */
		}
		if (!N3DS_FtpWaitFd(s->ctrl, POLLIN, N3DS_FTP_IDLE_MS))
		{
			return 0;
		}
		r = recv(s->ctrl, s->in + s->inUsed, sizeof(s->in) - 1 - s->inUsed, 0);
		if (r <= 0)
		{
			return 0;
		}
		s->inUsed += (unsigned)r;
	}
}

/* ---- paths: "/" is the root of the SD card ---- */

static void N3DS_FtpResolvePath(const N3DS_FtpSession *s, const char *arg, char *out, unsigned cap)
{
	char joined[N3DS_FTP_PATH_MAX * 2];
	char parts[64][128];
	int count = 0;
	int i;
	unsigned n = 0;
	char *save = NULL;
	char *p;

	if ((arg == NULL) || (arg[0] == 0))
	{
		snprintf(joined, sizeof(joined), "%s", s->cwd);
	}
	else if (arg[0] == '/')
	{
		snprintf(joined, sizeof(joined), "%s", arg);
	}
	else
	{
		snprintf(joined, sizeof(joined), "%s/%s", s->cwd, arg);
	}

	/* Normalise: drop empty and "." parts, resolve "..". */
	for (p = strtok_r(joined, "/", &save); p != NULL; p = strtok_r(NULL, "/", &save))
	{
		if (strcmp(p, ".") == 0)
		{
			continue;
		}
		if (strcmp(p, "..") == 0)
		{
			if (count > 0)
			{
				count--;
			}
			continue;
		}
		if (count < 64)
		{
			snprintf(parts[count++], sizeof(parts[0]), "%s", p);
		}
	}

	out[0] = 0;
	for (i = 0; (i < count) && (n + 1 < cap); i++)
	{
		n += (unsigned)snprintf(out + n, cap - n, "/%s", parts[i]);
	}
	if (n == 0)
	{
		snprintf(out, cap, "/");
	}
}

static void N3DS_FtpRealPath(const char *virtualPath, char *out, unsigned cap)
{
	size_t n = strnlen(virtualPath, N3DS_FTP_PATH_MAX);

	if (n > cap - 6)
	{
		n = cap - 6;
	}
	memcpy(out, "sdmc:", 5);
	memcpy(out + 5, virtualPath, n);
	out[5 + n] = 0;
}

/* Install-only mode: may the client touch this path? `within`: 1 = inside the
 * update dir (files), 0 = also a parent of it (to walk down through). Full mode
 * allows everything. */
static int N3DS_FtpAllowed(const char *vpath, int within)
{
	const size_t n = strlen(N3DS_FTP_UPDATE_DIR);

	if (!s_ftpInstallOnly)
	{
		return 1;
	}
	if ((strncasecmp(vpath, N3DS_FTP_UPDATE_DIR, n) == 0) && ((vpath[n] == 0) || (vpath[n] == '/')))
	{
		return 1;
	}
	if (!within)
	{
		const size_t m = strlen(vpath);

		/* a parent: "/" or "/3ds" or "/3ds/ctr_native" */
		return (m == 1) || ((strncasecmp(vpath, N3DS_FTP_UPDATE_DIR, m) == 0) && (N3DS_FTP_UPDATE_DIR[m] == '/'));
	}
	return 0;
}

/* ---- data connection ---- */

static void N3DS_FtpClosePasv(N3DS_FtpSession *s)
{
	N3DS_FtpCloseSocket(&s->pasv);
}

static int N3DS_FtpOpenPasv(N3DS_FtpSession *s)
{
	unsigned attempt;

	N3DS_FtpClosePasv(s);
	for (attempt = 0; attempt < N3DS_FTP_PASV_COUNT; attempt++)
	{
		const unsigned port = N3DS_FTP_PASV_FIRST + (s_ftpNextPasv++ % N3DS_FTP_PASV_COUNT);
		struct sockaddr_in addr;
		int yes = 1;
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

		if (fd < 0)
		{
			return -1;
		}
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((uint16_t)port);
		addr.sin_addr.s_addr = INADDR_ANY;
		if ((bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) && (listen(fd, 1) == 0))
		{
			s->pasv = fd;
			return (int)port;
		}
		close(fd);
	}
	return -1;
}

/* Accepts the client's data connection; -1 when none came. */
static int N3DS_FtpAcceptData(N3DS_FtpSession *s)
{
	int fd = -1;

	if (s->pasv < 0)
	{
		return -1;
	}
	if (N3DS_FtpWaitFd(s->pasv, POLLIN, N3DS_FTP_DATA_ACCEPT_MS))
	{
		struct sockaddr_in peer;
		socklen_t len = sizeof(peer);

		fd = accept(s->pasv, (struct sockaddr *)&peer, &len);
	}
	N3DS_FtpClosePasv(s);
	return fd;
}

/* ---- commands ---- */

static void N3DS_FtpFormatListLine(char *out, unsigned cap, const char *name, const struct stat *st, int mlsd)
{
	const int dir = S_ISDIR(st->st_mode);

	if (mlsd)
	{
		snprintf(out, cap, "type=%s;size=%llu; %s\r\n", dir ? "dir" : "file", (unsigned long long)st->st_size, name);
		return;
	}
	/* libctru leaves st_mtime at 0: a fixed date reads better than 1970. */
	snprintf(out, cap, "%s 1 3ds 3ds %12llu Jan  1 00:00 %s\r\n", dir ? "drwxrwxrwx" : "-rw-rw-rw-",
	         (unsigned long long)st->st_size, name);
}

static void N3DS_FtpCmdList(N3DS_FtpSession *s, const char *arg, int kind /* 0 LIST, 1 NLST, 2 MLSD */)
{
	char vpath[N3DS_FTP_PATH_MAX], rpath[N3DS_FTP_PATH_MAX + 8];
	DIR *dir;
	struct dirent *e;
	int data;
	int ok = 1;

	/* Clients send options such as "-a" or "-la": not a path. */
	if ((arg != NULL) && (arg[0] == '-'))
	{
		const char *space = strchr(arg, ' ');

		arg = space ? space + 1 : "";
	}
	N3DS_FtpResolvePath(s, arg, vpath, sizeof(vpath));
	N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));
	dir = opendir(rpath);
	if (dir == NULL)
	{
		N3DS_FtpClosePasv(s);
		N3DS_FtpReply(s, "550 No such directory.");
		return;
	}
	N3DS_FtpReply(s, "150 Listing.");
	data = N3DS_FtpAcceptData(s);
	if (data < 0)
	{
		closedir(dir);
		N3DS_FtpReply(s, "425 No data connection.");
		return;
	}
	while (ok && ((e = readdir(dir)) != NULL))
	{
		char line[N3DS_FTP_PATH_MAX + 96];

		if ((strcmp(e->d_name, ".") == 0) || (strcmp(e->d_name, "..") == 0))
		{
			continue;
		}
		if (kind == 1)
		{
			snprintf(line, sizeof(line), "%s\r\n", e->d_name);
		}
		else
		{
			char full[N3DS_FTP_PATH_MAX * 2];
			struct stat st;

			snprintf(full, sizeof(full), "%s/%s", rpath, e->d_name);
			if (stat(full, &st) != 0)
			{
				memset(&st, 0, sizeof(st));
			}
			if (e->d_type == DT_DIR)
			{
				st.st_mode = S_IFDIR;
			}
			N3DS_FtpFormatListLine(line, sizeof(line), e->d_name, &st, kind == 2);
		}
		ok = N3DS_FtpSendAll(data, line, strlen(line));
	}
	closedir(dir);
	N3DS_FtpCloseSocket(&data);
	N3DS_FtpReply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void N3DS_FtpCmdRetr(N3DS_FtpSession *s, const char *arg)
{
	char vpath[N3DS_FTP_PATH_MAX], rpath[N3DS_FTP_PATH_MAX + 8];
	FILE *f;
	int data;
	int ok = 1;
	size_t n;

	N3DS_FtpResolvePath(s, arg, vpath, sizeof(vpath));
	N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));

	/* The game's log is buffered (16 KB) and flushed on a timer: commit what
	 * it has so the client sees the latest lines. */
	if (strncasecmp(vpath, "/ctr3ds", 7) == 0)
	{
		N3DS_LogFlushNow();
	}

	f = fopen(rpath, "rb");
	if (f == NULL)
	{
		N3DS_FtpClosePasv(s);
		N3DS_FtpReply(s, "550 Cannot open the file.");
		return;
	}
	if ((s->rest > 0) && (fseeko(f, (off_t)s->rest, SEEK_SET) != 0))
	{
		fclose(f);
		N3DS_FtpClosePasv(s);
		N3DS_FtpReply(s, "551 Cannot seek to the restart offset.");
		return;
	}
	N3DS_FtpReply(s, "150 Sending.");
	data = N3DS_FtpAcceptData(s);
	if (data < 0)
	{
		fclose(f);
		N3DS_FtpReply(s, "425 No data connection.");
		return;
	}
	while (ok && ((n = fread(s_ftpIo, 1, N3DS_FTP_IO_SIZE, f)) > 0))
	{
		ok = N3DS_FtpSendAll(data, (const char *)s_ftpIo, n);
	}
	fclose(f);
	N3DS_FtpCloseSocket(&data);
	N3DS_FtpReply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

/* STOR, or APPE when `append`. A pending REST makes STOR write from that offset
 * without truncating, so an interrupted upload resumes (curl -C -, lftp, WinSCP,
 * FileZilla all use REST or APPE for this). */
static void N3DS_FtpCmdStor(N3DS_FtpSession *s, const char *arg, int append)
{
	char vpath[N3DS_FTP_PATH_MAX], rpath[N3DS_FTP_PATH_MAX + 8];
	FILE *f;
	int data;
	int ok = 1;
	const int resume = (!append) && (s->rest > 0);
	int showCia = 0;

	N3DS_FtpResolvePath(s, arg, vpath, sizeof(vpath));
	N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));
	f = fopen(rpath, append ? "ab" : (resume ? "r+b" : "wb"));
	if ((f == NULL) || (resume && (fseeko(f, (off_t)s->rest, SEEK_SET) != 0)))
	{
		if (f != NULL)
		{
			fclose(f);
		}
		N3DS_FtpClosePasv(s);
		N3DS_FtpReply(s, "553 Cannot create the file.");
		return;
	}
	N3DS_FtpReply(s, "150 Receiving.");
	data = N3DS_FtpAcceptData(s);
	if (data < 0)
	{
		fclose(f);
		N3DS_FtpReply(s, "425 No data connection.");
		return;
	}
	/* A .cia on its way in is announced on the bottom screen (and its progress
	 * shown); any other file is not worth a banner. */
	if ((strlen(vpath) > 4) && (strcasecmp(vpath + strlen(vpath) - 4, ".cia") == 0) && (s_ftpPhase == 0))
	{
		s_ftpDone = s->rest;
		s_ftpTotal = s_ftpAllo;
		s_ftpNotice = "RECEIVING NEW BUILD";
		s_ftpPhase = 1;
		showCia = 1;
	}
	for (;;)
	{
		ssize_t r;

		if (!N3DS_FtpWaitFd(data, POLLIN, 30000))
		{
			ok = 0;
			break;
		}
		r = recv(data, s_ftpIo, N3DS_FTP_IO_SIZE, 0);
		if (r == 0)
		{
			break;
		}
		if ((r < 0) || (fwrite(s_ftpIo, 1, (size_t)r, f) != (size_t)r))
		{
			ok = 0;
			break;
		}
		if (showCia)
		{
			s_ftpDone += (unsigned long long)r;
		}
	}
	if (showCia)
	{
		s_ftpPhase = 0;
		s_ftpNotice = NULL;
	}
	s_ftpAllo = 0;
	/* The partial file stays on the card and is closed cleanly, so the client's
	 * SIZE says exactly where to resume from. */
	if (fclose(f) != 0)
	{
		ok = 0;
	}
	N3DS_FtpCloseSocket(&data);
	{
		char msg[N3DS_FTP_PATH_MAX + 64];

		snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: received %s%s", vpath, ok ? "" : " (failed)");
		N3DS_DebugLog(msg);
	}
	N3DS_FtpReply(s, ok ? "226 Done." : "451 Transfer failed.");
}

#ifdef CTR_INTERNAL
static char s_linkReply[660]; /* one FTP command at a time: no need for the stack */
#endif

static void N3DS_FtpCmdSite(N3DS_FtpSession *s, char *arg)
{
	char *path = strchr(arg, ' ');
	char *c;
	char vpath[N3DS_FTP_PATH_MAX];

	if (path != NULL)
	{
		*path++ = 0;
	}
	for (c = arg; *c; c++)
	{
		if ((*c >= 'a') && (*c <= 'z'))
		{
			*c = (char)(*c - 32);
		}
	}
	if ((strcmp(arg, "INSTALL") == 0) && (path != NULL) && (*path != 0))
	{
		char error[N3DS_FTP_PATH_MAX + 64];

		N3DS_FtpResolvePath(s, path, vpath, sizeof(vpath));
		if (!N3DS_FtpAllowed(vpath, 1))
		{
			N3DS_FtpReply(s, "550 Not allowed.");
			return;
		}
		s_ftpNotice = "INSTALLING NEW BUILD";
		s_ftpPhase = 2;
		if (N3DS_CiaInstall(vpath, &s_ftpDone, &s_ftpTotal, NULL, error, sizeof(error)))
		{
			char msg[N3DS_FTP_PATH_MAX + 48];

			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: installed %s, restarting in 10 s", vpath);
			N3DS_DebugLog(msg);
			N3DS_LogFlushNow();
			N3DS_FtpReply(s, "200 Installed %s; the game restarts in 10 s.", vpath);
			N3DS_FtpScheduleRelaunch();
		}
		else
		{
			char msg[N3DS_FTP_PATH_MAX + 96];

			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: install failed: %s", error);
			N3DS_DebugLog(msg);
			s_ftpPhase = 0;
			s_ftpNotice = NULL;
			N3DS_FtpReply(s, "550 Install failed: %s", error);
		}
	}
	else if (strcmp(arg, "INSTALL") == 0)
	{
		N3DS_FtpReply(s, "501 SITE INSTALL <path to a .cia of this game>");
	}
#ifdef CTR_INTERNAL
	else if ((path != NULL) ? N3DS_DebugLinkCommand(arg, path, s_linkReply, sizeof(s_linkReply)) : N3DS_DebugLinkCommand(arg, "", s_linkReply, sizeof(s_linkReply)))
	{
		N3DS_FtpReply(s, "%s", s_linkReply);
	}
#endif
	else
	{
		N3DS_FtpReply(s, "504 Unknown SITE command.");
	}
}

static void N3DS_FtpHandleSession(int ctrl)
{
	N3DS_FtpSession *s = &s_session;
	char line[N3DS_FTP_LINE_MAX];

	memset(s, 0, sizeof(*s));
	s->ctrl = ctrl;
	s->pasv = -1;
	/* Log in at the SD card's root, like any FTP server. */
	snprintf(s->cwd, sizeof(s->cwd), "/");
	if (s_ftpInstallOnly)
	{
		N3DS_FtpReply(s, "220 CTR 3D FTP (install only: " N3DS_FTP_UPDATE_DIR ").");
	}
	else
	{
		N3DS_FtpReply(s, "220 CTR 3D FTP. SD card root; the log is /ctr3ds.log.");
	}

	for (;;)
	{
		char vpath[N3DS_FTP_PATH_MAX], rpath[N3DS_FTP_PATH_MAX + 8];
		struct stat st;
		char *arg;
		char *c;
		int wasRest = 0;

		if (!s_ftpRun || !N3DS_FtpReadLine(s, line, sizeof(line)))
		{
			break;
		}
		arg = strchr(line, ' ');
		if (arg != NULL)
		{
			*arg++ = 0;
		}
		else
		{
			arg = line + strlen(line);
		}
		for (c = line; *c; c++)
		{
			if ((*c >= 'a') && (*c <= 'z'))
			{
				*c = (char)(*c - 32);
			}
		}

		/* Install-only: every path-taking command is checked up front. */
		if (s_ftpInstallOnly)
		{
			static const char *const pathCmds[] = {"LIST", "NLST", "MLSD", "RETR", "STOR", "APPE", "SIZE", "MDTM", "DELE", "RMD",
			                                       "XRMD", "MKD", "XMKD", "RNFR", "RNTO", "CWD", "CDUP"};
			unsigned k;

			for (k = 0; k < sizeof(pathCmds) / sizeof(pathCmds[0]); k++)
			{
				if (strcmp(line, pathCmds[k]) == 0)
				{
					const int isCd = (k >= 15);
					const char *a = (isCd && (line[1] == 'D')) ? ".." : arg;

					N3DS_FtpResolvePath(s, a, vpath, sizeof(vpath));
					if (!N3DS_FtpAllowed(vpath, isCd ? 0 : 1))
					{
						N3DS_FtpReply(s, "550 Not allowed (install-only mode).");
						goto next;
					}
					break;
				}
			}
		}

		if ((strcmp(line, "USER") == 0))
		{
			N3DS_FtpReply(s, "331 Any password will do.");
		}
		else if (strcmp(line, "PASS") == 0)
		{
			N3DS_FtpReply(s, "230 Logged in.");
		}
		else if (strcmp(line, "SYST") == 0)
		{
			N3DS_FtpReply(s, "215 UNIX Type: L8");
		}
		else if (strcmp(line, "FEAT") == 0)
		{
			N3DS_FtpReply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n MLSD\r\n REST STREAM\r\n EPSV\r\n PASV\r\n UTF8\r\n211 End");
		}
		else if ((strcmp(line, "OPTS") == 0) || (strcmp(line, "TYPE") == 0) || (strcmp(line, "MODE") == 0) ||
		         (strcmp(line, "STRU") == 0) || (strcmp(line, "NOOP") == 0))
		{
			N3DS_FtpReply(s, "200 OK.");
		}
		else if (strcmp(line, "ALLO") == 0)
		{
			/* The size of the next STOR (build3ds.sh --install sends it): the
			 * upload progress bar's total. */
			s_ftpAllo = strtoull(arg, NULL, 10);
			N3DS_FtpReply(s, "200 OK.");
		}
		else if (strcmp(line, "REST") == 0)
		{
			s->rest = strtoull(arg, NULL, 10);
			wasRest = 1;
			N3DS_FtpReply(s, "350 Restarting at %llu.", s->rest);
		}
		else if ((strcmp(line, "PWD") == 0) || (strcmp(line, "XPWD") == 0))
		{
			N3DS_FtpReply(s, "257 \"%s\"", s->cwd);
		}
		else if ((strcmp(line, "CWD") == 0) || (strcmp(line, "CDUP") == 0))
		{
			DIR *d;

			N3DS_FtpResolvePath(s, (strcmp(line, "CDUP") == 0) ? ".." : arg, vpath, sizeof(vpath));
			N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));
			d = opendir(rpath);
			if (d != NULL)
			{
				closedir(d);
				snprintf(s->cwd, sizeof(s->cwd), "%s", vpath);
				N3DS_FtpReply(s, "250 OK.");
			}
			else
			{
				N3DS_FtpReply(s, "550 No such directory.");
			}
		}
		else if (strcmp(line, "PASV") == 0)
		{
			int port = N3DS_FtpOpenPasv(s);

			if (port < 0)
			{
				N3DS_FtpReply(s, "425 Cannot open a data port.");
			}
			else
			{
				N3DS_FtpReply(s, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%d,%d).", (unsigned long)(s_ftpIp & 0xFF),
				              (unsigned long)((s_ftpIp >> 8) & 0xFF), (unsigned long)((s_ftpIp >> 16) & 0xFF),
				              (unsigned long)((s_ftpIp >> 24) & 0xFF), port >> 8, port & 0xFF);
			}
		}
		else if (strcmp(line, "EPSV") == 0)
		{
			int port = N3DS_FtpOpenPasv(s);

			if (port < 0)
			{
				N3DS_FtpReply(s, "425 Cannot open a data port.");
			}
			else
			{
				N3DS_FtpReply(s, "229 Entering Extended Passive Mode (|||%d|).", port);
			}
		}
		else if (strcmp(line, "LIST") == 0)
		{
			N3DS_FtpCmdList(s, arg, 0);
		}
		else if (strcmp(line, "NLST") == 0)
		{
			N3DS_FtpCmdList(s, arg, 1);
		}
		else if (strcmp(line, "MLSD") == 0)
		{
			N3DS_FtpCmdList(s, arg, 2);
		}
		else if (strcmp(line, "RETR") == 0)
		{
			N3DS_FtpCmdRetr(s, arg);
		}
		else if (strcmp(line, "STOR") == 0)
		{
			N3DS_FtpCmdStor(s, arg, 0);
		}
		else if (strcmp(line, "APPE") == 0)
		{
			N3DS_FtpCmdStor(s, arg, 1);
		}
		else if ((strcmp(line, "SIZE") == 0) || (strcmp(line, "MDTM") == 0))
		{
			N3DS_FtpResolvePath(s, arg, vpath, sizeof(vpath));
			N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));
			if ((strcmp(line, "SIZE") == 0) && (strncasecmp(vpath, "/ctr3ds", 7) == 0))
			{
				N3DS_LogFlushNow();
			}
			if ((stat(rpath, &st) != 0) || S_ISDIR(st.st_mode))
			{
				N3DS_FtpReply(s, "550 No such file.");
			}
			else if (line[0] == 'S')
			{
				N3DS_FtpReply(s, "213 %llu", (unsigned long long)st.st_size);
			}
			else
			{
				N3DS_FtpReply(s, "213 20260101000000");
			}
		}
		else if ((strcmp(line, "DELE") == 0) || (strcmp(line, "RMD") == 0) || (strcmp(line, "XRMD") == 0) ||
		         (strcmp(line, "MKD") == 0) || (strcmp(line, "XMKD") == 0))
		{
			int r;

			N3DS_FtpResolvePath(s, arg, vpath, sizeof(vpath));
			N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));
			if (line[0] == 'D')
			{
				r = remove(rpath);
			}
			else if (strstr(line, "RMD") != NULL)
			{
				r = rmdir(rpath);
			}
			else
			{
				r = mkdir(rpath, 0777);
			}
			if (r != 0)
			{
				N3DS_FtpReply(s, "550 Failed.");
			}
			else if (strstr(line, "MKD") != NULL)
			{
				N3DS_FtpReply(s, "257 \"%s\" created.", vpath);
			}
			else
			{
				N3DS_FtpReply(s, "250 OK.");
			}
		}
		else if (strcmp(line, "RNFR") == 0)
		{
			N3DS_FtpResolvePath(s, arg, s->rnfr, sizeof(s->rnfr));
			N3DS_FtpReply(s, "350 Ready for RNTO.");
		}
		else if (strcmp(line, "RNTO") == 0)
		{
			char from[N3DS_FTP_PATH_MAX + 8];

			N3DS_FtpRealPath(s->rnfr, from, sizeof(from));
			N3DS_FtpResolvePath(s, arg, vpath, sizeof(vpath));
			N3DS_FtpRealPath(vpath, rpath, sizeof(rpath));
			N3DS_FtpReply(s, (s->rnfr[0] && (rename(from, rpath) == 0)) ? "250 Renamed." : "550 Rename failed.");
			s->rnfr[0] = 0;
		}
		else if (strcmp(line, "SITE") == 0)
		{
			N3DS_FtpCmdSite(s, arg);
		}
		else if (strcmp(line, "QUIT") == 0)
		{
			N3DS_FtpReply(s, "221 Bye.");
			break;
		}
		else
		{
			N3DS_FtpReply(s, "502 Not implemented.");
		}

		/* REST only applies to the command right after it. */
		if (!wasRest)
		{
			s->rest = 0;
		}
	next:;
	}
	N3DS_FtpClosePasv(s);
}

/* The restart after a successful install, shared with the in-game updater: the
 * bottom screen's banner counts the 10 s down, then N3DS_FtpRelaunchDue (polled
 * by the main loop) makes the game quit through the normal path and chainload
 * itself. Callable from any thread. */
void N3DS_FtpScheduleRelaunch(void)
{
	s_ftpNotice = "RESTARTING IN";
	s_ftpPhase = 3;
	s_ftpTotal = N3DS_FTP_RELAUNCH_DELAY_MS;
	s_ftpRelaunchAt = osGetTime() + N3DS_FTP_RELAUNCH_DELAY_MS;
}

/* ---- thread ---- */

/* Opens the listen socket; -1 on failure (nothing left open). */
static int N3DS_FtpOpenListen(void)
{
	struct sockaddr_in addr;
	int yes = 1;
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

	if (fd < 0)
	{
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(N3DS_FTP_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;
	if ((bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) && (listen(fd, 1) == 0))
	{
		return fd;
	}
	N3DS_FtpCloseSocket(&fd);
	return -1;
}

/* Sleeps up to ms, in slices, returning early when the server is told to stop. */
static void N3DS_FtpNap(int ms)
{
	while ((ms > 0) && s_ftpRun)
	{
		int slice = (ms > 100) ? 100 : ms;

		svcSleepThread((s64)slice * 1000000);
		ms -= slice;
	}
}

/*
 * Called from the APT hook (main thread): sleep mode / suspend / wake / restore.
 * The Wi-Fi link and the sockets do not survive the console's sleep, so the
 * server tears the listen socket down and listens again. Never blocks.
 */
void N3DS_FtpAptEvent(int type)
{
	if ((type == APTHOOK_ONSLEEP) || (type == APTHOOK_ONWAKEUP))
	{
		s_ftpWakeGen++;
	}
}

static void N3DS_FtpThreadMain(void *argument)
{
	int backoffMs = 1000;

	(void)argument;

	while (s_ftpRun)
	{
		unsigned gen = s_ftpWakeGen;
		int reason = 0; /* 1 dead socket, 2 wake, 3 Wi-Fi lost, 4 address changed */
		int wifiLostMs = 0;
		int checkMs = 0;
		char msg[96];
		struct in_addr ia;

		s_ftpIp = 0;
		/* Wait for Wi-Fi (the game may start long before it is up, or the link
		 * may have dropped), then listen. */
		if (osGetWifiStrength() == 0)
		{
			N3DS_FtpNap(1000);
			continue;
		}
		s_ftpListen = N3DS_FtpOpenListen();
		ia.s_addr = (uint32_t)gethostid();
		if ((s_ftpListen < 0) || (ia.s_addr == 0))
		{
			N3DS_FtpCloseSocket(&s_ftpListen);
			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: listen failed (errno %d), retry in %d ms", errno, backoffMs);
			N3DS_DebugLog(msg);
			N3DS_FtpNap(backoffMs);
			backoffMs = (backoffMs < 8000) ? (backoffMs * 2) : 8000;
			continue;
		}
		backoffMs = 1000;
		s_ftpSessionGen = gen;
		s_ftpIp = ia.s_addr;
		snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: serving on %s:%d", inet_ntoa(ia), N3DS_FTP_PORT);
		N3DS_DebugLog(msg);

		while (s_ftpRun && (reason == 0))
		{
			struct sockaddr_in peer;
			socklen_t len = sizeof(peer);
			struct pollfd p;
			int fd;
			int r;

			if (s_ftpWakeGen != gen)
			{
				reason = 2;
				break;
			}
			/* Once a second: is the link still there, with the same address? */
			if (checkMs >= 1000)
			{
				uint32_t now;

				checkMs = 0;
				if (osGetWifiStrength() == 0)
				{
					wifiLostMs += 1000;
					if (wifiLostMs >= 3000)
					{
						reason = 3;
						break;
					}
				}
				else
				{
					wifiLostMs = 0;
					now = (uint32_t)gethostid();
					if ((now != 0) && (now != s_ftpIp))
					{
						reason = 4;
						break;
					}
				}
			}

			/* Nobody connected: a longer nap than a transfer's, to stay cheap. */
			p.fd = s_ftpListen;
			p.events = POLLIN;
			p.revents = 0;
			r = poll(&p, 1, 0);
			if ((r < 0) || ((r > 0) && ((p.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)))
			{
				reason = 1;
				break;
			}
			if (r == 0)
			{
				svcSleepThread((s64)N3DS_FTP_IDLE_NAP_MS * 1000000);
				checkMs += N3DS_FTP_IDLE_NAP_MS;
				continue;
			}
			fd = accept(s_ftpListen, (struct sockaddr *)&peer, &len);
			if (fd < 0)
			{
				if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == ECONNABORTED) || (errno == EINTR))
				{
					continue;
				}
				reason = 1;
				break;
			}
			N3DS_FtpHandleSession(fd);
			N3DS_FtpCloseSocket(&fd);
			checkMs = 1000; /* check the link right after a session */
		}

		N3DS_FtpCloseSocket(&s_ftpListen);
		s_ftpIp = 0;
		if (!s_ftpRun)
		{
			break;
		}
		switch (reason)
		{
		case 1:
			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: listen socket dead (errno %d), re-listening", errno);
			break;
		case 2:
			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: wake, re-listen");
			break;
		case 3:
			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: Wi-Fi lost, waiting for it");
			break;
		default:
			snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: address changed, re-listen");
			break;
		}
		N3DS_DebugLog(msg);
		/* Let the link settle (after wake it needs a moment) before listening again. */
		N3DS_FtpNap((reason == 2) ? 1500 : 500);
	}
	N3DS_FtpCloseSocket(&s_ftpListen);
}

/*
 * Exit: ask the thread to stop, wake it (shutdown() on a connected control or
 * data socket makes a blocked read/send return), join it with a timeout, and
 * only then close the listen socket. Called first of the exit sequence
 * (N3DS_ExitBegin) -- the relaunch after SITE INSTALL included -- so the thread
 * is gone before audio, GPU and the socket service are. If it does not end in
 * 3 s it is left alone and socExit is skipped (N3DS_FtpActive); the kernel
 * reaps it with the process.
 */
void N3DS_FtpShutdown(void)
{
	int fd;

	s_ftpRun = 0;
	if (s_ftpThread == NULL)
	{
		return;
	}
	fd = s_session.ctrl;
	if (fd >= 0)
	{
		shutdown(fd, SHUT_RDWR);
	}
	fd = s_session.pasv;
	if (fd >= 0)
	{
		shutdown(fd, SHUT_RDWR);
	}
	if (threadJoin(s_ftpThread, 3000000000ULL) == 0)
	{
		threadFree(s_ftpThread);
		N3DS_FtpCloseSocket(&s_ftpListen);
	}
	else
	{
		s_ftpStuck = 1;
		N3DS_DebugLog("[CTR-3DS] exit: ftp thread did not end in 3 s, left running");
	}
	s_ftpThread = NULL;
}

static void N3DS_FtpAtExit(void)
{
	N3DS_FtpShutdown();
}

/* socExit is skipped by the backend when the thread would not end: with a client still
 * connected it never returned in the author's other ports (the kernel closes
 * the SOC session when the process ends anyway). */
int N3DS_FtpActive(void)
{
	return s_ftpStuck;
}

/* Brings the server thread up (no flag-file check). Main thread only. */
static int N3DS_FtpStartThread(void)
{
	/* Cores 2 and 3 only exist on the New 3DS (and are withheld by the
	 * Old-3DS simulation); core 1 is forbidden. Else core 0 behind the game. */
	const int spareCores = N3DS_ExtraCoresUsable();
	char msg[128];

	if ((s_ftpThread != NULL) || s_ftpStuck || (s_ftpInstallOnly && s_ftpCancelled))
	{
		return 0; /* already up, still stopping, wedged, or cancelled for this run */
	}
	if (!N3DS_O3dsSimFtpAllowed())
	{
		N3DS_DebugLog("[CTR-3DS] ftp: NO (Old-3DS simulation with ftp=0)");
		return 0;
	}
	if (!N3DS_NetEnsureSoc())
	{
		N3DS_DebugLog("[CTR-3DS] ftp: NO (socket service unavailable)");
		return 0;
	}
	s_ftpIo = (unsigned char *)malloc(N3DS_FTP_IO_SIZE);
	if (s_ftpIo == NULL)
	{
		N3DS_DebugLog("[CTR-3DS] ftp: NO (out of memory)");
		return 0;
	}

	s_ftpIp = 0;
	s_ftpRun = 1;
	{
		static const int newCores[] = {3, 2};
		static const int oldCores[] = {0};
		const int *cores = spareCores ? newCores : oldCores;
		const int coreCount = spareCores ? 2 : 1;
		int i;
		int core = -1;

		for (i = 0; (i < coreCount) && (s_ftpThread == NULL); i++)
		{
			core = cores[i];
			s_ftpThread = threadCreate(N3DS_FtpThreadMain, NULL, N3DS_FTP_STACK_SIZE, N3DS_FTP_PRIORITY, core, false);
		}
		if (s_ftpThread == NULL)
		{
			s_ftpRun = 0;
			free(s_ftpIo);
			s_ftpIo = NULL;
			N3DS_DebugLog(spareCores ? "[CTR-3DS] ftp: NO (threadCreate failed on cores 3 and 2)" : "[CTR-3DS] ftp: NO (threadCreate failed on core 0)");
			return 0;
		}
		snprintf(msg, sizeof(msg), "[CTR-3DS] ftp: thread up (priority 0x%X, core %d%s), waiting for Wi-Fi", N3DS_FTP_PRIORITY, core,
		         spareCores ? "" : ": Old 3DS, behind the game, polls every 50 ms idle");
	}
	if (!s_ftpAtExitSet)
	{
		atexit(N3DS_FtpAtExit);
		s_ftpAtExitSet = 1;
	}
	N3DS_DebugLog(msg);
	return 1;
}

/*
 * Boot decision (N3DS_EarlyInit, after the settings file is read):
 *   CTR_INTERNAL builds: the full server (the whole SD card as "/", every
 *     command, SITE INSTALL) is ON unless the bottom UI's Debug > FTP toggle
 *     turned it off (persisted as ftp=0 in ctr3d.cfg).
 *   Production builds: nothing, unless dev-ftp.txt is in the app dir; then the
 *     install-only server (only the update dir, SITE INSTALL only, CANCEL on
 *     the bottom screen, never started again after a cancel).
 */
void N3DS_FtpBoot(void)
{
#ifdef CTR_INTERNAL
	if (N3DS_ConfigGet("ftp", 1))
	{
		mkdir(N3DS_APP_DIR "/update", 0777);
		(void)N3DS_FtpStartThread();
	}
#else
	struct stat st;

	if (stat(N3DS_FTP_DEV_FLAG, &st) == 0)
	{
		mkdir(N3DS_APP_DIR "/update", 0777);
		s_ftpInstallOnly = 1;
		(void)N3DS_FtpStartThread();
	}
#endif
}

int N3DS_FtpInstallOnly(void)
{
	return s_ftpInstallOnly && (s_ftpThread != NULL) && s_ftpRun;
}

/* The CANCEL button (install-only): ends the server for the rest of the run. */
void N3DS_FtpCancel(void)
{
	s_ftpCancelled = 1;
	if ((s_ftpThread != NULL) && s_ftpRun)
	{
		s_ftpRun = 0;
		N3DS_DebugLog("[CTR-3DS] ftp: install server cancelled from the bottom screen");
	}
}

const char *N3DS_FtpNotice(void)
{
	return s_ftpNotice;
}

int N3DS_FtpProgress(unsigned long long *done, unsigned long long *total)
{
	const int phase = s_ftpPhase;

	*done = *total = 0;
	if (phase == 3)
	{
		const unsigned long long now = osGetTime(), at = s_ftpRelaunchAt;

		*done = (now >= at) ? N3DS_FTP_RELAUNCH_DELAY_MS : (N3DS_FTP_RELAUNCH_DELAY_MS - (at - now));
		*total = N3DS_FTP_RELAUNCH_DELAY_MS;
	}
	else if (phase != 0)
	{
		*done = s_ftpDone;
		*total = s_ftpTotal;
	}
	return phase;
}

/* ---- runtime control (debug UI, main thread only) ---- */

int N3DS_FtpStatus(void)
{
	if (s_ftpStuck)
	{
		return N3DS_FTP_STATUS_STUCK;
	}
	if (s_ftpThread == NULL)
	{
		return N3DS_FTP_STATUS_OFF;
	}
	if (!s_ftpRun)
	{
		return N3DS_FTP_STATUS_STOPPING;
	}
	return (s_ftpIp != 0) ? N3DS_FTP_STATUS_LISTENING : N3DS_FTP_STATUS_WAITING_WIFI;
}

unsigned int N3DS_FtpAddress(void)
{
	return (unsigned int)s_ftpIp;
}

int N3DS_FtpPort(void)
{
	return N3DS_FTP_PORT;
}

/* Asks the thread to stop without waiting for it: a join here would stall the
 * game thread for up to the Wi-Fi poll nap or a transfer chunk. N3DS_FtpPoll
 * reaps the thread once it has ended. */
void N3DS_FtpSetEnabled(int on)
{
	if (on)
	{
		(void)N3DS_FtpStartThread();
	}
	else if ((s_ftpThread != NULL) && s_ftpRun)
	{
		s_ftpRun = 0;
		N3DS_DebugLog("[CTR-3DS] ftp: stop requested from the debug UI");
	}
}

/* Reaps a stopped server thread (non-blocking). Called once per frame while
 * the debug UI is open; if the UI is closed mid-stop the at-exit join still
 * covers it. */
void N3DS_FtpPoll(void)
{
	if ((s_ftpThread != NULL) && !s_ftpRun && !s_ftpStuck)
	{
		if (threadJoin(s_ftpThread, 0ULL) == 0)
		{
			threadFree(s_ftpThread);
			s_ftpThread = NULL;
			free(s_ftpIo);
			s_ftpIo = NULL;
			s_ftpIp = 0;
			N3DS_DebugLog("[CTR-3DS] ftp: stopped");
		}
	}
}

int N3DS_FtpRelaunchDue(void)
{
	if ((s_ftpRelaunchAt == 0) || (osGetTime() < s_ftpRelaunchAt))
	{
		return 0;
	}
	if (!s_ftpRelaunchArmed)
	{
		s_ftpRelaunchArmed = 1;
		/* libctru's aptExit() starts this title again once the process has exited
		 * through the normal path, as HOME -> close does; jumping at once made the
		 * loader load the title twice and fault out of memory on the author's
		 * other ports (hence the 10 s above). The installed CIA IS this title, so
		 * "self" is the new build -- no .3dsx path is involved. */
		aptSetChainloaderToSelf();
		N3DS_DebugLog("[CTR-3DS] ftp: relaunching into the installed build");
		N3DS_LogFlushNow();
	}
	return 1;
}
