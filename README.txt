Gens/GS r7

Copyright (c) 1999-2002 by Stéphane Dallongeville
Copyright (c) 2003-2004 by Stéphane Akhoun
Copyright (c) 2008-2009 by David Korth
Copyright (c) 2026 by Tetsuro Mori (64-bit port)

This version of Gens is maintained by David Korth.
The 64-bit (x86_64) port was done by Tetsuro Mori. See section 1.1.

For news on Gens/GS, visit Sonic Retro:
http://www.sonicretro.org

================================================================

1. What is Gens/GS?

Gens/GS r7 is David Korth's fork of Gens, initially based on
Gens for Linux v2.15.2. The number after the "r" indicates the
Gens/GS release. Releases are usually made after a significant new
feature has been added, with minor releases if a major bug is found.

----------------------------------------------------------------

1.1. 64-bit (x86_64) port

Port by Tetsuro Mori (2026).

The original Gens core is written in 32-bit x86 assembly, so it could
not be built as a 64-bit program. This port replaces all of the
assembly emulation core (Mega Drive / Genesis, Sega CD and 32X) with
C, so that Gens/GS builds and runs as a native 64-bit Windows program
(x86_64).

Changes:
- New configure option "--enable-core-c" (GENS_CORE_C). It is always
  on for x86_64 builds. 32-bit builds keep the original assembly core
  unless the option is given.
- 68000: the Starscream assembly core is replaced by Musashi (C).
  src/gens/gens_core/cpu/68k/star_68k_c.c keeps the Starscream API
  (main68k_*), so the rest of Gens is unchanged.
- Z80: the mdZ80 assembly core is replaced by a C Z80 core
  (src/mdZ80/z80core.c). src/mdZ80/mdZ80_c.c keeps the mdZ80 API.
- Genesis memory map: mem_m68k.asm is ported to C
  (src/gens/gens_core/mem/mem_m68k.c).
- Sega CD: the main 68000 memory map (mem_m68k_cd.c), the sub 68000
  memory map (mem_s68k.c), the graphics ASIC (gfx/gfx_cd.c), the CDD
  status export (misc/misc.c) and the debugger cell viewer
  (debugger/mcd_cell_dump.c) are ported to C. The ASIC's saved
  addresses are now offsets instead of raw pointers.
- 32X: the SH-2 interpreter (cpu/sh2/sh2a.c), the SH-2 on-chip
  peripherals (cpu/sh2/sh2_io.c), the SH-2 memory map
  (mem/mem_sh2.c) and the 68000-side 32X memory map
  (mem/mem_m68k_32x.c) are ported to C. In the C core the SH-2 PC is
  kept as an SH-2 address (not a host pointer), so savestates no
  longer contain raw pointers.
- 64-bit cleanup: pointer/integer casts, Win32 API calls that store
  pointers (GWLP_WNDPROC, GCLP_HICON, GCLP_HCURSOR), a truncated image
  list handle, a raw pointer saved in YM2612 savestates, and the
  manifest's processorArchitecture (was x86, now "*").
- Build fixes for current MinGW-w64 / GCC / autotools (dllimport on
  static functions in w32u, missing headers, a Makefile.am typo,
  libtool RC tag, libpng/zlib link order).

Current limitations / testing:
- Tested with small self-made test programs:
  - Genesis: 68000, VDP interrupts, Z80 and Z80 interrupts.
  - Sega CD: sub CPU start-up, COMM registers, Program RAM write
    protect, font registers, timer/INT2/graphics interrupts, Word RAM
    2M/1M modes, the graphics ASIC in 7 modes, Backup RAM and PCM RAM.
  - 32X (booted with the built-in open-source firmware): both SH-2s
    running an ALU / multiply / MAC / divide / shift / branch /
    delay-slot / addressing-mode exerciser, DIVU, DMAC, cache data
    array, VINT and CMD interrupts, packed-pixel frame buffer output,
    and the 68000-side adapter, COMM, bank and RV registers.
  For the Sega CD and 32X tests, the 64-bit C build gives exactly the
  same (pixel-identical) results as the original 32-bit assembly build.
  Commercial games, CD audio, sound, input and savestate compatibility
  with older builds have not been fully tested yet.

Building the 64-bit version (MSYS2, MINGW64 shell):

  autoreconf -fi
  mkdir build64 && cd build64
  ../configure --disable-debug --without-7z
  make -j8

