/**
 * win-kernel-lab / PoC 04 — user-mode reader for the minifilter watcher.
 *
 * Opens \\.\PocFileWatch and prints file create/write events once per second.
 */

#include <windows.h>
#include <stdio.h>

#define IOCTL_POC04_READ \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_READ_DATA)

#pragma pack(push, 8)
typedef struct _POC04_EVENT {
    HANDLE ProcessId;
    ULONG  Op;
    WCHAR  Path[260];
} POC04_EVENT;
#pragma pack(pop)

int main(void)
{
    HANDLE dev = CreateFileW(L"\\\\.\\PocFileWatch", GENERIC_READ,
                             FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        printf("[-] cannot open device (error %lu) — is the minifilter loaded?\n", GetLastError());
        return 1;
    }

    puts("[*] watching file activity — Ctrl+C to quit\n");

    static POC04_EVENT events[32];
    for (;;) {
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(dev, IOCTL_POC04_READ,
                                  NULL, 0, events, sizeof(events), &bytes, NULL);
        if (!ok) {
            printf("[-] DeviceIoControl failed (%lu)\n", GetLastError());
            break;
        }

        DWORD count = bytes / sizeof(POC04_EVENT);
        for (DWORD i = 0; i < count; i++) {
            wprintf(L"[%s] pid=%5llu  %s\n",
                    events[i].Op == 1 ? L"CREATE" : L"WRITE ",
                    (unsigned long long)(uintptr_t)events[i].ProcessId,
                    events[i].Path);
        }
        Sleep(1000);
    }

    CloseHandle(dev);
    return 0;
}
