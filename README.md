# emuCasioFxCG

An emulator for Casio fx-CG50 add-ins (`.g3a`) on macOS and Linux.

It does not boot the calculator's OS. It runs the add-in's SH4AL-DSP machine
code on an interpreter and handles two kinds of add-ins:

- **OS-based add-ins** (libfxcg / PrizmSDK): whenever the add-in calls the OS
  (jumps to `0x80020070` with a syscall number in `r0`), that syscall is
  implemented on the host instead (high-level emulation).
- **gint add-ins** (most modern Planète Casio add-ins, built with the fxSDK):
  gint takes over the hardware, so the emulator models the parts of the
  SH7305 it drives: interrupt controller, timers, RTC, DMA, key scan
  interface, clock registers, exceptions and the R61524 display controller.

## Build and run

Needs CMake, SDL3 and Python 3 (`brew install cmake sdl3`; on Linux, SDL3 from
your package manager or from source, then pass `-DCMAKE_PREFIX_PATH=<prefix>`).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure

./build/emucasiofxcg build/hello.g3a            # window; ESC = EXIT key
./build/emucasiofxcg --trace some-addin.g3a     # log every syscall
./build/emucasiofxcg --trace-hw some-addin.g3a  # log exceptions, DMA, unmodelled I/O
./build/emucasiofxcg --headless --keys EXE,EXIT --screenshot out.bmp some-addin.g3a
./build/emucasiofxcg --icon icon.bmp some-addin.g3a     # also save the add-in's menu icon
./build/emucasiofxcg --info some-addin.g3a              # open the add-in info window at start
./build/emucasiofxcg --model cg20 some-addin.g3a        # force the fx-CG10/20 (default: detect)
./build/emucasiofxcg --timing fast some-addin.g3a       # no OS/memory/DMA delays (default: realistic)
./build/emucasiofxcg --speed 50 some-addin.g3a          # half speed (10-400, or max)

./run.sh                  # rebuilds if the sources changed, opens an empty window (File > Open)
./run.sh some-addin.g3a   # same, starting that add-in (quote paths with spaces)
```

The window has a menu bar: **File** (open another add-in, add-in info, save a
screenshot or the add-in's icon), **Emulation** (pause, reset, model, timing), **View**
(zoom 1x-6x, 4x at start) and **Help**, plus an **Info** button that opens a window with the
add-in's name, header fields and icon (hover the icon to see its selected
version), and its controls: the calculator keys it uses with the PC key for
each, when they can be found in its code (add-ins using the OS's GetKey), or
the whole key table otherwise. Shortcuts: Ctrl+O, Ctrl+I, Ctrl+S, Ctrl+P, Ctrl+R, Ctrl+1..6, Ctrl+Q.
When the add-in exits, the window stays open so you can reset or open another.
If it stops with an error, the error is printed on the terminal and shown on
the screen in a red-bordered box.

Saved data goes to a `storage` folder in the directory you run the emulator
from: files in the storage memory (`Bfile_*` syscalls, paths like
`\\fls0\GAME.sav`) under `storage/fls0/`, main memory items (`MCS*`
syscalls) under `storage/mcs/<folder>/<item>`. Files an add-in opens are also
copied into the emulated flash, where they live on the calculator: some
add-ins read them from there directly (CGDoom finds `doom.wad` that way, so
put it in `storage/fls0/doom.wad`).

**Copy add-in to calculator** (File menu): plug the calculator in by USB and
choose "USB Flash" (F1) on it; it shows up as a drive. The folder dialog
opens on that drive when one with an `@MainMem` folder is mounted; pick it
and the running .g3a is copied to its root (asking before replacing a file
of the same name or writing to a folder that isn't a calculator), then the
drive is ejected (`udisksctl` or `gio` on Linux, `diskutil` on macOS) so the
cable can be unplugged; the add-in then appears in the main menu. Every step
is logged on the terminal (`copy to calculator: ...`).

The calculator's USB storage (fx-CG50, `07cf:6103`) fails any transfer over
4 kB, while Linux sends up to 120 kB at once, so ordinary copies to it fail
with "Input/output error" (the kernel log shows `DID_ERROR`). The emulator
sends each 4 kB block on its own. To fix copies from any program, limit
transfers for the calculator (as root), then plug it in again:

```sh
echo 'ACTION=="add|change", SUBSYSTEM=="block", ENV{DEVTYPE}=="disk", ATTRS{idVendor}=="07cf", ATTR{queue/max_sectors_kb}="4"' \
  | sudo tee /etc/udev/rules.d/90-casio-calculator.rules
