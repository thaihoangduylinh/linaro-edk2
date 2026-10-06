/** @file
  Dump the raw internal disk exposed by Block I/O to a USB filesystem.

  Dump operations access the source exclusively through ReadBlocks.
  The separate Security Toggle menu launches a user-supplied USB application
  which can change security variables or reset the device.
  Device discovery is deliberately strict:
  a unique known eMMC User device path is preferred over boot-area handles.
  Otherwise a unique non-removable, non-partition, non-USB candidate is required.
**/

#include <Uefi.h>
#include <Guid/FileSystemInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "Screen.h"
#include "Input.h"
#include "Partitions.h"

#define DUMP_BUFFER_SIZE  (4U * 1024U * 1024U)
#define DUMP_PART_SIZE    (1024U * 1024U * 1024U)
#define SPACE_RESERVE     (16U * 1024U * 1024U)
#define FILE_CREATE       (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)

STATIC EFI_FILE_PROTOCOL *mLog;
STATIC UINT32 mCrcTable[256];

STATIC EFI_STATUS
WriteAll (EFI_FILE_PROTOCOL *File, CONST VOID *Buffer, UINTN Size)
{
  EFI_STATUS Status;
  UINTN Written;
  CONST UINT8 *Bytes;

  Bytes = Buffer;
  while (Size != 0) {
    Written = Size;
    Status = File->Write (File, &Written, (VOID *)Bytes);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Written == 0 || Written > Size) {
      return EFI_DEVICE_ERROR;
    }
    Bytes += Written;
    Size -= Written;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Log (CONST CHAR8 *Format, ...)
{
  CHAR8 Text[2048];
  VA_LIST Args;
  EFI_STATUS Status;
  UINTN Length;

  VA_START (Args, Format);
  Length = AsciiVSPrint (Text, sizeof (Text), Format, Args);
  VA_END (Args);
  ScreenWrite (Text);
  if (mLog == NULL) {
    return EFI_SUCCESS;
  }
  Status = WriteAll (mLog, Text, Length);
  if (!EFI_ERROR (Status)) {
    Status = mLog->Flush (mLog);
  }
  return Status;
}

STATIC BOOLEAN
IsUsbPath (EFI_DEVICE_PATH_PROTOCOL *Path)
{
  EFI_DEVICE_PATH_PROTOCOL *Node;
  UINT8 SubType;

  if (Path == NULL || !IsDevicePathValid (Path, 0)) {
    return FALSE;
  }
  for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    SubType = DevicePathSubType (Node);
    if (DevicePathType (Node) == MESSAGING_DEVICE_PATH &&
        (SubType == MSG_USB_DP || SubType == MSG_USB_CLASS_DP ||
         SubType == MSG_USB_WWID_DP)) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN
PathPrefix (EFI_DEVICE_PATH_PROTOCOL *Parent, EFI_DEVICE_PATH_PROTOCOL *Child)
{
  UINTN Size;

  if (Parent == NULL || Child == NULL ||
      !IsDevicePathValid (Parent, 0) || !IsDevicePathValid (Child, 0)) {
    return FALSE;
  }
  Size = GetDevicePathSize (Parent) - END_DEVICE_PATH_LENGTH;
  return (BOOLEAN)(Size != 0 && GetDevicePathSize (Child) >= Size &&
                   CompareMem (Parent, Child, Size) == 0);
}

STATIC EFI_STATUS
LogPath (CONST CHAR8 *Label, EFI_DEVICE_PATH_PROTOCOL *Path)
{
  CHAR16 *Text;
  EFI_STATUS Status;

  Text = ConvertDevicePathToText (Path, FALSE, FALSE);
  Status = Log ("%a=%s\r\n", Label, Text == NULL ? L"unavailable" : Text);
  if (Text != NULL) {
    FreePool (Text);
  }
  return Status;
}

STATIC EFI_STATUS
VolumeInfo (EFI_FILE_PROTOCOL *Root, EFI_FILE_SYSTEM_INFO **Info)
{
  EFI_STATUS Status;
  UINTN Size;

  *Info = NULL;
  Size = 0;
  Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || Size < SIZE_OF_EFI_FILE_SYSTEM_INFO ||
      Size > 65536) {
    return EFI_DEVICE_ERROR;
  }
  *Info = AllocateZeroPool (Size);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, *Info);
  if (EFI_ERROR (Status)) {
    FreePool (*Info);
    *Info = NULL;
  }
  return Status;
}

