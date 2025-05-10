/**
 * win-kernel-lab / PoC 02 — user-mode tool for the memory RW driver.
 *
 *   poc02.exe read  <pid> <hex-address> <size>     hexdump target memory
 *   poc02.exe write <pid> <hex-address> <hex-bytes>  e.g. write 1234 7ff6..  "90 90 C3"
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define IOCTL_MEM_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_MEM_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_WRITE_DATA)

#pragma pack(push, 8)
typedef struct _POC_MEM_REQ {
    HANDLE ProcessId;
    PVOID  Address;
    ULONG  Size;
} POC_MEM_REQ;
#pragma pack(pop)

static HANDLE openDriver(void)
{
    HANDLE h = CreateFileW(L"\\\\.\\PocMemRW", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        printf("[-] cannot open driver (error %lu) — is poc02.sys loaded?\n", GetLastError());
    return h;
}

static void hexdump(const unsigned char* p, size_t n, uintptr_t base)
{
    for (size_t off = 0; off < n; off += 16) {
        printf("0x%016llx  ", (unsigned long long)(base + off));
        for (size_t i = 0; i < 16 && off + i < n; i++) printf("%02X ", p[off + i]);
        printf("\n");
    }
}

static int doRead(HANDLE dev, ULONG pid, uintptr_t addr, ULONG size)
{
    unsigned char* buf = malloc(size);
    POC_MEM_REQ* req = malloc(sizeof(POC_MEM_REQ));
    if (!buf || !req) return 1;

    req->ProcessId = (HANDLE)(uintptr_t)pid;
    req->Address = (PVOID)addr;
    req->Size = size;

    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_MEM_READ, req, sizeof(*req), buf, size, &bytes, NULL);
    if (ok) hexdump(buf, bytes, addr);
    else printf("[-] read failed (%lu)\n", GetLastError());

    free(buf);
    free(req);
    return ok ? 0 : 1;
}

static int doWrite(HANDLE dev, ULONG pid, uintptr_t addr, const char* hex)
{
    /* "90 90 C3" -> bytes */
    unsigned char payload[4096];
    size_t n = 0;
    while (*hex && n < sizeof(payload)) {
        while (*hex == ' ') hex++;
        if (!*hex) break;
        payload[n++] = (unsigned char)strtoul(hex, (char**)&hex, 16);
    }
    if (n == 0) { printf("[-] no bytes parsed\n"); return 1; }

    size_t total = sizeof(POC_MEM_REQ) + n;
    POC_MEM_REQ* req = malloc(total);
    if (!req) return 1;
    req->ProcessId = (HANDLE)(uintptr_t)pid;
    req->Address = (PVOID)addr;
    req->Size = (ULONG)n;
    memcpy((unsigned char*)req + sizeof(*req), payload, n);

    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_MEM_WRITE, req, (DWORD)total, NULL, 0, &bytes, NULL);
    printf(ok ? "[+] wrote %lu bytes\n" : "[-] write failed (%lu)\n", ok ? bytes : 0ul, GetLastError());

    free(req);
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        puts("usage:\n  poc02 read  <pid> <hex-addr> <size>\n  poc02 write <pid> <hex-addr> \"90 90 C3\"");
        return 1;
    }

    HANDLE dev = openDriver();
    if (dev == INVALID_HANDLE_VALUE) return 1;

    ULONG pid = strtoul(argv[2], NULL, 10);
    uintptr_t addr = (uintptr_t)_strtoui64(argv[3], NULL, 16);
    int rc;

    if (_stricmp(argv[1], "read") == 0)
        rc = doRead(dev, pid, addr, strtoul(argv[4], NULL, 10));
    else if (_stricmp(argv[1], "write") == 0)
        rc = doWrite(dev, pid, addr, argv[4]);
    else {
        puts("[-] unknown command");
        rc = 1;
    }

    CloseHandle(dev);
    return rc;
}
