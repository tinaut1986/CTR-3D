/*
 * CIA install over the running title through AM. See n3ds_ciainstall.h. Shared by
 * the FTP server's SITE INSTALL and the in-game updater. Plain libctru TU (built
 * with -fshort-enums like the other platform/n3ds files).
 */
#include <3ds.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "n3ds_ciainstall.h"

#define N3DS_CIA_IO_SIZE (128 * 1024)

int N3DS_CiaInstall(const char *sdPath, volatile unsigned long long *done, volatile unsigned long long *total,
                    const volatile int *cancel, char *error, unsigned errorCap)
{
	u64 title = 0;
	u8 mediaByte = MEDIATYPE_SD;
	FS_MediaType media;
	bool registered = false, loaded = false;
	APT_AppletAttr attr;
	Result rc;
	Handle input = 0, output = 0;
	int started = 0, ok = 0;
	u64 size = 0, required = 0, offset;
	AM_TitleInfo info;
	FS_ArchiveResource res;
	unsigned char *data;

	/* The running title's ID and media: the CIA must be for this very title. */
	if (R_FAILED(APT_GetAppletInfo(APPID_APPLICATION, &title, &mediaByte, &registered, &loaded, &attr)) || (title == 0))
	{
		snprintf(error, errorCap, "not running as an installed title");
		return 0;
	}
	media = (mediaByte == MEDIATYPE_NAND) ? MEDIATYPE_NAND : MEDIATYPE_SD;

	rc = amInit();
	if (R_FAILED(rc))
	{
		snprintf(error, errorCap, "AM unavailable (%08lx)", (unsigned long)rc);
		return 0;
	}

	data = (unsigned char *)malloc(N3DS_CIA_IO_SIZE);
	snprintf(error, errorCap, "out of memory");
	if (data == NULL)
	{
		goto done;
	}
	rc = FSUSER_OpenFileDirectly(&input, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, sdPath), FS_OPEN_READ, 0);
	snprintf(error, errorCap, "cannot open %s", sdPath);
	if (R_FAILED(rc) || R_FAILED(FSFILE_GetSize(input, &size)))
	{
		goto done;
	}
	rc = AM_GetCiaFileInfo(media, &info, input);
	snprintf(error, errorCap, "not a CIA of this game (title %016llx)", R_SUCCEEDED(rc) ? (unsigned long long)info.titleID : 0ull);
	if (R_FAILED(rc) || (info.titleID != title))
	{
		goto done;
	}
	rc = AM_GetCiaRequiredSpace(&required, media, input);
	snprintf(error, errorCap, "not enough SD space");
	if (R_FAILED(rc) || R_FAILED(FSUSER_GetSdmcArchiveResource(&res)) || ((u64)res.freeClusters * res.clusterSize < required + 4 * 1024 * 1024ull))
	{
		goto done;
	}
	if (done != NULL)
	{
		*done = 0;
	}
	if (total != NULL)
	{
		*total = size;
	}
	/* Overwrite through AM; the installed title is never deleted first. */
	rc = AM_StartCiaInstallOverwrite(&output, media);
	snprintf(error, errorCap, "install start failed (%08lx)", (unsigned long)rc);
	if (R_FAILED(rc))
	{
		goto done;
	}
	started = 1;
	for (offset = 0; offset < size;)
	{
		u32 read = 0, wrote = 0;
		u32 want = (size - offset > N3DS_CIA_IO_SIZE) ? N3DS_CIA_IO_SIZE : (u32)(size - offset);

		if ((cancel != NULL) && *cancel)
		{
			snprintf(error, errorCap, "cancelled");
			goto done;
		}

		rc = FSFILE_Read(input, &read, offset, data, want);
		snprintf(error, errorCap, "SD read failed (%08lx)", (unsigned long)rc);
		if (R_FAILED(rc) || (read != want))
		{
			goto done;
		}
		rc = FSFILE_Write(output, &wrote, offset, data, read, 0);
		snprintf(error, errorCap, "install write failed (%08lx)", (unsigned long)rc);
		if (R_FAILED(rc) || (wrote != read))
		{
			goto done;
		}
		offset += read;
		if (done != NULL)
		{
			*done = offset;
		}
	}
	rc = AM_FinishCiaInstall(output);
	started = 0;
	ok = R_SUCCEEDED(rc);
	snprintf(error, errorCap, ok ? "" : "install finish failed (%08lx)", (unsigned long)rc);

done:
	if (started)
	{
		AM_CancelCIAInstall(output);
	}
	if (input != 0)
	{
		FSFILE_Close(input);
	}
	free(data);
	amExit();
	return ok;
}
