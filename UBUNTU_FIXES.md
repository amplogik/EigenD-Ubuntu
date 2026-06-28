# EigenD Ubuntu 24 / 25 Build Fixes

## Instructions

- Run `make pkg` in the EigenD project to build a debian/ubuntu package
- Use `make` for quick test builds and `make pkg` for full package builds

## Issues with mainline

1. **GCC 13+ strictness**: `-Werror` combined with new GCC warnings required fixes across multiple files for `sys_errlist` removal, buffer overflows, uninitialized variables, non-POD memset, format truncation.

2. **HarfBuzz symbol conflict**: The system HarfBuzz (v12.3.2) and JUCE's bundled HarfBuzz (v10.1.0) had conflicting symbols. The build was compiling `harfbuzz.cc` directly (without `HAVE_FREETYPE=1`) and linking against system `-lharfbuzz` to fill missing symbols. At runtime, function calls resolved to the system version with incompatible struct layouts, causing `hb_font_get_nominal_glyph()` to always fail (glyph_count=0). **Fix**: Compile `juce_graphics_Harfbuzz.cpp` instead of `harfbuzz.cc`, and remove system harfbuzz from linker flags.

3. **Python version mismatch**: EigenD was configured for Python 3.14, but system wxPython (`python3-wxgtk4.0 4.2.1`) is compiled for Python 3.12. No prebuilt wxPython wheels exist for 3.14 and building from source fails. **Fix**: Switch to Python 3.12.

4. **Python 2 syntax in pip_cmd/lex.py**: `except re.error,e:` syntax (Python 2) fails under Python 3.12.

5. **Compile-time Python version check**: `lib_juce/epython.cpp` had a hardcoded check requiring Python 3.14 (`PY_VERSION_HEX < 0x030E0000`).

6. **Quit crash - GIL not held**: The pip-generated Python wrappers (`prepare_quit()`, `quit()`) assume the GIL is held when `pip_usegil=true`, but `lock_c2p` does nothing in that mode. The JUCE message thread doesn't hold the GIL. Calling `PyObject_CallFunction` without the GIL causes NULL return -> `pip_err_t("call problem")` -> `std::terminate`.

7. **Quit crash - GIL deadlock**: Attempting `PyGILState_Ensure()` or `PyEval_RestoreThread()` from the JUCE thread deadlocks because Python background threads (piw agents) actively hold the GIL.

8. **Quit crash - pure virtual in destructor**: Deleting `EigenMainWindow` triggers destructors of C++/Python bridge objects that call virtual methods on partially-destroyed objects.

9. **Quit fix**: Skip all Python backend calls during quit, skip window deletion, call `cleanup()` then `_exit(0)` for immediate clean termination.

10. **commander and browser crashing on launch: opening them resulted in immediate termination with no clearly caught exception.

### Build Fixes (all completed)
- `picross/src/pic_resources.cpp` - Added `get_global_resources()` for Linux
- `picross/src/pic_thread_posix.cpp` - Added `string.h` include, fixed uninitialized array
- `piagent/src/pia_udpnet_linux.cpp` - Replaced `sys_errlist` with `strerror()`
- `piw/src/piw_wavrecorder.cpp` - Fixed memset on non-POD struct
- `tools/generic_tools.py` - Fixed sprintf buffer issues, converted to std::string
- `lib_juce/epython.cpp` - Same std::string conversion
- `lib_midi/src/control_mapper_gui.cpp` - Fixed viewport logic error
- `plg_t3d/src/Madrona/SoundplaneOSCOutput.cpp` - Fixed buffer size

### Font/Text Rendering Fix
- `tools/linux_tools.py` - Removed `harfbuzz` from system library linking, fixed `libfontconfig` -> `fontconfig`
- `lib_juce/SConscript` - Changed from `harfbuzz.cc` to `juce_graphics_Harfbuzz.cpp`

