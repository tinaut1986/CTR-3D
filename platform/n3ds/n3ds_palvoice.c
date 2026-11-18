#include <3ds.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n3ds_backend.h"
#include "n3ds_palvoice.h"

/* Game-side helpers (game TU); plain C types only, so the enum ABI split does not matter. */
extern const char *NativeAssets_GetBaseDir(void);
extern void NativeAudio_PalVoiceRescan(void);

#define PV_RAW_SECTOR   2352
#define PV_DATA_OFFSET  24 /* mode 2 form 1 user data */
#define PV_DATA_SECTOR  2048
#define PV_MAX_JOBS     96
#define PV_PATH_MAX     256
#define PV_CHUNK        64 /* raw sectors per read/write: ~150 KB */
#define PV_DIR_MAX      (16 * PV_DATA_SECTOR)
#define PV_THREAD_STACK (16 * 1024)
#define PV_PRIORITY     0x3F /* the lowest: the game never waits for the copy */

struct PvJob
{
	char rel[48];  /* "XA/SPN/GAME/S00.XA" */
	int lang;      /* index into s_codes */
	unsigned lba;
	unsigned size; /* ISO size in bytes (data bytes, 2048 per sector) */
	int raw;       /* 1: copy raw 2352-byte sectors (XA), 0: data bytes (XNF) */
	int there;     /* already extracted with the right size */
};

static const char *const s_codes[N3DS_PALVOICE_LANGS] = {"FRN", "GRM", "ITL", "SPN", "DCH"};
/* each language's own name, so a player finds theirs whatever language the menu is in */
static const char *const s_names[N3DS_PALVOICE_LANGS] = {"FRANCAIS", "DEUTSCH", "ITALIANO", "ESPANOL", "NEDERLANDS"};

static volatile int s_state = N3DS_PALVOICE_NONE;
static int s_raised;
static char s_image[PV_PATH_MAX];
static long s_imageBytes;
static char s_failure[48];

static struct PvJob s_jobs[PV_MAX_JOBS];
static int s_numJobs;
static int s_inImage[N3DS_PALVOICE_LANGS];
static int s_installed[N3DS_PALVOICE_LANGS];
static int s_selected[N3DS_PALVOICE_LANGS];

/* worker */
static Thread s_worker;
static volatile int s_workerDone;   /* 1 ok, -1 failed */
static volatile unsigned long long s_totalBytes;
static volatile unsigned long long s_doneBytes;
static int s_written;
static unsigned char *s_buf;

static void Pv_Log(const char *msg)
{
	char line[320];

	snprintf(line, sizeof(line), "[PALVOICE] %s", msg);
	N3DS_DebugLog(line);
}

static int Pv_ReadData(FILE *f, unsigned lba, unsigned count, unsigned char *out)
{
	unsigned i;

	for (i = 0; i < count; i++)
	{
		if ((fseek(f, (long)(lba + i) * PV_RAW_SECTOR + PV_DATA_OFFSET, SEEK_SET) != 0) || (fread(out + (size_t)i * PV_DATA_SECTOR, 1, PV_DATA_SECTOR, f) != PV_DATA_SECTOR))
		{
			return 0;
		}
	}
	return 1;
}

static unsigned Pv_Le32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* One ISO9660 directory: calls back for every entry except "." and "..". */
typedef void (*PvDirFn)(FILE *f, const char *name, unsigned lba, unsigned size, int isDir, const char *prefix);