// Prefer the USB filesystem from which this image was loaded. Otherwise require
// exactly one USB filesystem. RemovableMedia alone does not identify USB.
STATIC EFI_STATUS
FindDestination (EFI_HANDLE Image, EFI_HANDLE *Destination, EFI_FILE_PROTOCOL **Root)
{
  EFI_HANDLE *Handles;
  EFI_HANDLE LoadedDevice;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_STATUS Status;
  UINTN Count;
  UINTN Matches;
  UINTN Index;

  *Destination = NULL;
  *Root = NULL;
  LoadedDevice = NULL;
  Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (!EFI_ERROR (Status)) {
    LoadedDevice = Loaded->DeviceHandle;
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                   NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Matches = 0;
  for (Index = 0; Index < Count; Index++) {
    if (!IsUsbPath (DevicePathFromHandle (Handles[Index]))) {
      continue;
    }
    *Destination = Handles[Index];
    Matches++;
    if (Handles[Index] == LoadedDevice) {
      Matches = 1;
      break;
    }
  }
  FreePool (Handles);
  if (Matches != 1) {
    Log ("USB filesystem candidates=%d; expected exactly one (or image's USB volume).\r\n",
         Matches);
    return EFI_NOT_FOUND;
  }
  Status = gBS->HandleProtocol (*Destination, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Fs->OpenVolume (Fs, Root);
}

// Vendor paths used by the phone firmware. The User GUID also appears in
// QcomPkg/Msm8960Pkg/Dxe/MMCHSDxe/MMCHS.c. Match a complete single-node path;
// a partition or descendant of the user-area handle is not the raw device.
// See https://github.com/MobileTooling/img2ffu#samples for the phone path map.
STATIC UINTN
EmmcArea (EFI_DEVICE_PATH_PROTOCOL *Path)
{
  STATIC CONST EFI_GUID Areas[] = {
    {0xb615f1f5, 0x5088, 0x43cd, {0x80,0x9c,0xa1,0x6e,0x52,0x48,0x7d,0x00}},
    {0x12c55b20, 0x25d3, 0x41c9, {0x8e,0x06,0x28,0x2d,0x94,0xc6,0x76,0xad}},
    {0x6b76a6db, 0x0257, 0x48a9, {0xaa,0x99,0xf6,0xb1,0x65,0x5f,0x7b,0x00}},
    {0xc49551ea, 0xd6bc, 0x4966, {0x94,0x99,0x87,0x1e,0x39,0x31,0x33,0xcd}}
  };
  UINTN Index;

  if (DevicePathType (Path) == HARDWARE_DEVICE_PATH &&
      DevicePathSubType (Path) == HW_VENDOR_DP &&
      DevicePathNodeLength (Path) == sizeof (VENDOR_DEVICE_PATH) &&
      IsDevicePathEnd (NextDevicePathNode (Path))) {
    for (Index = 0; Index < sizeof (Areas) / sizeof (Areas[0]); Index++) {
      if (CompareMem (&((VENDOR_DEVICE_PATH *)Path)->Guid, &Areas[Index], sizeof (EFI_GUID)) == 0) {
        return Index + 1; // User=1, Boot1=2, Boot2=3, RPMB=4
      }
    }
  }
  return 0;
}

STATIC EFI_STATUS
FindSource (EFI_HANDLE Destination, EFI_BLOCK_IO_PROTOCOL **Source, EFI_HANDLE *SourceHandle)
{
  EFI_HANDLE *Handles;
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_DEVICE_PATH_PROTOCOL *DestPath;
  EFI_STATUS Status;
  UINTN Count;
  UINTN Index;
  UINTN Matches;
  UINTN UserMatches;
  UINTN Area;
  EFI_BLOCK_IO_PROTOCOL *UserIo;
  EFI_HANDLE UserHandle;

  *Source = NULL;
  *SourceHandle = NULL;
  DestPath = DevicePathFromHandle (Destination);
  Matches = 0;
  UserMatches = 0;
  UserIo = NULL;
  UserHandle = NULL;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                   NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Status) || Io->Media == NULL) {
      continue;
    }
    Path = DevicePathFromHandle (Handles[Index]);
    if (!Io->Media->MediaPresent || Io->Media->LogicalPartition ||
        Io->Media->RemovableMedia || Path == NULL || !IsDevicePathValid (Path, 0) ||
        IsUsbPath (Path) || Handles[Index] == Destination ||
        PathPrefix (Path, DestPath) || PathPrefix (DestPath, Path)) {
      continue;
    }
    Area = EmmcArea (Path);
    Status = Log ("device_index=%d area=%a block_size=%d last_lba=%Ld media_id=%08x\r\n",
                  (UINT32)Index,
                  Area == 1 ? "User" : Area == 2 ? "Boot1" : Area == 3 ? "Boot2" :
                  Area == 4 ? "RPMB" : "Unknown",
                  Io->Media->BlockSize, Io->Media->LastBlock, Io->Media->MediaId);
    if (!EFI_ERROR (Status)) {
      Status = LogPath ("candidate_path", Path);
    }
    if (EFI_ERROR (Status)) {
      FreePool (Handles);
      return Status;
    }
    if (Area >= 2) {
      continue; // Do not mistake a boot area or RPMB for the user-area disk.
    }
    Matches++;
    *Source = Io;
    *SourceHandle = Handles[Index];
    if (Area == 1) {
      UserMatches++;
      UserIo = Io;
      UserHandle = Handles[Index];
    }
  }
  FreePool (Handles);
  if (UserMatches == 1) {
    *Source = UserIo;
    *SourceHandle = UserHandle;
    return Log ("selection=known_eMMC_User_GUID (Boot1/Boot2/RPMB excluded)\r\n");
  }
  if (Matches != 1) {
    Log ("Source candidates=%d; cannot identify a unique raw internal disk.\r\n", Matches);
    *Source = NULL;
    return EFI_NOT_FOUND;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
NewDirectory (EFI_FILE_PROTOCOL *Root, EFI_FILE_PROTOCOL **Directory)
{
  CHAR16 Name[32];
  EFI_FILE_PROTOCOL *Existing;
  EFI_STATUS Status;
  UINTN Index;

  for (Index = 0; Index < 10000; Index++) {
    UnicodeSPrint (Name, sizeof (Name), L"EmmcDump-%04d", Index);
    Status = Root->Open (Root, &Existing, Name, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      Existing->Close (Existing);
      continue;
    }
    if (Status != EFI_NOT_FOUND) {
      return Status;
    }
    Status = Root->Open (Root, Directory, Name, FILE_CREATE, EFI_FILE_DIRECTORY);
    if (!EFI_ERROR (Status)) {
      Log ("Output directory: \\%s\r\n", Name);
    }
    return Status;
  }
  return EFI_VOLUME_FULL;
}

STATIC VOID
InitCrc (VOID)
{
  UINT32 Index;
  UINT32 Bit;
  UINT32 Crc;

  for (Index = 0; Index < 256; Index++) {
    Crc = Index;
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xEDB88320U : 0);
    }
    mCrcTable[Index] = Crc;
  }
}

