/** @file
  Small ASCII log display drawn with GOP Blt, including PixelBltOnly modes.
  Does not require GraphicsConsoleDxe, HII fonts or a working text console.
**/
#include "Screen.h"
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

// Five-bit rows, seven rows per glyph. Lowercase is displayed as uppercase.
STATIC CONST CHAR8 mCharacters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:;!?-_=+/\\()[]<>%#*'\"";
STATIC CONST UINT8 mFont[][7] = {
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, // AB
  {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30}, // CD
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, // EF
  {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, // GH
  {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12}, // IJ
  {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31}, // KL
  {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, // MN
  {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16}, // OP
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, // QR
  {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, // ST
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, // UV
  {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17}, // WX
  {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}, // YZ
  {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, // 01
  {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30}, // 23
  {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30}, // 45
  {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, // 67
  {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}, // 89
  {0,0,0,0,0,0,0}, {0,0,0,0,0,6,6}, // space .
  {0,0,0,0,6,6,4}, {0,6,6,0,6,6,0}, // , :
  {0,6,6,0,6,6,4}, {4,4,4,4,4,0,4}, // ; !
  {14,17,1,2,4,0,4}, {0,0,0,31,0,0,0}, // ? -
  {0,0,0,0,0,0,31}, {0,0,31,0,31,0,0}, // _ =
  {0,4,4,31,4,4,0}, {1,2,2,4,8,8,16}, // + /
  {16,8,8,4,2,2,1}, {2,4,8,8,8,4,2}, // backslash (
  {8,4,2,2,2,4,8}, {14,8,8,8,8,8,14}, // ) [
  {14,2,2,2,2,2,14}, {2,4,8,16,8,4,2}, // ] <
  {8,4,2,1,2,4,8}, {17,2,4,8,16,17,0}, // > %
  {10,10,31,10,31,10,10}, {0,21,14,31,14,21,0}, // # *
  {4,4,8,0,0,0,0}, {10,10,0,0,0,0,0} // ' "
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mLine;
STATIC CHAR8 *mCells;
STATIC UINTN mColumns;
STATIC UINTN mRows;
STATIC UINTN mScale;
STATIC UINTN mRow;
STATIC UINTN mColumn;
STATIC EFI_STATUS mGopStatus;
STATIC EFI_STATUS mTextStatus;

STATIC VOID
TextConsole (CONST CHAR8 *Text)
{
  // OutputString returns an actual status; Print() returns only a character count.
  CHAR16 Buffer[128];
  UINTN Index;

  if (gST->ConOut == NULL) {
    mTextStatus = EFI_NOT_FOUND;
    return;
  }
  while (*Text != '\0') {
    for (Index = 0; Index < sizeof (Buffer) / sizeof (Buffer[0]) - 1 && *Text != '\0'; Index++) {
      Buffer[Index] = (UINT8)*Text++;
    }
    Buffer[Index] = 0;
    mTextStatus = gST->ConOut->OutputString (gST->ConOut, Buffer);
    if (EFI_ERROR (mTextStatus)) {
      break;
    }
  }
}

STATIC EFI_STATUS
DrawRow (UINTN Row, CONST CHAR8 *Text, BOOLEAN Progress)
{
  UINTN Column;
  UINTN Glyph;
  UINTN X;
  UINTN Y;
  UINTN Width;
  UINTN Height;
  CHAR8 Ch;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixel;

  Width = mColumns * 6 * mScale;
  Height = 8 * mScale;
  ZeroMem (mLine, Width * Height * sizeof (*mLine));
  for (Column = 0; Column < mColumns && Text[Column] != '\0'; Column++) {
    Ch = Text[Column];
    if (Ch >= 'a' && Ch <= 'z') {
      Ch -= 'a' - 'A';
    }
    for (Glyph = 0; Glyph < sizeof (mCharacters) - 1; Glyph++) {
      if (mCharacters[Glyph] == Ch) {
        break;
      }
    }
    if (Glyph == sizeof (mCharacters) - 1) {
      Glyph = 42; // '?' for unsupported characters
    }
    for (Y = 0; Y < 7 * mScale; Y++) {
      for (X = 0; X < 5 * mScale; X++) {
        if ((mFont[Glyph][Y / mScale] & (16 >> (X / mScale))) != 0) {
          Pixel = &mLine[Y * Width + Column * 6 * mScale + X];
          Pixel->Red = Progress ? 96 : 240;
          Pixel->Green = 240;
          Pixel->Blue = Progress ? 128 : 240;
        }
      }
    }
  }
  mGopStatus = mGop->Blt (mGop, mLine, EfiBltBufferToVideo, 0, 0,
                           0, Row * Height, Width, Height,
                           Width * sizeof (*mLine));
  return mGopStatus;
}

VOID
ScreenRelease (VOID)
{
  if (mLine != NULL) {
    FreePool (mLine);
    mLine = NULL;
  }
  if (mCells != NULL) {
    FreePool (mCells);
    mCells = NULL;
  }
  mGop = NULL;
}

STATIC EFI_STATUS
TryGraphics (EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop)
{
  UINTN Index;
  UINTN Width;
  UINTN Height;
  EFI_STATUS Status;

  if (Gop == NULL || Gop->Mode == NULL || Gop->Blt == NULL) {
    return EFI_UNSUPPORTED;
  }
  if (Gop->Mode->Info == NULL) {
    if (Gop->Mode->MaxMode == 0) {
      return EFI_UNSUPPORTED;
    }
    Status = Gop->SetMode (Gop, 0);
    if (EFI_ERROR (Status) || Gop->Mode->Info == NULL) {
      return EFI_UNSUPPORTED;
    }
  }
  Width = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  if (Width < 120 || Height < 80 || Width > 8192 || Height > 8192) {
    return EFI_UNSUPPORTED;
  }
  mScale = (Width >= 480 && Height >= 480) ? 2 : 1;
  mColumns = MIN (Width / (6 * mScale), 160);
  mRows = MIN (Height / (8 * mScale), 120) - 1; // Last row is progress.
  mLine = AllocateZeroPool (mColumns * 6 * mScale * 8 * mScale * sizeof (*mLine));
  mCells = AllocateZeroPool ((mColumns + 1) * mRows);
  if (mLine == NULL || mCells == NULL) {
    ScreenRelease ();
    return EFI_OUT_OF_RESOURCES;
  }
  mGop = Gop;
  for (Index = 0; Index <= mRows; Index++) {
    Status = DrawRow (Index, "", FALSE);
    if (EFI_ERROR (Status)) {
      ScreenRelease ();
      return Status;
    }
  }
  mRow = 0;
  mColumn = 0;
  return EFI_SUCCESS;
}

VOID
ScreenInit (VOID)
{
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_STATUS Status;

  mGopStatus = EFI_NOT_FOUND;
  mTextStatus = EFI_NOT_STARTED;
  if (gST->ConsoleOutHandle != NULL) {
    Status = gBS->HandleProtocol (gST->ConsoleOutHandle,
                                 &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
    if (!EFI_ERROR (Status)) {
      mGopStatus = TryGraphics (Gop);
      if (!EFI_ERROR (mGopStatus)) {
        return;
      }
    }
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                   NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid,
                                   (VOID **)&Gop);
      if (!EFI_ERROR (Status)) {
        mGopStatus = TryGraphics (Gop);
        if (!EFI_ERROR (mGopStatus)) {
          break;
        }
      }
    }
    FreePool (Handles);
  }
  if (mGop == NULL && gST->ConOut != NULL) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }
}

STATIC VOID
NextRow (VOID)
{
  UINTN Index;

  mColumn = 0;
  mRow++;
  if (mRow == mRows) {
    // Redraw from our backing text. No overlapping VideoToVideo requirement.
    CopyMem (mCells, mCells + mColumns + 1, (mRows - 1) * (mColumns + 1));
    ZeroMem (mCells + (mRows - 1) * (mColumns + 1), mColumns + 1);
    mRow--;
    for (Index = 0; Index < mRows; Index++) {
      if (EFI_ERROR (DrawRow (Index, mCells + Index * (mColumns + 1), FALSE))) {
        return;
      }
    }
  }
}

VOID
ScreenWrite (CONST CHAR8 *Text)
{
  CONST CHAR8 *Original;

  Original = Text;
  if (mGop == NULL || EFI_ERROR (mGopStatus)) {
    TextConsole (Text);
    return;
  }
  while (*Text != '\0') {
    if (*Text == '\r') {
      Text++;
      continue;
    }
    if (*Text == '\n' || mColumn == mColumns) {
      if (EFI_ERROR (DrawRow (mRow, mCells + mRow * (mColumns + 1), FALSE))) {
        break;
      }
      NextRow ();
      if (EFI_ERROR (mGopStatus)) {
        break;
      }
      if (*Text == '\n') {
        Text++;
        continue;
      }
    }
    mCells[mRow * (mColumns + 1) + mColumn++] = *Text++;
  }
  if (!EFI_ERROR (mGopStatus)) {
    DrawRow (mRow, mCells + mRow * (mColumns + 1), FALSE);
  }
  if (EFI_ERROR (mGopStatus)) {
    TextConsole (Original);
  }
}

VOID
ScreenProgress (UINT64 Done, UINT64 Total)
{
  CHAR8 Text[100];

  AsciiSPrint (Text, sizeof (Text), "Copied %Ld / %Ld MiB",
              RShiftU64 (Done, 20), RShiftU64 (Total, 20));
  if (mGop != NULL && !EFI_ERROR (mGopStatus)) {
    DrawRow (mRows, Text, TRUE);
  } else {
    TextConsole ("\r");
    TextConsole (Text);
  }
}

VOID
ScreenDescription (CHAR8 *Text, UINTN Size)
{
  AsciiSPrint (Text, Size, "display=%a gop_status=%r text_status=%r cols=%d rows=%d",
              (mGop != NULL && !EFI_ERROR (mGopStatus)) ? "GOP" : "ConOut",
              mGopStatus, mTextStatus, (UINT32)mColumns, (UINT32)mRows);
}
