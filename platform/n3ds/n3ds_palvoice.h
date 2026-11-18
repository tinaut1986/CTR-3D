#ifndef N3DS_PALVOICE_H
#define N3DS_PALVOICE_H

/* PAL voice packs extracted on the console (issue #20).
 *
 * The NTSC-U disc only has English speech. When a PAL disc image of the player's own disc sits
 * next to ctr-u.bin in <base dir>/assets/ (any .bin other than ctr-u.bin), and some of its five
 * voice languages are not in <base dir>/mods/pal-voices yet, the bottom screen offers to extract
 * them (raw XA sectors and the XNF tables, the layout native_audio.c reads), with a checkbox per
 * language. The copy runs on its own low-priority thread; once done the screen offers to delete
 * the image, which the game does not need for anything else. Drawn by n3ds_bottomui.c, like the
 * DSP firmware question. */
enum
{
	N3DS_PALVOICE_NONE = 0,
	N3DS_PALVOICE_ASK,      /* image found, voices missing: language checkboxes, EXTRACT / NOT NOW */
	N3DS_PALVOICE_WORKING,  /* extracting on the worker thread */
	N3DS_PALVOICE_ASKDEL,   /* done: DELETE IMAGE / KEEP */
	N3DS_PALVOICE_FAILED    /* OK */
};

#define N3DS_PALVOICE_LANGS 5 /* FR, DE, IT, ES, NL */

/* Once, after boot: looks for a PAL image and opens the question if it has voices to extract. */
void N3DS_PalVoice_Raise(void);
/* The VOICES tab: rescans the SD card (image and extracted packs) when it is opened. */
void N3DS_PalVoice_Refresh(void);
int N3DS_PalVoice_HasImage(void);
const char *N3DS_PalVoice_ImageFile(void);  /* "ctr-e.bin" */
int N3DS_PalVoice_UsDiscPresent(void);
void N3DS_PalVoice_Extract(void);            /* the ticked languages, from the tab */
void N3DS_PalVoice_DeleteLang(int lang);     /* removes an extracted pack */
void N3DS_PalVoice_DeleteImage(void);
int N3DS_PalVoice_State(void);

/* ASK: the language rows. Name in its own language ("FRANCAIS"...), whether the image has it,
 * whether it is already extracted, and whether it is ticked. */
const char *N3DS_PalVoice_LangName(int lang);
int N3DS_PalVoice_LangInImage(int lang);
int N3DS_PalVoice_LangInstalled(int lang);
int N3DS_PalVoice_LangSelected(int lang);
void N3DS_PalVoice_Toggle(int lang);       /* ignored for an installed or absent language */
int N3DS_PalVoice_SelectedMB(void);        /* MB the ticked languages will write */

int N3DS_PalVoice_Percent(void);           /* WORKING: 0..100 */
int N3DS_PalVoice_ImageMB(void);           /* ASKDEL: size of the image */
const char *N3DS_PalVoice_Failure(void);   /* FAILED: one short line */
void N3DS_PalVoice_Answer(int yes);        /* the two buttons: left = yes */
void N3DS_PalVoice_Pump(void);             /* once per input poll, game thread: finishes a done job */

#endif