STATIC UINT32
UpdateCrc (UINT32 Crc, CONST UINT8 *Data, UINTN Size)
{
  while (Size-- != 0) {
    Crc = mCrcTable[(Crc ^ *Data++) & 0xFF] ^ (Crc >> 8);
  }
  return Crc;
}

STATIC BOOLEAN
Cancelled (VOID)
{
  EFI_INPUT_KEY Key;

  if (gST->ConIn == NULL) {
    return FALSE;
  }
  if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
    return (BOOLEAN)(Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1B);
  }
  return FALSE;
}

STATIC EFI_STATUS
DumpRange (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, EFI_LBA Start, UINT64 Total)
{
  EFI_BLOCK_IO_MEDIA Media;
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  EFI_FILE_PROTOCOL *File;
  VOID *Buffer;
  UINTN BufferSize;
  UINTN Pages;
  UINTN Alignment;
  UINTN Chunk;
  UINTN PartBytes;
  UINTN PartLimit;
  UINT32 Part;
  UINT32 Crc;
  UINT64 Done;
  UINT64 Remaining;
  UINT64 NextProgress;
  EFI_LBA Lba;
  CHAR16 Name[32];
  UINT64 Blocks;
  UINT32 Remainder;

  // Revision 1 firmware need not allocate the optional revision 2/3 fields.
  ZeroMem (&Media, sizeof (Media));
  Media.MediaId = Io->Media->MediaId;
  Media.BlockSize = Io->Media->BlockSize;
  Media.IoAlign = Io->Media->IoAlign;
  Media.LastBlock = Io->Media->LastBlock;
  // Buffer and file boundaries must be whole sectors, including the final read.
  if (Media.BlockSize == 0 || Media.BlockSize > DUMP_BUFFER_SIZE ||
      (Media.IoAlign > 1 && (Media.IoAlign & (Media.IoAlign - 1)) != 0) ||
      Media.IoAlign > DUMP_BUFFER_SIZE) {
    return EFI_UNSUPPORTED;
  }
  Blocks = DivU64x32Remainder (Total, Media.BlockSize, &Remainder);
  if (Media.LastBlock == MAX_UINT64 || Total == 0 || Remainder != 0 ||
      Start > Media.LastBlock || Blocks > Media.LastBlock - Start + 1) {
    return EFI_INVALID_PARAMETER;
  }
  Alignment = MAX ((UINTN)Media.IoAlign, (UINTN)EFI_PAGE_SIZE);
  BufferSize = DUMP_BUFFER_SIZE;
  Buffer = NULL;
  Pages = 0;
  while (BufferSize >= Media.BlockSize) {
    BufferSize -= BufferSize % Media.BlockSize;
    Pages = EFI_SIZE_TO_PAGES (BufferSize);
    Buffer = AllocateAlignedPages (Pages, Alignment);
    if (Buffer != NULL) {
      break;
    }
    BufferSize /= 2;
  }
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  PartLimit = DUMP_PART_SIZE - (DUMP_PART_SIZE % Media.BlockSize);
  File = NULL;
  Done = 0;
  Lba = Start;
  Part = 0;
  NextProgress = 0;
  InitCrc ();
  Status = EFI_SUCCESS;
  while (Done < Total) {
    UnicodeSPrint (Name, sizeof (Name), L"emmc-%04d.bin", Part);
    Status = Directory->Open (Directory, &File, Name, FILE_CREATE, 0);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    PartBytes = 0;
    Crc = 0xFFFFFFFFU;
    while (PartBytes < PartLimit && Done < Total) {
      if (Cancelled ()) {
        Status = EFI_ABORTED;
        goto Exit;
      }
      if (!Io->Media->MediaPresent || Io->Media->MediaId != Media.MediaId ||
          Io->Media->IoAlign != Media.IoAlign ||
          Io->Media->BlockSize != Media.BlockSize || Io->Media->LastBlock != Media.LastBlock) {
        Status = EFI_MEDIA_CHANGED;
        goto Exit;
      }
      Chunk = MIN (BufferSize, PartLimit - PartBytes);
      Remaining = Total - Done;
      if (Remaining < Chunk) {
        Chunk = (UINTN)Remaining;
      }
      Status = Io->ReadBlocks (Io, Media.MediaId, Lba, Chunk, Buffer);
      if (EFI_ERROR (Status)) {
        goto Exit;
      }
      Status = WriteAll (File, Buffer, Chunk);
      if (EFI_ERROR (Status)) {
        goto Exit;
      }
      Crc = UpdateCrc (Crc, Buffer, Chunk);
      Done += Chunk;
      PartBytes += Chunk;
      Lba += Chunk / Media.BlockSize;
      if (Done >= NextProgress || Done == Total) {
        ScreenProgress (Done, Total);
        NextProgress = Done + 64U * 1024U * 1024U;
      }
    }
    Status = File->Flush (File);
    CloseStatus = File->Close (File);
    File = NULL;
    if (!EFI_ERROR (Status)) {
      Status = CloseStatus;
    }
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    Status = Log ("\r\npart=emmc-%04d.bin bytes=%d crc32=%08x\r\n",
                  Part, PartBytes, Crc ^ 0xFFFFFFFFU);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    Part++;
  }
  Status = Directory->Flush (Directory);
  if (!EFI_ERROR (Status)) {
    Status = Log ("COMPLETE bytes=%Ld parts=%d\r\n", Done, Part);
  }

Exit:
  if (File != NULL) {
    File->Flush (File);
    File->Close (File);
  }
  if (EFI_ERROR (Status)) {
    Log ("\r\nINCOMPLETE status=%r next_lba=%Ld bytes_in_full_chunks=%Ld\r\n",
         Status, Lba, Done);
  }
  FreeAlignedPages (Buffer, Pages);
  return Status;
}

