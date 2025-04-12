# 🪟 win-kernel-lab

Original Windows kernel experiments, one focused PoC at a time — every PoC is
small, compiles clean, and exists to teach exactly one mechanism.

> ⚠️ Run everything on a **test VM** with test-signing enabled. Kernel bugs
> bugcheck the machine; that's the point of a lab.

## PoCs

| # | Name | Mechanism it teaches |
| - | ---- | -------------------- |
| 01 | [process-watch](poc01-process-watch) | `PsSetCreateProcessNotifyRoutine`, device objects, IOCTL (`METHOD_BUFFERED`), spin-locked ring buffer, IRP completion |

## Toolchain

- Visual Studio 2022 + **WDK** (Windows Driver Kit)
- x64 Release builds
- A test VM: `bcdedit /set testsigning on` → reboot

## Build & load (PoC 01)

1. Open VS → *Create project* → **Empty WDM Driver** → drop in
   `driver/main.c` → build (x64 Release)
2. Usermode reader: plain *Windows Console App* → `usermode/main.c` → build x64
3. Sign the driver on the test VM:

   ```powershell
   New-SelfSignedCertificate -Type CodeSigning -Subject "CN=PocLab" `
     -CertStoreLocation Cert:\CurrentUser\My | Out-Null
   signtool sign /fd sha256 /a poc01.sys
   ```

4. Load & run:

   ```cmd
   sc create poc01 type= kernel binPath= C:\lab\poc01.sys
   sc start poc01
   poc01-reader.exe     :: watch START/STOP lines roll in
   sc stop poc01
   ```

## Roadmap

- **02** — memory read/write driver, done cleanly (inspired by studying public
  DrvRW-style samples, written from scratch)
- **03** — ETW consumer → live web dashboard (kernel meets the web stack)
