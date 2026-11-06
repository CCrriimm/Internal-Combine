#if defined(__has_include) && __has_include(<ntddk.h>)
#include <ntddk.h>
#include <ntifs.h>
#else
#include "nt_standalone.h"
#endif
#include "bskdrv_ioctl.h"

NTSTATUS NTAPI ObOpenObjectByPointer(
    PVOID Object,
    ULONG HandleAttributes,
    PVOID PassedAccessState,
    ACCESS_MASK DesiredAccess,
    PVOID ObjectType,
    KPROCESSOR_MODE AccessMode,
    PHANDLE Handle);

NTSTATUS NTAPI RtlCreateUserThread(
    HANDLE ProcessHandle,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    BOOLEAN CreateSuspended,
    ULONG StackZeroBits,
    SIZE_T StackReserve,
    SIZE_T StackCommit,
    PVOID StartAddress,
    PVOID Parameter,
    PHANDLE ThreadHandle,
    PCLIENT_ID ClientId);

typedef struct _BSK_PEB {
    UCHAR Reserved[0x18];
    PVOID Ldr;
} BSK_PEB, *PBSK_PEB;

typedef struct _BSK_PEB_LDR {
    ULONG Length;
    UCHAR Initialized;
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
} BSK_PEB_LDR, *PBSK_PEB_LDR;

typedef struct _BSK_LDR_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} BSK_LDR_ENTRY, *PBSK_LDR_ENTRY;

static PDEVICE_OBJECT g_device = NULL;
static UNICODE_STRING g_symlink;

static BOOLEAN BskNameEq(PCUNICODE_STRING name, const WCHAR* want) {
    USHORT i;
    USHORT len;
    if (!name || !name->Buffer || !want) return FALSE;
    len = name->Length / sizeof(WCHAR);
    for (i = 0; i < len; ++i) {
        if (!want[i]) return FALSE;
    }
    if (want[i] != 0) return FALSE;
    for (i = 0; i < len; ++i) {
        WCHAR a = name->Buffer[i];
        WCHAR b = want[i];
        if (a >= L'a' && a <= L'z') a = (WCHAR)(a - 32);
        if (b >= L'a' && b <= L'z') b = (WCHAR)(b - 32);
        if (a != b) return FALSE;
    }
    return TRUE;
}

static NTSTATUS BskCopyOut(PEPROCESS proc, PVOID remote, PVOID local, SIZE_T size) {
    SIZE_T done = 0;
    if (size == 0) return STATUS_SUCCESS;
    if (!remote || !local) return STATUS_INVALID_PARAMETER;
    return MmCopyVirtualMemory(proc, remote, PsGetCurrentProcess(), local, size, KernelMode, &done);
}

static NTSTATUS BskCopyIn(PEPROCESS proc, PVOID remote, PVOID local, SIZE_T size) {
    SIZE_T done = 0;
    if (size == 0) return STATUS_SUCCESS;
    if (!remote || !local) return STATUS_INVALID_PARAMETER;
    return MmCopyVirtualMemory(PsGetCurrentProcess(), local, proc, remote, size, KernelMode, &done);
}

static NTSTATUS BskAlloc(PEPROCESS proc, ULONG64 desired, SIZE_T size, ULONG prot, ULONG64* out) {
    NTSTATUS st;
    KAPC_STATE apc;
    PVOID base = (PVOID)(ULONG_PTR)desired;
    SIZE_T region = size;
    if (size == 0 || size > 256ull * 1024 * 1024) return STATUS_INVALID_PARAMETER;
    KeStackAttachProcess(proc, &apc);
    st = ZwAllocateVirtualMemory((HANDLE)(LONG_PTR)-1, &base, 0, &region,
                                 MEM_COMMIT | MEM_RESERVE, prot);
    KeUnstackDetachProcess(&apc);
    if (NT_SUCCESS(st) && out) *out = (ULONG64)(ULONG_PTR)base;
    return st;
}

