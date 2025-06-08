# PoC 03 — ETW → live web dashboard

Real-time process start/stop events streamed into a browser dashboard —
**without a driver**. This is the supported-path counterpart of PoC 01.

## What it does

```
Microsoft-Windows-Kernel-Process (ETW provider)
        │  realtime session
        ▼
poc03.exe  ── SSE broadcast ──►  http://localhost:9180  (live dashboard)
```

Run `poc03.exe`, open the URL, and watch process events land in the page the
instant they happen.

## The mechanism

- **ETW realtime session** via [KrabsETW](https://github.com/microsoft/krabsetw)
  (Microsoft's C++ wrapper, vendored under `third_party/`). We arm the kernel
  process provider and filter to event IDs 1 (start) / 2 (stop), parsing
  `ProcessID` / `ParentID` / `ImageFileName` from each record.
- **Tiny embedded HTTP server** on raw Winsock (~80 lines): serves the
  dashboard on `GET /` and an SSE stream on `GET /events`. Each browser
  client sits on an open socket; every ETW event is broadcast as a
  `data: {...}\n\n` frame.
- **The dashboard** is a single embedded HTML page using `EventSource` —
  no JS framework, no build step.

## Driver callback vs ETW (why both PoCs exist)

| | PoC 01 (driver) | PoC 03 (ETW) |
| - | ------------- | ------------ |
| Needs a driver / test signing | yes | no |
| Delivery | IOCTL pull | push, buffered by the OS |
| Historic events | only while loaded | OS-managed trace session |
| Teaches | device objects, IRPs | ETW sessions, schema parsing |

Same signal, two transport philosophies — knowing both is the point.

## Build notes

- MSVC: `/std:c++17 /EHsc /DUNICODE /D_UNICODE`, link `ws2_32.lib` (krabs
  pulls in `tdh`/`advapi32` via its own pragmas)
- Vendored krabs carries one marked patch: `kt.hpp` returns the narrow
  `KERNEL_LOGGER_NAME` literal as `std::wstring`; we return
  `KERNEL_LOGGER_NAMEW` instead (required by modern MSVC conformance)

## Files

- `consumer/main.cpp` — ETW session + HTTP/SSE server + embedded dashboard (~200 lines)
- `third_party/krabs/` — KrabsETW headers (MIT, © Microsoft)
