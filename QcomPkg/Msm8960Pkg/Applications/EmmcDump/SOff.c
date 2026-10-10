/** Experimental HTC PGFS/E42T writer. All backups must verify before writes. */
#include <Uefi.h>
#include <Guid/FileSystemInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "SOff.h"

#define CHUNK (1024U * 1024U)
#define LIMIT (64U * 1024U * 1024U)
typedef struct {
  CONST CHAR16 *Name;
  CONST CHAR16 *File;
  DUMP_PARTITION Part;
  UINT8 *Before;
  UINT8 *After;
  UINTN Size;
  UINTN Pages;
} IMAGE;

STATIC UINT32 R32 (CONST UINT8 *P) {
  return P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
STATIC VOID W32 (UINT8 *P, UINT32 V) {
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8);
  P[2] = (UINT8)(V >> 16); P[3] = (UINT8)(V >> 24);
}
STATIC UINT32 RawCrc (UINT32 C, CONST UINT8 *P, UINTN N) {
  UINTN I;
  while (N-- != 0) {
    C ^= *P++;
    for (I = 0; I < 8; I++) { C = (C >> 1) ^ ((C & 1) ? 0xEDB88320U : 0); }
  }
  return C;
}
STATIC BOOLEAN Tag (CONST UINT8 *P, CONST CHAR8 *S) { return CompareMem (P, S, 4) == 0; }
STATIC BOOLEAN Same (CONST CHAR16 *A, CONST CHAR16 *B) {
  CHAR16 X, Y;
  do {
    X = *A++; Y = *B++;
    if (X >= L'a' && X <= L'z') { X -= L'a' - L'A'; }
    if (Y >= L'a' && Y <= L'z') { Y -= L'a' - L'A'; }
    if (X != Y) { return FALSE; }
  } while (X != 0);
  return TRUE;
}

