#ifndef EMMC_DUMP_INPUT_H
#define EMMC_DUMP_INPUT_H
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include "Partitions.h"

// Keep EXIT last whenever adding a main-menu action.
enum { MENU_FULL, MENU_PARTITIONS, MENU_GPT, MENU_SECURITY, MENU_MASS_STORAGE,
       MENU_SHUTDOWN, MENU_EXIT, MENU_COUNT };

EFI_STATUS ChooseDumpMode (EFI_FILE_PROTOCOL *Root, UINTN *Mode);
EFI_STATUS ChoosePartition (CONST PARTITION_PLAN *Plan, UINTN *Selected);
#endif
