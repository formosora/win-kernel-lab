# PoC 02 — mem-rw

Read and write another process's user-mode memory from kernel space.

## What it does

```
poc02.exe read  7340 7ff6a1b20000 128      → hexdump 128 bytes from notepad
poc02.exe write 7340 7ff6a1b20000 "90 90"  → patch two NOPs into it
```

## The mechanism

The naive ways to touch foreign memory — `KeStackAttachProcess` +
raw pointer access, or `MmMapIoSpace` against physical pages — both work and
both are fragile. This PoC uses **`MmCopyVirtualMemory`**, which exists
precisely for cross-process copies:

- the kernel switches address spaces internally and safely
- partial reads/writes report the exact byte count moved
- no manual attachment, no `__try/__except` gymnastics around user pages

```c
// read:  foreign process -> our buffer
MmCopyVirtualMemory(targetProc, addr, PsGetCurrentProcess(), buf, size, KernelMode, &moved);

// write: our buffer -> foreign process
MmCopyVirtualMemory(PsGetCurrentProcess(), payload, targetProc, addr, size, KernelMode, &moved);
```

Everything else is PoC-01 plumbing: device object, symlink, one
`IRP_MJ_DEVICE_CONTROL` dispatch handling two IOCTLs (`READ` / `WRITE`) with
`METHOD_BUFFERED`, and a 64 KB sanity cap per call.

## Safety rails (deliberately present)

- size cap (`POC02_MAX_TRANSFER`) — a typo'd length can't dump gigabytes
- input length validation before touching the request buffer
- `PsLookupProcessByProcessId` result always dereferenced

## Files

- `driver/main.c` — the whole driver (~130 lines, commented)
- `usermode/main.c` — CLI with hexdump output (~120 lines)