sudo udevadm control --reload
```

Keyboard (all 50 calculator keys): arrows, Enter = EXE, Esc = EXIT,
Backspace = DEL, Delete = AC, Home = MENU, Tab = SHIFT, Left Alt = ALPHA,
O = OPTN, V = VARS, F1–F6, digits, `. + - * / ( ) ,`, E = ×10^x, U = (-),
X = X,θ,T, L = log, N = ln, S/C/T = sin/cos/tan, F = a b/c, D = F↔D,
A = →, Q = x², P = ^ (Help > Keyboard controls lists them).

The status row under the menu bar shows SHIFT, ALPHA, the battery, the
message language and the add-in's status message. The Language menu (or
`--language en|es|de|fr|pt|zh`) sets the calculator's message language:
add-ins read it with `LocalizeMessage1(1)` (the language's name), and the
OS messages (`PrintXY_2`, `LocalizeMessage1`) follow it. Click the battery, or use Emulation > Battery, to
set it to full, half, low or empty (`--battery low` or `--battery 4.6` at
start); add-ins see it in `GetMainBatteryVoltage` and on the OS status bar,
which `DisplayStatusArea` and `GetKey` draw at the top of the screen.

In OS add-ins, `GetKey` handles SHIFT and ALPHA like the calculator: the
next key returns its yellow or red function (ALPHA 7 = `M`, SHIFT EXIT =
QUIT), and SHIFT then ALPHA locks ALPHA. With
ALPHA on, PC letters and space type those characters directly. gint add-ins
read the keyboard themselves and see SHIFT and ALPHA as plain keys; the
window follows them with the OS's rules, which most add-ins share, so PC
letters work there too.

`--keys` works for both kinds of add-ins: OS `GetKey` calls pop the keys in
order; for gint add-ins each key is held down for 100 ms of emulated time
while the add-in reads the keyboard (not while it is in the OS world, where
its `GetKey` calls take them).
Headless gint runs stop 0.5 s (emulated) after the last scripted key.

### gint test add-ins

`tests/gint-hello` (display, keyboard, timers, RTC), `tests/gint-stress`
(rendering through gint's DSP-loop blitters, heap, frame timing, and F1 for a
deliberate address error that gint's panic screen must catch) and
`tests/gint-files` (`Bfile_*` file operations) are fxSDK projects. When `fxsdk` is installed (on the PATH, or in `~/.local/bin` as
GiteaPC puts it), the CMake build compiles them and CTest runs them.

## Layout

| Folder / file | What it does |
|---|---|
| `src/main.c` | Command line, main loop (runs the add-in, handles Open/Reset/Quit) |
| **`src/engine/`** | **The emulated machine** |
| `cpu.c` | SH4AL-DSP integer interpreter: privileged mode, register banks, exceptions and interrupts through VBR, SLEEP, DSP repeat loops (no DSP arithmetic, no FPU) |
| `mem.c` | 4 kB page table over the 32-bit address space, MMIO devices, faults |
| `sh7305.c` | On-chip peripherals: INTC, TMU/ETMU, RTC, DMA, KEYSC, CPG, MMU registers, UTLB arrays |
| `r61524.c` | Display controller (396x224 panel) |
| `emu.c` | Builds the machine (memory map, devices), key script |
| **`src/engine/os/`** | **High-level emulation of the Casio OS** |
| `syscalls.c` | Syscall table and system calls (memory, strings, time, settings) |
| `messages.c` | OS messages in the six languages (table generated by `tools/gen_messages.py`) |
| `syscalls_status.c` | The status area: battery, SHIFT/ALPHA, message, glyphs |
| `syscalls_draw.c` | Drawing: fills, rectangles, points, bitmaps (`Bdisp_WriteGraph*`), the display (`_DD`) |
| `syscalls_text.c` | Text: PrintXY/PrintMini/PrintCXY, message boxes, VRAM save/load |
| `syscalls_dialogs.c` | OS dialogs: message box, colour picker, progress bars, scrollbar |
| `syscalls_keys.c` | Keyboard: GetKey, GetKeyWait_OS, PRGM_GetKey_OS, line editing |
| `syscalls_bfile.c` | Storage memory files (`Bfile_*`: files, folders, searches, free space), stored in `./storage/fls0` |
| `syscalls_mcs.c` | Main memory (MCS) items, stored in `./storage/mcs` |
| `display.c` | VRAM drawing used by the syscalls |
| **`src/views/`** | **Windows (SDL3)** |
| `window.c` | Main window, keyboard, the `platform.h` interface |
| `menubar.c` | Menu bar: menus, Info button, shortcuts, actions |
| `info_window.c` | Add-in information window |
| `send_addin.c` | File > Copy add-in to calculator |
| `app_icon.c`, `font.c` | The emulator's icon (drawn in code), the 8x8 font |
| **`src/utils/`** | **Helpers** |
| `g3a.c` | Reads a .g3a: header (name, icons...) and code, mapped at `0x00300000` |
| `keys.c` | Calculator keys: names, GetKey codes, key matrix positions |
| `tests/test_cpu.c` | CPU tests (division, delay slots, carries, shifts, repeat loops, TRAPA/RTE/banks) |
| `tests/gint-*` | gint add-ins used as integration tests |
| `tools/make_hello_g3a.py` | Hand-assembles the `hello.g3a` smoke test (HLE) |

## Models

The fx-CG10, fx-CG20 and fx-CG50 share the CPU, the display and the add-in
format, so most add-ins run on all of them. They differ in where RAM is, how
much there is (2 MB vs 8 MB), where the OS puts VRAM and the stack, and the
clock (58.98 MHz vs 117.96 MHz CPU). The emulator can be either:

- **Automatic** (default): g3a files have no "made for" field, so the model is
  guessed. The build date in the header decides when there is one (before
  2017, when the fx-CG50 came out: fx-CG10/20). Without a date, code that
  refers to fx-CG50 memory runs as an fx-CG50, code that draws straight to
  the fx-CG10/20 VRAM address as an fx-CG10/20, anything else as an fx-CG50.
- **Forced**: Emulation > Model, or `--model cg20` / `--model cg50`.

The model and the reason it was picked appear in the window title, in
Add-in info and on the terminal.

## Memory map

Physical memory: ROM at `0x00000000` (32 MB), RAM at `0x0C000000` (8 MB, fx-CG50)
or `0x08000000` (2 MB, fx-CG10/20; see `Layout` in `src/engine/emu.c`), the
display at `0x14000000`, reachable through P1
(`0x8xxxxxxx`) and P2 (`0xAxxxxxxx`). The MMU is not emulated: the U0
mappings the OS would make are fixed, and described in the UTLB arrays for
gint.

Addresses on the fx-CG50 (fx-CG10/20 in brackets):

| Address | What |
|---|---|
| `0x00300000` | add-in code (in ROM; writable, some add-ins patch their own data) |
| `0x08100000` | add-in static RAM, 512 kB (= physical `0x0C200000` [`0x08180000`]) |
| `0x8C000000` [`0x88000000`] | RAM through P1; gint borrows `0x8C0F0000` [`0x880F0000`] for its VRAMs |
| `0xAC000000` [`0xA8000000`] | OS VRAM (384x216, `GetVRAMAddress`); on the fx-CG50 `0xA8000000` is aliased to it for old add-ins |
| `0x8C180000` [`0x88170000`] | initial stack pointer (gint tells the model from it) |
| `0xAC030000` [`0xA8030000`] | secondary VRAM (`SaveVRAM_1`, `GetSecondaryVRAMAddress`) |
| `0x8C400000` [`0x88060000`] | heap for `sys_malloc` (HLE) |
| `0xE5200000`, `0xE5000000` | on-chip IL and X/Y memory |
| `0xA4xxxxxx`, `0xFxxxxxxx` | peripheral registers; unknown ones read back what was written |

## Timing

One instruction counts as one CPU cycle at the model's default clock (117.96
MHz on the fx-CG50, 58.98 MHz on the fx-CG10/20), read from the emulated CPG
registers. Timers and the RTC run from that count, `SLEEP` skips ahead to the
next timer interrupt, and the window keeps emulated time in step with real
time.

Two timing modes (Emulation > Timing, or `--timing`):

- **Realistic** (default): things the calculator spends time on take emulated
  time too. Each OS syscall costs a few microseconds (key polls about 20 µs),
  `Bdisp_PutDisp_DD` and `Bdisp_AllClr_VRAM` cost what their DMA transfer
  does, accesses to uncached memory (the OS's VRAM at `0xAC000000`) stall
  the CPU, and DMA transfers report completion after roughly the time real
  hardware takes (a full-screen `dupdate()` is about 11 ms). Add-ins that run
  "as fast as they can" then run at about their real speed. The figures are
  estimates (constants in `emu.c`, `sh7305.h`, `syscalls*.c`), not
  measurements.
- **Fast**: all of the above takes no time; add-ins run as fast as the
  interpreter allows.

Independently of the mode, the speed sets how fast emulated time runs
compared with real time. It is 100% unless you pick Emulation > Speed:
custom, which shows a slider under the menus (10% to 400%, or Max for no
limit); Speed: normal hides it again. Ctrl+= / Ctrl+- also open the slider
and change the speed, Ctrl+0 goes back to normal, and `--speed` sets it from
the command line.

## How to grow it

For OS-based add-ins the loop is: run an add-in, hit `unimplemented syscall
0x....`, look it up, implement it, repeat. References:
- WikiPrizm syscall list: https://prizm.cemetech.net/Syscalls/
- libfxcg headers and stubs: https://github.com/Jonimoose/libfxcg

For gint add-ins, run with `--trace-hw`: it reports CPU exceptions, DMA
transfers and the first access to every peripheral page we don't model. The
gint sources (`include/gint/mpu/*.h`, `src/*/`) document each register.

Next steps:
1. **More HLE syscalls** (see `TODO.md`): text and fonts, main memory
   (MCS), the OS's localised messages (`PrintXY_2` shows message numbers).
2. **Text input**: lowercase alpha and the multi-byte character set.
3. **Real font** for PrintXY (18x24) and PrintMini.
4. **Proper `free`/`realloc`** in the HLE heap (it is a bump allocator).
5. **USB** (fxlink), the SPU, and overclocking tools if add-ins need them.

### Known limits

- MENU in a gint add-in returns to the add-in at once (there is no OS menu).
- No DSP arithmetic instructions, FPU, USB or sound.
- Built-in apps (Run-Matrix, Graph...) are part of the OS and can't run here.

## Legal

Don't commit or share OS dumps or other people's add-ins. This project only
contains code written for it.
