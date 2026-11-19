# Start-up hashing (fork-only)

These switches and the `startup-load:` record exist only in the IvanShift fork.
They tune the walk that opens and quick-checks session downloads after a restart.

## Switches

| Command | Default | Meaning |
| --- | --- | --- |
| `pieces.hash.slice_ms` | `100` | Time budget of one walk over the hashing view, in milliseconds. After the budget the walk stops, with `slice_yield=0` once a download it quick-checked waits for its `hash_done()`, with `1` once it opened a closed download; it continues as `pieces.hash.slice_yield` says. Values are clamped to `0..60000`; `0` walks the whole view at once, as upstream does. |
| `pieces.hash.slice_yield` | `0` | `0`: the last `hash_done()` of the stopped walk starts the next walk in the same scheduler pass, so no RPC is answered during the load. `1`: the next walk runs in the next pass, so RPC is answered between walks, while downloads that were not reached yet still read as closed. |
| `pieces.hash.skip_unchanged_save` | `1` | After a passing quick check, skip the resume save when `resume_progress_unchanged()` finds the bitfield and each file's completed count as saved and a real mtime for every file. It does not compare the mtimes: the quick check that just passed did. |

A download is started right after its own walk, so its files are still open when libtorrent's
`DownloadMain::start()` runs. `hash_done()` passes libtorrent's `start_keep_files` only for a
download whose quick check passed in a walk that found it closed and opened it; libtorrent then
keeps the open files when `system.files.advise_random` equals `system.files.advise_random.hashing`.
Every other start reopens the files by path, as upstream does: a full check, a later Start (also of
a download `hash_done()` left stopped), a start after `d.stop`, a download opened before its walk.

Setting `system.files.advise_random.hashing.set = 1` (with `advise_random` left at 0) makes every
start reopen its files. It is libtorrent's own setting and does more: files opened for hashing get
`POSIX_FADV_RANDOM` and hashed chunks `MADV_RANDOM`, so full hash checks read without read-ahead
and run slower, on spinning disks especially.

To switch the walk, the skipped save and the kept files off, put these lines in the rc file
(`catch` keeps an older daemon parsing it). The fork's libtorrent still stamps each file use with
the current time, which orders the eviction of open files.

    catch = {"pieces.hash.slice_ms.set=0"}
    catch = {"pieces.hash.skip_unchanged_save.set=0"}
    catch = {"system.files.advise_random.hashing.set=1"}

## The startup-load record

One line per process, written to the `errors` log group (with the image's rc: `/tmp/rtorrent.log`).
A walk that ends with nothing left to open arms it for the next scheduler pass, so it follows the
`hash_done()` work of that walk; it waits while a yielded walk is pending. A walk runs in the
first scheduler pass, so the record also comes when the session starts nothing.
No line is written once a shutdown has begun, and later walks write nothing.

    startup-load: slice_ms=100 slice_yield=0 skip_unchanged_save=1 downloads=421 slices=120 total_ms=36679 files_opened=15422 files_closed=11326

(lab: 421 downloads, 15,422 files, 4,096 open files at most, every path call delayed by 300 µs)

- the switch values are those of the first walk;
- `downloads` counts the closed downloads the walks opened (a failed open included), `slices` the
  walks that opened at least one;
- `total_ms` runs from the start of the first walk that opened a download to the record (0 when
  none was opened);
- `files_opened` and `files_closed` are libtorrent's file counters since the process started
  (`system.files.opened_counter`, `system.files.closed_counter`).
