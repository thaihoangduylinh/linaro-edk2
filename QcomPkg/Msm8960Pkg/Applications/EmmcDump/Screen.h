#ifndef EMMC_DUMP_SCREEN_H
#define EMMC_DUMP_SCREEN_H

#include <Uefi.h>

VOID ScreenInit (VOID);
VOID ScreenWrite (CONST CHAR8 *Text);
VOID ScreenProgress (UINT64 Done, UINT64 Total);
VOID ScreenDescription (CHAR8 *Text, UINTN Size);
VOID ScreenRelease (VOID);

#endif