STATIC EFI_STATUS
DumpPartitions (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, PARTITION_PLAN *Plan)
{
  EFI_FILE_PROTOCOL *ParentLog;
  EFI_FILE_PROTOCOL *PartDirectory;
  EFI_FILE_PROTOCOL *PartLog;
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  UINTN Index;
  UINTN Ch;
  CHAR16 SafeName[37];
  CHAR16 DirectoryName[64];

  ParentLog = mLog;
  for (Index = 0; Index < Plan->Count; Index++) {
    if (!Io->Media->MediaPresent || Io->Media->MediaId != Plan->MediaId ||
        Io->Media->BlockSize != Plan->BlockSize || Io->Media->LastBlock != Plan->LastBlock) {
      return EFI_MEDIA_CHANGED;
    }
    ZeroMem (SafeName, sizeof (SafeName));
    for (Ch = 0; Ch < 36 && Plan->Part[Index].Name[Ch] != 0; Ch++) {
      SafeName[Ch] = Plan->Part[Index].Name[Ch];
      if (!((SafeName[Ch] >= L'A' && SafeName[Ch] <= L'Z') ||
            (SafeName[Ch] >= L'a' && SafeName[Ch] <= L'z') ||
            (SafeName[Ch] >= L'0' && SafeName[Ch] <= L'9') ||
            SafeName[Ch] == L'_' || SafeName[Ch] == L'-')) {
        SafeName[Ch] = L'_';
      }
    }
    UnicodeSPrint (DirectoryName, sizeof (DirectoryName), L"p%04d-%s", (UINT32)Index, SafeName);
    Status = Log ("partition_dir=%s start_lba=%Ld end_lba=%Ld bytes=%Ld name=%s\r\n",
                  DirectoryName, Plan->Part[Index].Start, Plan->Part[Index].End,
                  Plan->Part[Index].Bytes, Plan->Part[Index].Name);
    if (EFI_ERROR (Status)) { return Status; }
    Status = Directory->Open (Directory, &PartDirectory, DirectoryName, FILE_CREATE, EFI_FILE_DIRECTORY);
    if (EFI_ERROR (Status)) { return Status; }
    Status = PartDirectory->Open (PartDirectory, &PartLog, L"manifest.txt", FILE_CREATE, 0);
    if (EFI_ERROR (Status)) {
      PartDirectory->Close (PartDirectory);
      return Status;
    }
    mLog = PartLog;
    Status = Log ("EmmcDump format=1\r\nScope=%a\r\n"
                  "partition_name=%s source_start_lba=%Ld source_end_lba=%Ld\r\n"
                  "total_bytes=%Ld block_size=%d last_lba=%Ld\r\n",
                  Plan->IsGpt ? "GPT metadata region (not a partition or full disk image)" : "single GPT partition (not a full disk image)",
                  Plan->Part[Index].Name, Plan->Part[Index].Start, Plan->Part[Index].End,
                  Plan->Part[Index].Bytes, Plan->BlockSize,
                  Plan->Part[Index].End - Plan->Part[Index].Start);
    if (!EFI_ERROR (Status)) {
      Status = DumpRange (Io, PartDirectory, Plan->Part[Index].Start, Plan->Part[Index].Bytes);
    }
    if (EFI_ERROR (Status)) { Log ("FAILED partition=%s status=%r\r\n", Plan->Part[Index].Name, Status); }
    CloseStatus = PartLog->Close (PartLog);
    mLog = ParentLog;
    if (!EFI_ERROR (Status)) { Status = CloseStatus; }
    CloseStatus = PartDirectory->Close (PartDirectory);
    if (!EFI_ERROR (Status)) { Status = CloseStatus; }
    if (EFI_ERROR (Status)) { return Status; }
  }
  Status = Directory->Flush (Directory);
  if (EFI_ERROR (Status)) { return Status; }
  return Log ("PARTITION_SET_COMPLETE bytes=%Ld partitions=%d\r\n", Plan->Total, (UINT32)Plan->Count);
}

