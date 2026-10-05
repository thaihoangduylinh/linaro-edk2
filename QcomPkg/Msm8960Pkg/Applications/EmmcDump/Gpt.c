/** Read-only GPT metadata backup. Sizes come from validated on-disk headers. */
#include "Partitions.h"
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define GPT_MAX_BYTES (4U * 1024U * 1024U)

STATIC EFI_STATUS
ReadCopy (EFI_BLOCK_IO_PROTOCOL *Io, PARTITION_PLAN *Plan, BOOLEAN Backup,
          EFI_PARTITION_TABLE_HEADER *Header, EFI_LBA *TableEnd)
{
  EFI_STATUS Status;
  VOID *Buffer;
  UINTN Pages;
  UINTN Alignment;
  UINTN ReadSize;
  UINT64 Bytes;
  UINT64 Blocks;
  UINT32 Crc;
  EFI_LBA Lba;

  Alignment = MAX ((UINTN)Io->Media->IoAlign, (UINTN)EFI_PAGE_SIZE);
  Pages = EFI_SIZE_TO_PAGES (Plan->BlockSize);
  Buffer = AllocateAlignedPages (Pages, Alignment);
  if (Buffer == NULL) { return EFI_OUT_OF_RESOURCES; }
  Lba = Backup ? Plan->LastBlock : 1;
  Status = Io->ReadBlocks (Io, Plan->MediaId, Lba, Plan->BlockSize, Buffer);
  if (EFI_ERROR (Status)) { goto Exit; }
  CopyMem (Header, Buffer, sizeof (*Header));
  if (Header->Header.Signature != EFI_PTAB_HEADER_ID ||
      Header->Header.Revision != 0x00010000 || Header->Header.Reserved != 0 ||
      Header->Header.HeaderSize < sizeof (*Header) ||
      Header->Header.HeaderSize > Plan->BlockSize || Header->MyLBA != Lba ||
      Header->AlternateLBA != (Backup ? 1 : Plan->LastBlock) ||
      Header->FirstUsableLBA <= 1 || Header->FirstUsableLBA > Header->LastUsableLBA ||
      Header->LastUsableLBA >= Plan->LastBlock ||
      Header->NumberOfPartitionEntries == 0 || Header->NumberOfPartitionEntries > 4096 ||
      Header->SizeOfPartitionEntry < sizeof (EFI_PARTITION_ENTRY) ||
      Header->SizeOfPartitionEntry > 4096 ||
      (Header->SizeOfPartitionEntry & (Header->SizeOfPartitionEntry - 1)) != 0) {
    Status = EFI_COMPROMISED_DATA;
    goto Exit;
  }
  ((EFI_PARTITION_TABLE_HEADER *)Buffer)->Header.CRC32 = 0;
  Status = gBS->CalculateCrc32 (Buffer, Header->Header.HeaderSize, &Crc);
  if (EFI_ERROR (Status)) { goto Exit; }
  if (Crc != Header->Header.CRC32) { Status = EFI_CRC_ERROR; goto Exit; }
  Bytes = MultU64x32 (Header->NumberOfPartitionEntries, Header->SizeOfPartitionEntry);
  if (Bytes > GPT_MAX_BYTES) { Status = EFI_UNSUPPORTED; goto Exit; }
  Blocks = DivU64x32 (Bytes + Plan->BlockSize - 1, Plan->BlockSize);
  if (Backup) {
    if (Header->PartitionEntryLBA <= Header->LastUsableLBA ||
        Header->PartitionEntryLBA >= Plan->LastBlock ||
        Blocks > Plan->LastBlock - Header->PartitionEntryLBA) {
      Status = EFI_COMPROMISED_DATA;
      goto Exit;
    }
  } else {
    if (Header->PartitionEntryLBA < 2 ||
        Header->PartitionEntryLBA >= Header->FirstUsableLBA ||
        Blocks > Header->FirstUsableLBA - Header->PartitionEntryLBA) {
      Status = EFI_COMPROMISED_DATA;
      goto Exit;
    }
  }
  *TableEnd = Header->PartitionEntryLBA + Blocks - 1;
  FreeAlignedPages (Buffer, Pages);
  ReadSize = (UINTN)MultU64x32 (Blocks, Plan->BlockSize);
  Pages = EFI_SIZE_TO_PAGES (ReadSize);
  Buffer = AllocateAlignedPages (Pages, Alignment);
  if (Buffer == NULL) { return EFI_OUT_OF_RESOURCES; }
  Status = Io->ReadBlocks (Io, Plan->MediaId, Header->PartitionEntryLBA, ReadSize, Buffer);
  if (EFI_ERROR (Status)) { goto Exit; }
  Status = gBS->CalculateCrc32 (Buffer, (UINTN)Bytes, &Crc);
  if (!EFI_ERROR (Status) && Crc != Header->PartitionEntryArrayCRC32) {
    Status = EFI_CRC_ERROR;
  }
Exit:
  FreeAlignedPages (Buffer, Pages);
  return Status;
}