static void Pv_ListDir(FILE *f, unsigned lba, unsigned size, const char *prefix, PvDirFn fn)
{
	/* per call, not static: the callbacks recurse into subdirectories while this one is walked */
	unsigned char *dir;
	unsigned sectors = (size + PV_DATA_SECTOR - 1) / PV_DATA_SECTOR;
	unsigned i = 0;

	if ((sectors == 0) || (sectors > PV_DIR_MAX / PV_DATA_SECTOR))
	{
		return;
	}
	dir = (unsigned char *)malloc((size_t)sectors * PV_DATA_SECTOR);
	if (dir == NULL)
	{
		return;
	}
	if (!Pv_ReadData(f, lba, sectors, dir))
	{
		free(dir);
		return;
	}
	while (i < sectors * PV_DATA_SECTOR)
	{
		unsigned n = dir[i];
		char name[40];
		unsigned len, k;

		if (n == 0)
		{
			i = (i / PV_DATA_SECTOR + 1) * PV_DATA_SECTOR;
			continue;
		}
		if ((n < 34) || (i + n > sectors * PV_DATA_SECTOR))
		{
			break;
		}
		len = dir[i + 32];
		if ((len > 0) && !((len == 1) && (dir[i + 33] <= 1)))
		{
			for (k = 0; (k < len) && (k < sizeof(name) - 1) && (dir[i + 33 + k] != ';'); k++)
			{
				name[k] = (char)dir[i + 33 + k];
			}
			name[k] = 0;
			fn(f, name, Pv_Le32(dir + i + 2), Pv_Le32(dir + i + 10), (dir[i + 25] & 2) != 0, prefix);
		}
		i += n;
	}
	free(dir);
}

static int Pv_CodeIndex(const char *s)
{
	int i;

	for (i = 0; i < N3DS_PALVOICE_LANGS; i++)
	{
		if (strcmp(s, s_codes[i]) == 0)
		{
			return i;
		}
	}
	return -1;
}

static void Pv_AddJob(const char *rel, int lang, unsigned lba, unsigned size, int raw)
{
	if ((s_numJobs >= PV_MAX_JOBS) || (strlen(rel) >= sizeof(s_jobs[0].rel)))
	{
		return;
	}
	memset(&s_jobs[s_numJobs], 0, sizeof(s_jobs[0]));
	strcpy(s_jobs[s_numJobs].rel, rel);
	s_jobs[s_numJobs].lang = lang;
	s_jobs[s_numJobs].lba = lba;
	s_jobs[s_numJobs].size = size;
	s_jobs[s_numJobs].raw = raw;
	s_numJobs++;
}

/* "XA/SPN/GAME" -> 3 */
static int Pv_LangOfPrefix(const char *prefix)
{
	char code[4];

	if (strlen(prefix) < 6)
	{
		return -1;
	}
	memcpy(code, prefix + 3, 3);
	code[3] = 0;
	return Pv_CodeIndex(code);
}

/* XA/<CODE>/<EXTRA|GAME>/Sxx.XA */
static void Pv_OnCategoryDir(FILE *f, const char *name, unsigned lba, unsigned size, int isDir, const char *prefix)
{
	char rel[64];
	size_t len = strlen(name);

	(void)f;
	if (isDir || (len < 4) || (strcmp(name + len - 3, ".XA") != 0) || (strlen(prefix) + 1 + len >= sizeof(rel)))
	{
		return;
	}
	snprintf(rel, sizeof(rel), "%s/%s", prefix, name);
	Pv_AddJob(rel, Pv_LangOfPrefix(prefix), lba, size, 1);
}

/* XA/<CODE>/ */
static void Pv_OnLanguageDir(FILE *f, const char *name, unsigned lba, unsigned size, int isDir, const char *prefix)
{
	char sub[48];

	if (!isDir || ((strcmp(name, "EXTRA") != 0) && (strcmp(name, "GAME") != 0)))
	{
		return;
	}
	snprintf(sub, sizeof(sub), "%s/%s", prefix, name);
	Pv_ListDir(f, lba, size, sub, Pv_OnCategoryDir);
}

/* XA/ : <CODE>.XNF files and <CODE> dirs. The XNF jobs are added last, so a pack only counts as
 * installed (native_audio.c looks for the XNF) once its tracks are there. */
static struct
{
	int lang;
	unsigned lba, size;
} s_xnf[N3DS_PALVOICE_LANGS];
static int s_numXnf;

