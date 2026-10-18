# TODO

What emuCasioFxCG still lacks. The syscall list is measured against
[libfxcg](https://github.com/Jonimoose/libfxcg)'s stubs: 193 of its 273 syscalls
are implemented, 80 are left. Add-ins only use a small part of them, so the
groups are ordered by how likely real add-ins are to need them.

Approach that has worked so far: run an add-in, implement what it stops at,
check it on screen. For calls no add-in at hand uses, add a test add-in to
`tests/os-apps` (one source per group: `os-syscalls`, `draw-syscalls`,
`key-modifiers`, `status-bar`, `file-syscalls`, `time-keys`, `language`,
`text-syscalls`, `mcs-system`) and an `os_test` line in `CMakeLists.txt`:
CTest then runs it. Semantics come from WikiPrizm (prizm.cemetech.net),
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
Done (test add-in `tests/os-apps/src/text-syscalls.c`). View > Font picks
the glyphs: Classic (default: the 8x8 font scaled, 2x3 in PrintXY's 18x24
cells, fixed widths), Spleen (12x24, 8x16 proportional for PrintMini and
MMPrint, 5x8 for PrintMiniMini) or Unifont (8x16, scaled); symbols missing
from them come from Unifont. None is Casio's, which differ in shape. Strings stay in the OS character set:
2-byte characters count and edit as one.

Guessed or partial: only the symbols whose codes are known (the GetKey
character codes: pi, theta, r, square root, squared, ->, <=, ...) are
drawn; other Casio characters draw as U+FFFD (Casio's character table
isn't documented); Chinese (GB 18030) is decoded but drawn as empty boxes
(no CJK font embedded); `PrintGlyph` and `PrintMiniGlyph` only understand
`GetMiniGlyphPtr`'s pointers (add-ins' own glyphs leave a blank cell);
`EditMBStringCtrl3` / `4` act like `Ctrl2`; `DispInt` does nothing;
`CharacterSelectDialog` offers the known symbols and ASCII punctuation in
one page.

### Language
Done (test add-in `tests/os-apps/src/language.c`): Language menu / `--language`,
`LocalizeMessage1`, `PrintXY_2`, `APP_SYSTEM_LANGUAGE`. Only the ~40
messages WikiPrizm lists are known (others read `<msg N>`); the Spanish,
German, French, Portuguese and Chinese texts are our translations, except
message 1 (the language's name); accents are dropped, as the OS's codes
for accented letters aren't documented; Chinese is GB 18030, shown once
the text and fonts work decodes it. Where the OS keeps the setting is
unknown: it lasts while the emulator runs.

### Files
Done (test add-in `tests/os-apps/src/file-syscalls.c`) except `SMEM_FindFirst` (0DAC): its
return values are not documented.

Guessed: searches list folders first, then files, sorted by name, without
"." and ".."; a search with no match returns -16 on the fx-CG10/20
(WikiPrizm) and -1 on the fx-CG50 (gint); storage is 16 MB for
`Bfile_GetMediaFree_OS`; `fileinfo.address` is only set for files already
copied into flash (opened before); `Bfile_GetBlockAddress` points at the
copy made when the file was opened.

### Keyboard
SHIFT / ALPHA in `GetKey` are done (test add-in `tests/os-apps/src/key-modifiers.c`). Not
modelled: lowercase alpha (fx-CG50 text entry), the SHIFT functions of
F1–F6 and of 3, 6, `,` and → (they return the plain key), SHIFT AC/ON
switching the calculator off (ignored).

The keyboard syscalls are done (test add-in `tests/os-apps/src/time-keys.c`). Guessed or
partial: `Keyboard_PutKeycode` returns 0 (-1 when its buffer is full);
the Bkey flags are kept but GetKey doesn't act on them; with the return to
the main menu on (the default), GetKey drops MENU and says so on the menu
bar, as there is no main menu; `SpecialMatrixcodeProcessing`,
`Keyboard_CursorFlash` and `SetCursorFlashToggle` do nothing (function
unknown); `FKey_Display` draws an empty label tab (the bitmap format is
unknown, and `GetFKeyPtr` gives blank ones).

### Timing
Done (test add-in `tests/os-apps/src/time-keys.c`). OS timers call the add-in's handler like
an interrupt: between run slices, while GetKey waits (in real time), and
during delays. Guessed: periods are whole 40 ms ticks (25 Hz, nearest, at
least one); a blocking key read first runs a timer due within 50 ms (it
may inject a key, as gint does to reach the main menu); `RTC_Reset` with a
nonzero mode sets the clock to 2000-01-01 00:00:00; `RTC_GetTicks` counts
from the add-in's start, not from midnight.

### Memory and strings
`memcmp3` (115E), `strcmp_ignore_case` (1635), `ItoA_10digit` (1633): their
exact results are not documented. `BCDtoInternal` (0160) is done (sign 1 for
negative numbers, the unknown field 0: guessed).

Guessed: the `*ToHex` calls write uppercase digits without a NUL; `HexTo*`
return 1 on a non-hex character.

## Syscalls, less likely

### Status bar
Drawn (test add-in `tests/os-apps/src/status-bar.c`). Missing: `Bdisp_HeaderText2` /
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
Done (test add-in `tests/os-apps/src/mcs-system.c`). Guessed: main memory holds 61 KB,
counted as the items' sizes; `MCS_GetMainMemoryStart` points at unused RAM
(the MCS isn't laid out in memory); items' type flags (read-only...) aren't
kept; the alpha variables are BCD values in the item @REV2/ALPHAMEM;
`CLIP_Store` returns 0.

### System
Done (test add-in `tests/os-apps/src/mcs-system.c`): `Restart` ends the add-in;
`TakeScreenshot` / `TakeScreenshot2` save VRAM as `\\fls0\CAPTnnnn.bmp`
at once (no dialog); `SMEM_MapIconToExt` draws its own icons (a page with
the extension, coloured by kind, or a folder) and gives message number 0;
`App_InitDlgDescriptor` writes what WikiPrizm describes; `SMEM_maint`
does nothing.

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
1E05, 1E07, 1E0A, 1E17, 1E34); its language page (1E0D) is done.

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
- **Fonts**: Casio's character table (codes above 0x80 and the 2-byte
  characters) and a CJK font for Chinese text.
- **Main menu**: there is no OS main menu to show. In OS add-ins GetKey
  drops MENU (unless the add-in turned the return to the menu off); gint
  add-ins' `gint_osmenu()` returns at once.
