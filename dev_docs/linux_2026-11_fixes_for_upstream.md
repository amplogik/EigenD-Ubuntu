# Fixes from the Linux fork (amplogik/EigenD-Ubuntu), November 2026

Notes for cherry-picking into upstream (TheTechnobear/EigenD `3.0`). Everything below was found
with stack samples (gdb) and logs on a live rig, not guessed. Branch: `pytstate-reuse` on
`amplogik/EigenD-Ubuntu`. Environment: Ubuntu 26.04, Python 3.14.4, JUCE 8, a setup with
~150 agents and ~2,700 wires.

Short version: **Python 3.14 makes EigenD's C++→Python calls about 2,500× more expensive on
Linux's main thread**, which froze Workbench for minutes. One template change fixes it for
every pip wrapper. The rest are Workbench UI hot paths that the freeze had been hiding.

## 1. Python 3.14: one PyThreadState per call (the big one)

**Commits: `1247a2f9` + `1d4026f1` (take both, in that order).** File: `tools/pip_cmd/template`.

`lock_c2p` created and deleted a `PyThreadState` on *every* C++→Python call:

```cpp
t_ = PyThreadState_New(interp);  PyEval_AcquireThread(t_);
...                              PyEval_ReleaseThread(t_); PyThreadState_Delete(t_);
```

Python 3.14 sizes its recursion limit from the real C stack. The first time a *fresh* thread
state attaches, it runs `_Py_InitializeRecursionLimits → pthread_getattr_np`. On the **main
thread**, glibc answers that by reading and parsing `/proc/self/maps`, so the cost grows with
every library loaded.

Measured on 3.14.4 (stand-alone benchmark):

| `/proc/self/maps` lines | new state per call | reused state |
|---|---|---|
| 52 | 21 µs | 0.02 µs |
| 508 (Workbench: 517) | **104 µs** | 0.04 µs |
| 988 | 188 µs | 0.05 µs |

Workbench calls into Python per wire while agents load (`Wire::calculateRoute →
Workspace::getParentId → c2p get_parent`), so its UI thread was pinned for minutes. EigenD
itself pays the same tax.

Fix: keep one thread state per OS thread (`thread_local`, per module) and reuse it.

> **Gotcha, from experience:** don't track nesting with a depth counter (that was
> `1247a2f9`'s first version, and it segfaulted). EigenD nests C→Py→C→Py through
> `unlock_p2c`, e.g. `client_sync → Python → manager_checkpoint`, which **releases** the GIL
> in between. `1d4026f1` checks whether *this thread's* state is currently attached
> (`PyThreadState_GetUnchecked()`, `_PyThreadState_UncheckedGet()` before 3.13) and only
> re-attaches if not.

**Build gotcha:** `make` does not regenerate `*_native_python.cpp` when the template changes.
Delete `tmp/obj/**/*_python.cpp` (36 files) first.

macOS is probably far less affected: its stack lookup doesn't parse a maps file. The pattern is
still worth fixing everywhere.

## 2. Workbench UI hot paths (all platforms)

Once the Python cost was gone, these showed up in the same samples:

| Commit | What |
|---|---|
| `8cc11a89` | `MainComponent::clearGridMap(w)` walked all 100×50 grid squares per wire. Since `4b68b9dc`, every zoom re-routes every wire, which made each zoom quadratic (30–120 s with 2,700 wires). Each wire now records its own squares. |
| `c0294582` | The zoom re-route (`zoomChildren`) now runs in ~20 ms slices on the message thread instead of blocking. Routing is in true coordinates, so the re-route may be redundant; it's kept, but off the critical path. |
| `eb7da654`, `a11e97ac`, `4f432ea7` | `listWiresAt` runs on **every mouse move**. It copied the list, rebuilt and scaled each wire's path, and flattened its curves. It now skips repeats, rejects on cached route bounds, and tests **per segment** in true coordinates. Long wires have bounds covering half the canvas, so the per-segment test is the one that matters. |

Side effect worth knowing: while the UI thread was blocked, clicks queued up, and a popup menu
opening a minute later **grabbed keyboard focus** from other apps. That came across as "focus
stealing" and is gone with the above.

## 3. Loading progress window (all platforms)

`e78d75b0`, `400d60d4`, `267cc3a4`, `95ac969b`:
- `Workspace::oldProgress_` was **never initialised**, so `loaded()` dropped progress updates at
  random.
- The bar closed when the agents arrived, but the layout (`stateChanged` → `readState`) can
  arrive before or after them: first on a warm start, ~45 s later on a cold one. The bar now goes
  to 90% on agents, shows "Arranging workspace...", and closes once the layout is applied.
  There's a 120 s no-progress safety close. The index's 1-of-1 phase (only `<workbench>`) is
  ignored.
