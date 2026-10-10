# S-OFF implementation findings (2026-10-10)

Requested operation: security=0 in PGFS, board_info CID=11111111, replace final five of nine MFG product-ID characters with *, and directly modify UEFI_BS_NV. Preserve first four PID characters (PM2310000 becomes PM23*****). Do not substitute SecurityToggleApp for the requested raw NV operation.

## Verified against the supplied DIAG

DIAG SHA-256: d3a3f65f94465f0b2a42a0b84d9ef15b2ded9163419ad57b2854a309daf9ddd6 (same binary as prior analysis).

- security: see DIAG-HTC8X-security-analysis.md. Must locate the PGFS entry, validate extents and CRC, update data and metadata CRC. Fixed partition offset is not a portable implementation.
- board_info: init at RVA 0x2939C allocates/reads 0x400 bytes, checks a 16-byte signature and product integer at +0x10. CID write paths at 0x29918-0x29934 and 0x29A8C-0x29AA8 copy 8 bytes to +0x14 and call partition writer with 0x400 bytes. This is a finding for this binary, not verified layout for every requested phone.
- local 8X board_info image has the HTC-BOARD-INFO! signature and CID at +0x14.
- local 8X MFG starts with a UTF-16LE product string; a byte-oriented ASCII replacement would corrupt it. Full MFG integrity/replica requirements still need tracing.

## NV comparison (resolved for the inspected profile)

The user identified E:/UEFI_BS_NV as the original HTC 8X image and supplied
D:/Git/WPinternals-2.9.2/UEFI_BS_NV as the reference resource. Both are 256 KiB.
The latter has SecureBoot under global GUID 8BE4DF61-93CA-11D2-AA0D-00E098032B8C
with NAME attributes 3 and a DATA record containing four zero bytes.

The inspected stores have STOR at 0x8400 and 0xFC00, INFO at +0x200, GUID at
INFO+0x200, and BLOC at INFO+0x7000. GUID records are 16-byte GUID plus relative
512-byte table index. BLOC has 56 four-byte entries: low word free bytes, high
word type (0 free, 1 info, 2 GUID, 3 name table, 4 data). BLOC CRC is standard
CRC32 over its full 512 bytes with +0x0C cleared; this calculation matches both
provided images. Whole empty blocks are EMPT with payload length 504 and 0xFF fill.
The two original HTC store copies differ in existing counter bytes, which are
preserved independently. The new code does not clone one store over the other.

SOff.c inserts a global GUID entry, a separate RTBL with the SecureBoot NAME and
one DATA block into two validated empty blocks per store, then recalculates BLOC
CRC. Existing variables and unrelated stores are retained. This is inferred from
binary structure and donor records; no firmware/hardware test confirms effectiveness.

M8 excluded as requested. Common checksum behavior reported by the user does not
remove per-image structure/CRC checks. Unsupported extents or NV variants return
EFI_UNSUPPORTED before any writes. This implementation is an experimental
on-disk patcher, not a claim of verified S-OFF on all three device families.

## Requirements before enabling a writing menu

1. Resolve partitions uniquely using validated GPT and validate all four patch plans before any eMMC writes.
2. Back up full original affected partitions and GPT to a fresh USB directory, flush and reopen/read back backups to verify against source before writing.
3. Refuse unsupported formats, ambiguous partition names, out-of-bounds extents, invalid checksums or changed media.
4. Write only planned blocks, flush eMMC and read back to verify. A partial failure must be reported per partition and must not be described as S-OFF success.
5. Secure Boot effectiveness requires verification after reboot; an NV byte equal to zero is not proof firmware stopped authentication.
6. Keep EXIT last; return to the menu. No builds/tests requested.

Status: SOff.c/SOff.h and menu integration added. Original images are not modified on the host. Source inspection only; no builds or tests run. Read-back verifies disk patches, not post-reboot enforcement state.
