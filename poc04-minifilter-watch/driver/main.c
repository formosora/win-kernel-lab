/**
 * win-kernel-lab / PoC 04 — minifilter file watcher
 *
 * A file-system minifilter that records file create/open and write activity
 * system-wide, using the Filter Manager (fltMgr) — the supported way to sit
 * inside the I/O path. Events go to a ring buffer; user mode reads them
 * through IOCTL, same pattern as PoC 01.
 *
 * Teaches: FLT_REGISTRATION, post-operation callbacks, file-name retrieval,
 * IRQL discipline at DISPATCH_LEVEL, and how minifilters load (inf + fltmc).
 */

#include <fltKernel.h>
#include <ntstrsafe.h>

#define POC04_DEVICE_NAME  L"\\Device\\PocFileWatch"
#define POC04_SYMLINK_NAME L"\\DosDevices\\PocFileWatch"

#define IOCTL_POC04_READ \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_READ_DATA)

#define POC04_OP_CREATE 1
#define POC04_OP_WRITE  2

typedef struct _POC04_EVENT {
    HANDLE  ProcessId;
    ULONG   Op;                  // POC04_OP_*
    WCHAR   Path[260];           // opened path (truncated)
} POC04_EVENT, *PPOC04_EVENT;

#define POC04_RING_CAPACITY 256

static POC04_EVENT g_Ring[POC04_RING_CAPACITY];
static LONG        g_WriteIndex = 0;
static LONG        g_ReadIndex  = 0;
static KSPIN_LOCK  g_RingLock;

static PFLT_FILTER g_Filter = NULL;

/* ---------------- event capture ---------------- */

static VOID Poc04Push(ULONG op, _In_ PUNICODE_STRING name)
{
    POC04_EVENT evt = { 0 };
    evt.ProcessId = PsGetCurrentProcessId();
    evt.Op = op;

    ULONG bytes = min(name->Length, sizeof(evt.Path) - sizeof(WCHAR));
    RtlCopyMemory(evt.Path, name->Buffer, bytes);
    evt.Path[bytes / sizeof(WCHAR)] = L'\0';

    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);
    LONG next = (g_WriteIndex + 1) % POC04_RING_CAPACITY;
    if (next == g_ReadIndex)
        g_ReadIndex = (g_ReadIndex + 1) % POC04_RING_CAPACITY;   // drop oldest
    g_Ring[g_WriteIndex] = evt;
    g_WriteIndex = next;
    KeReleaseSpinLock(&g_RingLock, irql);
}

/**
 * Post-op callbacks run at IRQL <= DISPATCH_LEVEL, so no paging I/O here.
 * FLT_FILE_NAME_OPENED | QUERY_DEFAULT serves the cached opened name without
 * querying lower filters — safe at DISPATCH.
 */
static NTSTATUS Poc04PostOp(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ ULONG op)
{
    if (!NT_SUCCESS(Data->IoStatus.Status))
        return FLT_POSTOP_FINISHED_PROCESSING;
    if (Data->RequestorMode == KernelMode)
        return FLT_POSTOP_FINISHED_PROCESSING;

    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    if (NT_SUCCESS(FltGetFileNameInformation(Data,
                                             FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT,
                                             &nameInfo))) {
        FltParseFileNameInformation(nameInfo);
        Poc04Push(op, &nameInfo->Name);
        FltReleaseFileNameInformation(nameInfo);
    }
    return FLT_POSTOP_FINISHED_PROCESSING;
}

static FLT_POSTOP_CALLBACK_STATUS Poc04PostCreate(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);
    Poc04PostOp(Data, POC04_OP_CREATE);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

static FLT_POSTOP_CALLBACK_STATUS Poc04PostWrite(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);
    Poc04PostOp(Data, POC04_OP_WRITE);
    return FLT_POSTOP_FINISHED_PROCESSING;
}

/* ---------------- IOCTL (same ring-reader pattern as PoC 01) ---------------- */

static NTSTATUS Poc04CreateClose(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS Poc04DeviceControl(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_POC04_READ) {
        PVOID outBuf = Irp->AssociatedIrp.SystemBuffer;
        ULONG outCap = stack->Parameters.DeviceIoControl.OutputBufferLength / sizeof(POC04_EVENT);
        ULONG copied = 0;

        KIRQL irql;
        KeAcquireSpinLock(&g_RingLock, &irql);
        while (copied < outCap && g_ReadIndex != g_WriteIndex) {
            ((PPOC04_EVENT)outBuf)[copied++] = g_Ring[g_ReadIndex];
            g_ReadIndex = (g_ReadIndex + 1) % POC04_RING_CAPACITY;
        }
        KeReleaseSpinLock(&g_RingLock, irql);

        info = copied * sizeof(POC04_EVENT);
        status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ---------------- registration & entry ---------------- */

static NTSTATUS Poc04Unload(_In_ FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    if (g_Filter)
        FltUnregisterFilter(g_Filter);
    return STATUS_SUCCESS;
}

static const FLT_OPERATION_REGISTRATION g_Callbacks[] = {
    { IRP_MJ_CREATE, 0, NULL, Poc04PostCreate },
    { IRP_MJ_WRITE,  0, NULL, Poc04PostWrite  },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION g_Registration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,                 // flags
    NULL,              // context registration
    g_Callbacks,
    Poc04Unload,
    NULL,              // instance setup
    NULL,              // instance query teardown
    NULL,              // instance teardown start
    NULL,              // instance teardown complete
    NULL, NULL, NULL   // name generation / normalization callbacks
};

static VOID Poc04DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    if (g_Filter) {
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
    }
    UNICODE_STRING sym = RTL_CONSTANT_STRING(POC04_SYMLINK_NAME);
    IoDeleteSymbolicLink(&sym);
    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);
    DbgPrint("[poc04] unloaded\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&g_RingLock);

    // control device for user-mode readers
    UNICODE_STRING devName = RTL_CONSTANT_STRING(POC04_DEVICE_NAME);
    PDEVICE_OBJECT devObj = NULL;
    NTSTATUS status = IoCreateDevice(DriverObject, 0, &devName,
                                     FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &devObj);
    if (!NT_SUCCESS(status))
        return status;

    UNICODE_STRING sym = RTL_CONSTANT_STRING(POC04_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&sym, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(devObj);
        return status;
    }

    // register with the Filter Manager and start filtering
    status = FltRegisterFilter(DriverObject, &g_Registration, &g_Filter);
    if (!NT_SUCCESS(status)) {
        IoDeleteSymbolicLink(&sym);
        IoDeleteDevice(devObj);
        return status;
    }

    status = FltStartFiltering(g_Filter);
    if (!NT_SUCCESS(status)) {
        FltUnregisterFilter(g_Filter);
        g_Filter = NULL;
        IoDeleteSymbolicLink(&sym);
        IoDeleteDevice(devObj);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = Poc04CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = Poc04CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Poc04DeviceControl;
    DriverObject->DriverUnload                         = Poc04DriverUnload;

    DbgPrint("[poc04] loaded, watching file activity\n");
    return STATUS_SUCCESS;
}
