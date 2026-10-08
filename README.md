# Infinity Blade II — Nintendo Switch port

`infinityblade2_nx` is a native wrapper that runs the ARM64 Android **libib3.so**
compatibility runtime for **Infinity Blade II** on Nintendo Switch. It recreates
the Android, Bionic, audio, input and graphics services expected by the runtime
under Horizon OS.

This tree is based on the IB3 Switch stability pass (unified audio, forced
vsync, early-touch gate, memory tuning) and is retargeted for the
**InfinityBladeII-Android-1.6.1** runtime library (libib3.so, 4,444,616 bytes).

The repository does **not** include the game, APK, libraries or assets. You must
provide your own legally obtained compatible copy:

1. **APK** — `InfinityBladeII-Android-1.6.1.apk` (supplies `libib3.so`)
2. **IPA** — original Infinity Blade II iOS package (supplies `Payload/SwordGame.app`)

---

## Controls

Same mapping as the IB3 Switch port. Controller inputs are translated into the
taps and swipes used by the original mobile interface. Touchscreen remains
available in handheld mode.

| Input | Normal mode | Cursor mode |
| --- | --- | --- |
| **Touchscreen** | Native touch | Native touch |
| **Left Stick** | Combat swipe | Move cursor |
| **Right Stick** | Camera swipe | Shorter camera swipe |
| **D-Pad** | Directional swipe | Directional swipe |
| **A** | Tap centre | Click at cursor |
| **B** | Hold shield | Hold bottom-right |
| **Y / X** | Sword / magic | Sword / magic |
| **ZL / ZR** | Dodge left / right | Click at cursor |
| **+ / –** | Pause | Pause |
| **L** | — | Recenter cursor |
| **R** | Enable cursor mode | Return to normal mode |

Early touches during boot are held until the first frame is presented (avoids a
null-deref crash in the guest). You do not need to avoid the screen.

---

## Build

### Requirements

- devkitPro (devkitA64, libnx)
- Switch Mesa, libdrm_nouveau
- Switch SDL2, mpg123, zlib, libpng
- GNU Make

```bash
pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-mpg123 switch-zlib switch-libpng
```

**Important:** build from a path **without spaces or parentheses**
(e.g. not `IB2-Switch-stable (1)`).

```bash
cd IB2-Switch
make -j
```

Output: `infinityblade2_nx.nro`

---

## Preparing the runtime

```bash
python3 prepare_runtime.py ib2.apk --ipa ib2.ipa --output runtime --nro path/to/infinityblade3_nx.nro
```

Copy the `runtime/` folder to the SD card as:

```text
sd:/switch/infinityblade3_nx/
├── infinityblade_nx.nro
├── cursor.png
├── libIB3.so
├── game/
├── userdata/
└── SaveData/
```

Launch with a **full-memory title override**, not Album applet mode.

---
to report bugs upload your log file in the issues tab

Log file: `infinityblade2_nx.log`

---

## Notes
- 1.0.9: audio plays and the output no longer dies mid-session
- 1.1.0: ETC2 textures are converted to DXT1/DXT5 on upload, cutting texture
  memory 5-9x. This targets the ~25-minute `std::bad_alloc` crash and textures
  that render black. Not yet verified on hardware, see STABLE.md
- The log prints a `mem:` line every 10 s; include the log when reporting crashes
  
Credits 
aks796 - uses the ib1 nx port src as a base to get the game running
