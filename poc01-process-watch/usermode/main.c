/**
 * win-kernel-lab / PoC 01 — user-mode reader for the process-watch driver.
 *
 * Opens \\.\PocWatch and polls the driver's event ring once per second,
 * printing process start/exit events as they arrive.
 */

#include <windows.h>
#include <stdio.h>

/* must match the driver */
#define IOCTL_POCWATCH_READ \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)

#pragma pack(push, 8)
typedef struct _POC_EVENT {
    HANDLE  ProcessId;
    HANDLE  ParentId;
    BOOLEAN Created;
    CHAR    ImageName[16];
} POC_EVENT;
#pragma pack(pop)

int main(void)
{
    HANDLE dev = CreateFileW(L"\\\\.\\PocWatch", GENERIC_READ,
                             FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        printf("[-] cannot open device (error %lu) — is the driver loaded?\n", GetLastError());
        return 1;
    }

    puts("[*] watching processes — Ctrl+C to quit\n");

    static POC_EVENT events[64];
    for (;;) {
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(dev, IOCTL_POCWATCH_READ,
                                  NULL, 0, events, sizeof(events), &bytes, NULL);
        if (!ok) {
            printf("[-] DeviceIoControl failed (%lu)\n", GetLastError());
            break;
        }

        DWORD count = bytes / sizeof(POC_EVENT);
        for (DWORD i = 0; i < count; i++) {
            printf("[%s] pid=%5llu  ppid=%5llu  %s\n",
                   events[i].Created ? "START" : "STOP ",
                   (unsigned long long)(uintptr_t)events[i].ProcessId,
                   (unsigned long long)(uintptr_t)events[i].ParentId,
                   events[i].ImageName);
        }
        Sleep(1000);
    }

    CloseHandle(dev);
    return 0;
}
