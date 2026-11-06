#pragma once

#ifndef _KERNEL_MODE
#define _KERNEL_MODE
#endif

#define VOID void
#define NTAPI __stdcall

typedef unsigned char UCHAR, *PUCHAR;
typedef unsigned short USHORT, *PUSHORT;
typedef unsigned long ULONG, *PULONG;
typedef unsigned long long ULONG64, *PULONG64;
typedef unsigned __int64 ULONG_PTR, *PULONG_PTR;
typedef __int64 LONG_PTR;
typedef unsigned __int64 SIZE_T, *PSIZE_T;
typedef unsigned short WCHAR, *PWCHAR;
typedef const unsigned short *PCWCHAR;
typedef void *PVOID;
typedef void *HANDLE, **PHANDLE;
typedef unsigned char BOOLEAN;
typedef long NTSTATUS;
typedef char CCHAR, KPROCESSOR_MODE;
typedef unsigned long ACCESS_MASK, *PACCESS_MASK;

#define OBJ_KERNEL_HANDLE    0x00000200L
#define PROCESS_ALL_ACCESS   0x001FFFFFL
#define STANDARD_RIGHTS_ALL  0x001F0000L

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010L)
#define STATUS_NOT_FOUND                 ((NTSTATUS)0xC0000225L)

#define PAGE_READWRITE 0x04
#define MEM_COMMIT     0x1000
#define MEM_RESERVE    0x2000
#define MEM_RELEASE    0x8000

#define FILE_DEVICE_UNKNOWN     0x00000022
#define FILE_DEVICE_SECURE_OPEN 0x00000100
#define DO_BUFFERED_IO          0x00000004
#define DO_DEVICE_INITIALIZING  0x00000080

#define METHOD_BUFFERED   0
#define FILE_ANY_ACCESS   0
#ifndef CTL_CODE
#define CTL_CODE( DeviceType, Function, Method, Access ) ( \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
)
#endif

#define IRP_MJ_CREATE         0x00
#define IRP_MJ_CLOSE          0x02
#define IRP_MJ_DEVICE_CONTROL 0x0e

#define IO_NO_INCREMENT 0
#define KernelMode      0

#define EXCEPTION_EXECUTE_HANDLER 1
#define GetExceptionCode() 0xC0000005

#define UNREFERENCED_PARAMETER(P) (void)(P)

#define CONTAINING_RECORD(address, type, field) ((type *)( \
    (char *)(address) - \
    (ULONG_PTR)(&((type *)0)->field)))

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWCHAR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

typedef struct _KAPC_STATE {
    LIST_ENTRY ApcListHead[2];
    PVOID Process;
    UCHAR KernelApcInProgress;
    UCHAR KernelApcPending;
    UCHAR UserApcPending;
} KAPC_STATE, *PKAPC_STATE;

typedef void *PEPROCESS;
typedef void *PSECURITY_DESCRIPTOR;

typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _IRP IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_INITIALIZE)(
    struct _DRIVER_OBJECT *DriverObject,
    PUNICODE_STRING RegistryPath
);

typedef struct _DEVICE_OBJECT {
    short Type;
    USHORT Size;
    int ReferenceCount;
    struct _DRIVER_OBJECT *DriverObject;
    struct _DEVICE_OBJECT *NextDevice;
    struct _DEVICE_OBJECT *AttachedDevice;
    struct _IRP *CurrentIrp;
    PVOID Timer;
    ULONG Flags;
    ULONG Characteristics;
    PVOID DoNotUse1;
    PVOID DeviceExtension;
    ULONG DeviceType;
    char StackSize;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _DRIVER_OBJECT {
    short Type;
    short Size;
    PDEVICE_OBJECT DeviceObject;
    ULONG Flags;
    PVOID DriverStart;
    ULONG DriverSize;
    PVOID DriverSection;
    PVOID DriverExtension;
    UNICODE_STRING DriverName;
    PUNICODE_STRING HardwareDatabase;
    PVOID FastIoDispatch;
    PVOID DriverInit;
    PVOID DriverStartIo;
    void (*DriverUnload)(struct _DRIVER_OBJECT *DriverObject);
    NTSTATUS (*MajorFunction[28])(struct _DEVICE_OBJECT *DeviceObject, struct _IRP *Irp);
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;
    union {
        struct {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
    } Parameters;
    PDEVICE_OBJECT DeviceObject;
    PVOID FileObject;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
    short Type;
    USHORT Size;
    PVOID MdlAddress;
    ULONG Flags;
    union {
        struct _IRP *MasterIrp;
        int IrpCount;
        PVOID SystemBuffer;
    } AssociatedIrp;
    IO_STATUS_BLOCK IoStatus;
    char RequestorMode;
    BOOLEAN PendingReturned;
    char StackCount;
    char CurrentLocation;
    BOOLEAN Cancel;
    UCHAR CancelIrql;
    char ApcEnvironment;
    UCHAR AllocationFlags;
    PIO_STATUS_BLOCK UserIosb;
    PVOID UserEvent;
    union {
        struct {
            PVOID UserApcRoutine;
            PVOID UserApcContext;
        } AsynchronousParameters;
        ULONG_PTR AllocationSize;
    } Overlay;
    PVOID CancelRoutine;
    PVOID UserBuffer;
    union {
        struct {
            union {
                PVOID DeviceQueueEntry;
                struct {
                    PVOID DriverContext[4];
                };
            };
            PVOID Thread;
            PVOID AuxiliaryBuffer;
            struct {
                LIST_ENTRY ListEntry;
                union {
                    struct _IO_STACK_LOCATION *CurrentStackLocation;
                    ULONG PacketType;
                };
            };
            PVOID OriginalFileObject;
        } Overlay;
    } Tail;
} IRP, *PIRP;

#define IoGetCurrentIrpStackLocation(Irp) ((Irp)->Tail.Overlay.CurrentStackLocation)
#define IoCompleteRequest(Irp, PriorityBoost) IofCompleteRequest(Irp, PriorityBoost)
#define ObDereferenceObject(a) ObfDereferenceObject(a)

#ifdef __cplusplus
extern "C" {
#endif

void RtlInitUnicodeString(PUNICODE_STRING DestinationString, PCWCHAR SourceString);
void RtlCopyMemory(PVOID Destination, const void *Source, SIZE_T Length);
void *memcpy(void *dest, const void *src, size_t count);

NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, PUNICODE_STRING DeviceName, ULONG DeviceType, ULONG DeviceCharacteristics, BOOLEAN Exclusive, PDEVICE_OBJECT *DeviceObject);
NTSTATUS IoCreateDriver(PUNICODE_STRING DriverName, PDRIVER_INITIALIZE InitializationFunction);
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName);
void IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
void IofCompleteRequest(PIRP Irp, char PriorityBoost);

PEPROCESS PsGetCurrentProcess(void);
NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId, PEPROCESS *Process);
LONG_PTR ObfDereferenceObject(PVOID Object);
PVOID PsGetProcessPeb(PEPROCESS Process);

NTSTATUS MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress, PEPROCESS TargetProcess, PVOID TargetAddress, SIZE_T BufferSize, char PreviousMode, PSIZE_T NumberOfBytesCopied);
void KeStackAttachProcess(PEPROCESS Process, PKAPC_STATE ApcState);
void KeUnstackDetachProcess(PKAPC_STATE ApcState);

NTSTATUS ZwAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS ZwFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS ZwProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewAccessProtection, PULONG OldAccessProtection);
NTSTATUS ZwClose(HANDLE Handle);

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

#ifdef __cplusplus
}
#endif
