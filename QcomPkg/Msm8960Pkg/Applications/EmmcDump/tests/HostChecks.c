// Host-only regression checks. Uses the repository's real BasePrintLib.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#undef NULL
#include "../Screen.c"
#include "../EmmcDump.c"

EFI_BOOT_SERVICES *gBS;
EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiFileSystemInfoGuid = EFI_FILE_SYSTEM_INFO_ID;
// These legacy host checks cover printing/GOP, not menu or GPT/storage execution.
EFI_STATUS ChooseDumpMode (EFI_FILE_PROTOCOL *Root, UINTN *Mode) { assert (0); return EFI_UNSUPPORTED; }
EFI_STATUS LoadPartitionPlan (EFI_FILE_PROTOCOL *Root, EFI_BLOCK_IO_PROTOCOL *Io,
                              PARTITION_PLAN **Plan, DUMP_LOG Log) { assert (0); return EFI_UNSUPPORTED; }

VOID *EFIAPI AllocateZeroPool (UINTN Size) { return calloc (1, Size); }
VOID EFIAPI FreePool (VOID *Buffer) { free (Buffer); }
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Size) { return memset (Buffer, 0, Size); }
VOID *EFIAPI CopyMem (VOID *To, CONST VOID *From, UINTN Size) { return memmove (To, From, Size); }
INTN EFIAPI CompareMem (CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp (A, B, Size); }
UINT64 EFIAPI RShiftU64 (UINT64 Value, UINTN Count) { return Value >> Count; }
UINT64 EFIAPI MultU64x32 (UINT64 Value, UINT32 Other) { return Value * Other; }
UINT64 EFIAPI DivU64x32 (UINT64 Value, UINT32 Other) { return Value / Other; }
UINT16 EFIAPI ReadUnaligned16 (CONST UINT16 *P) { UINT16 V; memcpy (&V, P, sizeof (V)); return V; }
UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *P) { UINT32 V; memcpy (&V, P, sizeof (V)); return V; }
// Storage and filesystem execution is outside these host display/format tests.
VOID *EFIAPI AllocateAlignedPages (UINTN Pages, UINTN Alignment) { assert (0); return NULL; }
VOID EFIAPI FreeAlignedPages (VOID *Buffer, UINTN Pages) { assert (0); }
BOOLEAN EFIAPI IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *P, UINTN Size) { return P != NULL; }
UINT8 EFIAPI DevicePathType (CONST VOID *P) { return ((EFI_DEVICE_PATH_PROTOCOL *)P)->Type; }
UINT8 EFIAPI DevicePathSubType (CONST VOID *P) { return ((EFI_DEVICE_PATH_PROTOCOL *)P)->SubType; }
UINTN EFIAPI DevicePathNodeLength (CONST VOID *P) { return ReadUnaligned16 ((UINT16 *)((EFI_DEVICE_PATH_PROTOCOL *)P)->Length); }
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode (CONST VOID *P) { return (VOID *)((UINT8 *)P + DevicePathNodeLength (P)); }
BOOLEAN EFIAPI IsDevicePathEnd (CONST VOID *P) { return DevicePathType (P) == END_DEVICE_PATH_TYPE && DevicePathSubType (P) == END_ENTIRE_DEVICE_PATH_SUBTYPE; }
UINTN EFIAPI GetDevicePathSize (CONST EFI_DEVICE_PATH_PROTOCOL *P) { assert (0); return 0; }
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle (EFI_HANDLE Handle) { assert (0); return NULL; }
CHAR16 *EFIAPI ConvertDevicePathToText (CONST EFI_DEVICE_PATH_PROTOCOL *P, BOOLEAN Display, BOOLEAN Shortcuts) { assert (0); return NULL; }
UINT64 EFIAPI DivU64x32Remainder (UINT64 Value, UINT32 Divisor, UINT32 *Remainder)
{
  if (Remainder != NULL) { *Remainder = (UINT32)(Value % Divisor); }
  return Value / Divisor;
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL TestGop;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE TestMode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION TestInfo;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL Frame[240 * 160];
STATIC UINTN TextCalls;
STATIC BOOLEAN FailBlt;

STATIC EFI_STATUS EFIAPI
MockBlt (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,
         EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op, UINTN Sx, UINTN Sy,
         UINTN Dx, UINTN Dy, UINTN Width, UINTN Height, UINTN Delta)
{
  UINTN Y;
  assert (Op == EfiBltBufferToVideo);
  assert (Dx + Width <= 240 && Dy + Height <= 160);
  assert (Delta >= (Sx + Width) * sizeof (*Buffer));
  if (FailBlt) { return EFI_DEVICE_ERROR; }
  for (Y = 0; Y < Height; Y++) {
    memcpy (Frame + (Dy + Y) * 240 + Dx,
            (UINT8 *)Buffer + (Sy + Y) * Delta + Sx * sizeof (*Buffer),
            Width * sizeof (*Buffer));
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockLocate (EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid, VOID *Key, UINTN *Count, EFI_HANDLE **Handles)
{
  *Count = 1;
  *Handles = AllocateZeroPool (sizeof (**Handles));
  (*Handles)[0] = (EFI_HANDLE)1;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockHandle (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Protocol)
{
  *Protocol = &TestGop;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockText (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  TextCalls++;
  return EFI_SUCCESS;
}

STATIC VOID
CheckFormatting (VOID)
{
  CHAR8 Text[256];
  CHAR16 Name[32];

  UnicodeSPrint (Name, sizeof (Name), L"EmmcDump-%04d", 12);
  AsciiSPrint (Text, sizeof (Text), "%s", Name);
  assert (strcmp (Text, "EmmcDump-0012") == 0);
  AsciiSPrint (Text, sizeof (Text), "part=emmc-%04d.bin bytes=%d crc32=%08x", 1, 1073741824, 0xCBF43926U);
  assert (strcmp (Text, "part=emmc-0001.bin bytes=1073741824 crc32=CBF43926") == 0);
  AsciiSPrint (Text, sizeof (Text), "total_bytes=%Ld block_size=%d last_lba=%Ld",
              (UINT64)16777216000ULL, 512, (UINT64)32767999);
  assert (strcmp (Text, "total_bytes=16777216000 block_size=512 last_lba=32767999") == 0);
  // In this legacy PrintLib, %u is literal 'u' and does not consume an argument.
  AsciiSPrint (Text, sizeof (Text), "%u", 512);
  assert (strcmp (Text, "u") == 0);
  InitCrc ();
  assert ((UpdateCrc (0xFFFFFFFFU, (UINT8 *)"123456789", 9) ^ 0xFFFFFFFFU) == 0xCBF43926U);
}

STATIC VOID
CheckPaths (VOID)
{
  struct {
    VENDOR_DEVICE_PATH Vendor;
    EFI_DEVICE_PATH_PROTOCOL End;
  } Path;
  EFI_GUID User = {0xb615f1f5,0x5088,0x43cd,{0x80,0x9c,0xa1,0x6e,0x52,0x48,0x7d,0x00}};
  EFI_GUID Boot1 = {0x12c55b20,0x25d3,0x41c9,{0x8e,0x06,0x28,0x2d,0x94,0xc6,0x76,0xad}};
  EFI_GUID Boot2 = {0x6b76a6db,0x0257,0x48a9,{0xaa,0x99,0xf6,0xb1,0x65,0x5f,0x7b,0x00}};

  ZeroMem (&Path, sizeof (Path));
  Path.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  Path.Vendor.Header.SubType = HW_VENDOR_DP;
  Path.Vendor.Header.Length[0] = sizeof (VENDOR_DEVICE_PATH);
  Path.End.Type = END_DEVICE_PATH_TYPE;
  Path.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  Path.End.Length[0] = sizeof (Path.End);
  Path.Vendor.Guid = User;
  assert (EmmcArea ((EFI_DEVICE_PATH_PROTOCOL *)&Path) == 1);
  Path.Vendor.Guid = Boot1;
  assert (EmmcArea ((EFI_DEVICE_PATH_PROTOCOL *)&Path) == 2);
  Path.Vendor.Guid = Boot2;
  assert (EmmcArea ((EFI_DEVICE_PATH_PROTOCOL *)&Path) == 3);
  Path.Vendor.Guid = User;
  Path.End.Type = MEDIA_DEVICE_PATH;
  assert (EmmcArea ((EFI_DEVICE_PATH_PROTOCOL *)&Path) == 0);
}

STATIC VOID
CheckScreen (VOID)
{
  EFI_BOOT_SERVICES Services;
  EFI_SYSTEM_TABLE System;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
  UINTN I;
  UINTN Lit;
  CHAR8 Description[192];

  ZeroMem (&Services, sizeof (Services));
  ZeroMem (&System, sizeof (System));
  ZeroMem (&Console, sizeof (Console));
  gBS = &Services;
  gST = &System;
  Services.LocateHandleBuffer = MockLocate;
  Services.HandleProtocol = MockHandle;
  TestInfo.HorizontalResolution = 240;
  TestInfo.VerticalResolution = 160;
  TestInfo.PixelFormat = PixelBltOnly;
  TestMode.Info = &TestInfo;
  TestGop.Mode = &TestMode;
  TestGop.Blt = MockBlt;
  ScreenInit ();
  assert (mGop != NULL && mGopStatus == EFI_SUCCESS);
  assert (sizeof (mFont) / sizeof (mFont[0]) == sizeof (mCharacters) - 1);
  ScreenWrite ("EmmcDump 1.1\r\nDevice: eMMC User\r\n");
  ScreenProgress (5ULL << 30, 16ULL << 30);
  Lit = 0;
  for (I = 0; I < sizeof (Frame) / sizeof (Frame[0]); I++) {
    if (Frame[I].Green != 0) { Lit++; }
  }
  assert (Lit > 100);
  assert (mRow == 2);
  for (I = 0; I < 100; I++) { ScreenWrite ("scroll test\n"); }
  assert (mRow == mRows - 1 && mGopStatus == EFI_SUCCESS);
  ScreenDescription (Description, sizeof (Description));
  assert (strstr (Description, "display=GOP") != NULL);
  Console.OutputString = MockText;
  System.ConOut = &Console;
  FailBlt = TRUE;
  ScreenWrite ("force a graphics failure\n");
  assert (TextCalls > 0 && mGopStatus == EFI_DEVICE_ERROR);
  ScreenDescription (Description, sizeof (Description));
  assert (strstr (Description, "display=ConOut") != NULL);
  ScreenRelease ();
}

int main (void)
{
  CheckFormatting ();
  CheckPaths ();
  CheckScreen ();
  puts ("PASS: real legacy PrintLib numbers/names, CRC32, User/Boot paths, GOP drawing/scroll/fallback");
  return 0;
}