STATIC EFI_STATUS
RunDump (EFI_HANDLE ImageHandle, UINTN Mode, CONST PARTITION_PLAN *MenuPlan,
         UINTN Selected, EFI_HANDLE ExpectedSource)
{
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  EFI_HANDLE Destination;
  EFI_HANDLE SourceHandle;
  EFI_BLOCK_IO_PROTOCOL *Source;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *Directory;
  EFI_FILE_SYSTEM_INFO *Info;
  UINT64 Total;
  UINT64 Blocks;
  CHAR8 Display[192];
  PARTITION_PLAN *Plan;

  Root = NULL;
  Directory = NULL;
  Info = NULL;
  mLog = NULL;
  Plan = NULL;
  Status = FindDestination (ImageHandle, &Destination, &Root);
  if (EFI_ERROR (Status)) {
    Log ("No unambiguous USB filesystem: %r\r\n", Status);
    goto Exit;
  }
  Status = VolumeInfo (Root, &Info);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  if (Info->ReadOnly) {
    Status = EFI_WRITE_PROTECTED;
    goto Exit;
  }
  Status = NewDirectory (Root, &Directory);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  Status = Directory->Open (Directory, &mLog, L"manifest.txt", FILE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  Status = Log ("EmmcDump format=1\r\nScope=raw BlockIO device; boot0/boot1/RPMB not included unless exposed separately.\r\n");
  if (!EFI_ERROR (Status)) {
    ScreenDescription (Display, sizeof (Display));
    Status = Log ("%a\r\n", Display);
  }
  if (!EFI_ERROR (Status)) {
    Status = LogPath ("destination_path", DevicePathFromHandle (Destination));
  }
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  Status = Log ("mode=%a\r\n", Mode == 0 ? "full" : (Mode == 1 ? "partitions" : "gpt"));
  if (EFI_ERROR (Status)) { goto Exit; }
  Status = FindSource (Destination, &Source, &SourceHandle);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  if (Source->Media->BlockSize == 0 || Source->Media->LastBlock == MAX_UINT64) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }
  Blocks = Source->Media->LastBlock + 1;
  if (Blocks > DivU64x32 (MAX_INT64 - SPACE_RESERVE, Source->Media->BlockSize)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }
  Total = MultU64x32 (Blocks, Source->Media->BlockSize);
  if (Mode != 0) {
    Status = Mode == MENU_PARTITIONS ? LoadPartitionPlan (NULL, Source, &Plan, Log) : LoadGptPlan (Source, &Plan, Log);
    if (EFI_ERROR (Status)) { goto Exit; }
    if (Mode == MENU_PARTITIONS) {
      if (MenuPlan == NULL || SourceHandle != ExpectedSource ||
          Plan->MediaId != MenuPlan->MediaId || Plan->BlockSize != MenuPlan->BlockSize ||
          Plan->LastBlock != MenuPlan->LastBlock || Plan->Count != MenuPlan->Count ||
          Selected >= Plan->Count ||
          CompareMem (&Plan->Part[Selected], &MenuPlan->Part[Selected], sizeof (DUMP_PARTITION)) != 0) {
        Log ("Source/GPT changed. Reopen the partition menu.\r\n");
        Status = EFI_MEDIA_CHANGED;
        goto Exit;
      }
      Plan->Part[0] = Plan->Part[Selected];
      Plan->Count = 1;
      Plan->Total = Plan->Part[0].Bytes;
    }
    Total = Plan->Total;
  }
  // Refresh after reading and validating GPT.
  FreePool (Info);
  Info = NULL;
  Status = VolumeInfo (Root, &Info);
  if (EFI_ERROR (Status)) { goto Exit; }
  // Includes slack for directory entries, manifest and filesystem allocation.
  if (Info->FreeSpace < SPACE_RESERVE || Total > Info->FreeSpace - SPACE_RESERVE) {
    Log ("Insufficient USB space: dump=%Ld free=%Ld reserve=%d\r\n",
         Total, Info->FreeSpace, SPACE_RESERVE);
		 
	Log ("USB SPACE INSUFFICIENT\r\n");
	Log ("Dump size : %Ld bytes\r\n", Total);
	Log ("USB free  : %Ld bytes\r\n", Info->FreeSpace);
	Log ("Required  : %Ld bytes\r\n", Total + SPACE_RESERVE);
	Log ("Missing   : %Ld bytes\r\n", Total + SPACE_RESERVE - Info->FreeSpace);
    Log ("Use a larger USB or select fewer partitions.\r\n");
    Status = EFI_VOLUME_FULL;
    goto Exit;
  }
  Status = LogPath ("source_path", DevicePathFromHandle (SourceHandle));
  if (!EFI_ERROR (Status)) {
    if (Mode == 0) {
      Status = Log ("total_bytes=%Ld block_size=%d last_lba=%Ld\r\n",
                    Total, Source->Media->BlockSize, Source->Media->LastBlock);
    } else {
      Status = Log ("partition_set block_size=%d last_lba=%Ld\r\n",
                    Plan->BlockSize, Plan->LastBlock);
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = Mode == 0 ? DumpRange (Source, Directory, 0, Total) : DumpPartitions (Source, Directory, Plan);
  }

Exit:
  ScreenDescription (Display, sizeof (Display));
  Log ("%a\r\n", Display);
  if (EFI_ERROR (Status)) {
    Log ("FAILED status=%r; partial files must not be treated as a full dump.\r\n", Status);
  }
  if (mLog != NULL) {
    CloseStatus = mLog->Close (mLog);
    mLog = NULL;
    if (!EFI_ERROR (Status)) {
      Status = CloseStatus;
    }
  }
  if (Directory != NULL) {
    CloseStatus = Directory->Close (Directory);
    if (!EFI_ERROR (Status)) {
      Status = CloseStatus;
    }
  }
  if (Root != NULL) {
    Root->Close (Root);
  }
  if (Info != NULL) {
    FreePool (Info);
  }
  if (Plan != NULL) { FreePool (Plan); }
  Log ("EmmcDump finished: %r\r\n", Status);
  return Status;
}

STATIC EFI_STATUS
PartitionMenu (EFI_HANDLE ImageHandle)
{
  EFI_HANDLE Destination;
  EFI_HANDLE SourceHandle;
  EFI_FILE_PROTOCOL *Root;
  EFI_BLOCK_IO_PROTOCOL *Source;
  PARTITION_PLAN *Plan;
  EFI_STATUS Status;
  UINTN Selected;

  Root = NULL;
  Plan = NULL;
  Status = FindDestination (ImageHandle, &Destination, &Root);
  if (EFI_ERROR (Status)) { goto Exit; }
  Root->Close (Root);
  Root = NULL;
  Status = FindSource (Destination, &Source, &SourceHandle);
  if (EFI_ERROR (Status)) { goto Exit; }
  Status = LoadPartitionPlan (NULL, Source, &Plan, Log);
  if (EFI_ERROR (Status)) { goto Exit; }
  Selected = 0;
  for (;;) {
    Status = ChoosePartition (Plan, &Selected);
    if (EFI_ERROR (Status) || Selected == Plan->Count) { break; }
    RunDump (ImageHandle, MENU_PARTITIONS, Plan, Selected, SourceHandle);
    ScreenWrite ("\r\nReturning to partition menu in 5 seconds...\r\n");
    gBS->Stall (5000000);
  }
Exit:
  if (Root != NULL) { Root->Close (Root); }
  if (Plan != NULL) { FreePool (Plan); }
  if (EFI_ERROR (Status)) {
    Log ("Partition menu failed: %r\r\n", Status);
    gBS->Stall (5000000);
  }
  return Status;
}

STATIC EFI_STATUS
LaunchUsbApp (EFI_HANDLE ImageHandle, CONST CHAR16 *FileName,
              CONST CHAR16 *Arguments, UINT32 ArgumentsSize)
{
  EFI_HANDLE Destination;
  EFI_HANDLE Child;
  EFI_FILE_PROTOCOL *Root;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_STATUS Status;
  EFI_STATUS WatchdogStatus;
  UINTN ExitDataSize;
  CHAR16 *ExitData;
  VOID *Options;

  if (ArgumentsSize != 0 && Arguments == NULL) { return EFI_INVALID_PARAMETER; }
  Root = NULL;
  Child = NULL;
  Status = FindDestination (ImageHandle, &Destination, &Root);
  if (EFI_ERROR (Status)) {
    Log ("Cannot find USB for %s: %r\r\n", FileName, Status);
    return Status;
  }
  Root->Close (Root);
  Path = FileDevicePath (Destination, FileName);
  if (Path == NULL) { return EFI_OUT_OF_RESOURCES; }
  Log ("Loading USB:%s args=%s\r\n", FileName, ArgumentsSize != 0 ? Arguments : L"(none)");
  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Child);
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    // LoadImage can return a handle even when authentication rejects the image.
    if (Child != NULL) { gBS->UnloadImage (Child); }
    Log ("LoadImage failed: %r\r\n", Status);
    Log ("Place %s in the USB root.\r\n"
         "The firmware must allow this ARM EFI application to run.\r\n", FileName);
    return Status;
  }
  Status = gBS->HandleProtocol (Child, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status) || Loaded->ImageCodeType != EfiLoaderCode) {
    gBS->UnloadImage (Child);
    Log ("%s must be a UEFI application.\r\n", FileName);
    return EFI_UNSUPPORTED;
  }
  Options = NULL;
  if (ArgumentsSize != 0) {
    Options = AllocateZeroPool (ArgumentsSize);
    if (Options == NULL) {
      gBS->UnloadImage (Child);
      Log ("Cannot allocate application arguments.\r\n");
      return EFI_OUT_OF_RESOURCES;
    }
    // Pass exactly the Arg string as UTF-16, including its terminating NUL.
    // LoadOptionsSize is a byte count; no executable name or shell quotes.
    CopyMem (Options, Arguments, ArgumentsSize);
  }
  Loaded->LoadOptions = Options;
  Loaded->LoadOptionsSize = ArgumentsSize;
  Log ("%s controls the next screen and operation.\r\n", FileName);
  gBS->Stall (2000000);
  ExitData = NULL;
  ExitDataSize = 0;
  ScreenRelease ();
  Status = gBS->StartImage (Child, &ExitDataSize, &ExitData);
  // An application that exits is unloaded by firmware. If StartImage was
  // rejected before entry, its LoadedImage handle can still be present.
  if (!EFI_ERROR (gBS->HandleProtocol (Child, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded))) {
    Loaded->LoadOptions = NULL;
    Loaded->LoadOptionsSize = 0;
    gBS->UnloadImage (Child);
  }
  if (Options != NULL) { FreePool (Options); }
  if (ExitData != NULL) { FreePool (ExitData); }
  ScreenInit ();
  WatchdogStatus = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  if (EFI_ERROR (WatchdogStatus)) {
    Log ("Cannot disable watchdog after child application: %r\r\n", WatchdogStatus);
  }
  Log ("%s returned: %r\r\n", FileName, Status);
  return Status;
}

