/** Parse requested GPT names and validate all ranges before creating any dump. */
#include "Partitions.h"
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define MAX_LIST_BYTES 16384
#define MAX_GPT_BYTES (4U * 1024U * 1024U)

STATIC BOOLEAN
SameName (CONST CHAR16 *A, CONST CHAR16 *B)
{
  CHAR16 Ac;
  CHAR16 Bc;
  do {
    Ac = *A++;
    Bc = *B++;
    if (Ac >= L'a' && Ac <= L'z') { Ac -= L'a' - L'A'; }
    if (Bc >= L'a' && Bc <= L'z') { Bc -= L'a' - L'A'; }
    if (Ac != Bc) { return FALSE; }
  } while (Ac != 0);
  return TRUE;
}

STATIC EFI_STATUS
ReadList (EFI_FILE_PROTOCOL *Root, PARTITION_PLAN *Plan, DUMP_LOG Log)
{
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  CHAR8 *Text;
  UINTN Size;
  UINTN Got;
  UINTN Pos;
  UINTN Start;
  UINTN End;
  UINTN Index;
  UINTN Other;

  Status = Root->Open (Root, &File, L"partition.txt", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Log ("Cannot open USB root partition.txt: %r\r\n", Status);
    return Status;
  }
  Text = AllocateZeroPool (MAX_LIST_BYTES + 2);
  if (Text == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Size = 0;
  while (Size <= MAX_LIST_BYTES) {
    Got = MAX_LIST_BYTES + 1 - Size;
    Status = File->Read (File, &Got, Text + Size);
    if (EFI_ERROR (Status) || Got == 0) { break; }
    Size += Got;
  }
  File->Close (File);
  if (EFI_ERROR (Status)) { goto Exit; }
  if (Size == 0 || Size > MAX_LIST_BYTES) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Exit;
  }
  Pos = 0;
  if (Size >= 3 && (UINT8)Text[0] == 0xEF && (UINT8)Text[1] == 0xBB && (UINT8)Text[2] == 0xBF) {
    Pos = 3; // Optional UTF-8 BOM; partition names themselves are ASCII.
  }
  while (Pos < Size) {
    Start = Pos;
    while (Pos < Size && Text[Pos] != '\r' && Text[Pos] != '\n') {
      if (Text[Pos] == 0 || (UINT8)Text[Pos] >= 0x80) {
        Status = EFI_UNSUPPORTED;
        goto Exit;
      }
      Pos++;
    }
    End = Pos++;
    while (Start < End && (Text[Start] == ' ' || Text[Start] == '\t')) { Start++; }
    while (End > Start && (Text[End - 1] == ' ' || Text[End - 1] == '\t')) { End--; }
    if (Start == End || Text[Start] == '#') { continue; }
    if (End - Start > 36 || Plan->Count == MAX_DUMP_PARTITIONS) {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Exit;
    }
    for (Index = 0; Index < End - Start; Index++) {
      if (Text[Start + Index] < 32) {
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
      }
      Plan->Part[Plan->Count].Name[Index] = (UINT8)Text[Start + Index];
    }
    for (Other = 0; Other < Plan->Count; Other++) {
      if (SameName (Plan->Part[Other].Name, Plan->Part[Plan->Count].Name)) {
        Log ("Duplicate name in partition.txt: %s\r\n", Plan->Part[Other].Name);
        Status = EFI_INVALID_PARAMETER;
        goto Exit;
      }
    }
    Plan->Count++;
  }
  Status = Plan->Count == 0 ? EFI_NOT_FOUND : EFI_SUCCESS;
Exit:
  FreePool (Text);
  return Status;
}

