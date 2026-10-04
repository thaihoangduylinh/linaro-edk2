/** @file
  Dump the raw internal disk exposed by Block I/O to a USB filesystem.

  Source access is exclusively ReadBlocks. No source writes, partition changes,
  resets or vendor commands are issued. Device discovery is deliberately strict:
  a unique non-removable, non-partition, non-USB disk is required. This is a
  candidate for the eMMC user area, not proof of the physical storage type.
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
  if (gST->ConOut != NULL) {
    Print (L"%a", Text);
  }
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
    Log ("USB filesystem candidates=%u; expected exactly one (or image's USB volume).\r\n",
         Matches);
    return EFI_NOT_FOUND;
  }
  Status = gBS->HandleProtocol (*Destination, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Fs->OpenVolume (Fs, Root);
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

  *Source = NULL;
  *SourceHandle = NULL;
  DestPath = DevicePathFromHandle (Destination);
  Matches = 0;
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
    Matches++;
    *Source = Io;
    *SourceHandle = Handles[Index];
    Status = Log ("candidate=%u block_size=%u last_lba=%Lu\r\n",
                  Matches, Io->Media->BlockSize, Io->Media->LastBlock);
    if (!EFI_ERROR (Status)) {
      Status = LogPath ("candidate_path", Path);
    }
    if (EFI_ERROR (Status)) {
      FreePool (Handles);
      return Status;
    }
  }
  FreePool (Handles);
  if (Matches != 1) {
    Log ("Source candidates=%u; cannot identify a unique raw internal disk.\r\n", Matches);
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
    UnicodeSPrint (Name, sizeof (Name), L"EmmcDump-%04u", Index);
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
DumpDisk (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, UINT64 Total)
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
  if (Media.LastBlock == MAX_UINT64 ||
      Media.LastBlock + 1 > DivU64x32 (MAX_UINT64, Media.BlockSize) ||
      MultU64x32 (Media.LastBlock + 1, Media.BlockSize) != Total) {
    return EFI_MEDIA_CHANGED;
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
  Lba = 0;
  Part = 0;
  NextProgress = 0;
  InitCrc ();
  Status = EFI_SUCCESS;
  while (Done < Total) {
    UnicodeSPrint (Name, sizeof (Name), L"emmc-%04u.bin", Part);
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
        if (gST->ConOut != NULL) {
          Print (L"\rCopied %Lu / %Lu MiB", RShiftU64 (Done, 20), RShiftU64 (Total, 20));
        }
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
    Status = Log ("\r\npart=emmc-%04u.bin bytes=%u crc32=%08x\r\n",
                  Part, PartBytes, Crc ^ 0xFFFFFFFFU);
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
    Part++;
  }
  Status = Directory->Flush (Directory);
  if (!EFI_ERROR (Status)) {
    Status = Log ("COMPLETE bytes=%Lu parts=%u\r\n", Done, Part);
  }

Exit:
  if (File != NULL) {
    File->Flush (File);
    File->Close (File);
  }
  if (EFI_ERROR (Status)) {
    Log ("\r\nINCOMPLETE status=%r next_lba=%Lu bytes_in_full_chunks=%Lu\r\n",
         Status, Lba, Done);
  }
  FreeAlignedPages (Buffer, Pages);
  return Status;
}

EFI_STATUS EFIAPI
UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
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

  Root = NULL;
  Directory = NULL;
  Info = NULL;
  mLog = NULL;
  (VOID)SystemTable;
  Log ("EmmcDump 1.0: raw internal disk -> USB. ESC cancels.\r\n");
  // Long synchronous disk transfers must not trigger the boot watchdog.
  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    Log ("Cannot disable watchdog: %r\r\n", Status);
    goto Exit;
  }
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
    Status = LogPath ("destination_path", DevicePathFromHandle (Destination));
  }
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  Status = FindSource (Destination, &Source, &SourceHandle);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  if (Source->Media->BlockSize == 0 || Source->Media->LastBlock == MAX_UINT64) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }
  Blocks = Source->Media->LastBlock + 1;
  if (Blocks > DivU64x32 (MAX_UINT64, Source->Media->BlockSize)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }
  Total = MultU64x32 (Blocks, Source->Media->BlockSize);
  // Includes slack for directory entries, manifest and filesystem allocation.
  if (Info->FreeSpace < SPACE_RESERVE || Total > Info->FreeSpace - SPACE_RESERVE) {
    Log ("Insufficient USB space: dump=%Lu free=%Lu reserve=%u\r\n",
         Total, Info->FreeSpace, SPACE_RESERVE);
    Status = EFI_VOLUME_FULL;
    goto Exit;
  }
  Status = LogPath ("source_path", DevicePathFromHandle (SourceHandle));
  if (!EFI_ERROR (Status)) {
    Status = Log ("total_bytes=%Lu block_size=%u last_lba=%Lu\r\n",
                  Total, Source->Media->BlockSize, Source->Media->LastBlock);
  }
  if (!EFI_ERROR (Status)) {
    Status = DumpDisk (Source, Directory, Total);
  }

Exit:
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
  Log ("EmmcDump finished: %r\r\n", Status);
  // Phone loaders may have no keyboard; never block indefinitely waiting for one.
  gBS->Stall (10 * 1000 * 1000);
  return Status;
}
