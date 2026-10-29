/*
 * In-game updater. See updater.h. Adapted from zelda-tmc-3ds platform/3ds/source/
 * updater.c (same flow, same checks); differences: no 3DSX path (CTR 3D publishes
 * the CIA only), the channel and AUTO CHECK live in ctr3d.cfg, the CA bundle is
 * linked into the binary instead of a romfs, the install goes through the shared
 * N3DS_CiaInstall and the restart through the FTP server's relaunch.
 *
 * Compiled with -fshort-enums (libctru, libcurl and jansson were built with the
 * ARM EABI default; the game's own TU uses 4-byte enums, see CMakeLists.txt).
 */
#include "updater.h"
#include "update_manifest.h"
#include "ctr_version.h"

#include <3ds.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n3ds_backend.h"
#include "n3ds_ciainstall.h"
#include "n3ds_ftp.h"

/* The Mozilla CA set (curl.se/ca/cacert.pem), embedded by bin2s (CMakeLists.txt). */
#include "update_ca.h"

static void UpdateLog(const char *format, ...) {
  char line[256]; va_list args; va_start(args, format);
  vsnprintf(line, sizeof(line), format, args); va_end(args);
  N3DS_DebugLog(line);
}

#define UPDATE_DIR N3DS_APP_DIR "/update"
#define UPDATE_PART UPDATE_DIR "/download.part"
#define UPDATE_PART_FS "/3ds/ctr_native/update/download.part" /* the same file, as AM's SD path */
#define UPDATE_IO_SIZE (128u * 1024u)
#define MANIFEST_LIMIT (2u * 1024u * 1024u)
#define UPDATE_THREAD_STACK (96 * 1024)
#define UPDATE_PRIORITY 0x3F /* the lowest: the game never waits for the network */

static LightLock lock;
static UpdateStatus status;
static UpdateRelease release;
static Thread worker;
static bool initialized, busy, cancel, download_job, auto_check, stuck;
static volatile int install_cancel; /* read by N3DS_CiaInstall between chunks */

static bool cancelled(void) { return __atomic_load_n(&cancel, __ATOMIC_ACQUIRE) || aptShouldClose(); }
static void publish(UpdateState state, const char *message) {
  LightLock_Lock(&lock);
  status.state = state; status.progress = 0; status.revision++;
  snprintf(status.message, sizeof(status.message), "%s", message ? message : "");
  LightLock_Unlock(&lock);
}
void Updater_GetStatus(UpdateStatus *out) {
  if (!initialized) { memset(out, 0, sizeof(*out)); return; }
  LightLock_Lock(&lock); *out = status; LightLock_Unlock(&lock);
}
static void progress(unsigned percent) {
  if (percent > 100) percent = 100;
  LightLock_Lock(&lock);
  if (status.progress != percent) { status.progress = percent; status.revision++; }
  LightLock_Unlock(&lock);
}
bool Updater_Busy(void) { return __atomic_load_n(&busy, __ATOMIC_ACQUIRE); }
void Updater_Cancel(void) {
  __atomic_store_n(&cancel, true, __ATOMIC_RELEASE);
  install_cancel = 1;
}
bool Updater_CannotInstall(void) { return envIsHomebrew(); }

typedef struct Transfer { char *data; size_t size, capacity; FILE *file; uint32_t expected; size_t pending; unsigned writes; bool io_error; } Transfer;
/* Batch small TLS records before crossing into the SD service. Keep the buffer
 * on the heap: the worker stack must also accommodate curl and TLS. */
