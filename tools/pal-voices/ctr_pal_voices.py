#!/usr/bin/env python3
"""CTR 3D: extract the PAL voice packs from your own PAL disc image.

The NTSC-U disc the game runs from only has English speech. The PAL disc
(SCES-02105) also has French, German, Italian, Spanish and Dutch. This tool
copies those voices out of a raw PAL image (.bin, 2352-byte sectors) into the
layout the game reads, sdmc:/3ds/ctr_native/mods/pal-voices/:

    XA/<CODE>.XNF                     the track table (data bytes)
    XA/<CODE>/{EXTRA,GAME}/Sxx.XA     the speech (raw 2352-byte sectors)

with CODE one of FRN GRM ITL SPN DCH. It writes exactly what the game's own
extractor (VOICES tab) writes, so either can be used.

Run it without arguments for a small window, or from a terminal:

    python ctr_pal_voices.py IMAGE.bin DESTINATION [--lang FRN,SPN]

DESTINATION can be the SD card's root (the files go to
3ds/ctr_native/mods/pal-voices), the ctr_native folder itself, or any other
folder (they go to DESTINATION/pal-voices: copy that folder to
3ds/ctr_native/mods/ on the SD card afterwards).

Needs Python 3.8+ and nothing else. Nothing from the disc is ever sent
anywhere; this only reads your image and writes files where you say.
"""

import argparse
import os
import struct
import sys
import threading

RAW = 2352
DATA_OFFSET = 24  # mode 2 form 1 user data
DATA = 2048
CHUNK = 256  # raw sectors per read/write

LANGS = [
    ("FRN", "FRANCAIS / French"),
    ("GRM", "DEUTSCH / German"),
    ("ITL", "ITALIANO / Italian"),
    ("SPN", "ESPANOL / Spanish"),
    ("DCH", "NEDERLANDS / Dutch"),
]
CODES = [c for c, _ in LANGS]


class ImageError(Exception):
    pass


class Job:
    def __init__(self, rel, code, lba, size, raw):
        self.rel = rel  # "XA/SPN/GAME/S00.XA"
        self.code = code
        self.lba = lba
        self.size = size  # ISO size (data bytes)
        self.raw = raw  # True: raw sectors (XA); False: data bytes (XNF)

    @property
    def sectors(self):
        return (self.size + DATA - 1) // DATA

    @property
    def out_bytes(self):
        return self.sectors * RAW if self.raw else self.size


def read_data(f, lba, count):
    out = bytearray()
    for i in range(count):
        f.seek((lba + i) * RAW + DATA_OFFSET)
        block = f.read(DATA)
        if len(block) != DATA:
            raise ImageError("the image is cut short")
        out += block
    return bytes(out)


def list_dir(f, lba, size):
    """(name, lba, size, is_dir) for every entry but . and .."""
    data = read_data(f, lba, (size + DATA - 1) // DATA)
    entries = []
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // DATA + 1) * DATA
            continue
        if n < 34 or i + n > len(data):
            break
        name_len = data[i + 32]
        name = data[i + 33:i + 33 + name_len]
        if name not in (b"\x00", b"\x01"):
            entries.append((name.decode("ascii", "replace").split(";")[0],
                            struct.unpack_from("<I", data, i + 2)[0],
                            struct.unpack_from("<I", data, i + 10)[0],
                            bool(data[i + 25] & 2)))
        i += n
    return entries


def scan_image(path):
    """The jobs for every voice language the image has. Raises ImageError."""
    with open(path, "rb") as f:
        pvd = read_data(f, 16, 1)
        if pvd[1:6] != b"CD001":
            raise ImageError("not a raw PlayStation disc image (2352-byte sectors)")
        root = list_dir(f, struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0])
        names = {e[0] for e in root}
        if "SCES_021.05" not in names:
            if "SCUS_944.26" in names:
                raise ImageError("this is the US disc; the voices are on the PAL (European) one")
            raise ImageError("not the PAL Crash Team Racing disc (SCES-02105)")
        xa = [e for e in root if e[0] == "XA" and e[3]]
        if not xa:
            raise ImageError("the image has no XA folder")
        jobs, xnfs = [], []
        for name, lba, size, is_dir in list_dir(f, xa[0][1], xa[0][2]):
            if is_dir and name in CODES:
                for cat, clba, csize, cdir in list_dir(f, lba, size):
                    if cdir and cat in ("EXTRA", "GAME"):
                        for fname, flba, fsize, fdir in list_dir(f, clba, csize):
                            if not fdir and fname.endswith(".XA"):
                                jobs.append(Job("XA/%s/%s/%s" % (name, cat, fname), name, flba, fsize, True))
            elif not is_dir and len(name) == 7 and name.endswith(".XNF") and name[:3] in CODES:
                xnfs.append(Job("XA/%s" % name, name[:3], lba, size, False))
        # the XNF last: the game takes a language as installed once its XNF is there
        return jobs + xnfs


