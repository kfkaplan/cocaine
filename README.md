# cocaine

Runs old Mac programs on Apple Silicon Macs: PowerPC apps (the kind Rosetta 1
used to run) and 32-bit Intel apps (the kind macOS dropped in Catalina).

It is a compatibility layer, not a virtual machine. There is no guest
operating system to boot. cocaine loads the old program into its own process,
emulates the old CPU, runs Apple's own old system libraries as guest code,
and answers their requests to the Mac OS X kernel itself.

## Status

Command line programs from a real Mac OS X 10.5 run on both CPUs, including
ones built on CoreFoundation and Foundation, and so do its shells: they start
other programs, pipe them together and trap signals. Guest processes exchange
Mach messages, and six of Apple's own system daemons run as guests and
serve them: `notifyd`, the directory daemon (so the guest system knows the
person running it, by name), `dirhelper`, the security daemon, Launch
Services and `configd`, which keeps the system's settings (without its
plug-ins, which would manage the Mac's real network interfaces).

Real apps open windows, on both CPUs. Run Calculator and Apple's own
`WindowServer`, font server and `loginwindow` start behind it as guests; the
window server makes itself a virtual 2560 x 1600 display in shared memory
(with Leopard's hidden 2x drawing switched on, so things are their usual
size, only sharp),
Leopard's own code draws the menu bar and the window there
(`docs/first-keys.png`), and a viewer window opens showing that display
live. Click and type in the viewer and it reaches the app: the mouse (both
buttons, double clicks, dragging, the scroll wheel) and the keyboard go into
the guest's input system the way the kernel's would. A button press shows
on the guest's display in about 15 ms and a dragged window follows the
pointer within a couple. VLC 0.9.10 starts too, the PowerPC build and the
Intel one: its menu bar, its player window and its first-run question all
appear (`docs/first-vlc.png`, `docs/first-intel-vlc.png`). Both builds
play music: given an MP3 they play it through this Mac's speakers, at the
right speed, in stereo, by way of a stand-in Core Audio device ("Sound"
below).

Games run too, with this Mac's graphics card and speakers. Marble Blast Gold
(either half of the 2006 universal build, PowerPC or Intel) starts, plays its attract
demo, takes keys and clicks in its menus and plays its levels in an
800 x 600 window at a steady 60 frames a second (`docs/first-marbleblast.png`),
with its music and sound effects. Its OpenGL and OpenAL calls aren't run as
emulated code: cocaine takes those libraries over and makes each call on
the Mac's own OpenGL and OpenAL ("Games: OpenGL and OpenAL" below).

The Intel side needs one thing more than the PowerPC side. The Intel halves
of the font server and the login window are among the few programs Apple
shipped encrypted, with keys kept in a chip on every Intel Mac's logic
board. cocaine decrypts them as the real kernel does, given those keys,
which are not in this repository (see "What you need to bring"). Without
them an Intel app gets as far as asking for the window server and stops. A
universal program's PowerPC half is the one run unless `--arch i386` says
otherwise: it runs a little faster than the Intel half, and needs no keys.

Guest code is not interpreted but translated to ARM64, a block at a time,
on both CPUs. A Python loop that took 8.0 s interpreted on PowerPC takes
0.63 s; the same loop on Intel went from 31 s to 0.8 s. Intel floating
point is translated as well: a Mandelbrot set in doubles went from 6.0 s to
0.62 s, a loop over arrays of singles from 0.68 s to 0.06 s. VLC's windows are up
2.7 s after starting the PowerPC build and 4.8 s after starting the Intel
one; a button in Calculator reacts in under 10 ms.

| Piece | State |
|---|---|
| Mach-O loader (PowerPC, Intel, universal files, `.app` bundles) | works |
| PowerPC CPU (a G4): integer, floating point, AltiVec | works, tested; translated to ARM64, with the interpreter as reference and fallback |
| Intel CPU (a Core Duo): integer, x87, MMX, SSE, SSE2, SSE3 | works, tested; the common integer instructions, SSE floating point and the plainer vector instructions translated to ARM64, x87 called from translated code, the rest interpreted |
| System root import from a Leopard install DVD | works |
| Apple's `dyld` and `libSystem`, as guest code | work on both CPUs |
| Unix tools (`ls`, `cat`, `awk`, `date`, `printf`...) | run on both CPUs |
| CoreFoundation and Foundation programs (`sw_vers`, `plutil`, `defaults`) | run on both CPUs |
| Sockets | basic calls pass through |
| GUI apps (`Calculator.app`, VLC 0.9.10) | windows, menu bar, mouse and keyboard on both CPUs (Intel needs the SMC keys) |
| Apple-protected (encrypted) Intel system programs | decrypted on loading, given the keys |
| Threads (pthreads, mutexes, condition variables, semaphores) | work on both CPUs |
| Python 2.5, as shipped with Leopard | runs on both CPUs |
| Processes: `fork`, `exec`, `posix_spawn`, `wait`, pipes, `#!` scripts | work on both CPUs |
| Signals: handlers, masks, `kill`, timers, `sigsuspend`, alternate stacks | work on both CPUs |
| Shells and scripting (`sh`, `bash`, `zsh`, `tcsh`, `ksh`, Perl, Ruby, PHP, Tcl) | run on both CPUs |
| A PowerPC program starting an Intel one, and the reverse (`arch`) | works |
| Mach messages between guest processes: port rights, out-of-line memory, port sets, kernel notices | work on both CPUs |
| Session server (the stand-in for `launchd`): service lookup, check-in, daemons started on demand | works |
| Apple's `notifyd`, `DirectoryService`, `dirhelper`, `securityd`, `coreservicesd` and `configd`, running as guest daemons | work on both CPUs |
| Users and groups, resolved by the directory daemon (the current user is added to its records) | work on both CPUs |
| Named shared memory, shared file mappings, `kqueue`, record locks, passing files over sockets | work on both CPUs |
| Guests' `syslog` messages, collected in the session's log | works |
| The pasteboard, distributed notifications, configuration and disk daemons | not yet |
| Memory shared by passing a port (memory entries), from any 4 KB page | works between processes |
| Kernel calls made on another process's task port | answered by that process |
| IOKit: the registry root, power management, an input system with its event queue | enough for the window server; the queue is fed from the viewer |
| Apple's `WindowServer`, `ATSServer` and `loginwindow`, as guests | run on both CPUs, started when a program with windows is |
| The viewer: the guest's display in a real window, with mouse and keyboard | works (one window for the whole display) |
| OpenGL (AGL) and OpenAL, made on the Mac's own | enough for Marble Blast Gold on both CPUs |
| Sound through Core Audio: a stand-in output device and output units | works on both CPUs (VLC plays MP3s) |
| QuickTime | not yet |