static bool open_download(Transfer *t) {
  t->data = malloc(UPDATE_IO_SIZE);
  if (!t->data) return false;
  t->capacity = UPDATE_IO_SIZE;
  t->file = fopen(UPDATE_PART, "wb");
  if (!t->file) return false;
  /* Our explicit buffer sets the write size independently of newlib's BUFSIZ. */
  return setvbuf(t->file, NULL, _IONBF, 0) == 0;
}
static bool flush_download(Transfer *t) {
  if (cancelled() || t->io_error) return false;
  if (!t->pending) return true;
  t->writes++;
  if (write(fileno(t->file), t->data, t->pending) != (ssize_t)t->pending) {
    t->io_error = true;
    return false;
  }
  t->pending = 0;
  return true;
}
static bool close_download(Transfer *t, bool fetched) {
  /* Never accept an incomplete body, a failed final write, or a close error. */
  bool ok = fetched && t->size == t->expected && flush_download(t);
  if (fclose(t->file)) { t->io_error = true; ok = false; }
  t->file = NULL;
  free(t->data); t->data = NULL; t->capacity = 0;
  if (t->io_error) publish(UPDATE_ERROR, "SD CARD WRITE FAILED");
  if (ok) progress(100);
  return ok;
}
static size_t receive(void *ptr, size_t a, size_t b, void *userdata) {
  Transfer *t = userdata;
  if (a && b > SIZE_MAX / a) return 0;
  size_t n = a * b, limit = t->file ? t->expected : MANIFEST_LIMIT;
  if (cancelled() || t->size > limit || n > limit - t->size) return 0;
  if (t->file) {
    if (!t->data || !t->capacity || t->io_error) return 0;
    const char *source = ptr;
    size_t left = n;
    while (left) {
      size_t take = t->capacity - t->pending;
      if (take > left) take = left;
      memcpy(t->data + t->pending, source, take);
      t->pending += take; source += take; left -= take;
      if (t->pending == t->capacity && !flush_download(t)) return 0;
    }
    unsigned percent = t->expected ? (unsigned)((t->size + n) * 100ull / t->expected) : 0;
    progress(percent < 100 ? percent : 99);
  } else {
    if (t->size + n + 1 > t->capacity) {
      size_t cap = t->size + n + 4096;
      char *p = realloc(t->data, cap); if (!p) return 0;
      t->data = p; t->capacity = cap;
    }
    memcpy(t->data + t->size, ptr, n); t->data[t->size + n] = 0;
  }
  t->size += n; return n;
}
static int transfer_progress(void *p, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t sent) {
  (void)p; (void)total; (void)now; (void)up; (void)sent;
  return cancelled() ? 1 : 0;
}
static bool fetch(const char *url, Transfer *t) {
  CURL *c = curl_easy_init(); if (!c) return false;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "CTR-3D-tinaut/" CTR_PORT_VERSION);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  struct curl_blob ca = { (void *)update_ca_pem, (size_t)update_ca_pem_size, CURL_BLOB_NOCOPY };
  curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, t->file ? 1800L : 90L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 128L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 15L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)UPDATE_IO_SIZE);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, transfer_progress);
  CURLcode code = curl_easy_perform(c);
  long http = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  if (t->file) {
    double seconds = 0;
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &seconds);
    UpdateLog("[CTR-3DS] updater download: bytes=%lu seconds=%.2f KiB/s=%.1f SD-writes=%u",
      (unsigned long)t->size, seconds, seconds > 0 ? t->size / (1024.0 * seconds) : 0, t->writes);
  }
  curl_easy_cleanup(c);
  if (code != CURLE_OK || http != 200) {
    UpdateLog("[CTR-3DS] updater transfer failed: curl=%d http=%ld", code, http);
    return false;
  }
  return true;
}
static bool enough_space(uint64_t required) {
  FS_ArchiveResource r;
  return R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) &&
    (uint64_t)r.freeClusters * r.clusterSize >= required;
}
static bool verify_file(void) {
  FILE *f = fopen(UPDATE_PART, "rb"); if (!f) return false;
  mbedtls_sha256_context sha; mbedtls_sha256_init(&sha);
  bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0;
  unsigned char *data = malloc(UPDATE_IO_SIZE), digest[32]; size_t total = 0; ssize_t n = 0;
  ok = ok && data != NULL;
  while (ok && (n = read(fileno(f), data, UPDATE_IO_SIZE)) > 0) {
    total += n; ok = !cancelled() && mbedtls_sha256_update_ret(&sha, data, n) == 0;
  }
  ok = ok && n >= 0 && total == release.size && mbedtls_sha256_finish_ret(&sha, digest) == 0;
  fclose(f); free(data); mbedtls_sha256_free(&sha);
  char hex[65]; for (int i = 0; i < 32 && ok; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return ok && !strcmp(hex, release.sha256);
}
/* Install through the shared AM path (the FTP server's SITE INSTALL uses the same). */
static bool install_cia(void) {
  char error[128];
  install_cancel = 0;
  bool ok = N3DS_CiaInstall(UPDATE_PART_FS, NULL, NULL, &install_cancel, error, sizeof(error)) != 0;
  UpdateLog("[CTR-3DS] updater CIA installation: success=%d %s", ok, ok ? "" : error);
  return ok;
}
static void run_job(void *arg) {
  (void)arg;
  bool curl_ready = false, ssl_ready = false;
  bool ok = false; Transfer t = {0};
  UpdateStatus s; Updater_GetStatus(&s);
  if (osGetWifiStrength() == 0) { publish(UPDATE_ERROR, "NO WI-FI CONNECTION"); goto done; }
  /* The linked mbedTLS entropy callback needs the SSL service on both models. */
  if (R_FAILED(sslcInit(0))) { publish(UPDATE_ERROR, "TLS SERVICE FAILED"); goto done; }
  ssl_ready = true;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) goto done;
  curl_ready = true;
  if (!download_job) {
    /* The list, not /latest: with + BETAS the newest of betas and stable releases wins, and
     * the notes cover every release between this build and it (Update_ParseReleases). */
    const char *url = "https://api.github.com/repos/" UPDATE_REPOSITORY "/releases?per_page=100";
    if (!fetch(url, &t)) goto done;
    UpdateRelease candidate;
    /* A beta build is older than the stable release of its own number, but must never be
     * offered its own pre-release page: a pre-release is compared as "-beta" too. */
    const char *installed = CTR_PORT_IS_BETA ? CTR_PORT_VERSION "-beta" : CTR_PORT_VERSION;
    int result = Update_ParseReleases(t.data, t.size, s.betas, installed, &candidate);
    if (result < 0) { publish(UPDATE_ERROR, "INVALID RELEASE DATA"); goto done; }
    if (!result) { publish(UPDATE_EMPTY, s.betas ? "NO RELEASE AVAILABLE" : "NO STABLE RELEASE AVAILABLE"); ok = true; goto done; }
    LightLock_Lock(&lock); release = candidate;
    status.prerelease = release.prerelease;
    snprintf(status.version, sizeof(status.version), "%s%s", release.version, release.prerelease ? " BETA" : "");
    LightLock_Unlock(&lock);
    char candidate_version[64];
    snprintf(candidate_version, sizeof(candidate_version), "%s%s", release.version, release.prerelease ? "-beta" : "");
    bool newer = Update_IsNewer(candidate_version, installed);
    publish(newer ? UPDATE_AVAILABLE : UPDATE_CURRENT, newer ? "UPDATE AVAILABLE" : "YOU ARE UP TO DATE");
    ok = true;
  } else {
    if (!Update_AllowedDownloadUrl(release.url) || !enough_space(release.size * 2ull + 16 * 1024 * 1024)) {
      publish(UPDATE_ERROR, "NOT ENOUGH SD SPACE"); goto done;
    }
    mkdir(UPDATE_DIR, 0777);
    t.expected = release.size;
    if (!open_download(&t)) { publish(UPDATE_ERROR, "CANNOT WRITE TO SD CARD"); goto done; }
    bool fetched = fetch(release.url, &t);
    if (!close_download(&t, fetched)) goto done;
    publish(UPDATE_VERIFYING, "VERIFYING DOWNLOAD");
    if (!verify_file()) { publish(UPDATE_ERROR, "DOWNLOAD CHECK FAILED"); goto done; }
    if (cancelled() || !aptIsActive()) goto done;
    bool home_allowed = aptIsHomeAllowed(), sleep_allowed = aptIsSleepAllowed();
    aptSetHomeAllowed(false); aptSetSleepAllowed(false);
    publish(UPDATE_INSTALLING, "INSTALLING UPDATE");
    ok = install_cia();
    aptSetSleepAllowed(sleep_allowed); aptSetHomeAllowed(home_allowed);
    publish(ok ? UPDATE_DONE : UPDATE_ERROR, ok ? "UPDATE INSTALLED" : "INSTALLATION FAILED");
    /* The FTP server's restart: a 10 s countdown on the bottom screen, then the main loop quits
     * through the normal path and libctru starts the new build. */
    if (ok) N3DS_FtpScheduleRelaunch();
  }
done:
  if (t.file) fclose(t.file);
  free(t.data);
  if (download_job) remove(UPDATE_PART);
  if (curl_ready) curl_global_cleanup();
  if (ssl_ready) sslcExit();
  Updater_GetStatus(&s);
  if (!ok && s.state != UPDATE_ERROR)
    publish(UPDATE_ERROR, cancelled() ? "UPDATE CANCELLED" : "CONNECTION FAILED - RETRY");
  __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
}
static void start(bool downloading) {
  if (!initialized || Updater_Busy() || stuck) return;
  if (worker) { threadJoin(worker, UINT64_MAX); threadFree(worker); worker = NULL; }
  if (downloading) {
    UpdateStatus s; Updater_GetStatus(&s);
    if (s.state != UPDATE_AVAILABLE) return;
    if (Updater_CannotInstall()) { publish(UPDATE_ERROR, "INSTALL THE CIA TO UPDATE"); return; }
  }
  /* Sockets: one soc:u session for the process, brought up here on the main thread
   * (shared with the FTP server and the 3dslink stream). */
  if (!N3DS_NetEnsureSoc()) { publish(UPDATE_ERROR, "NETWORK SERVICE FAILED"); return; }
  if (!downloading) { LightLock_Lock(&lock); status.version[0] = 0; LightLock_Unlock(&lock); }
  download_job = downloading;
  __atomic_store_n(&cancel, false, __ATOMIC_RELEASE);
  __atomic_store_n(&busy, true, __ATOMIC_RELEASE);
  publish(downloading ? UPDATE_DOWNLOADING : UPDATE_CHECKING,
          downloading ? "DOWNLOADING UPDATE" : "CHECKING FOR UPDATES");
  /* Lowest priority on a spare core (3, else 2; the New 3DS' extra cores), core 0 behind the
   * game on an Old 3DS. Core 1 is the system's and never used. */
  static const int cores[] = { 3, 2, 0 };
  for (unsigned i = 0; i < sizeof(cores) / sizeof(cores[0]) && !worker; i++)
    worker = threadCreate(run_job, NULL, UPDATE_THREAD_STACK, UPDATE_PRIORITY, cores[i], false);
  if (!worker) { __atomic_store_n(&busy, false, __ATOMIC_RELEASE); publish(UPDATE_ERROR, "CANNOT START UPDATE"); }
}
void Updater_Check(void) { start(false); }
void Updater_Download(void) { start(true); }
void Updater_SetBetas(bool betas) {
  if (!initialized || Updater_Busy()) return;
  N3DS_ConfigSet("betas", betas ? 1 : 0);
  LightLock_Lock(&lock); status.betas = betas; status.version[0] = 0; LightLock_Unlock(&lock);
  Updater_Check();
}
bool Updater_AutoCheckEnabled(void) { return auto_check; }
void Updater_SetAutoCheck(bool on) {
  auto_check = on;
  N3DS_ConfigSet("autoupdate", on ? 1 : 0);
}
void Updater_Init(void) {
  if (initialized) return;
  LightLock_Init(&lock); initialized = true;
  status.betas = N3DS_ConfigGet("betas", 0) != 0;
  auto_check = N3DS_ConfigGet("autoupdate", 0) != 0;
  status.revision = 1;
  mkdir(UPDATE_DIR, 0777);
}
void Updater_AutoCheck(void) {
  static bool done;
  if (done || !initialized || !auto_check) return;
  done = true;
  Updater_Check();
}
void Updater_Shutdown(void) {
  if (!initialized) return;
  Updater_Cancel();
  if (worker) {
    /* A TLS handshake or a stalled socket can hold the thread for a while: wait 3 s like the FTP
     * server does, then leave it to the kernel and tell the exit path to skip socExit. */
    if (threadJoin(worker, 3000000000ull) == 0) { threadFree(worker); worker = NULL; }
    else stuck = true;
  }
  initialized = false;
}
int Updater_Active(void) { return stuck ? 1 : 0; }
unsigned Updater_GetNotes(char *out, unsigned capacity) {
  if (!initialized || !capacity) return 0;
  LightLock_Lock(&lock);
  snprintf(out, capacity, "%s", release.notes);
  unsigned revision = status.revision;
  LightLock_Unlock(&lock);
  return revision;
}
