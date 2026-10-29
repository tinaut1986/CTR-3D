/*
 * In-game updater (3DS). Copied from the author's zelda-tmc-3ds updater.c and
 * adapted: it looks at the GitHub releases of tinaut1986/CTR-3D, shows the newest
 * one of the chosen channel with its release notes, downloads the CIA to the SD
 * card, checks its SHA-256 and title ID, installs it through the same AM path as
 * the FTP server's SITE INSTALL (n3ds_ciainstall.c) and restarts the game through
 * the same relaunch (N3DS_FtpScheduleRelaunch / N3DS_FtpRelaunchDue).
 *
 * Nothing runs by itself except the optional check at boot (AUTO CHECK, off by
 * default); downloading and installing always wait for the player. The network
 * work is a thread of its own at the lowest priority, so the game never waits
 * for it.
 *
 * Plain scalars and C strings only: the bottom UI (a libctru TU) includes this.
 */
#ifndef CTR_UPDATER_H
#define CTR_UPDATER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum UpdateState {
  UPDATE_IDLE, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_AVAILABLE,
  UPDATE_EMPTY, UPDATE_DOWNLOADING, UPDATE_VERIFYING, UPDATE_INSTALLING,
  UPDATE_DONE, UPDATE_ERROR
} UpdateState;

typedef struct UpdateStatus {
  UpdateState state;
  bool betas;                 /* + BETAS channel selected */
  bool prerelease;            /* the release found is a pre-release */
  unsigned progress, revision; /* percent of the running step; bumps on every change */
  char version[56], message[80];
} UpdateStatus;

/* Main thread, after the settings file is read (N3DS_EarlyInit). Loads the
 * channel ("betas" in ctr3d.cfg); starts nothing. */
void Updater_Init(void);
/* Main thread: the check at boot when the player turned AUTO CHECK on (once the
 * first frame is up). A newer build only marks the UPDATER tab, it installs nothing. */
void Updater_AutoCheck(void);
bool Updater_AutoCheckEnabled(void);
void Updater_SetAutoCheck(bool on);

void Updater_Check(void);
void Updater_Download(void);
void Updater_Cancel(void);
/* Picks the channel (persisted) and checks again. */
void Updater_SetBetas(bool betas);
void Updater_GetStatus(UpdateStatus *out);
bool Updater_Busy(void);
/* The release body of the release found (or, for several newer ones, the joined
 * notes); returns the status revision it belongs to. */
unsigned Updater_GetNotes(char *out, unsigned capacity);
/* Exit: cancels and joins the thread (3 s at most). */
void Updater_Shutdown(void);
/* Nonzero when the thread did not end at exit (socExit must be skipped). */
int Updater_Active(void);
/* True when the running build is not an installed CIA (a .3dsx under the Homebrew Launcher): it can check but not install. */
bool Updater_CannotInstall(void);

#endif