static NTSTATUS BskFree(PEPROCESS proc, ULONG64 addr) {
    NTSTATUS st;
    KAPC_STATE apc;
    PVOID base = (PVOID)(ULONG_PTR)addr;
    SIZE_T region = 0;
    if (!addr) return STATUS_INVALID_PARAMETER;
    KeStackAttachProcess(proc, &apc);
    st = ZwFreeVirtualMemory((HANDLE)(LONG_PTR)-1, &base, &region, MEM_RELEASE);
    KeUnstackDetachProcess(&apc);
    return st;
}

static NTSTATUS BskProtect(PEPROCESS proc, ULONG64 addr, SIZE_T size, ULONG prot, ULONG* old) {
    NTSTATUS st;
    KAPC_STATE apc;
    PVOID base = (PVOID)(ULONG_PTR)addr;
    SIZE_T region = size;
    ULONG prev = 0;
    if (!addr || size == 0) return STATUS_INVALID_PARAMETER;
    KeStackAttachProcess(proc, &apc);
    st = ZwProtectVirtualMemory((HANDLE)(LONG_PTR)-1, &base, &region, prot, &prev);
    KeUnstackDetachProcess(&apc);
    if (NT_SUCCESS(st) && old) *old = prev;
    return st;
}

static NTSTATUS BskThread(PEPROCESS proc, ULONG64 start, ULONG64 param, ULONG64* tid) {
    NTSTATUS st;
    HANDLE proc_handle = NULL;
    HANDLE thread = NULL;
    CLIENT_ID cid = { 0 };

    st = ObOpenObjectByPointer(proc, OBJ_KERNEL_HANDLE, NULL, PROCESS_ALL_ACCESS,
                               NULL, KernelMode, &proc_handle);
    if (!NT_SUCCESS(st)) return st;

    st = RtlCreateUserThread(proc_handle, NULL, FALSE, 0, 0, 0,
                             (PVOID)(ULONG_PTR)start, (PVOID)(ULONG_PTR)param,
                             &thread, &cid);
    if (NT_SUCCESS(st)) {
        if (tid) *tid = (ULONG64)(ULONG_PTR)cid.UniqueThread;
        if (thread) ZwClose(thread);
    }
    ZwClose(proc_handle);
    return st;
}