EFI_STATUS
LoadPartitionPlan (EFI_FILE_PROTOCOL *Root, EFI_BLOCK_IO_PROTOCOL *Io,
                   PARTITION_PLAN **Result, DUMP_LOG Log)
{
  PARTITION_PLAN *Plan;
  EFI_STATUS Status;
  EFI_PARTITION_TABLE_HEADER Header;
  EFI_PARTITION_ENTRY Entry;
  EFI_GUID ZeroGuid;
  VOID *HeaderBuffer;
  VOID *Table;
  UINTN HeaderPages;
  UINTN TablePages;
  UINTN Alignment;
  UINTN ReadSize;
  UINT64 TableBytes;
  UINT64 TableBlocks;
  UINT32 Crc;
  UINTN Index;
  UINTN Wanted;
  UINTN Other;
  CHAR16 Name[37];

  *Result = NULL;
  HeaderBuffer = NULL;
  Table = NULL;
  HeaderPages = 0;
  TablePages = 0;
  Plan = AllocateZeroPool (sizeof (*Plan));
  if (Plan == NULL) { return EFI_OUT_OF_RESOURCES; }
  Plan->MediaId = Io->Media->MediaId;
  Plan->BlockSize = Io->Media->BlockSize;
  Plan->LastBlock = Io->Media->LastBlock;
  // NULL Root requests every populated GPT entry for the interactive menu.
  if (Root != NULL) {
    Status = ReadList (Root, Plan, Log);
    if (EFI_ERROR (Status)) { goto Exit; }
  }
  if (Plan->BlockSize < sizeof (Header) || Plan->BlockSize > 65536 ||
      Plan->LastBlock < 3 || Io->Media->IoAlign > MAX_GPT_BYTES ||
      (Io->Media->IoAlign > 1 && (Io->Media->IoAlign & (Io->Media->IoAlign - 1)) != 0)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }
  Alignment = MAX ((UINTN)Io->Media->IoAlign, (UINTN)EFI_PAGE_SIZE);
  HeaderPages = EFI_SIZE_TO_PAGES (Plan->BlockSize);
  HeaderBuffer = AllocateAlignedPages (HeaderPages, Alignment);
  if (HeaderBuffer == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Exit; }
  Status = Io->ReadBlocks (Io, Plan->MediaId, 1, Plan->BlockSize, HeaderBuffer);
  if (EFI_ERROR (Status)) { goto Exit; }
  CopyMem (&Header, HeaderBuffer, sizeof (Header));
  if (Header.Header.Signature != EFI_PTAB_HEADER_ID || Header.Header.Revision != 0x00010000 ||
      Header.Header.HeaderSize < sizeof (Header) || Header.Header.HeaderSize > Plan->BlockSize ||
      Header.Header.Reserved != 0 || Header.MyLBA != 1 || Header.AlternateLBA != Plan->LastBlock ||
      Header.FirstUsableLBA <= 1 || Header.FirstUsableLBA > Header.LastUsableLBA ||
      Header.LastUsableLBA >= Plan->LastBlock || Header.PartitionEntryLBA < 2 ||
      Header.NumberOfPartitionEntries == 0 || Header.NumberOfPartitionEntries > 4096 ||
      Header.SizeOfPartitionEntry < sizeof (Entry) || Header.SizeOfPartitionEntry > 4096 ||
      (Header.SizeOfPartitionEntry & (Header.SizeOfPartitionEntry - 1)) != 0) {
    Status = EFI_COMPROMISED_DATA;
    goto Exit;
  }
  ((EFI_PARTITION_TABLE_HEADER *)HeaderBuffer)->Header.CRC32 = 0;
  Status = gBS->CalculateCrc32 (HeaderBuffer, Header.Header.HeaderSize, &Crc);
  if (EFI_ERROR (Status)) { goto Exit; }
  if (Crc != Header.Header.CRC32) { Status = EFI_CRC_ERROR; goto Exit; }
  TableBytes = MultU64x32 (Header.NumberOfPartitionEntries, Header.SizeOfPartitionEntry);
  if (TableBytes > MAX_GPT_BYTES) { Status = EFI_UNSUPPORTED; goto Exit; }
  TableBlocks = DivU64x32 (TableBytes + Plan->BlockSize - 1, Plan->BlockSize);
  if (Header.PartitionEntryLBA >= Header.FirstUsableLBA ||
      TableBlocks > Header.FirstUsableLBA - Header.PartitionEntryLBA) {
    Status = EFI_COMPROMISED_DATA;
    goto Exit;
  }
  ReadSize = (UINTN)MultU64x32 (TableBlocks, Plan->BlockSize);
  TablePages = EFI_SIZE_TO_PAGES (ReadSize);
  Table = AllocateAlignedPages (TablePages, Alignment);
  if (Table == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Exit; }
  Status = Io->ReadBlocks (Io, Plan->MediaId, Header.PartitionEntryLBA, ReadSize, Table);
  if (EFI_ERROR (Status)) { goto Exit; }
  Status = gBS->CalculateCrc32 (Table, (UINTN)TableBytes, &Crc);
  if (EFI_ERROR (Status)) { goto Exit; }
  if (Crc != Header.PartitionEntryArrayCRC32) { Status = EFI_CRC_ERROR; goto Exit; }
  ZeroMem (&ZeroGuid, sizeof (ZeroGuid));
  for (Index = 0; Index < Header.NumberOfPartitionEntries; Index++) {
    CopyMem (&Entry, (UINT8 *)Table + Index * Header.SizeOfPartitionEntry, sizeof (Entry));
    if (CompareMem (&Entry.PartitionTypeGUID, &ZeroGuid, sizeof (ZeroGuid)) == 0) { continue; }
    if (Entry.StartingLBA < Header.FirstUsableLBA || Entry.EndingLBA > Header.LastUsableLBA ||
        Entry.StartingLBA > Entry.EndingLBA) {
      Status = EFI_COMPROMISED_DATA;
      goto Exit;
    }
    CopyMem (Name, Entry.PartitionName, sizeof (Entry.PartitionName));
    Name[36] = 0;
    if (Root == NULL) {
      if (Plan->Count == MAX_DUMP_PARTITIONS) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Exit;
      }
      CopyMem (Plan->Part[Plan->Count].Name, Name, sizeof (Name));
      Plan->Part[Plan->Count].Start = Entry.StartingLBA;
      Plan->Part[Plan->Count].End = Entry.EndingLBA;
      Plan->Part[Plan->Count].Matches = 1;
      Plan->Count++;
      continue;
    }
    Status = Log ("gpt_index=%d name=%s start_lba=%Ld end_lba=%Ld\r\n",
                  (UINT32)Index, Name, Entry.StartingLBA, Entry.EndingLBA);
    if (EFI_ERROR (Status)) { goto Exit; }
    for (Wanted = 0; Wanted < Plan->Count; Wanted++) {
      if (SameName (Name, Plan->Part[Wanted].Name)) {
        Plan->Part[Wanted].Matches++;
        Plan->Part[Wanted].Start = Entry.StartingLBA;
        Plan->Part[Wanted].End = Entry.EndingLBA;
      }
    }
  }
  for (Wanted = 0; Wanted < Plan->Count; Wanted++) {
    if (Plan->Part[Wanted].Matches != 1) {
      Log ("Requested name=%s GPT matches=%d (expected one)\r\n",
           Plan->Part[Wanted].Name, (UINT32)Plan->Part[Wanted].Matches);
      Status = EFI_NOT_FOUND;
      goto Exit;
    }
    TableBlocks = Plan->Part[Wanted].End - Plan->Part[Wanted].Start + 1;
    if (TableBlocks > DivU64x32 (MAX_INT64, Plan->BlockSize)) {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Exit;
    }
    Plan->Part[Wanted].Bytes = MultU64x32 (TableBlocks, Plan->BlockSize);
    if (Plan->Total > MAX_INT64 - Plan->Part[Wanted].Bytes) {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Exit;
    }
    Plan->Total += Plan->Part[Wanted].Bytes;
    for (Other = 0; Other < Wanted; Other++) {
      if (Plan->Part[Wanted].Start <= Plan->Part[Other].End &&
          Plan->Part[Other].Start <= Plan->Part[Wanted].End) {
        Status = EFI_COMPROMISED_DATA;
        goto Exit;
      }
    }
  }
  if (!Io->Media->MediaPresent || Io->Media->MediaId != Plan->MediaId ||
      Io->Media->BlockSize != Plan->BlockSize || Io->Media->LastBlock != Plan->LastBlock) {
    Status = EFI_MEDIA_CHANGED;
    goto Exit;
  }
  *Result = Plan;
  Plan = NULL;
  Status = EFI_SUCCESS;
Exit:
  if (HeaderBuffer != NULL) { FreeAlignedPages (HeaderBuffer, HeaderPages); }
  if (Table != NULL) { FreeAlignedPages (Table, TablePages); }
  if (Plan != NULL) { FreePool (Plan); }
  if (EFI_ERROR (Status)) { Log ("Partition plan failed: %r\r\n", Status); }
  return Status;
}