def resolve_destination(dest):
    """Where mods/pal-voices (or a pal-voices folder to copy) goes for a chosen folder."""
    dest = os.path.abspath(dest)
    if os.path.isdir(os.path.join(dest, "3ds", "ctr_native")):
        return os.path.join(dest, "3ds", "ctr_native", "mods", "pal-voices"), True
    if os.path.basename(dest.rstrip("\\/")).lower() == "ctr_native":
        return os.path.join(dest, "mods", "pal-voices"), True
    return os.path.join(dest, "pal-voices"), False


def already_there(out_root, job):
    p = os.path.join(out_root, *job.rel.split("/"))
    return os.path.isfile(p) and os.path.getsize(p) == job.out_bytes


def language_state(jobs, out_root):
    """{code: (in_image, installed)}"""
    state = {}
    for code in CODES:
        mine = [j for j in jobs if j.code == code]
        state[code] = (bool(mine), bool(mine) and all(already_there(out_root, j) for j in mine))
    return state


def extract(image, jobs, out_root, codes, progress=None, cancel=None):
    """Copies the jobs of `codes` that are not there yet. progress(done, total) per chunk."""
    todo = [j for j in jobs if j.code in codes and not already_there(out_root, j)]
    total = sum(j.out_bytes for j in todo)
    done = 0
    with open(image, "rb") as src:
        for job in todo:
            dst_path = os.path.join(out_root, *job.rel.split("/"))
            os.makedirs(os.path.dirname(dst_path), exist_ok=True)
            tmp = dst_path + ".part"
            try:
                with open(tmp, "wb") as dst:
                    sector = 0
                    while sector < job.sectors:
                        if cancel is not None and cancel.is_set():
                            raise KeyboardInterrupt
                        n = min(CHUNK, job.sectors - sector)
                        src.seek((job.lba + sector) * RAW)
                        raw = src.read(n * RAW)
                        if len(raw) != n * RAW:
                            raise ImageError("the image is cut short (%s)" % job.rel)
                        if job.raw:
                            out = raw
                        else:
                            out = b"".join(raw[k * RAW + DATA_OFFSET:k * RAW + DATA_OFFSET + DATA] for k in range(n))
                            if sector + n == job.sectors:
                                out = out[:len(out) - (job.sectors * DATA - job.size)]
                        dst.write(out)
                        sector += n
                        done += len(out)
                        if progress:
                            progress(done, total)
                os.replace(tmp, dst_path)
            except BaseException:
                if os.path.exists(tmp):
                    os.remove(tmp)
                raise
    return len(todo), done


# ------------------------------------------------------------------ command line ---

