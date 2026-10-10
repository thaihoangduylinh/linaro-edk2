#ifndef EMMC_SOFF_H
#define EMMC_SOFF_H
#include "Partitions.h"
EFI_STATUS RunSOff (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory,
                    DUMP_LOG Log);
EFI_STATUS RunDisableSecureBoot (EFI_BLOCK_IO_PROTOCOL *Io, EFI_FILE_PROTOCOL *Directory, DUMP_LOG Log);
#endif
