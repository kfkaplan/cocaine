# Known gaps

Moved from the README on 2026-11-18. What doesn't work yet, or works only partly, and why. The README keeps a short list of the ones a user would notice.

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
  is interpreted. The less common AltiVec and floating point instructions
  are handed to the interpreter one at a time, so code that is mostly
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
  from an Intel Mac's hardware (see "What you need" in the README). Without the
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
- Guests can't launch other apps through Launch Services: loginwindow's
  `LSOpenApplication` calls fail with -10827, so there is no Dock, Finder or
  SystemUIServer (on hold, by the user's choice, 2026-11-19). The desktop
  picture the Dock would draw is drawn by cocaine instead.
- Launch Services can't save the user's copy of its database: its `lssave`
  helper can't work out the user's cache folder (the lookup is in
  CarbonCore, not yet understood), so it writes to an empty path plus `~`,
  which lands in the system root as `sysroots/10.5/~`. Harmless: the
  database is built again in memory on each run.
- Leopard's open and save panels don't work: the file browser stays
  blank, and VLC's Browse button crashes in `DASessionScheduleWithRunLoop`
  (no disk arbitration daemon). Tried on 2026-11-20 and rolled back: running
  diskarbitrationd, and showing this Mac's picker in NSOpenPanel's place.
  Opening Leopard's panel at all upsets the program's window server traffic
  (oversized event messages, replies landing on the wrong call,
  `CGSAddTrackingRect` errors), leaving its dialogs unresponsive; that is
  the thing to fix first.
