#ifndef EMMC_DUMP_INPUT_H
#define EMMC_DUMP_INPUT_H
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>

EFI_STATUS ChooseDumpMode (EFI_FILE_PROTOCOL *Root, UINTN *Mode);
#endif
