/** Physical button mapping is learned, not guessed from GPIO addresses. */
#include "Input.h"
#include "Screen.h"
#include <Protocol/SimpleTextInEx.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define MAX_INPUTS 32
#define KEY_MAGIC SIGNATURE_32 ('E','D','K','1')
typedef struct {
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Basic;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Extended;
} INPUT_SOURCE;
typedef struct {
  UINT32 Magic;
  EFI_INPUT_KEY Key[3]; // volume up, volume down, power
} KEY_MAP;
STATIC INPUT_SOURCE mInputs[MAX_INPUTS];
STATIC UINTN mInputCount;
STATIC KEY_MAP mCachedMap;
STATIC BOOLEAN mHaveSessionMap = FALSE;

VOID
ResetKeySession (VOID)
{
  mHaveSessionMap = FALSE;
}

STATIC BOOLEAN
SameKey (CONST EFI_INPUT_KEY *A, CONST EFI_INPUT_KEY *B)
{
  return (BOOLEAN)(A->ScanCode == B->ScanCode && A->UnicodeChar == B->UnicodeChar);
}

STATIC VOID
AddInput (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Basic, EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Extended)
{
  UINTN Index;
  for (Index = 0; Index < mInputCount; Index++) {
    if ((Basic != NULL && mInputs[Index].Basic == Basic) ||
        (Extended != NULL && mInputs[Index].Extended == Extended)) {
      return;
    }
  }
  if (mInputCount < MAX_INPUTS && (Basic != NULL || Extended != NULL)) {
    mInputs[mInputCount].Basic = Basic;
    mInputs[mInputCount++].Extended = Extended;
  }
}

STATIC VOID
DiscoverInput (VOID)
{
  EFI_HANDLE *Handles;
  EFI_GUID *Guid;
  EFI_STATUS Status;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Basic;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Extended;
  UINTN Count;
  UINTN Index;
  UINTN Pass;

  mInputCount = 0;
  // Prefer Ex on each handle. Also include unconnected button input handles.
  for (Pass = 0; Pass < 2; Pass++) {
    Guid = Pass == 0 ? &gEfiSimpleTextInputExProtocolGuid : &gEfiSimpleTextInProtocolGuid;
    Status = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
    if (EFI_ERROR (Status)) {
      continue;
    }
    for (Index = 0; Index < Count; Index++) {
      Basic = NULL;
      Extended = NULL;
      gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextInputExProtocolGuid, (VOID **)&Extended);
      gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextInProtocolGuid, (VOID **)&Basic);
      AddInput (Basic, Extended);
    }
    FreePool (Handles);
  }
  AddInput (gST->ConIn, NULL);
}

STATIC EFI_STATUS
ReadInput (UINTN Index, EFI_INPUT_KEY *Key)
{
  EFI_KEY_DATA Data;
  EFI_STATUS Status;
  if (mInputs[Index].Extended != NULL) {
    Status = mInputs[Index].Extended->ReadKeyStrokeEx (mInputs[Index].Extended, &Data);
    if (!EFI_ERROR (Status)) {
      *Key = Data.Key;
    }
    return Status;
  }
  return mInputs[Index].Basic->ReadKeyStroke (mInputs[Index].Basic, Key);
}

STATIC VOID
DrainInput (VOID)
{
  EFI_INPUT_KEY Key;
  UINTN Index;
  UINTN Count;
  for (Index = 0; Index < mInputCount; Index++) {
    for (Count = 0; Count < 64; Count++) {
      if (EFI_ERROR (ReadInput (Index, &Key))) {
        break;
      }
    }
  }
}

STATIC EFI_STATUS
WaitInput (EFI_INPUT_KEY *Key)
{
  EFI_STATUS Status;
  EFI_STATUS Error;
  BOOLEAN Pending;
  UINTN Index;
  for (;;) {
    Pending = FALSE;
    Error = EFI_DEVICE_ERROR;
    for (Index = 0; Index < mInputCount; Index++) {
      Status = ReadInput (Index, Key);
      if (!EFI_ERROR (Status) && (Key->ScanCode != 0 || Key->UnicodeChar != 0)) {
        // Discard aliases/queued repeats from the same physical button press.
        gBS->Stall (200000);
        DrainInput ();
        return EFI_SUCCESS;
      }
      if (Status == EFI_NOT_READY || !EFI_ERROR (Status)) {
        Pending = TRUE;
      } else {
        Error = Status;
      }
    }
    if (!Pending) {
      return Error;
    }
    gBS->Stall (10000);
  }
}

