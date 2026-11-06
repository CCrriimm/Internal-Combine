#pragma once

#ifdef _KERNEL_MODE
#if defined(__has_include) && __has_include(<ntddk.h>)
#include <ntddk.h>
#else
#include "nt_standalone.h"
#endif
#else
#include <windows.h>
#endif

#define BSK_DEVICE_NAME   L"\\Device\\WinSyncDrv"
#define BSK_SYMLINK_NAME  L"\\DosDevices\\WinSyncDrv"
#define BSK_WIN32_NAME    L"\\\\.\\WinSyncDrv"

#define BSK_DEVICE_TYPE 0x8A31

#define IOCTL_BSK_READ    CTL_CODE(BSK_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BSK_WRITE   CTL_CODE(BSK_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BSK_ALLOC   CTL_CODE(BSK_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BSK_FREE    CTL_CODE(BSK_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BSK_PROTECT CTL_CODE(BSK_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BSK_THREAD  CTL_CODE(BSK_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_BSK_MODULE  CTL_CODE(BSK_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
typedef struct _BSK_REQ {
    unsigned int       pid;
    unsigned int       size;
    unsigned int       prot;
    unsigned int       flags;
    unsigned long long addr;
    unsigned long long param;
    unsigned long long out1;
    unsigned long long out2;
    unsigned short     name[128];
} BSK_REQ;
#pragma pack(pop)

#define BSK_FLAG_RESERVE 0x00000001u