## Build and run

Needs Xcode's command line tools, CMake and Ninja.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Old programs need old system libraries, so first unpack a system root from a
retail Mac OS X 10.5 Leopard install DVD image (see "What you need to bring"):

```bash
sh tools/import-sysroot.sh /path/to/leopard-install-dvd.iso
```

Then run things. `COCAINE_SYSROOT` saves typing `--sysroot` every time.

```bash
export COCAINE_SYSROOT=$PWD/sysroots/10.5
./build/cocaine sysroots/10.5/bin/ls -la /System/Library/Frameworks
./build/cocaine --arch ppc sysroots/10.5/usr/bin/sw_vers
./build/cocaine --info /path/to/Some.app
./build/cocaine sysroots/10.5/Applications/Calculator.app
```

The last one opens a window titled "cocaine: Mac OS X 10.5": the guest
system's display, with Calculator on it. Give it a few seconds: Apple's
daemons, window server and login window start first.

`cocaine --help` lists the options. The useful ones while developing are
`--strace` (log system calls and Mach messages), `--trace` and
`--trace-registers` (log every instruction), `--env` (set a variable for the
guest, such as `DYLD_PRINT_LIBRARIES=1`), `--stats` and `--info`.

For looking into what the system daemons are doing there are a few
environment variables, none of which change anything unless set:

| Variable | What it does |
|---|---|
| `COCAINE_DAEMONS=all`, `none`, or labels | which of Apple's daemons the session may start, instead of the usual list |
| `COCAINE_NO_VIEWER=1` | don't open the viewer window |
| `COCAINE_DUMP=address:bytes:seconds:file` | write that guest memory (hexadecimal address) to `file.<pid>` after that long |
| `COCAINE_SCRIPT="wait 20; click 300 400; key 28 56; shot /tmp/a.raw"` | play mouse and key actions into the guest's input system: `wait`, `move`, `down`, `up`, `dragto`, `click`, `drag`, `key`; `settle` and `change X Y W H` time how fast the window server and the display respond; `shot FILE` saves the display (see `run_input_script` in `src/kernel/iokit.cpp`) |
| `COCAINE_POKE=x,y,seconds` | short for a script that, after that long, clicks at x,y and types 8 and 9 |
| `COCAINE_NOTE_INPUT=1` | for three seconds after each click or key, log every message the window server sends and receives, with times |
| `COCAINE_VIEWER_SNAPSHOT=file.png` | have the viewer save what its window shows, twelve seconds in |
| `COCAINE_NOTE_MSG=number,first-last` | log every process that sends one of those Mach requests, and the answer it gets |
| `COCAINE_CALLS=address,address` | log each call a PowerPC guest makes to one of those addresses, with its arguments |
| `COCAINE_WATCH=address` | log each PowerPC store of a word to that address, and the code that made it |
| `COCAINE_SESSION_TRACE=1` | log every request the session server receives, with its sender |
| `COCAINE_JIT=0` | interpret guest code instead of translating it |
| `COCAINE_JIT_STATS=1` | on exit, say how much was translated and which instructions were left to the interpreter |
| `COCAINE_FAULT_MEM=r6:160,@2eebd0:16` | when a PowerPC guest crashes, also print memory at those registers or addresses (the crash report always names the library each caller is in) |
| `COCAINE_NAN=1` | report the first places a PowerPC guest's floating point arithmetic makes a NaN out of numbers, with the inputs (interpreted, so slow) |
| `COCAINE_GL_FRAME=n:file` | save OpenGL frame n, as cocaine read it back from the Mac's GPU, to `file` (raw, A R G B, bottom row first) |
| `COCAINE_GL_FPS=60` | the most frames a second an OpenGL game is shown at (60 unless set, as vertical sync held games to on a Mac of the time; 0 for no limit) |
| `COCAINE_DISPLAY=1280x1024` | the guest display's size (2560 x 1600 unless told otherwise; the tests use 1280 x 1024) |
| `COCAINE_UI_SCALE=1` | how many times bigger Leopard draws everything (2 on a display 2560 or more wide, else 1) |
| `COCAINE_AUDIO_DUMP=file` | also write everything the guest plays to `file`, as raw 44.1 kHz stereo 32-bit floats |
| `COCAINE_FAULT_CODE=1` | when an Intel guest crashes, also print the code around the instruction and the native backtrace |
| `COCAINE_JIT_VERIFY=1` | run each translated Intel SSE instruction through the interpreter too, and report any difference |
| `COCAINE_JIT_POISON=1` | report any read of flags the Intel translator decided nobody reads |
| `COCAINE_JIT_SKIP=70,62` | leave those Intel SSE instructions (by second opcode byte) to the interpreter |
| `COCAINE_JIT_FLAGS=0` | Intel translator: work out every instruction's flags, skip none, fuse no compare with its branch (3 is the usual) |
| `COCAINE_JIT_WRITABLE=0` | Intel translator: interpret code in writable memory instead of translating it |
| `COCAINE_JIT_MATHS=0` | have the Intel translator leave floating point and vector instructions to the interpreter, as it did at first |

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Each test in `tests/guest/` is built as a guest program and as a reference,
and the emulated program must print exactly what the reference prints, bit
for bit. Each is run twice, translated and interpreted, and each way must
match. A second, quick test checks every ARM64 instruction the translators
can emit against the system's assembler
(`tests/a64-check.sh`). There are three kinds of reference:

- Plain C tests (`t_int`, `t_fp`, `t_fma`, `t_signal`) are built natively for
  this Mac. `t_signal` interrupts a long calculation with a timer's signals
  and checks that the result comes out the same.
- The Intel-only tests (`t_x87`, `t_sse`) are built for 64-bit Intel and run
  under Rosetta, which has a real x87 and SSE to compare against. They are
  skipped if Rosetta isn't installed.
- No current Mac has AltiVec, so `t_altivec` writes every operation twice:
  the real instruction for PowerPC, and a plain C model of the documented
  behaviour for the reference.

If a system root has been imported, the tests also run real Mac OS X programs
from it on both CPUs and check their output. That includes a shell script
(`tests/guest/procs.sh`) that exercises processes, pipes and signals, Python
programs that use threads and fork from them, and `t_ipc`: two processes
exchanging Mach messages with port rights and out-of-line memory in them.
Three of Apple's daemons are tested through their clients: the guest system
must know the person running the tests by name, and one program must hear a
notification that another posts (`tests/guest/notify.sh`). And Calculator is
run on both CPUs. It must come up in front with Apple's window server
behind it and be handed a key typed some seconds in (which takes the window
server, the font server, the login window and the whole input path). If the
keys for the Intel system programs aren't there, the Intel one must instead
get as far as it can: through Launch Services to asking for the window
server.

The built guest programs live in `tests/bin/`, so running the tests needs
nothing extra. Rebuilding them after an edit needs Homebrew LLVM for the
PowerPC half: `sh tests/build-guest.sh`.

## How it works

- **Guest memory.** A native Apple Silicon process can't use addresses below
  4 GB, which is exactly where a 32-bit program lives. So cocaine reserves a
  4 GB window and guest address A is host address `base + A`.
- **CPU cores.** One interpreter per architecture (`src/cpu/ppc`,
  `src/cpu/x86`). They run guest code until it traps into the kernel. They are
  the simple, exact reference.
- **The translator.** PowerPC code doesn't normally go through the
  interpreter: it is translated to ARM64, a block at a time, the first time
  it runs (`src/cpu/ppc/ppc_jit.cpp`). A block is a straight run of guest
  instructions, each turned into a few host ones. The seventeen busiest
  guest registers (r0 to r12, r29 to r31 and the condition register) live
  in host registers while translated code runs, and are put back in the
  CPU state on every way out, so the rest of cocaine never sees the
  difference. Blocks jump straight to each other once both exist; a
  return or a call through a pointer finds its block in a table without
  leaving translated code. Anything rare (AltiVec, the odder floating
  point, traps) is done by calling the interpreter for that one
  instruction, which is what keeps the translator small and lets it be
  checked: the same tests run both ways and must agree bit for bit. When
  guest memory holding code is unmapped, mapped over, or flushed by the
  guest's own "I have written code here" call, the blocks from it are
  dropped. A forked child keeps its parent's translations.
- **The Intel translator** (`src/cpu/x86/x86_jit.cpp`) is built the same
  way, with two differences that x86 forces. Its flags: nearly every x86
  instruction sets six of them and nearly nobody reads them. The
  translator looks ahead, and where the flags are about to be overwritten
  unread it emits the bare operation. Where a compare (or any arithmetic)
  has a conditional jump, set or move close behind, the pair becomes ARM's
  own: a flag-setting ARM instruction and a branch on ARM's flags, with
  the x86 condition turned into the ARM one that means the same after
  that kind of instruction (ARM's carry is upside down after a
  subtraction; a byte-sized compare is done at the top of the register so
  that ARM's flags come out as an 8-bit operation's). And where the
  guest's flags might still be wanted later, the instruction leaves a
  note in the guest's state (which operation, on what) in place of
  working them out; the flags are worked out from the note only if
  something does read them, and always before anything outside translated
  code looks. The few instructions that take the carry in still call the
  interpreter's routine. And its code changes without notice (the stubs for
  calling other libraries are rewritten in place on first use), so code in
  memory the guest can write is translated an instruction at a time, and
  each such block first checks that the bytes it was made from are still
  there.

  Floating point is translated too. The SSE registers stay in the guest's
  state; each instruction loads what it needs, does the work with the
  Mac's own floating point or vector instruction, and stores the result.
  That covers arithmetic on one number or all of them at once, compares,
  conversions, moves, shuffles, the bitwise instructions and the plainer
  integer vector ones. Where x86 and ARM give different answers, the
  translated code notices and lets the interpreter do that one instruction:
  a result that is not a number (the two disagree about which one), and a
  conversion to an integer that doesn't fit. The Mac's rounding mode is set
  to the guest's while translated code runs. x87 keeps its exact 80-bit
  registers, and the interpreter's routine still does the arithmetic, but
  translated code calls it and carries on, where at first each x87
  instruction ended the block. What still ends a block and goes to the
  interpreter one instruction at a time: MMX, string moves, 16-bit
  operands, and the rarer SSE instructions.
- **System libraries.** Old apps need Cocoa, Carbon, QuickTime and the rest in
  PowerPC and 32-bit form, and modern macOS ships none of that. cocaine runs
  Apple's own old libraries, from a Mac OS X install you supply, as guest
  code. Guest paths under `/System`, `/usr`, `/Library` and friends are
  redirected into that system root; everything else is this Mac's own files.
- **Fake kernel.** `src/kernel` stands in for what sits underneath those
  libraries:
  - BSD system calls. The host is Darwin too, so most map onto the same host
    call after translating the calling convention, structure layouts and (for
    PowerPC) byte order.
  - Mach traps and messages. Most kernel services on Mac OS X are messages to
    kernel ports; cocaine keeps its own port table and answers the messages
    programs really send.
  - The comm page: routines such as `memcpy` and the clock that the kernel
    maps at fixed addresses. cocaine supplies its own, most of them one
    "host call" that runs the routine as native code.