STATIC EFI_STATUS
SaveMap (EFI_FILE_PROTOCOL *Root, KEY_MAP *Map)
{
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  UINTN Size;
  Status = Root->Open (Root, &File, L"EmmcDump.keys",
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Size = sizeof (*Map);
  Status = File->Write (File, &Size, Map);
  if (!EFI_ERROR (Status) && Size != sizeof (*Map)) {
    Status = EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }
  CloseStatus = File->Close (File);
  return EFI_ERROR (Status) ? Status : CloseStatus;
}

STATIC VOID
DrawMenu (UINTN Selected)
{
  ScreenClear ();
  ScreenWrite ("EMMCDUMP 1.3\r\n\r\n");
  ScreenWrite (Selected == 0 ? "> 1. FULL DUMP (1 GIB PARTS)\r\n" : "  1. FULL DUMP (1 GIB PARTS)\r\n");
  ScreenWrite (Selected == MENU_PARTITIONS ? "> 2. DUMP PARTITION (GPT MENU)\r\n" : "  2. DUMP PARTITION (GPT MENU)\r\n");
  ScreenWrite (Selected == 2 ? "> 3. GPT (PRIMARY + BACKUP)\r\n" : "  3. GPT (PRIMARY + BACKUP)\r\n");
  ScreenWrite (Selected == MENU_SECURITY ? "> 4. DISABLE SECURE BOOT\r\n" : "  4. DISABLE SECURE BOOT\r\n");
  ScreenWrite (Selected == MENU_MASS_STORAGE ? "> 5. MASSSTORAGE\r\n" : "  5. MASSSTORAGE\r\n");
  ScreenWrite (Selected == MENU_DIAG ? "> 6. DIAG\r\n" : "  6. DIAG\r\n");
  ScreenWrite (Selected == MENU_SOFF ? "> 7. S-OFF\r\n" : "  7. S-OFF\r\n");
  ScreenWrite (Selected == MENU_SHUTDOWN ? "> 8. SHUT DOWN\r\n" : "  8. SHUT DOWN\r\n");
  ScreenWrite (Selected == MENU_EXIT ? "> 9. EXIT\r\n" : "  9. EXIT\r\n");
  ScreenWrite ("\r\nVOLUME UP: UP\r\nVOLUME DOWN: DOWN\r\nPOWER: SELECT\r\n\r\nRelease each button after pressing.\r\n");
}

STATIC EFI_STATUS
ConfirmKeys (EFI_FILE_PROTOCOL *Root)
{
  KEY_MAP Map;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN Index, Previous;
  CHAR8 Text[128];
  STATIC CONST CHAR8 *Prompt[] = {"VOLUME UP", "VOLUME DOWN", "POWER"};
  DiscoverInput ();
  if (mInputCount == 0) {
    ScreenWrite ("No UEFI button/keyboard input protocol.\r\n");
    return EFI_UNSUPPORTED;
  }
  DrainInput ();
  if (mHaveSessionMap) { return EFI_SUCCESS; }
  ZeroMem (&Map, sizeof (Map));
  Map.Magic = KEY_MAGIC;
  ScreenClear ();
  ScreenWrite ("CONFIRM BUTTONS FOR THIS DEVICE\r\nTap briefly, then release.\r\n");
  for (Index = 0; Index < 3; Index++) {
    AsciiSPrint (Text, sizeof (Text), "\r\nPress %a now.\r\n", Prompt[Index]);
    ScreenWrite (Text);
    for (;;) {
      Status = WaitInput (&Key);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      for (Previous = 0; Previous < Index; Previous++) {
        if (SameKey (&Key, &Map.Key[Previous])) {
          break;
        }
      }
      if (Previous == Index) {
        break;
      }
      ScreenWrite ("Already assigned. Release and press the requested button.\r\n");
    }
    Map.Key[Index] = Key;
    AsciiSPrint (Text, sizeof (Text), "Captured scan=%04x unicode=%04x\r\n",
                (UINT32)Key.ScanCode, (UINT32)Key.UnicodeChar);
    ScreenWrite (Text);
  }
  Status = Root == NULL ? EFI_NOT_FOUND : SaveMap (Root, &Map);
  if (EFI_ERROR (Status)) {
    ScreenWrite ("Cannot save EmmcDump.keys; mapping works for this run.\r\n");
    gBS->Stall (2000000);
  }
  mCachedMap = Map;
  mHaveSessionMap = TRUE;
  DrainInput ();
  return EFI_SUCCESS;
}

EFI_STATUS
ChooseDumpMode (EFI_FILE_PROTOCOL *Root, UINTN *Mode)
{
  KEY_MAP Map;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN Selected;
  CHAR8 Text[128];
  Status = ConfirmKeys (Root);
  if (EFI_ERROR (Status)) { return Status; }
  Map = mCachedMap;
  Selected = 0;
  DrawMenu (Selected);
  for (;;) {
    Status = WaitInput (&Key);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (SameKey (&Key, &Map.Key[2])) {
      *Mode = Selected;
      ScreenClear ();
      return EFI_SUCCESS;
    }
    if (SameKey (&Key, &Map.Key[0])) {
      if (Selected > 0) { Selected--; }
      DrawMenu (Selected);
    } else if (SameKey (&Key, &Map.Key[1])) {
      if (Selected + 1 < MENU_COUNT) { Selected++; }
      DrawMenu (Selected);
    } else {
      AsciiSPrint (Text, sizeof (Text), "Unmapped key scan=%04x unicode=%04x\r\n",
                  (UINT32)Key.ScanCode, (UINT32)Key.UnicodeChar);
      ScreenWrite (Text);
    }
  }
}

EFI_STATUS
ChoosePartition (EFI_FILE_PROTOCOL *Root, CONST PARTITION_PLAN *Plan, UINTN *Selected)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN First;
  UINTN Index;
  CHAR8 Text[192];

  Status = ConfirmKeys (Root);
  if (EFI_ERROR (Status)) { return Status; }
  if (*Selected > Plan->Count) { *Selected = 0; }
  DiscoverInput ();
  if (mInputCount == 0) { return EFI_UNSUPPORTED; }
  DrainInput ();
  for (;;) {
    ScreenClear ();
    ScreenWrite ("DUMP PARTITION - GPT\r\n\r\n");
    First = (*Selected == Plan->Count && Plan->Count != 0) ? Plan->Count - 1 : *Selected;
    First = (First / 8) * 8;
    for (Index = First; Index < Plan->Count && Index < First + 8; Index++) {
      AsciiSPrint (Text, sizeof (Text), "%a %d. %s\r\n    %Ld bytes\r\n",
                  *Selected == Index ? ">" : " ", (UINT32)(Index + 1),
                  Plan->Part[Index].Name[0] == 0 ? L"(unnamed)" : Plan->Part[Index].Name,
                  Plan->Part[Index].Bytes);
      ScreenWrite (Text);
    }
    if (Plan->Count == 0) { ScreenWrite ("No populated GPT partitions.\r\n"); }
    // BACK is always the final row, including when the list spans pages.
    ScreenWrite (*Selected == Plan->Count ? "\r\n> BACK\r\n" : "\r\n  BACK\r\n");
    ScreenWrite ("\r\nVOLUME UP/DOWN: MOVE\r\nPOWER: SELECT\r\nUP FROM FIRST: BACK\r\n");
    Status = WaitInput (&Key);
    if (EFI_ERROR (Status)) { return Status; }
    if (SameKey (&Key, &mCachedMap.Key[2])) {
      ScreenClear ();
      return EFI_SUCCESS;
    }
    if (SameKey (&Key, &mCachedMap.Key[0])) {
      *Selected = *Selected == 0 ? Plan->Count : *Selected - 1;
    } else if (SameKey (&Key, &mCachedMap.Key[1])) {
      *Selected = *Selected == Plan->Count ? 0 : *Selected + 1;
    }
  }
}