def main_cli(argv):
    ap = argparse.ArgumentParser(description="Extract the PAL voice packs for CTR 3D from your own PAL disc image.")
    ap.add_argument("image", help="your PAL Crash Team Racing image (.bin, raw 2352-byte sectors)")
    ap.add_argument("destination", help="SD card root, its 3ds/ctr_native folder, or any folder")
    ap.add_argument("--lang", default=",".join(CODES), help="comma-separated codes among %s (default: all)" % ",".join(CODES))
    args = ap.parse_args(argv)

    codes = [c.strip().upper() for c in args.lang.split(",") if c.strip()]
    bad = [c for c in codes if c not in CODES]
    if bad:
        ap.error("unknown language code(s): %s" % ",".join(bad))
    try:
        jobs = scan_image(args.image)
    except (OSError, ImageError) as e:
        sys.exit("error: %s" % e)
    out_root, on_sd = resolve_destination(args.destination)
    print("writing to", out_root)

    def progress(done, total):
        sys.stdout.write("\r  %3d%%  %d / %d MB" % (done * 100 // max(total, 1), done >> 20, total >> 20))
        sys.stdout.flush()

    try:
        files, written = extract(args.image, jobs, out_root, codes, progress)
    except (OSError, ImageError) as e:
        sys.exit("\nerror: %s" % e)
    print("\n%d file(s), %d MB written." % (files, written >> 20))
    if not on_sd:
        print("Copy the pal-voices folder to 3ds/ctr_native/mods/ on the SD card.")
    print("In the game, pick the language in OPTIONS > LANGUAGE.")


# ------------------------------------------------------------------ window ---

def main_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("CTR 3D - PAL voices")
    root.resizable(False, False)
    st = {"jobs": None, "image": None, "cancel": None}
    image_var, dest_var, info_var, status_var = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar()
    lang_vars = {c: tk.IntVar(value=0) for c in CODES}
    lang_checks, lang_notes = {}, {}

    frm = ttk.Frame(root, padding=12)
    frm.grid()

    ttk.Label(frm, text="1. Your PAL disc image (.bin)").grid(row=0, column=0, columnspan=3, sticky="w")
    ttk.Entry(frm, textvariable=image_var, width=58, state="readonly").grid(row=1, column=0, columnspan=2, sticky="we")
    ttk.Button(frm, text="Browse...", command=lambda: pick_image()).grid(row=1, column=2, padx=(6, 0))
    ttk.Label(frm, textvariable=info_var, foreground="#666").grid(row=2, column=0, columnspan=3, sticky="w")

    ttk.Label(frm, text="2. Where to put the voices (SD card root, or any folder)").grid(row=3, column=0, columnspan=3, sticky="w", pady=(10, 0))
    ttk.Entry(frm, textvariable=dest_var, width=58, state="readonly").grid(row=4, column=0, columnspan=2, sticky="we")
    ttk.Button(frm, text="Browse...", command=lambda: pick_dest()).grid(row=4, column=2, padx=(6, 0))

    ttk.Label(frm, text="3. Languages").grid(row=5, column=0, columnspan=3, sticky="w", pady=(10, 0))
    for i, (code, label) in enumerate(LANGS):
        cb = ttk.Checkbutton(frm, text=label, variable=lang_vars[code], command=lambda: refresh_size())
        cb.grid(row=6 + i, column=0, sticky="w", padx=(12, 0))
        note = ttk.Label(frm, foreground="#666")
        note.grid(row=6 + i, column=1, sticky="w")
        lang_checks[code], lang_notes[code] = cb, note

    bar = ttk.Progressbar(frm, length=420, maximum=1000)
    bar.grid(row=12, column=0, columnspan=3, sticky="we", pady=(12, 4))
    ttk.Label(frm, textvariable=status_var).grid(row=13, column=0, columnspan=2, sticky="w")
    go = ttk.Button(frm, text="Extract", command=lambda: start())
    go.grid(row=13, column=2, sticky="e")

    def out_root():
        return resolve_destination(dest_var.get())[0] if dest_var.get() else None

    def refresh_langs():
        jobs, dest = st["jobs"], out_root()
        for code in CODES:
            if not jobs:
                lang_checks[code].state(["disabled"])
                lang_notes[code].config(text="")
                continue
            in_image, installed = language_state(jobs, dest)[code] if dest else (any(j.code == code for j in jobs), False)
            if installed:
                lang_vars[code].set(0)
                lang_checks[code].state(["disabled"])
                lang_notes[code].config(text="already extracted there")
            elif in_image:
                lang_checks[code].state(["!disabled"])
                lang_notes[code].config(text="")
            else:
                lang_vars[code].set(0)
                lang_checks[code].state(["disabled"])
                lang_notes[code].config(text="not in this image")
        refresh_size()

    def selected():
        return [c for c in CODES if lang_vars[c].get()]

    def refresh_size():
        jobs, dest = st["jobs"], out_root()
        if not jobs or not dest:
            go.state(["disabled"])
            status_var.set("Pick the image and the destination.")
            return
        mb = sum(j.out_bytes for j in jobs if j.code in selected() and not already_there(dest, j)) >> 20
        if selected():
            go.state(["!disabled"])
            status_var.set("About %d MB to write to %s" % (mb, dest))
        else:
            go.state(["disabled"])
            status_var.set("Tick at least one language.")

    def pick_image():
        p = filedialog.askopenfilename(title="Your PAL CTR disc image", filetypes=[("Disc image", "*.bin"), ("All files", "*.*")])
        if not p:
            return
        try:
            jobs = scan_image(p)
        except (OSError, ImageError) as e:
            messagebox.showerror("CTR 3D - PAL voices", str(e))
            return
        st["jobs"], st["image"] = jobs, p
        image_var.set(p)
        info_var.set("PAL disc found: %d voice files in %d languages" % (len(jobs), len({j.code for j in jobs})))
        for code in CODES:
            lang_vars[code].set(1)
        refresh_langs()

    def pick_dest():
        p = filedialog.askdirectory(title="SD card root, its 3ds/ctr_native folder, or any folder")
        if p:
            dest_var.set(p)
            refresh_langs()

    def start():
        codes, dest = selected(), out_root()
        if not codes or not st["jobs"]:
            return
        go.state(["disabled"])
        st["cancel"] = threading.Event()
        result = {}

        def progress(done, total):
            result["p"] = (done, total)

        def work():
            try:
                result["ok"] = extract(st["image"], st["jobs"], dest, codes, progress, st["cancel"])
            except BaseException as e:  # shown to the user
                result["err"] = e

        threading.Thread(target=work, daemon=True).start()

        def poll():
            if "p" in result:
                done, total = result["p"]
                bar["value"] = 1000 * done // max(total, 1)
                status_var.set("Extracting... %d / %d MB" % (done >> 20, total >> 20))
            if "ok" in result or "err" in result:
                bar["value"] = 1000 if "ok" in result else 0
                if "err" in result:
                    messagebox.showerror("CTR 3D - PAL voices", "Could not extract the voices:\n%s" % result["err"])
                else:
                    files, written = result["ok"]
                    _, on_sd = resolve_destination(dest_var.get())
                    msg = "%d file(s), %d MB written to\n%s\n\n" % (files, written >> 20, dest)
                    if not on_sd:
                        msg += "Copy the pal-voices folder to 3ds/ctr_native/mods/ on the SD card.\n\n"
                    msg += "In the game, pick the language in OPTIONS > LANGUAGE."
                    messagebox.showinfo("CTR 3D - PAL voices", msg)
                refresh_langs()
                return
            root.after(100, poll)

        poll()

    refresh_langs()
    root.mainloop()


if __name__ == "__main__":
    if len(sys.argv) > 1:
        main_cli(sys.argv[1:])
    else:
        main_gui()
