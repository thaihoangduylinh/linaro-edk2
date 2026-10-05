#ifndef EMMC_DUMP_PARTITIONS_H
#define EMMC_DUMP_PARTITIONS_H
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

#define MAX_DUMP_PARTITIONS 64
typedef struct {
  CHAR16 Name[37];
  EFI_LBA Start;
  EFI_LBA End;
  UINT64 Bytes;
  UINTN Matches;
} DUMP_PARTITION;
typedef struct {
  BOOLEAN IsGpt;
  UINTN Count;
  UINT64 Total;
  UINT32 MediaId;
  UINT32 BlockSize;
  EFI_LBA LastBlock;
  DUMP_PARTITION Part[MAX_DUMP_PARTITIONS];
} PARTITION_PLAN;

typedef EFI_STATUS (*DUMP_LOG) (CONST CHAR8 *Format, ...);
EFI_STATUS LoadPartitionPlan (EFI_FILE_PROTOCOL *Root, EFI_BLOCK_IO_PROTOCOL *Io,
                              PARTITION_PLAN **Plan, DUMP_LOG Log);
EFI_STATUS LoadGptPlan (EFI_BLOCK_IO_PROTOCOL *Io, PARTITION_PLAN **Plan, DUMP_LOG Log);
#endif