static NTSTATUS BskModule(PEPROCESS proc, const WCHAR* name, ULONG64* base, ULONG64* size) {
    NTSTATUS st = STATUS_NOT_FOUND;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    __try {
        PBSK_PEB peb = (PBSK_PEB)PsGetProcessPeb(proc);
        if (peb && peb->Ldr) {
            PBSK_PEB_LDR ldr = (PBSK_PEB_LDR)peb->Ldr;
            PLIST_ENTRY head = &ldr->InLoadOrderModuleList;
            PLIST_ENTRY cur = head->Flink;
            while (cur && cur != head) {
                PBSK_LDR_ENTRY ent = CONTAINING_RECORD(cur, BSK_LDR_ENTRY, InLoadOrderLinks);
                if (BskNameEq(&ent->BaseDllName, name)) {
                    if (base) *base = (ULONG64)(ULONG_PTR)ent->DllBase;
                    if (size) *size = ent->SizeOfImage;
                    st = STATUS_SUCCESS;
                    break;
                }
                cur = cur->Flink;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
    }
    KeUnstackDetachProcess(&apc);
    return st;
}

static NTSTATUS BskDeviceControl(PDEVICE_OBJECT device, PIRP irp) {
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG in_len = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st = STATUS_INVALID_PARAMETER;
    ULONG info = 0;
    BSK_REQ req;
    PEPROCESS proc = NULL;
    ULONG pid;
    SIZE_T size;

    UNREFERENCED_PARAMETER(device);

    if (!buffer || in_len < sizeof(BSK_REQ)) {
        st = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    RtlCopyMemory(&req, buffer, sizeof(req));
    pid = req.pid;
    size = req.size;
    if (size > 16ull * 1024 * 1024) {
        st = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (!pid) {
        st = STATUS_INVALID_PARAMETER;
        goto done;
    }
    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) {
        proc = NULL;
        goto done;
    }

    switch (code) {
    case IOCTL_BSK_READ:
        if (out_len < sizeof(BSK_REQ) + size) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = BskCopyOut(proc, (PVOID)(ULONG_PTR)req.addr, (PUCHAR)buffer + sizeof(BSK_REQ), size);
        if (NT_SUCCESS(st)) info = sizeof(BSK_REQ) + (ULONG)size;
        break;

    case IOCTL_BSK_WRITE:
        if (in_len < sizeof(BSK_REQ) + size) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = BskCopyIn(proc, (PVOID)(ULONG_PTR)req.addr, (PUCHAR)buffer + sizeof(BSK_REQ), size);
        break;

    case IOCTL_BSK_ALLOC: {
        ULONG64 out_base = 0;
        st = BskAlloc(proc, req.addr, size, req.prot ? req.prot : PAGE_READWRITE, &out_base);
        req.out1 = out_base;
        break;
    }

    case IOCTL_BSK_FREE:
        st = BskFree(proc, req.addr);
        break;

    case IOCTL_BSK_PROTECT: {
        ULONG old = 0;
        st = BskProtect(proc, req.addr, size, req.prot ? req.prot : PAGE_READWRITE, &old);
        req.out2 = old;
        break;
    }

    case IOCTL_BSK_THREAD: {
        ULONG64 tid = 0;
        st = BskThread(proc, req.addr, req.param, &tid);
        req.out1 = tid;
        break;
    }

    case IOCTL_BSK_MODULE: {
        ULONG64 base = 0, mod_size = 0;
        st = BskModule(proc, (const WCHAR*)req.name, &base, &mod_size);
        req.out1 = base;
        req.out2 = mod_size;
        break;
    }

    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (NT_SUCCESS(st) && code != IOCTL_BSK_READ && out_len >= sizeof(BSK_REQ)) {
        RtlCopyMemory(buffer, &req, sizeof(req));
        info = sizeof(req);
    }

done:
    if (proc) ObDereferenceObject(proc);
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS BskCreate(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS BskClose(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID BskUnload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    if (g_symlink.Buffer) IoDeleteSymbolicLink(&g_symlink);
    if (g_device) {
        IoDeleteDevice(g_device);
        g_device = NULL;
    }
}

static NTSTATUS BskRealDriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    NTSTATUS st;
    UNICODE_STRING dev_name;
    PDEVICE_OBJECT device = NULL;

    UNREFERENCED_PARAMETER(registry);

    RtlInitUnicodeString(&dev_name, BSK_DEVICE_NAME);
    RtlInitUnicodeString(&g_symlink, BSK_SYMLINK_NAME);

    st = IoCreateDevice(driver, 0, &dev_name, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(st)) return st;

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    g_device = device;

    st = IoCreateSymbolicLink(&g_symlink, &dev_name);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(device);
        g_device = NULL;
        return st;
    }

    driver->MajorFunction[IRP_MJ_CREATE] = BskCreate;
    driver->MajorFunction[IRP_MJ_CLOSE] = BskClose;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = BskDeviceControl;
    driver->DriverUnload = BskUnload;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    if (!driver) {
        UNICODE_STRING drv_name;
        NTSTATUS st;
        RtlInitUnicodeString(&drv_name, L"\\Driver\\WinSyncDrv");
        st = IoCreateDriver(&drv_name, BskRealDriverEntry);
        if (st == (NTSTATUS)0xC0000035L) { // STATUS_OBJECT_NAME_COLLISION if already loaded
            return STATUS_SUCCESS;
        }
        return st;
    }
    return BskRealDriverEntry(driver, registry);
}