static void Pv_OnXaDir(FILE *f, const char *name, unsigned lba, unsigned size, int isDir, const char *prefix)
{
	(void)prefix;
	if (isDir)
	{
		if ((strlen(name) == 3) && (Pv_CodeIndex(name) >= 0))
		{
			char sub[8];

			snprintf(sub, sizeof(sub), "XA/%s", name);
			Pv_ListDir(f, lba, size, sub, Pv_OnLanguageDir);
		}
		return;
	}
	if ((strlen(name) == 7) && (strcmp(name + 3, ".XNF") == 0) && (s_numXnf < N3DS_PALVOICE_LANGS))
	{
		char code[4];
		int lang;

		memcpy(code, name, 3);
		code[3] = 0;
		lang = Pv_CodeIndex(code);
		if (lang >= 0)
		{
			s_xnf[s_numXnf].lang = lang;
			s_xnf[s_numXnf].lba = lba;
			s_xnf[s_numXnf].size = size;
			s_numXnf++;
		}
	}
}

static int s_rootIsPal;

static void Pv_OnRoot(FILE *f, const char *name, unsigned lba, unsigned size, int isDir, const char *prefix)
{
	(void)prefix;
	if (!isDir && (strcmp(name, "SCES_021.05") == 0))
	{
		s_rootIsPal = 1;
	}
	if (isDir && (strcmp(name, "XA") == 0))
	{
		Pv_ListDir(f, lba, size, "XA", Pv_OnXaDir);
	}
}

static unsigned long long Pv_OutBytes(const struct PvJob *job)
{
	unsigned sectors = (job->size + PV_DATA_SECTOR - 1) / PV_DATA_SECTOR;

	return job->raw ? (unsigned long long)sectors * PV_RAW_SECTOR : job->size;
}

static int Pv_OutPath(char *out, size_t outSize, const char *rel)
{
	int n = snprintf(out, outSize, "%s/mods/pal-voices/%s", NativeAssets_GetBaseDir(), rel);

	return (n > 0) && ((size_t)n < outSize);
}

static int Pv_AlreadyThere(const struct PvJob *job)
{
	char path[PV_PATH_MAX];
	struct stat st;

	return Pv_OutPath(path, sizeof(path), job->rel) && (stat(path, &st) == 0) && ((unsigned long long)st.st_size == Pv_OutBytes(job));
}

/* Scans one candidate image: the job list, and per language whether it is in the image and
 * already extracted. */
static int Pv_ScanImage(const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char pvd[PV_DATA_SECTOR];
	int i;

	if (f == NULL)
	{
		return 0;
	}
	s_numJobs = 0;
	s_numXnf = 0;
	s_rootIsPal = 0;
	if (!Pv_ReadData(f, 16, 1, pvd) || (memcmp(pvd + 1, "CD001", 5) != 0))
	{
		fclose(f);
		return 0;
	}
	Pv_ListDir(f, Pv_Le32(pvd + 158), Pv_Le32(pvd + 166), "", Pv_OnRoot);
	fseek(f, 0, SEEK_END);
	s_imageBytes = ftell(f);
	fclose(f);

	if (!s_rootIsPal || (s_numXnf == 0))
	{
		return 0;
	}
	for (i = 0; i < s_numXnf; i++)
	{
		char rel[16];

		snprintf(rel, sizeof(rel), "XA/%s.XNF", s_codes[s_xnf[i].lang]);
		Pv_AddJob(rel, s_xnf[i].lang, s_xnf[i].lba, s_xnf[i].size, 0);
	}

	memset(s_inImage, 0, sizeof(s_inImage));
	for (i = 0; i < N3DS_PALVOICE_LANGS; i++)
	{
		s_installed[i] = 1;
	}
	for (i = 0; i < s_numJobs; i++)
	{
		if (s_jobs[i].lang < 0)
		{
			continue;
		}
		s_inImage[s_jobs[i].lang] = 1;
		s_jobs[i].there = Pv_AlreadyThere(&s_jobs[i]);
		if (!s_jobs[i].there)
		{
			s_installed[s_jobs[i].lang] = 0;
		}
	}
	for (i = 0; i < N3DS_PALVOICE_LANGS; i++)
	{
		if (!s_inImage[i])
		{
			s_installed[i] = 0;
		}
		s_selected[i] = s_inImage[i] && !s_installed[i];
	}
	return 1;
}

