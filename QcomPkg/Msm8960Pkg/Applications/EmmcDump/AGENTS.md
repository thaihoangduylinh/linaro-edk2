# EmmcDump user preferences

- Always keep EXIT as the last main-menu item when adding or rearranging actions.
- Keep BACK at the bottom of the partition submenu.
- A partition dump returns to the partition submenu after success or failure.
- The user builds this application themselves. Do not run builds or tests unless
  they ask; source inspection and edits are allowed.

- Learn Volume Up, Volume Down and Power once per EFI invocation. Reuse the in-memory map across all menus in that session. Each new EFI invocation must learn again, ignoring any old EmmcDump.keys; save the newly learned map to USB.