- **Mach ports and sessions.** Every port a guest names is a real port of
  the host process running it, under the same name. This Mac's own kernel
  therefore does the hard parts of Mach for the guests: queueing, moving
  port rights between processes, port sets, semaphores, timers, and noticing
  when a port dies. Guest messages are laid out for a 32-bit machine (and in
  PowerPC byte order half the time), so each one travels inside a host
  message that carries its port rights and out-of-line memory as host
  descriptors (`src/kernel/tunnel.h`). The kernel's own objects (task,
  thread, host, clock) are ports cocaine holds the receiving end of; what a
  guest sends them is answered in place.
- **The session server.** `launchd` is the directory of system services on a
  real Mac. cocaine's stand-in (`src/session/`) is a small native program,
  one per run, started the first time a guest needs it and ended when the
  run's first program ends. It reads the job descriptions in the system root
  to learn which daemon provides which service, hands out service ports, and
  starts Apple's real daemon behind a port, as a guest, when the first
  message arrives for it. Only daemons known to work are on call (`notifyd`,
  `DirectoryService`, `dirhelper`, `securityd`, `coreservicesd`, and
  `WindowServer`, `ATSServer` and `loginwindow`, the last three for Intel
  programs only where the keys to them have been supplied);
  `COCAINE_DAEMONS=all`, or a
  comma-separated list of job labels, tries others. Daemons are told they are
  root, and their messages are stamped as root's, because both they and
  their clients check. A daemon can collect its ports either of the two ways
  `launchd` offers: by name over Mach, or by asking on the socket every job
  is started with. With `-v` cocaine says where the session's log is; it
  holds what the server did, what the daemons printed, and everything any
  guest sent to `syslog`, which is often the only place a daemon explains
  itself.
- **Windows.** The window server is Apple's, run as a guest like the other
  daemons. A real Mac has it up, and a login session set up by `loginwindow`,
  long before any app starts, and apps depend on that order: one that
  registers with the window server before the login window has finished is
  told it is the app in front, then quietly isn't, and never sees a key. So
  for a program with windows (one in an `.app`, or linking Cocoa or Carbon)
  the session server starts the window server at once and holds the
  program's service lookups until the login window has put itself in front,
  which that process's cocaine reports. Any other session leaves the window
  server alone until something asks for it. The window server draws into a
  "VirtualDisplay" of its own making; its cocaine copies whatever changes
  there into a small shared file, checking 120 times a second, and starts
  `cocaine --view` on it. cocaine hooks the window server's own
  `CGXUpdateDisplay` (a private CoreGraphics function, on both CPUs): when
  it returns, the screen holds a whole picture, and what changed is copied
  to the viewer then, on the window server's thread, before the next update
  can start, so a window halfway through being redrawn is never shown torn.
  (A polling copier remains for a window server where the hook isn't
  working, and stands down once the hook has published.) The file's frame
  number is odd while a picture is being written, so the viewer never takes
  a half-written one. The viewer is a plain macOS window that hands each new picture
  to its layer, so this Mac's graphics hardware does the scaling
  (`src/viewer/`), and sends mouse and keys back down a pipe. Leopard's
  window server holds every screen update for the display's next refresh,
  or for an eighth of a second where a display can't say when that is, as
  this one can't; the session server switches that off in the system root's
  window server preferences, which is the difference between a button
  taking 145 ms to react and 15. Mouse and keys are written into the input system's
  shared event queue (`post_input_event` in `src/kernel/iokit.cpp`), where
  the window server finds them as it would on real hardware, and it decides
  which app gets what.
- **Games: OpenGL and OpenAL.** Leopard's OpenGL drives graphics cards no
  Apple Silicon Mac has, and run as guest code it finds no renderer at all:
  every call quietly does nothing (Marble Blast crashed several calls
  later, on a camera matrix OpenGL never filled in). So when a guest maps
  the code of libGL, AGL or an OpenAL framework (its own bundled copy
  counts), cocaine reads the library's exported symbols and writes a host
  call over the first instruction of each function it supplies
  (`src/kernel/hooks.cpp`). Whoever calls it, the game or another library,
  lands in native code that reads the arguments the PowerPC way (integers
  in r3-r10, floats in f1-f13, the rest on the stack) and makes the call on
  the Mac's own library (`src/gl/gl.cpp`, `src/al/al.cpp`). Functions of
  those libraries that aren't done yet say so once and return 0. Byte
  order is the work: values in registers need nothing, but matrices,
  vertex and index arrays, glGet answers and 16-bit sound samples are
  big-endian in guest memory and get swapped on the way through. Pixel
  data needs no copy, because the host's swap-bytes pixel setting is kept
  the opposite of the guest's. Vertex arrays are noted when the guest
  points at them and converted at each draw. Each guest OpenGL context
  draws into an offscreen framebuffer on the Mac's GPU. At
  `aglSwapBuffers` cocaine reads the frame back and copies it into the
  game window's own pixels, then marks the window changed and has the
  guest's QuickDraw flush it, the way a game that drew in software did. The
  window server, the viewer and window moves then need nothing new. Those
  QuickDraw calls are guest functions, run from native code
  (`Process::call_guest`): a fresh stack frame, a return address that
  holds a "come back" host call, and the CPU loop run until it hits it.
