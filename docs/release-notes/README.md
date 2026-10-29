# Release notes

One file per tag: `vX.Y.Z.md`. The release workflow copies it into the GitHub
release body between `<!-- ctr3d-notes -->` markers, and the console's updater shows
it before the player installs.

Rules (details in `CLAUDE.md`, "Release notes"):

- 3 to 6 short lines, each starting with `- `. Plain text for the player, in
  English, under ~2.5 KB. The updater draws it in a 42-column text view: no
  tables, links or images.
- A **stable** release lists everything since the previous *stable* tag, betas
  included (stable players never see beta pages). A **beta** lists only what is
  new since the previous tag.
- Written when the tag is made, as one summary of the range, and merged before
  the tag exists.

Example:

```
- Installs as its own app from the HOME menu.
- New bottom-screen tabs: status, map and options.
- Smaller text and a tidier bottom screen.
```