EFI_STATUS EFIAPI
UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_HANDLE Destination;
  EFI_FILE_PROTOCOL *Root;
  UINTN Mode;

  (VOID)SystemTable;
  Root = NULL;
  ScreenInit ();
  // Disable the watchdog for transfers and indefinite waits at the menu.
  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  if (EFI_ERROR (Status)) { goto Exit; }
  Status = FindDestination (ImageHandle, &Destination, &Root);
  if (EFI_ERROR (Status)) { goto Exit; }
  for (;;) {
    Status = ChooseDumpMode (Root, &Mode);
    if (Root != NULL) { Root->Close (Root); Root = NULL; }
    if (EFI_ERROR (Status)) { goto Exit; }
    if (Mode == MENU_EXIT) {
      ScreenWrite ("Exiting EmmcDump...\r\n");
      ScreenRelease ();
      return EFI_SUCCESS;
    }
    // Reopen the USB and source each time; a failed job must not end the menu.
    if (Mode == MENU_SHUTDOWN) {
      // No dump files remain open when control is back at the main menu.
      ScreenWrite ("Shutting down...\r\n");
      if (gST->RuntimeServices != NULL && gST->RuntimeServices->ResetSystem != NULL) {
        gST->RuntimeServices->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        // ResetSystem should not return after a successful shutdown.
        Log ("Firmware returned without shutting down.\r\n");
      } else {
        Log ("Firmware shutdown service is unavailable.\r\n");
      }
    } else if (Mode == MENU_PARTITIONS) {
      PartitionMenu (ImageHandle);
      continue;
    } else if (Mode == MENU_SECURITY) {
      LaunchUsbApp (ImageHandle, L"\\SecurityToggleApp.efi",
                    L"/SecureBootDisable", sizeof (L"/SecureBootDisable"));
      Log ("Return status alone does not confirm Secure Boot was disabled.\r\n");
    } else if (Mode == MENU_MASS_STORAGE) {
      LaunchUsbApp (ImageHandle, L"\\Cmd.efi", L"MassStorage", sizeof (L"MassStorage"));
    } else if (Mode == MENU_DIAG) {
      LaunchUsbApp (ImageHandle, L"\\DIAG.efi", NULL, 0);
    } else {
      RunDump (ImageHandle, Mode, NULL, 0, NULL);
    }
    ScreenWrite ("\r\nReturning to menu in 5 seconds...\r\n");
    gBS->Stall (5000000);
  }
Exit:
  if (Root != NULL) { Root->Close (Root); }
  Log ("Cannot start/continue menu: %r\r\n", Status);
  gBS->Stall (5000000);
  ScreenRelease ();
  return Status;
}
