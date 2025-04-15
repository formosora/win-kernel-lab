# PoC 01 — process-watch

Watch every process start and exit on the system, from kernel space, in real
time.

## What it does

```
user mode                       kernel
─────────                       ───────
poc01-reader.exe                poc01.sys
  │ CreateFile(\\.\PocWatch)      │ DriverEntry → IoCreateDevice + symlink
  │ DeviceIoControl(READ) ──────► │ ring buffer  ◄── PsSetCreateProcessNotifyRoutine
  │ print START/STOP lines        │    (256 events, spinlock)
```

## The mechanism

- `PsSetCreateProcessNotifyRoutine(cb, FALSE)` registers `cb` to fire on
  every process creation and termination. It runs at `PASSIVE_LEVEL` in an
  arbitrary thread context — so the callback only receives PIDs, and we use
  `PsLookupProcessByProcessId` + `PsGetProcessImageFileName` to grab the image
  name (max 15 chars, that's an OS limit).
- Events go into a 256-slot ring guarded by a spinlock. When full, the oldest
  event is overwritten — the reader never blocks the writer.
- User mode reads through `IOCTL_POCWATCH_READ` (`METHOD_BUFFERED`): the I/O
  manager gives both sides one shared buffer, we copy events out and advance
  the read cursor. No `IRP_MJ_READ` boilerplate needed for a lab.
- Everything unwinds in `DriverUnload`: unregister the callback → delete
  symlink → delete device. Clean unload means `sc stop` + `sc start` cycles
  without reboots.

## Why WDM and not KMDF?

KMDF is the right answer in production. For the first PoC we stay WDM so the
whole IRP flow — dispatch routines, `IoCompleteRequest`, status codes — is
visible end to end. PoC 02 stays WDM too; the KMDF rewrite is a deliberate
later exercise.

## Files

- `driver/main.c` — the whole driver (~150 lines, commented)
- `usermode/main.c` — the console reader (~60 lines)