### Python Version Fix (3.14 -> 3.12)
- `tools/linux_tools.py` - Changed Python path to `/usr/bin/python3.12`
- `Makefile` - Changed Linux `PYTHON_BUILD` to `/usr/bin/python3.12`
- `tools/pip_cmd/lex.py` - Fixed two `except re.error,e:` -> `except re.error as e:`
- `lib_juce/epython.cpp` - Updated version check from 3.14 to 3.12

### Quit Fix
- `lib_juce/epython.h` - Added `lock()` and `unlock()` methods to `PythonInterface`
- `lib_juce/epython.cpp` - Implemented `lock()` (`PyEval_RestoreThread`) and `unlock()` (`PyEval_SaveThread`)
- `app_eigend2/eigend.cpp` - Rewrote `do_quit()` to skip Python backend calls; rewrote `systemRequestedQuit()` to skip window deletion, use `cleanup()` + `_exit(0)`

### Build system files
- `Makefile` - Build entry point, Python path config
- `tools/linux_tools.py` - Linux build config, library linking, deb packaging, desktop entry installation
- `tools/generic_tools.py` - Generic build system, Python stub template, packaging helpers
- `tools/unix_tools.py` - Unix build base class
- `tools/pip_cmd/lex.py` - Fixed Python 2 syntax
- `lib_juce/SConscript` - JUCE library build, HarfBuzz compilation fix
- `SConscript.first` - Package descriptions, release version

### Core fixes
- `picross/src/pic_resources.cpp` - Linux resource path functions
- `picross/src/pic_thread_posix.cpp` - Thread init fix
- `piagent/src/pia_udpnet_linux.cpp` - Network error string fix
- `piw/src/piw_wavrecorder.cpp` - Non-POD memset fix
- `lib_midi/src/control_mapper_gui.cpp` - Viewport fix
- `plg_t3d/src/Madrona/SoundplaneOSCOutput.cpp` - Buffer size fix

### Python/JUCE integration
- `lib_juce/epython.h` - PythonInterface class with lock/unlock
- `lib_juce/epython.cpp` - Python startup, GIL management, version check
- `app_eigend2/eigend.cpp` - Main app, quit handling, menu system
- `app_eigend2/eigend.h` - Backend interface (c2p_t)
- `app_eigend2/backend.py` - Python backend (prepare_quit, quit)

### Font/HarfBuzz
- `lib_juce/juce/modules/juce_graphics/juce_graphics_Harfbuzz.cpp` - Proper HarfBuzz wrapper with HAVE_FREETYPE
- `lib_juce/juce/modules/juce_graphics/native/juce_Fonts_linux.cpp` - Linux font resolution
- `lib_juce/juce/modules/juce_graphics/native/juce_Fonts_freetype.cpp` - FreeType typeface creation

### Tool launching
- `picross/src/pic_tool_linux.cpp` - fork/exec tool launching

### Desktop integration
- `resources/eigend.png` - Application icon (512x512 RGBA PNG)

### Additional Fixes
- app_browser and app_commander now working as expected. The source of the issue appeared to be wxPython version throwing uncaught exceptions.