- **Sound.** Everything a Mac program does with sound ends up at Core
  Audio's hardware layer (`AudioHardware...`, `AudioDevice...`,
  `AudioStream...`, `AudioObject...`), and Leopard's version needs drivers
  and a daemon cocaine doesn't have, so it finds no devices. cocaine takes
  that layer over, on both CPUs, and offers one device of its own,
  "cocaine output": stereo, 32-bit float, 44.1 kHz (`src/audio/`). When the
  device starts, a guest thread (made with the guest's own `pthread_create`,
  running a native function cocaine plants for it) calls the guest's IO
  procedures a buffer at a time and puts what they write into a ring, which
  a native output unit plays; the ring is kept a few buffers ahead, which
  paces the guest. The guest's Component Manager finds no components under
  cocaine either, so the output units (AUHAL, default and system output)
  are done the same way, on the same thread: their render callbacks are
  called for the format the guest set (16- or 32-bit, either byte order,
  interleaved or not, resampled if the rate differs), and requests for any
  other kind of component go on to the guest's own Component Manager. The
  times the guest is told are what matters most: a sample's host time is
  when it will really be heard, measured on the guest's clock (which ticks
  in nanoseconds), from an anchor moved every turn. Get that wrong and VLC,
  thinking every buffer late, plays silence.
- **Calling on, and passing on.** Taking over works on Intel guests too:
  the host call is `int 0xFE` and its number, arguments come off the stack,
  and the function returns by popping its return address. A function taken
  over can keep the guest's own version callable: its first instructions
  are copied out, followed by a jump to the rest, so a handler can pass on
  the calls it doesn't handle itself.
- **Atomic instructions.** An Intel `lock` instruction (and `xchg`) has to be
  atomic against every access to its operand, including plain stores from
  other threads, and not only against other locked instructions. The
  interpreter reads the operand once, holds the write back, and puts it in
  with a real compare-and-swap; if another thread changed the operand
  meanwhile, the instruction runs again. (At first they only took one
  process-wide lock, so a plain store landing in the middle, such as a
  spinlock's release, was lost; faster translated code made that common
  enough to crash Intel VLC about one run in three.)
- **The person running cocaine.** The system root has its own list of
  users, which doesn't include you. The first time a session starts, the
  server adds a record for the current user (name, numbers, home folder) to
  the directory daemon's files in the system root, and programs that read
  `/etc/passwd` directly are shown a copy with a line for you.
- **Your home folder, minus its Library.** Old programs see your real home
  folder, so they can open your documents. They do not see its `Library`
  or its dot-files: those are where programs keep settings, caches and
  keychains, under names that haven't changed in twenty years, and an old
  program let loose there reads settings it can't parse and writes over
  the ones a current program of the same name uses. The guest system has
  its own, in the system root at the same path
  (`sysroots/10.5/Users/<you>/Library`).

## What you need to bring

A retail **Mac OS X 10.5 Leopard** install DVD image. It is universal, so it
covers PowerPC and Intel at once. The grey restore discs that shipped with
one particular Mac are a poor substitute.

`tools/import-sysroot.sh` unpacks its installer packages into `sysroots/10.5`
(about 3.7 GB). Nothing is installed or booted. Git ignores `sysroots/`:
Apple's files stay out of the repository.

For Intel apps with windows, also **the two keys from an Intel Mac's SMC**.
Apple stored a few of the Intel system's own programs encrypted (the Finder,
the Dock, the login window, the font server), and Mac OS X's kernel
decrypts them with two 32-byte keys that are not on the disc: its "Dont
Steal Mac OS X" extension reads them, as `OSK0` and `OSK1`, from the System
Management Controller of the Intel Mac it runs on. That tied the Intel
system to Apple's hardware. An Apple Silicon Mac has no such keys, so
cocaine has to be given them: the 64 characters, `OSK0` then `OSK1`, in the
file `.cocaine/osk` in the system root, or in the environment variable
`COCAINE_OSK`. They are deliberately not in this repository, and should stay
out of it. PowerPC apps need none of this.

And the programs to run. None come with this repository; Git ignores disk
images, MP3s and the `test-games/` and `test-canidates/` folders they were
kept in while developing. The ones tried so far are VLC 0.9.10 (its PowerPC
and Intel builds) and the 2006 universal Marble Blast Gold 1.6: open a
program's disk image on this Mac, copy the `.app` out, and give its path to
`cocaine`.

## Roadmap

1. **Done.** Loader, both CPU cores complete (PowerPC with AltiVec; Intel
   with x87, MMX and SSE through SSE3), test harness.
2. **Mostly done.** Run command line tools from the system root: the real
   `dyld` and `libSystem`, the comm page, Mach traps and messages, the system
   calls they use, threads, processes and signals. Sockets only pass the
   basic calls through.
