# Native saves, backups and recovery

Saves live in `local/state/savedata` (or `PS3_SAVEDATA_ROOT` / launcher
`Savedata`), one directory per PS3 save such as `BLUS30016-...`. Their format
is unchanged and older builds still read them.

## How a save is written

Each save is written into a staged copy of its directory. The live directory
is replaced only after every guest callback, payload file, `PARAM.SFO` and
secure-file record succeeded and the staged files were flushed to disk. A
failed callback, short write, flush/close failure, disk-full or permission
error, or a save directory changed by something else while saving, reports a
save error to the game and leaves the previous save as it was. A second running
instance cannot write the same save at the same time.

Bookkeeping lives in a hidden `.haze-saves/<save>` directory beside the saves,
which the game's save list does not show:

| Entry | Meaning |
| --- | --- |
| `backup.1` .. `backup.3` | The three most recent committed versions; `backup.1` is a copy of the current save |
| `damaged.1` .. `damaged.3` | Saves that failed verification and were replaced |
| `current.manifest` | Size and checksum of every file in the current save |
| `stage`, `previous`, `next.manifest` | Present only while saving or after an interruption |

## Recovery

At startup and before every load, an interrupted save is either completed (the
new save had been fully staged) or rolled back to the previous one. The live
directory is then checked against `current.manifest`. A missing, truncated,
unreadable or modified save is moved to `damaged.1` and replaced by the newest
backup that verifies, preferring the copy of the current save. Every step is
logged with a `[save-backup]` prefix in the session log.

If no backup verifies, the damaged save is left in place, loads still read it,
and the damage stays flagged: `current.manifest` keeps describing the last good
commit, and a save is refused (reported to the game as a save error) unless it
rewrites every damaged file from the start or deletes it. A save that rewrites
the whole slot is therefore accepted and clears the damage; a save that changes
only some files is not, so it cannot make the damaged files look valid. The log
names the damaged files. To keep playing with the damaged save as it is, follow
the manual steps below.

Saves made before this feature have no manifest; they are adopted and backed
up unchanged the first time they are loaded or saved.

## Restoring a backup by hand

With the game closed, delete the save directory's files and copy the files of
the chosen `.haze-saves/<save>/backup.N` into it, leaving out `HAZE.MANIFEST`.
The next launch accepts it because it matches that backup. To keep a save you
edited with an external tool, or to accept a damaged save that has no backup,
delete `current.manifest`; otherwise it is treated as damaged and replaced (or,
without a backup, partial saves to it are refused). To delete a save permanently, delete both the
save directory and its `.haze-saves/<save>` directory, or a missing save is
restored from its backup.