- Ubuntu 24 LTS compatibility
> 1. moved to gcc 13.3.0, requiring C++ syntax updates to pass warnings and error checks.
> 2. changed Python requirement back to 3.12 (system native on Ubuntu 24. (A stable 3.14 will only be available in April with Ubuntu 26 unless you use deadsnakes ppa, and it has issues) 
> 3. Had to use custom harfbuzz built in project, and not system.  System harfbuzz is ahead of JUCE, and results in glyph positioning errors if you are using mesa built with AMD drivers.

### Install / Upgrade Gotcha — stale unversioned tree shadows new build

The `pi-eigend` deb installs into a **versioned subtree**: `/usr/local/pi/release-<version>/`. It does **not** overlay the unversioned `/usr/local/pi/{bin,modules,include,plugins,resources,tools}` paths. The desktop entry at `/usr/share/applications/eigend.desktop` correctly points at the versioned path, so launching via the desktop icon is fine.

If a stale unversioned install exists at `/usr/local/pi/{bin,modules,...}` (e.g. from a much older install, manual `make install`, or a system snapshot/restore), invoking `/usr/local/pi/bin/eigend` from the command line silently runs the **old** binary instead of the new one.

**Symptom observed (2026-05-24):** UI launches normally, libusb PSU mode-flip succeeds (EM→MM), but instrument input is dead and the console shows:
```
ImportError: No module named app_eigend2
NameError: name 'bugs_cli' is not defined
```
The stale binary was a 2024-12-17 build linked to `libpython2.7` with Python 2.7 `.pyc` files — broken because Ubuntu has progressively dropped Python 2.7 from the archive. The stale tree was not owned by any current dpkg package (`dpkg -S /usr/local/pi/bin/eigend` returned nothing); suspected origin is a system snapshot/restore post-Ubuntu-upgrade.

**Diagnosis:**
```sh
file /usr/local/pi/bin/eigend                       # check build date
ldd  /usr/local/pi/bin/eigend | grep python         # libpython2.7 is the giveaway
file /usr/local/pi/modules/app_eigend2/*.pyc        # "python 2.7 byte-compiled" confirms
```

**Fix:**
```sh
sudo apt-get purge -y pi-eigend
sudo mv /usr/local/pi /usr/local/pi.mixed            # archive anything that survived purge
sudo dpkg -i tmp/pkg/pi-eigend_<version>.deb
ls /usr/local/pi                                     # should contain only release-<version>/
```

### USB — base station goes silent (isochronous OUT wedged)

**Symptom (recurring, 2026-06-28):** The Alpha/Tau base station (PSU) enumerates fine and flips EM→MM normally (`2139:0003` PSU-EM → `2139:0105` PSU-MM). The controller's own LEDs show it linked to the base station, and the base station shows linked to the computer — but EigenD sends **nothing** to the controller: no tonic lights, no key splits, no setups. It behaves as if the controller were unplugged, even though control and IN traffic clearly work (the device stays connected). Works fine on native macOS/Windows EigenD.

**Key trait:** once wedged it stays wedged across EigenD restart, USB replug, **and a full host reboot**. The base station is mains-powered, so it retains the bad endpoint state; pulling its power did not clear it either. Only a USB-level reset fixed it.

**Confirmation:** with the rig wedged on Linux (no machine move), a host-side bus reset cleared it immediately:
```sh
lsusb | grep eigen                 # note the bus/dev, e.g. 2139:0105
sudo usbreset 2139:0105            # "Resetting PSU-MM ... ok"
# quit + restart EigenD -> lights and setups came back
```
`usbreset` == the `USBDEVFS_RESET` ioctl == libusb's `libusb_reset_device`.

**Root cause:** the Linux open path (`picross/src/pic_usb_linux.cpp`, `usbdevice_t::impl_t::impl_t`) only did `init → open → claim_interface → get_speed`. It never reset, reconfigured, or cleared the device. The native drivers do (see `SetConfiguration` / `ResetDevice` / `ClearPipeStallBothEnds` in `pic_usb_macosx.cpp`), which is why moving the rig to another machine "fixed" it — that machine issued the reset Linux never did.

**Fix:** `pic_usb_linux.cpp` now calls `libusb_reset_device()` on open (before claim), handling `LIBUSB_ERROR_NOT_FOUND` by reopening. The reset happens on the already-mode-switched `0x0105` handle (the EM→MM firmware download is done earlier in Python — `plg_keyboard/keyboard_X.py:518-520`, `lib_alpha2/`), so it clears the wedge without disturbing the downloaded MM firmware. Set `PI_USB_NO_RESET=1` to disable if it ever interferes with another device. Kept minimal — just the reset, which is the demonstrated fix.

### TODO
- plugin scanner doesn't work, and throws a bunch of errors on first load. 
>> I suspect that this is because I am using jBridge to load win VSTs on linux. Haven't really looke into it much.  However, most linux users are using other VST hosts or LV2, LADSPA plugins, which are not supported.  I will look into this when I have time.
>> for now, I am suppressing that window on linux, and outputting only to SDTOUT if someone wants to try loading things.  It's just annoying otherwise.  I'll look more into linux plugins when I have some time. 

- investigate parallel loading of agents to improve performance

