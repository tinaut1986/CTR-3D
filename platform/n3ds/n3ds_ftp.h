/*
 * In-game FTP server (3DS), for debugging and updating from a PC: pull
 * ctr3ds.log and files off the SD card, and send a new CIA that the game
 * installs over itself. Same design as the author's other 3DS ports
 * (zelda-tmc-3ds ftp_server_3ds.h, sm-3ds, mzm).
 *
 * Two modes, picked at boot (N3DS_FtpBoot):
 *   - CTR_INTERNAL (debug) builds: the full server, ON by default, the whole SD
 *     card as "/". The bottom UI's Debug > Actions > FTP button switches it
 *     off/on; the choice is saved (ftp=0/1 in sdmc:/3ds/ctr_native/ctr3d.cfg).
 *   - Production builds: no server, unless an (empty) dev-ftp.txt is in
 *     sdmc:/3ds/ctr_native/. Then the INSTALL-ONLY server runs: only
 *     /3ds/ctr_native/update is reachable, SITE INSTALL is the only SITE
 *     command, the bottom screen shows the address with a CANCEL button, and
 *     after a cancel it does not start again until the game is relaunched.
 *
 * Port 5000, anonymous (any user/password), one client at a time, passive mode
 * only. Commands: USER PASS SYST FEAT TYPE MODE STRU NOOP OPTS PWD CWD CDUP PASV
 * EPSV LIST NLST MLSD RETR STOR APPE REST SIZE MDTM DELE MKD RMD RNFR RNTO ALLO
 * QUIT SITE INSTALL.
 *
 * Remote CIA install: upload the CIA (announce its size with ALLO for a
 * percentage), then "SITE INSTALL <sd path>". curl:
 *   curl -T game.cia ftp://IP:5000/3ds/ctr_native/update/dev.cia \
 *        -Q "-SITE INSTALL /3ds/ctr_native/update/dev.cia"
 * (build3ds.sh --install does both.) The CIA must carry the RUNNING title's ID
 * (the game must be running as an installed CIA); it is installed over it
 * through AM, the bottom screen warns the whole time (upload %, install %, then a
 * 10 s restart countdown), the game then quits through the normal exit path and
 * libctru's aptExit() starts the freshly installed title.
 */
#ifndef N3DS_FTP_H
#define N3DS_FTP_H

/* N3DS_EarlyInit (main thread, before the worker threads), after the settings
 * file is read. */
void N3DS_FtpBoot(void);

/* Nonzero once an install has succeeded and its countdown has passed: the main
 * loop should quit so the new build starts. Arms the self-chainload. */
int N3DS_FtpRelaunchDue(void);

/* Arms the same restart as a successful SITE INSTALL (banner countdown of 10 s,
 * then the main loop quits and the freshly installed build starts). Used by the
 * in-game updater (n3ds_updater.c) after its own install. Any thread. */
void N3DS_FtpScheduleRelaunch(void);

/* Exit sequence: stop the thread (join, 3 s timeout) and close its sockets. */
void N3DS_FtpShutdown(void);

/* Nonzero when the server thread did not end at exit (socExit must be skipped). */
int N3DS_FtpActive(void);

/* Install-only mode is serving right now / end it for this run (CANCEL). */
int N3DS_FtpInstallOnly(void);
void N3DS_FtpCancel(void);

/* While a .cia is received or installed or the restart countdown runs: the line
 * for the bottom screen's banner; NULL otherwise. Progress: 1 receiving, 2
 * installing, 3 countdown (done/total in bytes, ms for 3); total 0 = unknown. */
const char *N3DS_FtpNotice(void);
int N3DS_FtpProgress(unsigned long long *done, unsigned long long *total);

/* Runtime control for the bottom UI (main thread only). Start/stop do not
 * block: stop only asks the thread to end, N3DS_FtpPoll reaps it. */
#define N3DS_FTP_STATUS_OFF 0
#define N3DS_FTP_STATUS_WAITING_WIFI 1
#define N3DS_FTP_STATUS_LISTENING 2
#define N3DS_FTP_STATUS_STOPPING 3
#define N3DS_FTP_STATUS_STUCK 4
int N3DS_FtpStatus(void);
unsigned int N3DS_FtpAddress(void); /* IPv4, network byte order; 0 until listening */
int N3DS_FtpPort(void);
void N3DS_FtpSetEnabled(int on);
void N3DS_FtpPoll(void);

#endif
