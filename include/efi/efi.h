/* Minimal UEFI types for freestanding x86_64 Helix boot.
 * Not a full EDK2/gnu-efi replacement — only what M0/M1 need.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint64_t  UINTN;
typedef int64_t   INTN;
typedef uint64_t  EFI_STATUS;
typedef uint8_t   BOOLEAN;
typedef uint16_t  CHAR16;
typedef void      *EFI_HANDLE;
typedef void      *EFI_EVENT;
typedef uint64_t  EFI_PHYSICAL_ADDRESS;
typedef uint64_t  EFI_VIRTUAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))

#define TRUE  1
#define FALSE 0

#define EFI_SUCCESS               0
#define EFI_ERROR_MASK            ((EFI_STATUS)1u << 63)
#define EFI_ERR(x)                (EFI_ERROR_MASK | (x))
#define EFI_LOAD_ERROR            EFI_ERR(1)
#define EFI_INVALID_PARAMETER     EFI_ERR(2)
#define EFI_UNSUPPORTED           EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE       EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL      EFI_ERR(5)
#define EFI_NOT_READY             EFI_ERR(6)
#define EFI_DEVICE_ERROR          EFI_ERR(7)
#define EFI_WRITE_PROTECTED       EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES      EFI_ERR(9)
#define EFI_NOT_FOUND             EFI_ERR(14)

#define EFI_ERROR(s)              (((INTN)(s)) < 0)

/* Memory types (UEFI Spec) */
typedef enum {
    EfiReservedMemoryType       = 0,
    EfiLoaderCode               = 1,
    EfiLoaderData               = 2,
    EfiBootServicesCode         = 3,
    EfiBootServicesData         = 4,
    EfiRuntimeServicesCode      = 5,
    EfiRuntimeServicesData      = 6,
    EfiConventionalMemory       = 7,
    EfiUnusableMemory           = 8,
    EfiACPIReclaimMemory        = 9,
    EfiACPIMemoryNVS            = 10,
    EfiMemoryMappedIO           = 11,
    EfiMemoryMappedIOPortSpace  = 12,
    EfiPalCode                  = 13,
    EfiPersistentMemory         = 14,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress
} EFI_ALLOCATE_TYPE;

/* Descriptor size is given by GetMemoryMap; fields are at fixed offsets
 * with 4 bytes padding after Type on x86_64. */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_TABLE_HEADER {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL  EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct EFI_SYSTEM_TABLE                EFI_SYSTEM_TABLE;
typedef struct EFI_BOOT_SERVICES               EFI_BOOT_SERVICES;
typedef struct EFI_RUNTIME_SERVICES            EFI_RUNTIME_SERVICES;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void *Reset;
    void *ReadKeyStroke;
    EFI_EVENT WaitForKey;
};

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    UINTN Pages);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    uint32_t *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType,
    UINTN Size,
    void **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINTN MapKey);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout,
    uint64_t WatchdogCode,
    UINTN DataSize,
    CHAR16 *WatchdogData);

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STALL Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
};

struct EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER Hdr;
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
};
