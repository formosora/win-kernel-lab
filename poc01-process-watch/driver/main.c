/**
 * win-kernel-lab / PoC 01 — process-watch driver
 *
 * A minimal WDM driver that watches process creation & termination with
 * PsSetCreateProcessNotifyRoutine, stores events in a small ring buffer,
 * and exposes them to user mode through an IOCTL.
 *
 * Deliberately WDM (not KMDF): the point of this PoC is seeing every moving
 * part — device object, symlink, IRP dispatch, completion — with no framework
 * hiding the flow.
 */

#include <ntddk.h>
#include <ntstrsafe.h>

#define POC_DEVICE_NAME  L"\\Device\\PocProcessWatch"
#define POC_SYMLINK_NAME L"\\DosDevices\\PocWatch"

#define IOCTL_POCWATCH_READ \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)

typedef struct _POC_EVENT {
    HANDLE  ProcessId;
    HANDLE  ParentId;
    BOOLEAN Created;                 // TRUE = started, FALSE = exited
    CHAR    ImageName[16];           // OS keeps at most 15 chars + NUL
} POC_EVENT, *PPOC_EVENT;

#define POC_RING_CAPACITY 256

static POC_EVENT  g_Ring[POC_RING_CAPACITY];
static LONG       g_WriteIndex = 0;   // next free slot
static LONG       g_ReadIndex  = 0;   // next unread slot
static KSPIN_LOCK g_RingLock;

/**
 * Called by the kernel on every process create/exit.
 * Runs at PASSIVE_LEVEL in the creator's thread context — but the "current"
 * process here is arbitrary, so we look the target up by PID.
 */
static VOID PocOnProcessNotify(_In_ HANDLE ParentId, _In_ HANDLE ProcessId, _In_ BOOLEAN Create)
{
    POC_EVENT evt = { 0 };
    evt.ProcessId = ProcessId;
    evt.ParentId  = ParentId;
    evt.Created   = Create;

    PEPROCESS ep = NULL;
    if (NT_SUCCESS(PsLookupProcessByProcessId(ProcessId, &ep))) {
        const UCHAR* name = PsGetProcessImageFileName(ep);   // points into EPROCESS
        if (name) {
            // always NUL-terminates, truncates safely
            RtlStringCchCopyA(evt.ImageName, RTL_NUMBER_OF(evt.ImageName), (PCSTR)name);
        }
        ObDereferenceObject(ep);
    }

    KIRQL irql;
    KeAcquireSpinLock(&g_RingLock, &irql);
    LONG next = (g_WriteIndex + 1) % POC_RING_CAPACITY;
    if (next == g_ReadIndex) {                      // full → drop the oldest
        g_ReadIndex = (g_ReadIndex + 1) % POC_RING_CAPACITY;
    }
    g_Ring[g_WriteIndex] = evt;
    g_WriteIndex = next;
    KeReleaseSpinLock(&g_RingLock, irql);
}

static NTSTATUS PocDispatchCreateClose(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS PocDispatchDeviceControl(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_POCWATCH_READ) {
        // METHOD_BUFFERED: the I/O manager gives us one shared buffer.
        PVOID outBuf = Irp->AssociatedIrp.SystemBuffer;
        ULONG outCap = stack->Parameters.DeviceIoControl.OutputBufferLength / sizeof(POC_EVENT);
        ULONG copied = 0;

        KIRQL irql;
        KeAcquireSpinLock(&g_RingLock, &irql);
        while (copied < outCap && g_ReadIndex != g_WriteIndex) {
            ((PPOC_EVENT)outBuf)[copied++] = g_Ring[g_ReadIndex];
            g_ReadIndex = (g_ReadIndex + 1) % POC_RING_CAPACITY;
        }
        KeReleaseSpinLock(&g_RingLock, irql);

        info = copied * sizeof(POC_EVENT);
        status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID PocUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    PsSetCreateProcessNotifyRoutine(PocOnProcessNotify, TRUE);   // unregister

    UNICODE_STRING sym = RTL_CONSTANT_STRING(POC_SYMLINK_NAME);
    IoDeleteSymbolicLink(&sym);
    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);

    DbgPrint("[poc01] unloaded\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&g_RingLock);

    UNICODE_STRING devName = RTL_CONSTANT_STRING(POC_DEVICE_NAME);
    PDEVICE_OBJECT devObj = NULL;
    NTSTATUS status = IoCreateDevice(DriverObject, 0, &devName,
                                     FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &devObj);
    if (!NT_SUCCESS(status))
        return status;

    UNICODE_STRING sym = RTL_CONSTANT_STRING(POC_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&sym, &devName);   // user mode opens \\.\PocWatch
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(devObj);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = PocDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = PocDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PocDispatchDeviceControl;
    DriverObject->DriverUnload                         = PocUnload;

    status = PsSetCreateProcessNotifyRoutine(PocOnProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        IoDeleteSymbolicLink(&sym);
        IoDeleteDevice(devObj);
        return status;
    }

    DbgPrint("[poc01] loaded, watching processes\n");
    return STATUS_SUCCESS;
}
