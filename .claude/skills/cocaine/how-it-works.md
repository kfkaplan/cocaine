# How cocaine works

Moved from the README on 2026-11-18. The design of each piece, with the reasons; keep it current when a piece changes.

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
  `DirectoryService`, `dirhelper`, `securityd`, `coreservicesd`, `configd`
  (without its plug-ins, `-b`, and in the foreground, `-d`), and
  `WindowServer`, `ATSServer` and `loginwindow`, the last three for Intel
  programs only where the keys to them have been supplied). Each starts in
  the guest's `/`, as launchd starts daemons;
  `COCAINE_DAEMONS=all`, or a
  comma-separated list of job labels, tries others. Daemons are told they are
  root, and their messages are stamped as root's, because both they and
  their clients check. A daemon can collect its ports either of the two ways
  `launchd` offers: by name over Mach, or by asking on the socket every job
  is started with. With `-v` cocaine says where the session's log is; it
  holds what the server did, what the daemons printed, and everything any
  guest sent to `syslog`, which is often the only place a daemon explains
  itself.
- **The desktop and the menu bar.** The Dock, which draws Leopard's desktop
  picture, can't be started (apps can't start other apps yet), so cocaine
  draws one itself from inside loginwindow once the session is ready: a
  desktop-level window that ignores the mouse, with `COCAINE_WALLPAPER` (or
  `images/linen.jpeg`) drawn to fill it (`src/kernel/wallpaper.cpp`). The
  Apple menu's logo is swapped for a pile of powder in a copy of CoreUI's art
  file that guests are shown instead of Apple's (`src/session/art.cpp`).
  Closing the viewer window ends the whole run. SKILL.md has the details.
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