/* Packs already on the SD card, image or not: a language counts as installed when its XNF is
 * there (native_audio.c needs nothing else to use it; a missing track plays in English). */
static void Pv_ScanInstalledOnly(void)
{
	int i;

	for (i = 0; i < N3DS_PALVOICE_LANGS; i++)
	{
		char rel[16], path[PV_PATH_MAX];
		struct stat st;

		snprintf(rel, sizeof(rel), "XA/%s.XNF", s_codes[i]);
		s_installed[i] = Pv_OutPath(path, sizeof(path), rel) && (stat(path, &st) == 0);
		s_inImage[i] = 0;
		s_selected[i] = 0;
	}
	s_numJobs = 0;
}

/* Looks for a PAL image in assets/ and fills the language table. 1 if an image was found. */
static int Pv_Refresh(void)
{
	char dirPath[PV_PATH_MAX];
	DIR *dir;
	struct dirent *e;
	int found = 0;

	s_image[0] = 0;
	s_imageBytes = 0;
	snprintf(dirPath, sizeof(dirPath), "%s/assets", NativeAssets_GetBaseDir());
	dir = opendir(dirPath);
	if (dir != NULL)
	{
		while (!found && ((e = readdir(dir)) != NULL))
		{
			size_t len = strlen(e->d_name);

			if ((len < 5) || (strcasecmp(e->d_name + len - 4, ".bin") != 0) || (strcasecmp(e->d_name, "ctr-u.bin") == 0) ||
			    (strlen(dirPath) + 1 + len >= sizeof(s_image)))
			{
				continue;
			}
			snprintf(s_image, sizeof(s_image), "%s/%s", dirPath, e->d_name);
			found = Pv_ScanImage(s_image);
		}
		closedir(dir);
	}
	if (!found)
	{
		s_image[0] = 0;
		s_imageBytes = 0;
		Pv_ScanInstalledOnly();
	}
	return found;
}

void N3DS_PalVoice_Refresh(void)
{
	if (s_state != N3DS_PALVOICE_WORKING)
	{
		Pv_Refresh();
	}
}

void N3DS_PalVoice_Raise(void)
{
	char note[PV_PATH_MAX + 64];
	int i, missing = 0;

	if (s_raised)
	{
		return;
	}
	s_raised = 1;
	if (!Pv_Refresh())
	{
		return;
	}
	for (i = 0; i < N3DS_PALVOICE_LANGS; i++)
	{
		missing += s_selected[i];
	}
	if (missing == 0)
	{
		snprintf(note, sizeof(note), "image %s: every voice pack is already extracted", s_image);
		Pv_Log(note);
		return;
	}
	snprintf(note, sizeof(note), "image %s: %d language(s) to extract, asking", s_image, missing);
	Pv_Log(note);
	s_state = N3DS_PALVOICE_ASK;
}

int N3DS_PalVoice_HasImage(void)
{
	return s_image[0] != 0;
}

const char *N3DS_PalVoice_ImageFile(void)
{
	const char *slash = strrchr(s_image, '/');

	return (slash != NULL) ? (slash + 1) : s_image;
}