- `267cc3a4` also flipped `MIDI_FROM_BELCANTO_DEBUG` to 1 by accident; `396763b4` turns it back
  off. Pick both, or skip that one line.

## 4. Smaller fixes

| Commit | Platform | What |
|---|---|---|
| `f9fbeadb` | all | **Ours.** Workbench's epython startup command was snprintf'd into a 256-byte buffer (we shrank it from 4096 in the Ubuntu port). An installed root like `/usr/local/pi/release-3.0.2-beta-5` makes it 266 bytes, so the string was truncated and Workbench died with `SyntaxError`. Only bites installed packages, not the build tree. Upstream's 4096 is fine; noting it in case you took our port. |
| `c7381be0` | all | `host_plg.py` `chunker()`: `zlib.compress(str)` raises on Python 3, so **plugin state and window state were never saved**, with a traceback on every setup save. `std::string` crosses the bridge as Latin-1, so encode before compressing and decode each chunk (mirrors `get_blob`). |
| `759098db` | Linux | Plugin RUNPATH `$ORIGIN/../bin` is one level short for `plugins/<vendor>/plg_x/`, so 18 `controller` agents and the convolver failed with `ImportError: libpilo.so` unless `LD_LIBRARY_PATH` was set. Adds `$ORIGIN/../../../bin`. |
| `2f106b4b` (part) | all | `tools/SConstruct`: skip hidden directories when collecting `SConscript`s. A git worktree under `.claude/` was being built twice (library-defined-twice assert). |

## 5. Plugin host additions (ours; take if useful)

- **CLAP hosting** with editor support on Linux (`b44cbc15`, `2f106b4b`, `e419860c`): XEmbed host
  window and the `clap.gui`, `clap.timer-support` and `clap.posix-fd-support` host extensions.
  Two non-obvious bugs:
  - **X11 race:** the plugin creates its window *inside our host window on its own X
    connection*. If its request beats our unflushed `CreateWindow`, it gets a silent
    `BadWindow`. Fix: `XSync` our display before handing out the window id
    (`lib_juce/pijuce_linux_glue.cpp`).
  - Dexed's (older clap-juce-extensions) `request_resize` applies the scale twice. We treat it
    as a hint and re-query `get_size`.
- **Plugin window Zoom** (`5b628051`, `ba68eac0`): 50–200%, saved per audio unit as a 5th int in
  the window-state blob (older 4-int blobs = 100%).
- **Clear plugin button** (`9da3cb1f`): an empty activate on the plugin browser root means "no
  plugin"; new `clearable` browse protocol.

## 6. Commander and Browser on Python 3 / wxPython 4 (all platforms)

**Commits: `ff5b081e`, `4b6190ee`.** Both apps were still written for wxPython "Classic" (Python 2)
and died at startup on Python 3 with wxPython 4 ("Phoenix"). On Ubuntu 26.04,
`python3-wxgtk4.0` 4.2.4 is now packaged for Python 3.14, so no source build is needed.
- **Renames:** `wx.SplashScreen` → `wx.adv.SplashScreen`; `wx.SystemSettings_GetFont` →
  `wx.SystemSettings.GetFont`; `evt.GetPositionTuple()` → `GetPosition()`; `wx.StockCursor` →
  `wx.Cursor`; `MenuItem.GetText` → `GetItemLabelText`; `unichr` → `chr`.
- **`wx.AutoBufferedPaintDC` → `wx.BufferedPaintDC`.** Phoenix asserts unless the window has
  `wxBG_STYLE_PAINT`.
- **Python 3 `/` gives floats; wxPython ≥ 4.1 rejects floats** for coordinates and sizes. Rather
  than audit every division, `pisession/gui.py` adds a small shim that rounds floats at the wx
  boundary: the DC drawing calls, the window scroll and size calls, and `Rect`/`Point`/`Size`/
  `Pen`/`Font` construction.
- **GTK3 on native Wayland drops `wx.ClientDC` drawing** done outside paint events, and both apps
  draw that way about 60 times. Typed text in Commander never appeared. Linux fix: set
  `GDK_BACKEND=x11` before GTK starts (in `pisession/gui.py`), so they run on XWayland like
  EigenD. Not relevant on macOS.
- Both now run with no uncaught exceptions. Known: Browser's target is saved with the setup and
  only changes via the Belcanto `browse` verb (which needs a singular target), so a target picker
  would help.

## Not fixed yet (known)

- CLAP: no `clap.state` (patches aren't saved), no params, no transport.
- An unknown audio device name in a setup (e.g. a factory setup asking for `Core Audio/Built-in
  Output`) can crash PipeWire's ALSA plugin inside eigend.
- Everything builds with `-O0 -g` (`tools/linux_tools.py`), so an optimised build is probably
  worth a look.
