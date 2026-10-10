#ifndef EMMC_DUMP_INPUT_H
#define EMMC_DUMP_INPUT_H
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include "Partitions.h"

// Keep EXIT last whenever adding a main-menu action.
enum { MENU_FULL, MENU_PARTITIONS, MENU_GPT, MENU_SECURITY, MENU_MASS_STORAGE,
       MENU_DIAG, MENU_SOFF, MENU_SHUTDOWN, MENU_EXIT, MENU_COUNT };

EFI_STATUS ChooseDumpMode (EFI_FILE_PROTOCOL *Root, UINTN *Mode);
VOID ResetKeySession (VOID);
EFI_STATUS ChoosePartition (EFI_FILE_PROTOCOL *Root, CONST PARTITION_PLAN *Plan, UINTN *Selected);
#endif