int N3DS_PalVoice_UsDiscPresent(void)
{
	char path[PV_PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/assets/ctr-u.bin", NativeAssets_GetBaseDir());
	return stat(path, &st) == 0;
}

/* removes every file in `dirPath` (one level), then the directory */
static void Pv_RemoveDir(const char *dirPath)
{
	DIR *dir = opendir(dirPath);
	struct dirent *e;
	char path[PV_PATH_MAX + 64];

	if (dir != NULL)
	{
		while ((e = readdir(dir)) != NULL)
		{
			if ((strcmp(e->d_name, ".") == 0) || (strcmp(e->d_name, "..") == 0))
			{
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s", dirPath, e->d_name);
			remove(path);
		}
		closedir(dir);
	}
	rmdir(dirPath);
}

void N3DS_PalVoice_DeleteLang(int lang)
{
	char path[PV_PATH_MAX], rel[24], note[64];

	if ((lang < 0) || (lang >= N3DS_PALVOICE_LANGS) || (s_state == N3DS_PALVOICE_WORKING))
	{
		return;
	}
	/* the XNF first: from then on the pack no longer counts as installed */
	snprintf(rel, sizeof(rel), "XA/%s.XNF", s_codes[lang]);
	if (Pv_OutPath(path, sizeof(path), rel))
	{
		remove(path);
	}
	snprintf(rel, sizeof(rel), "XA/%s/EXTRA", s_codes[lang]);
	if (Pv_OutPath(path, sizeof(path), rel))
	{
		Pv_RemoveDir(path);
	}
	snprintf(rel, sizeof(rel), "XA/%s/GAME", s_codes[lang]);
	if (Pv_OutPath(path, sizeof(path), rel))
	{
		Pv_RemoveDir(path);
	}
	snprintf(rel, sizeof(rel), "XA/%s", s_codes[lang]);
	if (Pv_OutPath(path, sizeof(path), rel))
	{
		rmdir(path);
	}
	NativeAudio_PalVoiceRescan();
	snprintf(note, sizeof(note), "deleted the %s voice pack", s_codes[lang]);
	Pv_Log(note);
	Pv_Refresh();
}

void N3DS_PalVoice_DeleteImage(void)
{
	char note[PV_PATH_MAX + 32];

	if ((s_image[0] == 0) || (s_state == N3DS_PALVOICE_WORKING))
	{
		return;
	}
	snprintf(note, sizeof(note), "%s %s", (remove(s_image) == 0) ? "deleted" : "could not delete", s_image);
	Pv_Log(note);
	if (s_state == N3DS_PALVOICE_ASKDEL)
	{
		s_state = N3DS_PALVOICE_NONE;
	}
	Pv_Refresh();
}

int N3DS_PalVoice_State(void)
{
	return s_state;
}

const char *N3DS_PalVoice_LangName(int lang)
{
	return ((lang >= 0) && (lang < N3DS_PALVOICE_LANGS)) ? s_names[lang] : "";
}

int N3DS_PalVoice_LangInImage(int lang)
{
	return (lang >= 0) && (lang < N3DS_PALVOICE_LANGS) && s_inImage[lang];
}

int N3DS_PalVoice_LangInstalled(int lang)
{
	return (lang >= 0) && (lang < N3DS_PALVOICE_LANGS) && s_installed[lang];
}

int N3DS_PalVoice_LangSelected(int lang)
{
	return (lang >= 0) && (lang < N3DS_PALVOICE_LANGS) && s_selected[lang];
}

void N3DS_PalVoice_Toggle(int lang)
{
	if ((s_state != N3DS_PALVOICE_WORKING) && N3DS_PalVoice_LangInImage(lang) && !s_installed[lang])
	{
		s_selected[lang] = !s_selected[lang];
	}
}

static unsigned long long Pv_SelectedBytes(void)
{
	unsigned long long total = 0;
	int i;

	for (i = 0; i < s_numJobs; i++)
	{
		if ((s_jobs[i].lang >= 0) && s_selected[s_jobs[i].lang] && !s_jobs[i].there)
		{
			total += Pv_OutBytes(&s_jobs[i]);
		}
	}
	return total;
}

int N3DS_PalVoice_SelectedMB(void)
{
	return (int)((Pv_SelectedBytes() + (1 << 20) - 1) >> 20);
}

int N3DS_PalVoice_Percent(void)
{
	unsigned long long total = s_totalBytes;

	return (total == 0) ? 100 : (int)((s_doneBytes * 100) / total);
}

int N3DS_PalVoice_ImageMB(void)
{
	return (int)(s_imageBytes >> 20);
}

const char *N3DS_PalVoice_Failure(void)
{
	return s_failure;
}

/* mkdir -p for the parent of `path` */
static void Pv_MakeParents(const char *path)
{
	char tmp[PV_PATH_MAX];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p != 0; p++)
	{
		if ((*p == '/') && (p[-1] != ':'))
		{
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
}

static int Pv_WorkerFail(FILE *src, FILE *dst, const char *tmp, const char *what, const struct PvJob *job)
{
	char note[160];

	snprintf(s_failure, sizeof(s_failure), "%s", what);
	snprintf(note, sizeof(note), "extraction failed: %s (%s, errno %d)", what, job->rel, errno);
	Pv_Log(note);
	if (dst != NULL)
	{
		fclose(dst);
		remove(tmp);
	}
	if (src != NULL)
	{
		fclose(src);
	}
	return 0;
}

/* Copies every ticked file that is not there yet; each lands as <file>.part and is renamed when
 * complete, so a power cut never leaves a short file under the real name. */
static int Pv_CopyAll(void)
{
	FILE *src = fopen(s_image, "rb");
	int j;

	if (src == NULL)
	{
		return Pv_WorkerFail(NULL, NULL, NULL, "CANNOT READ THE IMAGE", &s_jobs[0]);
	}
	for (j = 0; j < s_numJobs; j++)
	{
		const struct PvJob *job = &s_jobs[j];
		unsigned sectors = (job->size + PV_DATA_SECTOR - 1) / PV_DATA_SECTOR;
		unsigned done = 0, i;
		char path[PV_PATH_MAX], tmp[PV_PATH_MAX + 8];
		FILE *dst;

		if ((job->lang < 0) || !s_selected[job->lang] || job->there)
		{
			continue;
		}
		if (!Pv_OutPath(path, sizeof(path), job->rel))
		{
			return Pv_WorkerFail(src, NULL, NULL, "PATH TOO LONG", job);
		}
		snprintf(tmp, sizeof(tmp), "%s.part", path);
		Pv_MakeParents(path);
		dst = fopen(tmp, "wb");
		if (dst == NULL)
		{
			return Pv_WorkerFail(src, NULL, NULL, "CANNOT WRITE TO THE SD CARD", job);
		}
		while (done < sectors)
		{
			unsigned chunk = sectors - done;
			size_t outBytes;

			if (chunk > PV_CHUNK)
			{
				chunk = PV_CHUNK;
			}
			if ((fseek(src, (long)(job->lba + done) * PV_RAW_SECTOR, SEEK_SET) != 0) || (fread(s_buf, PV_RAW_SECTOR, chunk, src) != chunk))
			{
				return Pv_WorkerFail(src, dst, tmp, "THE IMAGE IS CUT SHORT", job);
			}
			if (job->raw)
			{
				outBytes = (size_t)chunk * PV_RAW_SECTOR;
			}
			else
			{
				/* data bytes only, and the last sector only up to the file size */
				for (i = 0; i < chunk; i++)
				{
					memmove(s_buf + (size_t)i * PV_DATA_SECTOR, s_buf + (size_t)i * PV_RAW_SECTOR + PV_DATA_OFFSET, PV_DATA_SECTOR);
				}
				outBytes = (size_t)chunk * PV_DATA_SECTOR;
				if ((done + chunk) == sectors)
				{
					outBytes -= (size_t)sectors * PV_DATA_SECTOR - job->size;
				}
			}
			if (fwrite(s_buf, 1, outBytes, dst) != outBytes)
			{
				return Pv_WorkerFail(src, dst, tmp, "THE SD CARD IS FULL", job);
			}
			done += chunk;
			s_doneBytes += outBytes;
		}
		if (fclose(dst) != 0)
		{
			remove(tmp);
			return Pv_WorkerFail(src, NULL, NULL, "THE SD CARD IS FULL", job);
		}
		remove(path);
		if (rename(tmp, path) != 0)
		{
			remove(tmp);
			return Pv_WorkerFail(src, NULL, NULL, "CANNOT WRITE TO THE SD CARD", job);
		}
		s_written++;
	}
	fclose(src);
	return 1;
}

static void Pv_WorkerMain(void *arg)
{
	(void)arg;
	s_workerDone = Pv_CopyAll() ? 1 : -1;
}

static void Pv_StartWorker(void)
{
	/* a spare core where there is one (3, else 2), else core 0 behind the game, at the lowest
	 * priority, as the updater does; core 1 is the system's */
	static const int cores[] = {3, 2, 0};
	unsigned i = N3DS_ExtraCoresUsable() ? 0u : 2u;

	s_doneBytes = 0;
	s_totalBytes = Pv_SelectedBytes();
	s_written = 0;
	s_workerDone = 0;
	s_failure[0] = 0;
	s_buf = (unsigned char *)malloc(PV_CHUNK * PV_RAW_SECTOR);
	s_worker = NULL;
	if (s_buf != NULL)
	{
		for (; (i < sizeof(cores) / sizeof(cores[0])) && (s_worker == NULL); i++)
		{
			s_worker = threadCreate(Pv_WorkerMain, NULL, PV_THREAD_STACK, PV_PRIORITY, cores[i], false);
		}
	}
	if (s_worker == NULL)
	{
		free(s_buf);
		s_buf = NULL;
		snprintf(s_failure, sizeof(s_failure), "CANNOT START THE COPY");
		Pv_Log("extraction failed: no worker thread");
		s_state = N3DS_PALVOICE_FAILED;
		return;
	}
	s_state = N3DS_PALVOICE_WORKING;
}

void N3DS_PalVoice_Extract(void)
{
	if ((s_state == N3DS_PALVOICE_NONE) || (s_state == N3DS_PALVOICE_ASK))
	{
		if ((s_image[0] != 0) && (Pv_SelectedBytes() > 0))
		{
			Pv_StartWorker();
		}
	}
}

void N3DS_PalVoice_Answer(int yes)
{
	if (s_state == N3DS_PALVOICE_ASK)
	{
		if (!yes)
		{
			Pv_Log("extraction declined");
			s_state = N3DS_PALVOICE_NONE;
		}
		else if (Pv_SelectedBytes() > 0)
		{
			Pv_StartWorker();
		}
	}
	else if (s_state == N3DS_PALVOICE_ASKDEL)
	{
		if (yes)
		{
			N3DS_PalVoice_DeleteImage();
		}
		else
		{
			char note[PV_PATH_MAX + 32];

			snprintf(note, sizeof(note), "kept %s", s_image);
			Pv_Log(note);
		}
		s_state = N3DS_PALVOICE_NONE;
	}
	else if (s_state == N3DS_PALVOICE_FAILED)
	{
		s_state = N3DS_PALVOICE_NONE;
	}
}

/* Game thread: picks up a finished copy and lets the audio see the new packs. */
void N3DS_PalVoice_Pump(void)
{
	char note[96];
	int done;

	if (s_state != N3DS_PALVOICE_WORKING)
	{
		return;
	}
	done = s_workerDone;
	if (done == 0)
	{
		return;
	}
	threadJoin(s_worker, UINT64_MAX);
	threadFree(s_worker);
	s_worker = NULL;
	free(s_buf);
	s_buf = NULL;
	if (s_written > 0)
	{
		NativeAudio_PalVoiceRescan();
	}
	if (done < 0)
	{
		s_state = N3DS_PALVOICE_NONE;
		Pv_Refresh();
		s_state = N3DS_PALVOICE_FAILED;
		return;
	}
	snprintf(note, sizeof(note), "extraction done: %d files, %llu KB", s_written, s_doneBytes >> 10);
	Pv_Log(note);
	s_state = N3DS_PALVOICE_NONE; /* so the refresh runs */
	Pv_Refresh();
	s_state = N3DS_PALVOICE_ASKDEL;
}
