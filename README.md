# vsfs-journal

A small C command-line tool that adds **write-ahead journaling** to a simple Unix-style file system image (VSFS). It logs file-creation transactions to an on-disk journal first, and applies them to the file system only when you run `install`. Incomplete transactions are never applied.

> Course project for CSE321 (Operating Systems), BRAC University.
> **My contribution:** I wrote the full implementation (journal format, `create`, `install`, replay logic and validation).

## What it does

| Command | Behavior |
|---|---|
| `./journalfs create <name>` | Builds a transaction (updated inode bitmap, inode table, root directory block), appends it to the journal, then writes a **commit record**. The main file system is not modified yet. |
| `./journalfs install` | Reads the journal, applies every **committed** transaction to the disk image, discards anything without a commit record, then clears the journal. |

`create` also replays committed-but-not-installed transactions in memory, so it sees a consistent view. A second `create` of the same name is rejected even before `install` has run.

## How the journal works

```
Journal (16 blocks, starting at block 1)
+--------------------+----------------------------------------------+
| journal_header     | magic (0x4A524E4C), nbytes_used              |
+--------------------+----------------------------------------------+
| DATA record        | rec_header | block number | 4096-byte image  |
| DATA record        | ...  (inode bitmap, inode table, dir block)  |
| COMMIT record      | rec_header only                              |
+--------------------+----------------------------------------------+
```

- Every record starts with a 4-byte `rec_header` (`type`, `size`).
- A DATA record holds the full new image of one block plus its block number.
- A COMMIT record marks the end of a transaction. During `install`, DATA records are buffered and written to their blocks only when the COMMIT record is reached. A crash before the commit leaves the file system untouched.

### Disk layout (4096-byte blocks, 85 total)

| Block(s) | Contents |
|---|---|
| 0 | Superblock |
| 1-16 | Journal |
| 17 | Inode bitmap |
| 18 | Data bitmap |
| 19-20 | Inode table (128-byte inodes) |
| 21-84 | Data blocks |

## Validation and error handling

- Superblock magic number and layout are checked before any operation.
- Journal magic and `nbytes_used` range are checked; a corrupted header aborts.
- File names longer than 27 characters are rejected.
- Duplicate names, no free inode, full root directory, and a full journal each produce a clear error message.
- Malformed or truncated journal records stop replay instead of being applied.

## Build and run

```bash
gcc -Wall -Wextra -o journalfs journalfs.c
```

The tool operates on a file named `vsfs.img` in the current directory.

### Create a test image

If you don't have the course's image-creation tool, this helper script builds a compatible empty image (root directory only):

```python
# make_image.py
import struct, sys
B = 4096
img = bytearray(85 * B)
sb = struct.pack('<9I', 0x56534653, 4096, 85, 64, 1, 17, 18, 19, 21)
img[0:len(sb)] = sb
img[17 * B] = 1   # inode bitmap: root inode in use
img[18 * B] = 1   # data bitmap: root directory block in use
ino = struct.pack('<HHI8III', 2, 2, 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0)
img[19 * B:19 * B + len(ino)] = ino
open(sys.argv[1], 'wb').write(img)
```

```bash
python3 make_image.py vsfs.img
```

### Example

```console
$ ./journalfs create notes.txt
Transaction committed for file 'notes.txt' (inode 1).
$ ./journalfs create report.txt
Transaction committed for file 'report.txt' (inode 2).
$ ./journalfs create notes.txt
ERROR: File already exists.
$ ./journalfs install
Replayed committed transactions.
Install complete (journal cleared).
$ ./journalfs install
ERROR: Journal is empty (nothing to install).
```

## Concepts demonstrated

Write-ahead logging and commit records, crash consistency, binary on-disk formats (`pread`/`pwrite`, fixed-size structs, `_Static_assert` size checks), bitmap allocation, inodes and directory entries, defensive parsing of untrusted record streams.

## Limitations

- Only supports creating empty files in the root directory.
- Journal capacity is fixed at 16 blocks; run `install` when it fills.
- The image file name is fixed to `vsfs.img`.
