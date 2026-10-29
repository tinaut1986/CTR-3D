/*
 * CIA install over the running title, through AM. The one implementation behind
 * both the FTP server's SITE INSTALL (n3ds_ftp.c) and the in-game updater
 * (n3ds_updater.c); same steps as the author's other ports (zelda-tmc-3ds
 * cia_install_3ds.c, mzm).
 *
 * The CIA must carry the RUNNING title's ID (so the game has to run as an
 * installed CIA); it is installed with AM_StartCiaInstallOverwrite, the
 * installed title is never deleted first. The caller restarts the game after a
 * success (N3DS_FtpScheduleRelaunch).
 */
#ifndef N3DS_CIAINSTALL_H
#define N3DS_CIAINSTALL_H

/* sdPath: an SD path such as "/3ds/ctr_native/update/dev.cia" (no "sdmc:"). On
 * success returns nonzero. On failure returns 0 and `error` holds a short reason.
 * done/total (both may be NULL) are bytes written/total, updated as it goes;
 * *cancel (may be NULL) is polled between chunks, nonzero aborts the install
 * (AM cancels it, the old build stays). */
int N3DS_CiaInstall(const char *sdPath, volatile unsigned long long *done, volatile unsigned long long *total,
                    const volatile int *cancel, char *error, unsigned errorCap);

#endif