3. **Done for command line programs.** CoreFoundation and Foundation.
4. **Mostly done.** The system daemons an app talks to while starting. Calculator
   asks for directory services, the notification centres, the security
   server and Launch Services (`coreservicesd`), and aborts without the last.
   Faking such daemons by hand doesn't scale: an earlier stand-in for
   `coreservicesd` got an app through check-in and no further, because
   clients read the daemon's shared-memory caches directly, so a stand-in
   would have to reproduce its internal data structures. The plan instead is
   to run Apple's real daemons from the system root as guest processes too.
   That needs, in order:
   - guest threads (**done**: each guest thread is a host thread);
   - more than one guest process at a time (**done**: each guest process is
     a host process running cocaine, so `fork` is the host's fork and `exec`
     re-runs cocaine on the new program), and with them signals;
   - Mach messages between guest processes, and named shared memory
     (**done**), plus a stand-in for `launchd` to start the daemons
     (**done**; Apple's `notifyd` is the first to run and serve).

   The directory daemon and `dirhelper` followed. What they needed, found
   by trying them: `launchd`'s socket check-in, a way to ask whose task
   port one holds, a power-management object in IOKit to register with,
   file record locks, and being believed when they say they are root.

   Then the security daemon and Launch Services itself: `coreservicesd`
   refuses clients that have no security session ("Client must provide a
   valid sessionID", it says in the session's log), and `securityd` hands
   those out. With both running, an app registers and moves on.

   Still to come here: one session shared by every app the user has open,
   so that apps can see each other and the daemons start once instead of
   with every run; and the smaller daemons apps look for but survive
   without (distributed notifications, configuration, disk arbitration).

   Only the daemons that touch hardware (the window server, audio) then need
   native replacements. Run cocaine with `-v`: it names the session's log,
   which lists every service a program looked for and every daemon started.
5. **Under way.** First window. The plan is the one that worked for the
   other daemons: run Apple's real `WindowServer` as a guest. It draws into
   a display that IOKit provides, so cocaine's IOKit gets a pretend
   framebuffer (ordinary shared memory), and a small native program shows
   that memory in a real macOS window and feeds mouse and keyboard back
   into the pretend input system. The first result will be a whole Leopard
   desktop in one window; showing each app window as its own can follow,
   read from the same server.

   It turned out not to need a pretend framebuffer: finding no display
   hardware, the server makes a "VirtualDisplay" of its own in shared
   memory and draws there. So the picture exists already, in the server's
   memory (`COCAINE_DUMP` writes it to a file, which is how it was first
   seen: one flat colour, the server's background).

   Done so far: the server starts when an app asks for it (the session
   server knows which names it publishes), opens the input system with its
   shared event queue, creates its shared buffers and its display, and
   stays up. Its buffers start one 4 KB page into each allocation, and this
   Mac shares only whole 16 KB pages, so an entry covers the host pages
   around the range and the session server records how far in it starts.
   The server maps those buffers into each app by calling the kernel on the
   app's task port, which the app's own cocaine now answers.

   (A range of guest memory is several objects to this Mac's kernel, and an
   ordinary entry stands for only the first; the entries are made with the
   flag that covers the whole range.)

   First pixels: with `COCAINE_DAEMONS` naming the default daemons plus
   `com.apple.WindowServer` and `com.apple.ATSServer`, and `--arch ppc`,
   Calculator runs and stays running, and the display shows its menu bar.
   That needed paths of the form `/.vol/volume/folder/name` (how Carbon
   names files) to go through the system root, and the two kernel calls the
   Objective-C runtime uses to look at a program's threads.

   The display's pixels start 4096 bytes into its shared memory, 1280 to a
   row, four bytes each.

   Why the window is missing: the app is idle in its event loop, and the
   window server's log says "CGXOrderWinsAndPanels: error retrieving front
   process" (-600). When it puts windows in order it asks Launch Services
   which app is in front, and Launch Services turns the window server away
   because, as a root daemon here, it has no security session ("SessionGetInfo()
   returned error -50 for sessionID=0", in the session's log). Starting the
   window server before the app registers anywhere (which the session
   server now does, holding apps' lookups until the window server has
   published its ports) did not change that. The likeliest missing piece
   is `loginwindow`: on a real Mac it, not the window server, creates the
   security session for a login and tells the window server about it.

   That was it. With `com.apple.loginwindow` added to `COCAINE_DAEMONS`,
   the session server starts Apple's `loginwindow` once the window server
   is up, the complaint goes away, and Calculator's window is drawn on the
   display, shadow and all: the first window.

   The viewer (`src/viewer/`): the cocaine process running the window
   server copies the display into a small shared file (thirty times a
   second at first) and starts `cocaine --view` on it, a plain macOS window
   that shows the file. `COCAINE_NO_VIEWER=1` turns that off. It finds the
   display by a guess (the first big piece of memory the window server
   shares, laid out as 10.5 does), which wants replacing with a proper
   question to the server.

   The mouse: clicks and movement in the viewer go back down a pipe and
   are put into the input system's event queue (`post_input_event`), the
   way the kernel's own input system would. Posting was tested by clicking
   Calculator's "7" at a fixed spot (`docs/first-click.png`: the readout
   says 7). Keys posted the same way went nowhere, and finding out why took
   four separate discoveries, each from reading the window server's own
   code (Apple left the names in):

   - The queue is laid out differently for the two CPUs. PowerPC programs
     keep 10.4's arrangement, with the queue's read and write marks at the
     front of the shared block; Intel ones have them at the back, and
     everything between shifts by twelve bytes. An event's details are in a
     different place too (28 bytes into the event on PowerPC, 40 on Intel).
     cocaine had the Intel layout for both, and the click had only worked
     by accident: the "buttons held" number happened to sit where the
     PowerPC window server looks for the write mark.
   - The window server gives the kernel a port to be told on when events
     are waiting. Nothing could be sent there, because the right to send is
     made by the message that hands it over, and that message never leaves
     the process here. Without it the server found events on its
     once-a-second timer: clicks took most of a second to land.
   - Keys go to the app in front. Calculator had registered a moment before
     the login window finished setting the session up, been told it was in
     front, and then been replaced without a word, so it never asked for
     the keyboard back and every key went to the login window. Apps are now
     held until the login window is done ("Windows", above).
   - Holding apps back stalled the login window itself: it runs a helper in
     a process group of its own and waits for it, and the session server
     took the helper for an app. It now follows parentage.

   With those, Calculator comes up as the app in front, draws its own menu
   bar, and takes typing (`docs/first-keys.png`: a click on 7, then 8 and 9
   from the keyboard). The window server, the font server and the login
   window are on call by default for PowerPC programs, so there is nothing
   to set. VLC 0.9.10's PowerPC build was the first other app tried, and it
   came up as it was: menu bar, player window, and a first-run question
   (`docs/first-vlc.png`).

   Then two things found by using it. Clicks landed sixteen pixels to the
   left of where they were aimed, which mostly showed on small targets (a
   window's close button could not be hit): the display's pixels start one
   page into its memory, not the 64 bytes more first guessed, so the whole
   picture had been shown shifted. And everything felt slow though neither
   the app nor the window server was busy: the window server was sitting on
   each update for an eighth of a second (see "Windows" above), on top of a
   viewer that copied and redrew thirty times a second each.

   Not yet: a pointer drawn on the guest's display (this Mac's own pointer
   over the viewer does the job); a logged-in user (the login window sets
   the session up and then waits, so the session is root's and "login not
   done", which apps so far don't mind); and finding the display by asking
   rather than by the guess described above. The server's own log is in the
   system root at `/var/log/windowserver.log`.

   The Intel path. The Intel window server ran from the start, but two of
   the programs it needs, the font server and the login window, are stored
   encrypted. The method is in plain sight in the system root: the "Dont
   Steal Mac OS X" kernel extension's page routine decrypts each 4096-byte
   page as two halves, AES with 256-bit keys in CBC mode from a vector of
   zeroes, the first half with the SMC's key `OSK0` and the second with
   `OSK1`, and the kernel applies it to a protected segment from the fourth
   page of the file on. cocaine's loader now does the same
   (`src/kernel/protect.cpp`) when it is given the keys. With them, the
   Intel Calculator came up on the Intel window server at the first try,
   mouse, keyboard and all: the Intel layout of the input queue, until then
   only read off the code, was right. The Intel build of VLC 0.9.10 followed
   (`docs/first-intel-vlc.png`), its windows up about 14 seconds after
   starting it at the time, when Intel code was still interpreted (4.8 s
   since it has been translated).

   Later in this step: bridge the window server to real macOS windows, plus
   events, fonts and the pasteboard.
6. **Under way.** Audio, OpenGL and QuickTime. VLC plays a video.

   OpenGL and OpenAL came first, for games: both are taken over and done
   by the Mac's own libraries ("Games: OpenGL and OpenAL" above). Marble
   Blast Gold was the first game to try; it crashed on a camera matrix
   full of leftover stack, which led back to OpenGL doing nothing at all
   as guest code. With both passed through it plays, sound and all
   (`docs/first-marbleblast.png`). Still to do: full screen, the same for
   Intel games (whose arguments come on the stack), and QuickTime.

   Core Audio's hardware layer came next ("Sound" above), and Intel VLC
   plays music through it. Getting there found three bugs that weren't
   about sound: the interpreter's `lock` instructions weren't atomic
   against plain stores ("Atomic instructions"); two threads mapping memory
   at once could be given the same range, because finding a free range and
   mapping it were two steps (`find_free` now reserves what it hands out);
   and code that Leopard's Intel graphics libraries write at run time was
   checked for changes only in its first 8 bytes, with the translator's
   look-ahead trusting the code after it.
7. **Mostly done.** Speed: JIT compilers for both CPUs. PowerPC has one
   (`src/cpu/ppc/ppc_jit.cpp`; "The translator" above says how it works).
   Its first version kept every guest register in memory and ran a Python
   loop 4 times as fast as the interpreter; keeping the busiest seventeen in
   host registers made that 12.7 times. Still to do there: AltiVec and the
   rarer floating point instructions go to the interpreter one at a time,
   and a compare followed by a branch could be one host instruction pair
   instead of a dozen.

   Intel followed (`src/cpu/x86/x86_jit.cpp`): the common integer
   instructions, with the interpreter's own routines called for arithmetic
   whose flags are wanted. That alone made the Python loop ten times
   faster.

   Floating point came next, because an integer-only translator made
   maths slower, not faster: every floating point instruction ended a
   block, and a Mandelbrot set that the interpreter drew in 6.0 s took
   11.5 s. With SSE translated and x87 called in line it takes 0.62 s.
   A small benchmark (`tests/bench/maths.c`; `sh tests/bench/build.sh maths`
   builds it and `tests/bench/time.sh` times it), each part translated and
   interpreted, on an M4:

   | Part | Interpreted | Integer-only translator | Now |
   |---|---|---|---|
   | Mandelbrot set, doubles | 6.01 s | 11.46 s | 0.62 s |
   | Arrays of singles (packed SSE) | 0.68 s | 1.42 s | 0.06 s |
   | Arrays of doubles, square roots | 1.22 s | 2.45 s | 0.09 s |
   | Conversions between integers, singles and doubles | 1.63 s | 2.40 s | 0.24 s |
   | `long double` arithmetic (x87) | 0.40 s | 0.58 s | 0.28 s |
   | Python loop of floating point arithmetic | 27.4 s | 4.1 s | 2.2 s |

   Flags were what was left. A wanted flag used to cost a call into the
   interpreter's arithmetic, and a compare and branch cost one every time.
   Now the pair is two ARM instructions, and flags nobody has asked for
   yet wait as a note ("The Intel translator" above). The Python loop of
   integer arithmetic went from 3.0 s to 0.8 s, and the floating point one
   in the table from 2.2 s to 0.75 s: about what the same loops take on
   the PowerPC side (0.55 s for the integer one).

   Still to do there: MMX, string moves, 16-bit operands and the rarer SSE
   instructions, which video will lean on; and keeping the SSE registers
   in the Mac's own between instructions, where now each instruction
   loads and stores them.

## Known gaps

- Some Mach port rights are never released: rights that arrive with
  requests from other processes, IOKit's registry and iterator ports, and
  the ports of threads that have ended. That only adds up over hours of
  heavy system traffic.

- OpenGL and OpenAL pass through for both CPUs, but only the
  functions Marble Blast uses: much of OpenGL 1.x, AGL's context calls and
  OpenAL 1.1's playback calls. Another game will want others, which
  announce themselves on the first call ("... is not passed through to the
  Mac yet"). Full screen (`aglSetFullScreen`) isn't done, so games have to
  be set to play in a window. DrawSprocket still runs as guest code and
  does nothing useful.
- Sound comes out of one stereo 44.1 kHz device, with no input. The guest's
  Component Manager finds no components at all under cocaine (why is not
  known yet); the output units are supplied instead, but other components
  (Sound Manager's, QuickTime's codecs) still can't be found.
- Games are held to 60 frames a second (`COCAINE_GL_FPS`); past that, each
  frame only costs power, twice, since the guest's window server composites
  each one. Even so, a game's window in 2x mode is enlarged by the emulated
  window server every frame, which is most of what it costs while playing
  (about a quarter of a core on PowerPC, two fifths on Intel, for Marble
  Blast's 800 x 600).
- Leopard's 2x drawing was never finished: Carbon programs that don't ask
  for it, Marble Blast among them, are drawn at 1x and enlarged, which looks
  as it did before, only bigger.

- Guest pages are 4 KB and Apple Silicon pages are 16 KB, so guest memory
  protections are tracked but not enforced. The exception is the zero page,
  which is arranged to have a host page to itself: null-pointer accesses
  fault as they should.
- The kernel's own objects are only answered for the process they belong
  to. A task port sent to another process is a real port there, and that
  process can ask whose it is, but other calls made on it (reading another
  process's memory, say) get no reply.
- IOKit has almost no devices: most searches come back empty. There is a
  power-management root that never announces sleep or wake, and an input
  system with one mouse and one keyboard's worth of events, fed from the
  viewer. No display device: the window server makes do with a virtual one.
- The guest's display is one fixed window holding everything (2560 x 1600 unless
  `COCAINE_DISPLAY` says otherwise),
  with no pointer of its own drawn on it. Nobody is logged in to the guest
  system: the login window sets the session up and stops there.
- The Intel translator leaves MMX, string moves, 16-bit operands and the
  rarer SSE instructions (packs, saturating arithmetic, shifts by a
  register, the SSE3 extras) to the interpreter (about 65 million
  instructions a second on an M4), so code made mostly of those is no
  faster than before. x87 arithmetic is done in software at its full 80
  bits, so `long double` code gains little from translation. The SSE
  registers are loaded and stored around every instruction that uses
  them, which is most of what a floating point loop now costs.
- Neither translator ever frees what it has translated. Each has room
  for 128 MB of ARM64 code, which is a lot of program; past that, new code
  is interpreted. AltiVec instructions, and the less common floating point
  ones, are handed to the interpreter one at a time, so code that is mostly
  those runs no faster than before.
- The pasteboard server isn't running, so copy and paste between apps
  doesn't work.
- The daemons start afresh with each run that needs them, which costs a
  second or so every time.
- One process can't get another's task port by process number
  (`task_for_pid`); the security daemon asks and carries on without.
- Each run of cocaine is a session of its own, with its own daemons: two
  apps started separately can't see each other.
- A guest's own crash (a bad memory access, an illegal instruction) always
  ends it with cocaine's report. A handler the guest installed for that is
  not called.
- A signal that lands in the instant between cocaine checking for one and a
  system call starting to block waits until that call returns. `sigsuspend`
  is exempt: it is exact.
- `ps` and `top` don't work (no process list `sysctl`).
- A guest that ends by a signal it sent itself for a crash (`abort`) exits
  with status 128 + the signal number rather than dying by the signal, to
  keep this Mac from writing a crash report about cocaine each time.
- A shared file mapping is really shared when the guest lets the kernel pick
  the address and the file offset is a multiple of 16 KB. Otherwise, and for
  all private mappings, the guest gets its own copy of the file's bytes.
- Memory shared by passing a port can be mapped at an address of the
  guest's choosing only if that address sits the same distance into a host
  page as where it came from (which is how the window server uses it).
- A few Intel system programs (the font server and the login window among
  them) are encrypted on disk, and decrypting them takes keys that come
  from an Intel Mac's hardware (see "What you need to bring"). Without the
  keys those programs can't run and Intel apps get no windows. Their
  PowerPC halves are not encrypted.
- Now and then a system daemon crashes while a session is starting (seen
  once with the Intel directory daemon). It is started again, and nothing
  waits on the window server's session for more than 25 seconds, but the
  cause isn't found.
- x87 arithmetic is exact 80-bit, but its transcendental instructions (`fsin`,
  `fyl2x` and friends) are computed in double precision, so their last few
  bits differ from a real x87.
- Floating point exception flags are not tracked, and neither are the
  flush-to-zero modes of SSE and AltiVec. `rcpps`, `rsqrtps` and AltiVec's
  estimate instructions return exact results where real CPUs approximate.
- The importer skips files that are hard links to other files (manual pages
  and a few tools such as `/bin/ln`), because this folder doesn't allow
  creating hard links.

## Layout

```
src/main.cpp        command line tool
src/macho/          Mach-O parser
src/mem/            the 4 GB guest address space
src/cpu/ppc/        PowerPC: the interpreter, AltiVec, and the translator to ARM64
                    (ppc_jit.cpp, with its instruction encoder in a64.h)
src/cpu/x86/        32-bit Intel: the interpreter, x87 (with its 80-bit maths), MMX
                    and SSE, and the translator to ARM64 (x86_jit.cpp)
src/kernel/         the fake kernel: process loading, BSD system calls, sysctl,
                    Mach ports and messages, the comm page, path translation,
                    getattrlist, sockets, threads, processes, signals, IOKit
                    and its input system, decrypting protected programs, and
                    taking over guest library functions (hooks.cpp)
src/session/        the session server, cocaine's stand-in for launchd
src/viewer/         the window that shows the guest system's display
src/gl/, src/al/    OpenGL and OpenAL for guests, done by the Mac's own
src/audio/          Core Audio's hardware layer and output units for guests, played
                    through the Mac's own output
tools/              import-sysroot.sh; find-mig-stub.py, for working out the
                    private message protocols of system daemons; and
                    ppc-dis.py, a small PowerPC disassembler for reading
                    Apple's PowerPC code
tests/guest/        test programs and their tiny runtime
tests/tools/        elf2macho.py, a small linker for PowerPC test programs, and
                    a64-check.cpp, which checks the translators' ARM64 encodings
docs/               screenshots of firsts
tests/bin/          the built guest test programs
tests/bench/        a maths benchmark for the Intel translator, and how to build and time it
tests/bin-dynamic/  ones linked against the real libSystem (need a system root to run)
sysroots/           imported Mac OS X system roots (not in git)
```