STATIC EFI_STATUS PatchPgfs (UINT8 *B, UINTN N) {
  UINTN Slots, I, Count, Base, Entry;
  UINT32 C, Saved;
  UINT8 Header[56];
  STATIC CONST UINT8 Extent[6] = {0, 0x80, 0, 0, 0, 3};
  if (N < 0x1000 || !Tag (B, "PGFS") || R32 (B + 4) != N ||
      R32 (B + 0x14) != 0x20000 || R32 (B + 0x18) != 0x400) { return EFI_UNSUPPORTED; }
  Slots = R32 (B + 8);
  if (Slots == 0 || Slots > 1024 || (Slots + 1) * 0x400 > N - 0x1000) { return EFI_COMPROMISED_DATA; }
  Base = (Slots + 1) * 0x400; Count = 0; Entry = 0;
  for (I = 1; I <= Slots; I++) {
    if (CompareMem (B + I * 0x400, "security", 9) == 0) { Entry = I * 0x400; Count++; }
  }
  // Only the single, contiguous four-block extent verified in the supplied DIAG.
  if (Count != 1 || R32 (B + Entry + 0x20) != 0x1000 ||
      R32 (B + Entry + 0x28) != 1 || R32 (B + Entry + 0x2c) != 3 ||
      CompareMem (B + Entry + 0x30, Extent, sizeof (Extent)) != 0 || R32 (B + Base) > 3) {
    return EFI_UNSUPPORTED;
  }
  CopyMem (Header, B + Entry, 54); Header[54] = Header[55] = 0;
  Saved = R32 (Header + 0x24); W32 (Header + 0x24, 0);
  C = RawCrc (RawCrc (0, B + Base, 0x1000), Header, sizeof (Header));
  if (C != Saved) { return EFI_CRC_ERROR; }
  W32 (B + Base, 0);
  W32 (B + Entry + 0x24, RawCrc (RawCrc (0, B + Base, 0x1000), Header, sizeof (Header)));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS PatchBoard (UINT8 *B, UINTN N) {
  if (N < 0x400 || CompareMem (B, "HTC-BOARD-INFO!@", 16) != 0) { return EFI_UNSUPPORTED; }
  CopyMem (B + 0x14, "11111111", 8);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PatchMfg (UINT8 *B, UINTN N) {
  UINTN I;
  // The supplied MFG starts with a nine-character UTF-16LE product ID.
  if (N < 512 || B[0] != 'P' || B[2] < 'A' || B[2] > 'Z' ||
      B[18] != 0 || B[19] != 0) { return EFI_UNSUPPORTED; }
  for (I = 0; I < 9; I++) {
    if (B[I * 2 + 1] != 0 || (I >= 2 &&
        !((B[I * 2] >= '0' && B[I * 2] <= '9') || (I >= 4 && B[I * 2] == '*')))) {
      return EFI_UNSUPPORTED;
    }
  }
  for (I = 4; I < 9; I++) { B[I * 2] = '*'; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS PatchStore (UINT8 *B) {
  STATIC CONST UINT8 Global[16] = {
    0x61,0xdf,0xe4,0x8b,0xca,0x93,0xd2,0x11,0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c
  };
  UINT8 *G, *Map, *T, *D, *E;
  UINTN I, J, Count, TableBlock, DataBlock, O, End, DataOffset;
  UINT32 Saved, C;
  // B points at INFO. Offsets/versions are the matching HTC and WPinternals profile.
  G = B + 0x200; Map = B + 0x7000;
  if (!Tag (B, "INFO") || R32 (B + 4) != 0x10001 || R32 (B + 8) != 512 ||
      R32 (B + 12) != 56 || !Tag (G, "GUID") || R32 (G + 4) != 0x10000 ||
      R32 (G + 8) != MAX_UINT32 || !Tag (Map, "BLOC") ||
      R32 (Map + 4) != 0x10000 || R32 (Map + 8) != 56) { return EFI_UNSUPPORTED; }
  Saved = R32 (Map + 12); W32 (Map + 12, 0);
  C = RawCrc (MAX_UINT32, Map, 512) ^ MAX_UINT32; W32 (Map + 12, Saved);
  if (C != Saved) { return EFI_CRC_ERROR; }
  Count = R32 (G + 12);
  if (Count == 0 || Count > 22) { return EFI_UNSUPPORTED; }
  for (I = 0; I < Count; I++) {
    E = G + 32 + I * 20;
    if (CompareMem (E, Global, 16) != 0) { continue; }
    J = R32 (E + 16);
    if (J < 2 || J >= 56 || R32 (Map + 16 + J * 4) != 0x30000) { return EFI_COMPROMISED_DATA; }
    T = B + J * 512;
    if (!Tag (T, "RTBL") || R32 (T + 8) != MAX_UINT32) { return EFI_UNSUPPORTED; }
    End = R32 (T + 16);
    if (End < 20 || End > 512) { return EFI_COMPROMISED_DATA; }
    O = 20;
    for (J = 0; J < R32 (T + 12); J++) {
      if (O > End || End - O < 48 || !Tag (T + O, "NAME")) { return EFI_COMPROMISED_DATA; }
      E = T + O;
      if (R32 (E + 12) > End - O - 48) { return EFI_COMPROMISED_DATA; }
      if (R32 (E + 12) == sizeof (L"SecureBoot") &&
          CompareMem (E + 48, L"SecureBoot", sizeof (L"SecureBoot")) == 0) {
        DataOffset = R32 (E + 8);
        if (DataOffset < 1024 || DataOffset > 0x7000 - 28 || (DataOffset & 3) != 0 ||
            DataOffset % 512 > 484 || (R32 (Map + 16 + (DataOffset / 512) * 4) >> 16) != 4 ||
            R32 (E + 4) != 3) { return EFI_UNSUPPORTED; }
        D = B + DataOffset;
        if (!Tag (D, "DATA") || R32 (D + 4) != 4 || R32 (D + 8) != 4 ||
            R32 (D + 12) != MAX_UINT32 || R32 (D + 16) != 0 ||
            R32 (D + 20) != 0 || R32 (D + 24) > 1) { return EFI_UNSUPPORTED; }
        W32 (D + 24, 0); return EFI_SUCCESS;
      }
      O += (48 + R32 (E + 12) + 3) & ~(UINTN)3;
    }
    // Do not expand an unknown pre-existing global-variable table.
    return EFI_UNSUPPORTED;
  }
  E = G + 32 + Count * 20;
  for (I = 0; I < 16; I++) { if (E[I] != 0) { return EFI_COMPROMISED_DATA; } }
  if (R32 (E + 16) != MAX_UINT32) { return EFI_COMPROMISED_DATA; }
  TableBlock = DataBlock = 0;
  for (I = 2; I < 56; I++) {
    if (R32 (Map + 16 + I * 4) != 512) { continue; }
    T = B + I * 512;
    if (!Tag (T, "EMPT") || R32 (T + 4) != 504) { return EFI_COMPROMISED_DATA; }
    for (J = 8; J < 512; J++) { if (T[J] != 0xff) { return EFI_COMPROMISED_DATA; } }
    if (TableBlock == 0) { TableBlock = I; } else { DataBlock = I; break; }
  }
  if (DataBlock == 0) { return EFI_VOLUME_FULL; }
  CopyMem (E, Global, 16); W32 (E + 16, (UINT32)TableBlock);
  ZeroMem (E + 20, 16); W32 (E + 36, MAX_UINT32); W32 (G + 12, (UINT32)Count + 1);
  T = B + TableBlock * 512; SetMem (T, 512, 0xff);
  CopyMem (T, "RTBL", 4); W32 (T + 4, 0x10001); W32 (T + 8, MAX_UINT32);
  W32 (T + 12, 1); W32 (T + 16, 92);
  E = T + 20; ZeroMem (E, 48); CopyMem (E, "NAME", 4);
  W32 (E + 4, 3); W32 (E + 8, (UINT32)DataBlock * 512);
  W32 (E + 12, sizeof (L"SecureBoot")); CopyMem (E + 48, L"SecureBoot", sizeof (L"SecureBoot"));
  D = B + DataBlock * 512; SetMem (D, 512, 0xff);
  ZeroMem (D, 28); CopyMem (D, "DATA", 4); W32 (D + 4, 4); W32 (D + 8, 4);
  W32 (D + 12, MAX_UINT32); CopyMem (D + 28, "EMPT", 4); W32 (D + 32, 476);
  W32 (Map + 16 + TableBlock * 4, 0x30000);
  W32 (Map + 16 + DataBlock * 4, 0x40000 | 484);
  W32 (Map + 12, 0); W32 (Map + 12, RawCrc (MAX_UINT32, Map, 512) ^ MAX_UINT32);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PatchNv (UINT8 *B, UINTN N) {
  EFI_STATUS S;
  UINTN I;
  if (N != 0x40000 || !Tag (B, "E42T") || R32 (B + 8) != 512 ||
      R32 (B + 0x28) != 65 || R32 (B + 0x2c) != 120) { return EFI_UNSUPPORTED; }
  for (I = 0x8400; I <= 0xfc00; I += 0x7800) {
    if (!Tag (B + I, "STOR") || R32 (B + I + 8) != 512 ||
        R32 (B + I + 12) != 0x7000 || R32 (B + I + 16) != 0x7200 ||
        !Tag (B + I - 0x400, "DStr") || !Tag (B + I - 0x200, "DStr")) { return EFI_UNSUPPORTED; }
    S = PatchStore (B + I + 512); if (EFI_ERROR (S)) { return S; }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Backup (EFI_FILE_PROTOCOL *Dir, IMAGE *Im, UINT8 *Scratch) {
  EFI_FILE_PROTOCOL *F;
  EFI_STATUS S, Close;
  UINTN O, N, Done, Want;
  S = Dir->Open (Dir, &F, (CHAR16 *)Im->File, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (S)) { F->Close (F); return EFI_ACCESS_DENIED; }
  if (S != EFI_NOT_FOUND) { return S; }
  S = Dir->Open (Dir, &F, (CHAR16 *)Im->File, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (S)) { return S; }
  for (O = 0; O < Im->Size && !EFI_ERROR (S); O += Done) {
    Want = MIN (CHUNK, Im->Size - O); Done = 0;
    while (Done < Want) {
      N = Want - Done; S = F->Write (F, &N, Im->Before + O + Done);
      if (EFI_ERROR (S)) { break; }
      if (N == 0 || N > Want - Done) { S = EFI_DEVICE_ERROR; break; }
      Done += N;
    }
  }
  if (!EFI_ERROR (S)) { S = F->Flush (F); }
  Close = F->Close (F); if (!EFI_ERROR (S)) { S = Close; }
  if (EFI_ERROR (S)) { return S; }
  S = Dir->Open (Dir, &F, (CHAR16 *)Im->File, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (S)) { return S; }
  for (O = 0; O < Im->Size && !EFI_ERROR (S); O += Want) {
    Want = MIN (CHUNK, Im->Size - O); Done = 0;
    while (Done < Want) {
      N = Want - Done; S = F->Read (F, &N, Scratch + Done);
      if (EFI_ERROR (S)) { break; }
      if (N == 0 || N > Want - Done) { S = EFI_DEVICE_ERROR; break; }
      Done += N;
    }
    if (!EFI_ERROR (S) && CompareMem (Scratch, Im->Before + O, Want) != 0) { S = EFI_CRC_ERROR; }
  }
  Close = F->Close (F); return EFI_ERROR (S) ? S : Close;
}

STATIC EFI_STATUS RunPatches (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, DUMP_LOG Log, BOOLEAN NvOnly) {
  IMAGE Im[4];
  CONST CHAR16 *Names[4] = {L"pg1fs", L"board_info", L"MFG", L"UEFI_BS_NV"};
  CONST CHAR16 *Files[4] = {L"pg1fs.original.img", L"board_info.original.img", L"MFG.original.img", L"UEFI_BS_NV.original.img"};
  PARTITION_PLAN *Plan, *Gpt, *Current;
  EFI_STATUS S;
  UINTN I, J, K, O, N, Alignment, Found, InfoSize, First;
  UINT32 MediaId, Block;
  UINT64 Total;
  UINT8 *Scratch;
  EFI_FILE_SYSTEM_INFO *Info;
  BOOLEAN WriteStarted;
  ZeroMem (Im, sizeof (Im)); Plan = NULL; Gpt = NULL; Current = NULL; Scratch = NULL; Info = NULL;
  WriteStarted = FALSE; Total = 0; First = NvOnly ? 3 : 0;
  if (Io->Media->ReadOnly || Io->Media->BlockSize != 512) { return EFI_UNSUPPORTED; }
  Block = Io->Media->BlockSize; MediaId = Io->Media->MediaId;
  Alignment = MAX ((UINTN)Io->Media->IoAlign, (UINTN)EFI_PAGE_SIZE);
  if ((Alignment & (Alignment - 1)) != 0) { return EFI_UNSUPPORTED; }
  S = LoadGptPlan (Io, &Gpt, Log); if (EFI_ERROR (S)) { goto Exit; }
  S = LoadPartitionPlan (NULL, Io, &Plan, Log); if (EFI_ERROR (S)) { goto Exit; }
  // Reject any overlapping GPT entries, including aliases of a target partition.
  for (I = 0; I < Plan->Count; I++) {
    for (J = I + 1; J < Plan->Count; J++) {
      if (Plan->Part[I].Start <= Plan->Part[J].End && Plan->Part[J].Start <= Plan->Part[I].End) {
        S = EFI_COMPROMISED_DATA; goto Exit;
      }
    }
  }
  Scratch = AllocateAlignedPages (EFI_SIZE_TO_PAGES (CHUNK), Alignment);
  if (Scratch == NULL) { S = EFI_OUT_OF_RESOURCES; goto Exit; }
  for (I = First; I < 4; I++) {
    Im[I].Name = Names[I]; Im[I].File = Files[I]; Found = 0;
    for (J = 0; J < Plan->Count; J++) {
      if (Same (Names[I], Plan->Part[J].Name)) { Im[I].Part = Plan->Part[J]; Found++; }
    }
    if (Found != 1) { S = EFI_NOT_FOUND; goto Exit; }
    if (Im[I].Part.Bytes == 0 || Im[I].Part.Bytes > LIMIT) { S = EFI_UNSUPPORTED; goto Exit; }
    Im[I].Size = (UINTN)Im[I].Part.Bytes; Total += Im[I].Size;
    if (Total > 80U * 1024U * 1024U) { S = EFI_OUT_OF_RESOURCES; goto Exit; }
    Im[I].Pages = EFI_SIZE_TO_PAGES (Im[I].Size);
    Im[I].Before = AllocateAlignedPages (Im[I].Pages, Alignment);
    Im[I].After = AllocateAlignedPages (Im[I].Pages, Alignment);
    if (Im[I].Before == NULL || Im[I].After == NULL) { S = EFI_OUT_OF_RESOURCES; goto Exit; }
    for (O = 0; O < Im[I].Size; O += N) {
      N = MIN (CHUNK, Im[I].Size - O);
      S = Io->ReadBlocks (Io, MediaId, Im[I].Part.Start + O / Block, N, Scratch);
      if (EFI_ERROR (S)) { goto Exit; }
      CopyMem (Im[I].Before + O, Scratch, N);
    }
    CopyMem (Im[I].After, Im[I].Before, Im[I].Size);
    switch (I) {
      case 0: S = PatchPgfs (Im[I].After, Im[I].Size); break;
      case 1: S = PatchBoard (Im[I].After, Im[I].Size); break;
      case 2: S = PatchMfg (Im[I].After, Im[I].Size); break;
      default: S = PatchNv (Im[I].After, Im[I].Size); break;
    }
    Log ("S-OFF preflight %s: %r\r\n", Names[I], S);
    if (EFI_ERROR (S)) { goto Exit; }
  }
  InfoSize = 0;
  S = Directory->GetInfo (Directory, &gEfiFileSystemInfoGuid, &InfoSize, NULL);
  if (S != EFI_BUFFER_TOO_SMALL) { if (!EFI_ERROR (S)) { S = EFI_DEVICE_ERROR; } goto Exit; }
  Info = AllocatePool (InfoSize); if (Info == NULL) { S = EFI_OUT_OF_RESOURCES; goto Exit; }
  S = Directory->GetInfo (Directory, &gEfiFileSystemInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (S)) { goto Exit; }
  if (Info->ReadOnly || Info->FreeSpace < Total + 16U * 1024U * 1024U) { S = EFI_VOLUME_FULL; goto Exit; }
  for (I = First; I < 4; I++) {
    S = Log ("backup=%s lba=%Ld bytes=%Ld crc32=%08x\r\n", Im[I].File,
         Im[I].Part.Start, (UINT64)Im[I].Size, RawCrc (MAX_UINT32, Im[I].Before, Im[I].Size) ^ MAX_UINT32);
    if (EFI_ERROR (S)) { goto Exit; }
    S = Backup (Directory, &Im[I], Scratch); if (EFI_ERROR (S)) { goto Exit; }
    S = Log ("Backup reopened and verified: %s\r\n", Im[I].File); if (EFI_ERROR (S)) { goto Exit; }
  }
  // Check all original source bytes again before the first write, including NV counters.
  for (I = First; I < 4; I++) {
    for (O = 0; O < Im[I].Size; O += N) {
      N = MIN (CHUNK, Im[I].Size - O);
      S = Io->ReadBlocks (Io, MediaId, Im[I].Part.Start + O / Block, N, Scratch);
      if (EFI_ERROR (S)) { goto Exit; }
      if (CompareMem (Scratch, Im[I].Before + O, N) != 0) { S = EFI_MEDIA_CHANGED; goto Exit; }
    }
  }
  S = Log ("All backups verified. Rechecking GPT before writes.\r\n");
  if (EFI_ERROR (S)) { goto Exit; }
  S = LoadPartitionPlan (NULL, Io, &Current, Log);
  if (EFI_ERROR (S)) { goto Exit; }
  if (Current->MediaId != MediaId || Current->Count != Plan->Count ||
      Current->BlockSize != Block || Current->LastBlock != Plan->LastBlock ||
      CompareMem (Current->Part, Plan->Part, Plan->Count * sizeof (DUMP_PARTITION)) != 0) {
    S = EFI_MEDIA_CHANGED; goto Exit;
  }
  for (I = First; I < 4; I++) {
    S = Log ("Writing %s; planned_crc32=%08x. Do not remove power.\r\n", Names[I],
             RawCrc (MAX_UINT32, Im[I].After, Im[I].Size) ^ MAX_UINT32);
    if (EFI_ERROR (S)) { goto Exit; }
    // NV second copy first; all other partitions in ascending LBA order.
    for (K = 0; K < Im[I].Size / Block; K++) {
      O = (I == 3 ? Im[I].Size / Block - 1 - K : K) * Block;
      if (CompareMem (Im[I].Before + O, Im[I].After + O, Block) == 0) { continue; }
      S = Io->ReadBlocks (Io, MediaId, Im[I].Part.Start + O / Block, Block, Scratch);
      if (EFI_ERROR (S)) { goto Exit; }
      if (CompareMem (Scratch, Im[I].Before + O, Block) != 0) { S = EFI_MEDIA_CHANGED; goto Exit; }
      CopyMem (Scratch, Im[I].After + O, Block); WriteStarted = TRUE;
      S = Io->WriteBlocks (Io, MediaId, Im[I].Part.Start + O / Block, Block, Scratch);
      if (EFI_ERROR (S)) { goto Exit; }
    }
    S = Io->FlushBlocks (Io); if (EFI_ERROR (S)) { goto Exit; }
    for (O = 0; O < Im[I].Size; O += N) {
      N = MIN (CHUNK, Im[I].Size - O);
      S = Io->ReadBlocks (Io, MediaId, Im[I].Part.Start + O / Block, N, Scratch);
      if (EFI_ERROR (S)) { goto Exit; }
      if (CompareMem (Scratch, Im[I].After + O, N) != 0) { S = EFI_CRC_ERROR; goto Exit; }
    }
    S = Log ("Written and read-back verified: %s\r\n", Names[I]); if (EFI_ERROR (S)) { goto Exit; }
  }
  S = Log ("Patches verified on disk. S-OFF/Secure Boot effectiveness must be checked after reboot.\r\n"
           "Firmware may cache NV; do not change firmware variables in this session.\r\n");
Exit:
  if (EFI_ERROR (S)) {
    Log (WriteStarted ? "S-OFF FAILED %r: writes began; partial changes possible. Keep ALL backups.\r\n" :
                        "S-OFF stopped %r: no eMMC writes performed.\r\n", S);
  }
  for (I = First; I < 4; I++) {
    if (Im[I].Before != NULL) { FreeAlignedPages (Im[I].Before, Im[I].Pages); }
    if (Im[I].After != NULL) { FreeAlignedPages (Im[I].After, Im[I].Pages); }
  }
  if (Scratch != NULL) { FreeAlignedPages (Scratch, EFI_SIZE_TO_PAGES (CHUNK)); }
  if (Info != NULL) { FreePool (Info); }
  if (Plan != NULL) { FreePool (Plan); }
  if (Gpt != NULL) { FreePool (Gpt); }
  if (Current != NULL) { FreePool (Current); }
  return S;
}

EFI_STATUS RunSOff (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, DUMP_LOG Log) {
  return RunPatches (Io, Directory, Log, FALSE);
}
EFI_STATUS RunDisableSecureBoot (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, DUMP_LOG Log) {
  return RunPatches (Io, Directory, Log, TRUE);
}