The program is built as build64/src/gens/.libs/gens.exe. It needs
libwinpthread-1.dll and libz-1.dll from the MSYS2 mingw64/bin folder.

Third-party code added by this port (both MIT license):
- Musashi 68000 core, Copyright Karl Stenerud (src/musashi/)
- z80 core, Copyright (c) 2019 Nicolas Allemand (src/mdZ80/z80core.c,
  license in src/mdZ80/z80core_LICENSE.txt)

----------------------------------------------------------------

1.2. Remote control (Windows)

Other programs can control Gens64 through a named pipe. It is off
unless Gens64 is started with the --control-pipe option:

  gens.exe --control-pipe gens64

Only the user who started Gens64 and administrators can send commands,
and only from the same computer. Send commands with
tools/gens64ctl.py, or write one command per line to
\\.\pipe\gens64. Each command gets one reply line that starts with
OK or ERR.

  python tools/gens64ctl.py status

Commands:
  ping, status, help
  load <file>            Load a ROM. Message boxes are not shown; a
                         problem gives an ERR reply. (An archive with
                         several ROMs still opens its selection window.)
  close                  Close the ROM.
  hold                   Stop running on its own, between frames.
  step [frames]          Hold, then run this many frames as fast as
                         possible. The sound of these frames is not
                         played. The reply comes when they are done.
  resume                 Go back to running on its own.
  reset                  Hard reset.
  pad <1|2> <buttons>    Hold these buttons, until the next pad command:
                         up down left right start a b c mode x y z.
  pad <1|2> none         No buttons pressed, ignoring the keyboard.
  pad <1|2> off          Give the pad back to the keyboard.
  read <mem> <addr> <n>  Read n bytes (at most 65536), as hex.
  write <mem> <addr> <hex>
                         mem is rom, ram (68000), vram or cram.
  screenshot <file>      Save the game screen. (.bmp, otherwise PNG)
  savestate <file>
  loadstate <file>       Use full paths. Relative paths are based on
                         the Gens64 save folder or working folder.
  quit                   Save the settings and exit.

Hold, pad and frame count settings stay in place between connections.
To start a game from its first frame: hold, then load, then step.

================================================================

2. Short History of Gens/GS (mostly taken from the Gens for Linux readme)

Stephane Dallongeville is the author of Gens (Windows version).
Caz has made a BeOS port, using Allegro. But this port was command line
oriented. And though Allegro is a great library, its GUI part is ... well ... rather ugly.

Thus, I (Stephane Akhoun) wanted to use GTK+ for the GUI. But as mixing
Allegro and GTK+ events loop was a bit of challenge (though possible) I
decided to switch to SDL. Fortunately, it was very easy (only 2 function 
calls to change !).

Finally, in the port process, my main work is the GTK+ GUI, and some work
here and there. Caz ported the gens core : I wanted to thank him/her one
more time for his/her pieces of advice.

Thanks also for Lester Barrows (aryarya) and El Pelos (wah wah 69) for
their contribution, patches, joystick support, ...
For all users that helped and help us in the porting process : thank you very much !

Gens/GS picks up where Gens for Linux left off. Among other things, it
integrates the Windows version into the same codebase. See the ChangeLog
file for a comprehensive version history of Gens/GS.

================================================================

3. Minimum System Software Requirements

Linux: [runtime requirements]
- SDL 1.2 (SDL 1.1.3 has been reported to work previously.)
- GTK+ 2.4

Linux: [build requirements]
- gcc 2.95
  - gcc 4.x recommended [there may be some unknown 2.95/3.x compatibility issues]
- nasm 2.05.1
- autoconf-2.63
- automake-1.10

Windows: [runtime requirements]
- Windows 9x, Me, 2000 or later.
  - Tested on 95, 98SE, 2000, and XP.
- DirectX 3 or later for keyboard input and GDI rendering. (No audio)
- DirectX 6 or later for joystick input and DirectDraw 4 rendering, and sound.

Windows: [build requirements]
- MinGW and mSYS
- MinGW Runtime 3.14, 3.15.1, or later
  - MinGW Runtime 3.15 has a bug that causes command-line parsing to crash.
- DirectX headers (specifically, ddraw.h, dinput.h, and dsound.h)
- nasm 2.05.1
- autoconf-2.63
- automake-1.10

================================================================

3.1. Easy Windows Build Instructions

Since not everyone is a computer wiz, I created an easier build system for
Win32. It consists of four easy steps.

