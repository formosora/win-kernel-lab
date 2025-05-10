/**
 * win-kernel-lab / PoC 02 — process memory read/write driver
 *
 * A minimal WDM driver that reads and writes another process's user-mode
 * memory via MmCopyVirtualMemory, driven by two IOCTLs.
 *
 * The point of this PoC: cross-process memory access done *cleanly* —
 * no KeStackAttachProcess, no MmMapIoSpace tricks, just the documented
 * MmCopyVirtualMemory which handles the address-space switch internally.
 */

#include <ntifs.h>
#include <ntstrsafe.h>

/* Exported by ntoskrnl since Vista; absent from some kits' public headers. */
NTKERNELAPI NTSTATUS MmCopyVirtualMemory(
    _In_ PEPROCESS SourceProcess,
    _In_ PVOID SourceAddress,
    _In_ PEPROCESS TargetProcess,
    _In_ PVOID TargetAddress,
    _In_ SIZE_T BufferSize,
    _In_ KPROCESSOR_MODE PreviousMode,
    _Out_ PSIZE_T ReturnSize);

#define POC02_DEVICE_NAME  L"\\Device\\PocMemRW"
#define POC02_SYMLINK_NAME L"\\DosDevices\\PocMemRW"

#define IOCTL_MEM_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_MEM_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_WRITE_DATA)

#define POC02_MAX_TRANSFER (64 * 1024)   // sanity cap per call

typedef struct _POC_MEM_REQ {
    HANDLE ProcessId;   // target process
    PVOID  Address;     // user-mode address inside the target
    ULONG  Size;        // bytes to move (payload follows the struct for WRITE)
} POC_MEM_REQ, *PPOC_MEM_REQ;

static NTSTATUS Poc02CreateClose(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS Poc02DeviceControl(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DevObj);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;   // METHOD_BUFFERED shared buffer
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (code == IOCTL_MEM_READ || code == IOCTL_MEM_WRITE) {
        if (inLen < sizeof(POC_MEM_REQ)) {
            status = STATUS_BUFFER_TOO_SMALL;
            goto done;
        }

        PPOC_MEM_REQ req = (PPOC_MEM_REQ)buf;
        if (req->Size == 0 || req->Size > POC02_MAX_TRANSFER) {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }

        PEPROCESS target = NULL;
        status = PsLookupProcessByProcessId(req->ProcessId, &target);
        if (!NT_SUCCESS(status))
            goto done;

        SIZE_T moved = 0;
        if (code == IOCTL_MEM_READ) {
            if (outLen < req->Size) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                // target -> our kernel buffer
                status = MmCopyVirtualMemory(target, req->Address,
                                             PsGetCurrentProcess(), buf,
                                             req->Size, KernelMode, &moved);
                info = (ULONG_PTR)moved;
            }
        } else {
            if (inLen < sizeof(POC_MEM_REQ) + req->Size) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                // our kernel buffer (payload follows the struct) -> target
                status = MmCopyVirtualMemory(PsGetCurrentProcess(), (PUCHAR)buf + sizeof(POC_MEM_REQ),
                                             target, req->Address,
                                             req->Size, KernelMode, &moved);
                info = (ULONG_PTR)moved;
            }
        }
        ObDereferenceObject(target);
    }

done:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID Poc02Unload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING sym = RTL_CONSTANT_STRING(POC02_SYMLINK_NAME);
    IoDeleteSymbolicLink(&sym);
    if (DriverObject->DeviceObject)
        IoDeleteDevice(DriverObject->DeviceObject);
    DbgPrint("[poc02] unloaded\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING devName = RTL_CONSTANT_STRING(POC02_DEVICE_NAME);
    PDEVICE_OBJECT devObj = NULL;
    NTSTATUS status = IoCreateDevice(DriverObject, 0, &devName,
                                     FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &devObj);
    if (!NT_SUCCESS(status))
        return status;

    UNICODE_STRING sym = RTL_CONSTANT_STRING(POC02_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&sym, &devName);   // user mode opens \\.\PocMemRW
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(devObj);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = Poc02CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = Poc02CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Poc02DeviceControl;
    DriverObject->DriverUnload                         = Poc02Unload;

    DbgPrint("[poc02] loaded\n");
    return STATUS_SUCCESS;
}
