/*
 * Minimal in-game FTP server (New 3DS), for pulling ctr3ds.log and files off
 * the SD card and for sending a new CIA without taking the card out. Same
 * design as the author's other 3DS ports (mzm, sm-3ds).
 *
 * Off by default. It starts only when the flag file
 *   sdmc:/3ds/ctr_native/ftp.on
 * exists at boot; with no file nothing is allocated and no thread exists.
 *
 * Port 5021, anonymous (any user/password), one client at a time, passive mode
 * only, root = sdmc:/. Commands: USER PASS SYST FEAT TYPE MODE STRU NOOP OPTS
 * PWD CWD CDUP PASV EPSV LIST NLST MLSD RETR STOR APPE REST SIZE MDTM DELE MKD
 * RMD RNFR RNTO ALLO QUIT SITE INSTALL.
 *
 * Remote CIA install: upload the CIA, then "SITE INSTALL <sd path>" (curl:
 * -T game.cia ftp://IP:5021/ctr.cia -Q "-SITE INSTALL /ctr.cia"). The CIA must
 * carry this title's ID; it is installed over the running title through AM,
 * then the game quits cleanly ~3 s later and libctru chainloads the new build.
 */
#ifndef N3DS_FTP_H
#define N3DS_FTP_H

/* Called from N3DS_EarlyInit (main thread, before the worker threads). */
void N3DS_FtpStartIfEnabled(void);

/* Nonzero once an install has succeeded and its grace period has passed: the
 * main loop should quit so the new build starts. */
int N3DS_FtpRelaunchDue(void);

/* Nonzero when the server thread did not end at exit (socExit must be skipped). */
int N3DS_FtpActive(void);

/* Runtime control for the debug UI (main thread only). Start/stop do not
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
