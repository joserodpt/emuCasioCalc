# TODO

What emuCasioFxCG still lacks. The syscall list is measured against
[libfxcg](https://github.com/Jonimoose/libfxcg)'s stubs: 139 of its 273 syscalls
are implemented, 134 are left. Add-ins only use a small part of them, so the
groups are ordered by how likely real add-ins are to need them.

Approach that has worked so far: run an add-in, implement what it stops at,
check it on screen. For calls no add-in at hand uses, write a test app in
`~/VSCode/casioPrizmApps` (see `apps/os-syscalls`, which tests the system-info
and dialog calls, and `apps/draw-syscalls`, the drawing and memory/string
calls). Semantics come from WikiPrizm (prizm.cemetech.net),
libfxcg's headers, and the add-in's source when it has one.

## Syscalls, likely to be needed

### Drawing
`Bdisp_AreaClr_DD_x3` (01B6), `DrawFrameWorkbench` (0923) and
`WriteBackground` (1EF7): parameters not documented.

Implemented, but guessed where WikiPrizm is silent: `Bdisp_WriteGraph*` with
4-bit bitmaps use the 8 PrintXY colours (the real palette is unknown), and
"halftone" draws every other pixel; the `_WB` variants act like the plain
ones; `Bdisp_AreaClr` and `Bdisp_WriteGraph*` take VRAM coordinates (no
status bar offset).

### Text and fonts
`Bdisp_MMPrint` (0D09), `Bdisp_MMPrintRef` (0D08), `PrintGlyph` (0238),
`PrintLine2` (1865), `DisplayMBString` (121D), `EditMBStringCtrl` (120E),
`EditMBStringCtrl3` (120B), `EditMBStringCtrl4` (120C), `ProcessPrintChars`
(1300), `CharacterSelectDialog` (07A2), `DispInt` (0259), `LocalizeMessage1`
(12FC), `MB_IsLead` (1161), `MB_ElementCount` (1163), `EnableGB18030` /
`DisableGB18030` (01E0, 01DF).

Also: the OS fonts are approximated by an 8x8 font (PrintXY 18x24, PrintMini,
PrintMiniMini); multi-byte characters show as `?`; bold is not drawn.

### Files
Done (test app `apps/file-syscalls`) except `SMEM_FindFirst` (0DAC): its
return values are not documented.

Guessed: searches list folders first, then files, sorted by name, without
"." and ".."; a search with no match returns -16 on the fx-CG10/20
(WikiPrizm) and -1 on the fx-CG50 (gint); storage is 16 MB for
`Bfile_GetMediaFree_OS`; `fileinfo.address` is only set for files already
copied into flash (opened before); `Bfile_GetBlockAddress` points at the
copy made when the file was opened.

### Keyboard
SHIFT / ALPHA in `GetKey` are done (test app `apps/key-modifiers`). Not
modelled: lowercase alpha (fx-CG50 text entry), the SHIFT functions of
F1–F6 and of 3, 6, `,` and → (they return the plain key), SHIFT AC/ON
switching the calculator off (ignored). The status bar's S / A indicator
is on the menu bar instead.

`Keyboard_PutKeycode` (12C6), `Keyboard_SpyMatrixCode` (12C9), `Bkey_ClrAllFlags`
(0111), `Bkey_SetFlag` (0112), `Bkey_GetAllFlags` (0EA0), `Bkey_SetAllFlags`
(0EA1), `GetGetkeyToMainFunctionReturnFlag` (1E99),
`EnableGetkeyToMainFunctionReturn` (1EA6), `DisableGetkeyToMainFunctionReturn`
(1EA7), `SpecialMatrixcodeProcessing` (1E60), `Keyboard_CursorFlash` (08CA),
`SetCursorFlashToggle` (08D2), `FKey_Display` (0131).

### Timing
`RTC_Elapsed_ms` (02C2), `RTC_Reset` (02BF), `RTC_SetDateTime` (11DE),
`CMT_Delay_micros` (11D6), `CMT_Delay_100micros` (11D7).

OS timers: `Timer_Install` (08D9) is answered with "no timer free";
`Timer_Start` / `Timer_Stop` / `Timer_Deinstall` (08DB, 08DC, 08DA) are
missing. Doing them properly means calling the add-in's handler
periodically, like an interrupt.

### Memory and strings
`memcmp3` (115E), `strcmp_ignore_case` (1635), `ItoA_10digit` (1633): their
exact results are not documented. `BCDtoInternal` (0160): needs the OS's
BCD number format.

Guessed: the `*ToHex` calls write uppercase digits without a NUL; `HexTo*`
return 1 on a non-hex character.

## Syscalls, less likely

### Status bar
Drawn (test app `apps/status-bar`). Missing: `Bdisp_HeaderText2` /
`Bdisp_HeaderFill2` (1D85, 1D87), `APP_EACT_StatusIcon` (0A87),
`SetupMode_StatusIcon` (0A8B): parameters unknown. `KeyboardIcon`, `LineIcon`,
`RadIcon`, `RealIcon`, `NormIcon`, `d_c_Icon` are accepted and ignored
(parameters unknown; the `SAF_*` flags draw those items).

Guessed: the look of the area (layout, the battery's 3 bars and their
voltages, the S / A indicator), the two header colours as a top-to-bottom
gradient, and the setup items, which show the angle unit from the setup
and fixed "Math", "d/c", "Real", "Norm1" for the rest.

### Dialogs (not documented well enough to implement yet)
`Box` (092B) and `Box2` (17FC): "function unknown" on WikiPrizm.
`StandardScrollbar` (0C5A): its structure is unknown. `MsgBoxMoveWB` (0938):
direction values unknown. `OpenFileDialog` (17E9), `SaveFileDialog` (0C66):
g3p file dialogs, return values unknown. `ConfirmFileOverwriteDialog` (1802),
`OverwriteConfirmation` (0D91): return values unknown.
`AUX_DisplayErrorMessage` (0C01), `Bdisp_WriteSystemMessage` (1906).

### Main memory (MCS)
`MCS_WriteItem` (151A), `MCS_DeleteDirectory` (1516), `MCS_GetState` (1529),
`MCS_GetCapa` (1532), `MCS_GetMainMemoryStart` (1543), `MCSGetOpenItem` (1560),
`Alpha_GetData` / `Alpha_SetData` (0034, 0035: alpha variables, value format
to check), `CLIP_Store` (07E5).

### System
`App_InitDlgDescriptor` (0D79, parameters unknown), `Restart` (1187),
`TakeScreenshot` / `TakeScreenshot2` (17E6, 17E7), `SMEM_MapIconToExt`
(0C2C), `SMEM_maint` (0DDF).

## Syscalls, unlikely

### Link cable and USB
Serial: `Serial_Close` (1BB8), `Serial_ReadSingle` (1BB9), `Serial_Peek`
(1BBB), `Serial_WriteSingle` (1BBC), `Serial_WriteUnbuffered` (1BBD),
`Serial_ClearRX` / `ClearTX` (1BC1, 1BC2); the implemented ones behave as if
nothing is plugged in. USB: `USB_Open` / `Close` / `IsOpen` (134E, 1350,
1ED9), `USB_Read` / `ReadSingle` / `Write` / `WriteSingle` / `Peek` (1EDE,
1EDD, 1EDB, 1EDA, 1EDF), `USB_PollRX` / `PollTX` (1EE0, 1EE1), `USB_ClearRX` /
`ClearTX` (1EE2, 1EE3). Protocol: `Comm_Open` / `Close` / `Terminate` /
`TryCheckPacket` (1353, 1354, 13F1, 1396), `App_LINK_*` (1397-1399, 13A6,
13A7, 1409, 140A), `APP_LINK_transmit_select_dialog` (1384).

### Built-in applications
Casio's own apps are not emulated: `APP_RUNMAT` (1A03), `APP_Program` (1945),
`APP_FINANCE` (0C6B), `APP_MEMORY` (1632), `APP_SYSTEM` and its pages (1E13,
1E05, 1E07, 1E0A, 1E0D, 1E17, 1E34).

### Low-level display
`Bdisp_ShapeBase` (01C7), `Bdisp_ShapeBase3XVRAM` (01BE), `Bdisp_ShapeToDD`
(01C0), `Bdisp_ShapeToVRAM16C` (01C4), `Bdisp_DDRegisterSelect` (01A2),
`Bdisp_DefineDMARange` (01A3), `Bdisp_WriteDDRegister3_bit7` (01A6).

### Undocumented / test entry points
`Test01A8`, `Test127E`, `Test17BC`, `Test1B0E`, `Test1E44`, `TestMode`
(0EA7), `Syscall02A4`, `Syscall02A7`, `Syscall02A9`, `Syscall1816`.

## Beyond syscalls

- **gint add-ins**: USB (fxlink), the SPU (sound), DSP arithmetic
  instructions, the FPU-less soft-float paths are fine; overclocking tools
  that reprogram the CPG mostly work but are untested.
- **Timing**: syscall, uncached-memory and DMA costs are estimates; the
  fx-CG10/20 clock (58.98 MHz) is from memory, not measured.
- **Model detection** is a guess (build date, then memory addresses);
  Emulation > Model overrides it.
- **Fonts**: real Casio fonts instead of the 8x8 approximation.
- **Status bar**: draw it (battery, SHIFT/ALPHA, the add-in's text).
- **Main menu**: MENU does nothing in OS add-ins and returns at once in gint
  ones; there is no OS main menu to show.
