# PoC 04 — minifilter-watch

Watch every file create/open and write across the system, from a Filter
Manager minifilter — the supported way to live inside the I/O path.

## What it does

```
[CREATE] pid= 7340  \Device\HarddiskVolume3\Users\me\Desktop\note.txt
[WRITE ] pid= 7340  \Device\HarddiskVolume3\Users\me\Desktop\note.txt
```

## The mechanism

- `FltRegisterFilter` + `FltStartFiltering` attach us to the I/O stack;
  post-operation callbacks fire for `IRP_MJ_CREATE` and `IRP_MJ_WRITE`.
- Post-op callbacks run at **IRQL ≤ DISPATCH_LEVEL**, so the name query uses
  `FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT` — the cached opened
  name, served without touching lower filters or paging. Getting this wrong
  is the classic minifilter bugcheck.
- Events flow into the same spin-locked ring + IOCTL reader pattern as
  PoC 01 — one mechanism, learned once, reused.

## Why minifilter and not a legacy filter driver?

Legacy sfilter-style drivers hook the stack raw and are fragile across
releases. Minifilters delegate attachment, ordering (altitudes) and teardown
to the Filter Manager — less code, supported semantics, and safe unload.

## Load it (test VM)

Minifilters need an altitude from an inf:

```cmd
RUNDLL32.EXE SETUPAPI.DLL,InstallHinfSection DefaultInstall 132 .\poc04.inf
fltmc load poc04
poc04-reader.exe        :: CREATE/WRITE lines roll in
fltmc unload poc04
```

## Files

- `driver/main.c` — the minifilter (~200 lines, commented)
- `driver/poc04.inf` — altitude / instance registration
- `usermode/main.c` — console reader