EFI_STATUS
LoadGptPlan (EFI_BLOCK_IO_PROTOCOL *Io, PARTITION_PLAN **Result, DUMP_LOG Log)
{
  PARTITION_PLAN *Plan;
  EFI_PARTITION_TABLE_HEADER Primary;
  EFI_PARTITION_TABLE_HEADER Backup;
  EFI_LBA PrimaryEnd;
  EFI_LBA BackupEnd;
  EFI_STATUS Status;
  UINT64 Blocks;
  UINTN Index;
  UINT32 IoAlign;

  *Result = NULL;
  Plan = AllocateZeroPool (sizeof (*Plan));
  if (Plan == NULL) { return EFI_OUT_OF_RESOURCES; }
  Plan->IsGpt = TRUE;
  Plan->MediaId = Io->Media->MediaId;
  Plan->BlockSize = Io->Media->BlockSize;
  Plan->LastBlock = Io->Media->LastBlock;
  IoAlign = Io->Media->IoAlign;
  if (Plan->BlockSize < sizeof (Primary) || Plan->BlockSize > 65536 ||
      Plan->LastBlock < 5 || Plan->LastBlock == MAX_UINT64 ||
      IoAlign > GPT_MAX_BYTES || (IoAlign > 1 && (IoAlign & (IoAlign - 1)) != 0)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }
  Status = ReadCopy (Io, Plan, FALSE, &Primary, &PrimaryEnd);
  if (EFI_ERROR (Status)) { goto Exit; }
  Status = ReadCopy (Io, Plan, TRUE, &Backup, &BackupEnd);
  if (EFI_ERROR (Status)) { goto Exit; }
  if (CompareMem (&Primary.DiskGUID, &Backup.DiskGUID, sizeof (EFI_GUID)) != 0 ||
      Primary.FirstUsableLBA != Backup.FirstUsableLBA ||
      Primary.LastUsableLBA != Backup.LastUsableLBA ||
      Primary.NumberOfPartitionEntries != Backup.NumberOfPartitionEntries ||
      Primary.SizeOfPartitionEntry != Backup.SizeOfPartitionEntry ||
      Primary.PartitionEntryArrayCRC32 != Backup.PartitionEntryArrayCRC32) {
    Status = EFI_COMPROMISED_DATA;
    goto Exit;
  }
  Plan->Count = 2;
  CopyMem (Plan->Part[0].Name, L"GPT-primary", sizeof (L"GPT-primary"));
  Plan->Part[0].Start = 0; // Include the protective MBR and primary header.
  Plan->Part[0].End = PrimaryEnd;
  CopyMem (Plan->Part[1].Name, L"GPT-backup", sizeof (L"GPT-backup"));
  Plan->Part[1].Start = Backup.PartitionEntryLBA;
  Plan->Part[1].End = Plan->LastBlock; // Include the backup header.
  for (Index = 0; Index < Plan->Count; Index++) {
    Blocks = Plan->Part[Index].End - Plan->Part[Index].Start + 1;
    if (Blocks > DivU64x32 (MAX_INT64, Plan->BlockSize)) {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Exit;
    }
    Plan->Part[Index].Bytes = MultU64x32 (Blocks, Plan->BlockSize);
    if (Plan->Total > MAX_INT64 - Plan->Part[Index].Bytes) {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Exit;
    }
    Plan->Total += Plan->Part[Index].Bytes;
  }
  if (!Io->Media->MediaPresent || Io->Media->MediaId != Plan->MediaId ||
      Io->Media->BlockSize != Plan->BlockSize || Io->Media->LastBlock != Plan->LastBlock ||
      Io->Media->IoAlign != IoAlign) {
    Status = EFI_MEDIA_CHANGED;
    goto Exit;
  }
  Status = Log ("GPT headers and entry arrays validated; includes MBR, primary and backup GPT.\r\n"
                "GPT regions use header-defined boundaries, not a fixed 2 MiB.\r\n");
  if (EFI_ERROR (Status)) { goto Exit; }
  *Result = Plan;
  Plan = NULL;
Exit:
  if (Plan != NULL) { FreePool (Plan); }
  if (EFI_ERROR (Status)) { Log ("GPT plan failed: %r\r\n", Status); }
  return Status;
}