1. Copy all files from C:\WINDOWS into the Gens/GS source code directory.

2. Run "compile.hta". Internet Explorer will ask you to download and install
an unsigned ActiveX control. Click Yes. If your antivirus program warns you,
click "Ignore". "compile.hta" will generate a Visual Studio 2008 project,
"gensgs.sln".

3. Open "gensgs.sln" in Visual Studio 2008, Professional Edition or higher.
You must use a legally licensed copy of Visual Studio 2008; if your copy is
pirated, you will be redirected to newegg.com, where you may purchase a copy
of Visual Studio 2008, Professional Edition, for the low price of $649.99.

4. Compile the project using Visual Studio 2008. The final binary will be
called "gens.exe".

================================================================

3.2. Feature Requests

Want a new feature in Gens/GS? Here's a proven method for getting it done.

1. Contact GerbilSoft asking for a new feature.

2. Don't tell him what the new feature is, but claim that Gens/GS is
   incomplete without it.

3. ?????

4. PROFIT!

If it can work for administrators of popular websites, it can work for you!

================================================================

4. Where are gens.cfg and language.dat stored?

On Linux: ~/.gens/
On Windows: the same directory as gens.exe

================================================================

5. What is the syntax of gens.cfg ?

gens.cfg uses the standard INI format, which looks like this:

[section1]
key=value
...
...

[section2]
key=value
...

================================================================

6. How do I configure controller inputs without the GUI?

----------------------------------------------------------------

6.1. Old Gens joystick configuration format.

Older versions of Gens/GS (m6 and older), and the original Gens, used
a relatively limited format for storing controller configuration.

Older joystick input format: (16-bit binary)
0001 JJJJ TTTT WWWW

where:
- 0001: High nybble is always set to 1.
- JJJJ: Joystick number.
- TTTT: Type of input. (0 == axis; 1-7 == button; 8 == POV hat)
- WWWW: Which axis, button, or POV hat.

For axes, the following values are allowed:
- 1: Axis 1, negative. (Up) [TODO: Original documentation said Down, verify this!]
- 2: Axis 1, positive. (Down) [TODO: Original documentation said Up, verify this!]
- 3: Axis 0, negative. (Left)
- 4: Axis 0, positive. (Right)
- 5: Axis 3, negative.
- 6: Axis 3, positive.
- 7: Axis 2, negative.
- 8: Axis 2, positive.
- 9: Axis 5, negative.
- 10: Axis 5, positive.
- 11: Axis 4, negative.
- 12: Axis 4, positive.

Among the problems with this scheme are the "backwards" notation (axis 1 before 0)
and the 6 axis limitation. On Windows, the 6 axis limitation (actually 5, since
the DirectInput handler ignores the 6th axis) isn't much of a problem, since
most Windows joystick drivers map some axes to POV hats. On Linux, most
joystick drivers map all axes to axes instead of POV hats, which does create
a problem. Notably, the Xbox 360 controller's D-pad is mapped as axes 6 and 7,
which doesn't work with this scheme.

For buttons, TTTT WWWW is taken as a whole byte. The number 0x10 is subtracted
from the byte, resulting in the button value. For example, the first button,
button 0, would have a TTTT WWWW value of 0x10.

For POV hats, the following format is used: NNDD

where:
- NN: POV hat number.
- DD: Direction. (0 = up; 1 = right; 2 = down; 3 = left)

Thus, the "Up" direction on the first POV hat on the first joystick would be
indicated as 0x1080.

----------------------------------------------------------------

6.2. New Gens/GS joystick configuration format.

Starting with Gens/GS r7, a newer format is used for storing joystick
configuration. The new format allows up to 128 axes, 256 buttons, and
64 POV hats.

New joystick input format: (16-bit binary)
1TTT JJJJ WWWW WWWW

where:
- 1: High bit is always set to indicate new joystick input format.
- TTT: Type of input. (0 == axis; 1 == button; 2 == POV hat)
- JJJJ: Joystick number.
- WWWW WWWW: Which axis, button, or POV hat.

For axes, the following format is used: NNNN NNND

where:
- NNNN NNN: Axis number.
- D: Axis direction. (0 == negative; 1 == positive)

For buttons, the entire WWWW WWWW value is used as the button number.

For POV hats, the following format is used: NNNN NNDD

where:
- NNNN NN: POV hat number.
- DD: Direction. (0 = up; 1 = right; 2 = down; 3 = left)
