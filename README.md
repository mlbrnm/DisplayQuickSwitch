# Nvidia Quick Switch

A native **Windows x64 tray application** for manually switching display presets on an NVIDIA GPU. Written in C++20/Win32; no .NET runtime, browser engine, resident service, or Profile Inspector installation is required.

I created this tool because I found keeping track of all the settings while switching monitors or display modes to be a hassle. I'm one of those crazy people who uses a 4K OLED TV as a monitor so I often like to play at alternate resolutions such as 3840x1600 or 3440x1440 ultrawide, and I also sometimes use a small CRT monitor that requires very different settings from a modern display.

**Status: first working build, tested on an RTX 5000 series GPU / driver 617.42.** 

<img height="200" alt="Screenshot 2026-10-10 210525" src="https://github.com/user-attachments/assets/2eb47b2e-1bab-4472-9e29-5395838bbcaf" />
<img height="200" alt="quickswitch" src="https://github.com/user-attachments/assets/fd2da02f-3d4a-4e23-8aa9-973f43d3e083" />

## Run

1. Launch the executable and find the icon in the system tray (possibly inside the overflow menu).
2. Right-click → **Manage presets…** → **New…**.
3. Select the target monitor. **Make this the primary monitor** is checked by default.
4. Optionally choose **Capture current settings**, then edit the values you want.
5. Save, then select the preset from the tray menu.
6. Choose **Keep** within 20 seconds. Otherwise, the app attempts to restore the previous display and global driver settings.

Starting the application, creating/editing presets, and opening diagnostics **do not apply settings**. Capture is read-only. Nothing is automatically applied on startup.

### Included settings

- Primary monitor selection. Other active monitors remain enabled; their desktop positions are translated so the new primary is at `(0, 0)`.
- Resolution and refresh rate, with refresh values stored in millihertz (`59.94`, `119.88`, etc.).
- GPU scaling: no scaling, aspect ratio, full-screen/stretch, integer scaling.
- Per-monitor **Windows scaling** (text/apps/UI): leave unchanged, or a standard percentage from 100% to 500%, subject to the monitor's supported range. This is separate from GPU scaling.
- Global G-SYNC policy: off, full-screen only, full-screen + windowed.
- Global V-SYNC: application-controlled, off, on. Both the Direct3D setting and OpenGL default swap-interval policy are updated.
- Global NVIDIA frame limiter: disabled or a cap up to the public SDK's limit of 1023 FPS.
- **Restore previous settings** after a confirmed switch. One undo snapshot is retained in memory; the next successful switch replaces it.
- Optional **Start with Windows**, using only the current user's Run registry entry. Off by default; no scheduled task or service.

<img width="1450" height="962" alt="Screenshot 2026-10-10 211258" src="https://github.com/user-attachments/assets/fefc4c57-a2bc-4e45-ac19-966bbba70c59" />


Blank resolution/refresh/FPS fields and **Leave unchanged** options do not override those settings. FPS `0` explicitly disables the limiter. Resolution must have both dimensions supplied, or both blank.

Presets and bounded error logs are stored in:

```text
%LOCALAPPDATA%\NvidiaQuickSwitch\presets.ini
%LOCALAPPDATA%\NvidiaQuickSwitch\errors.log
```

The editor saves presets atomically as UTF-16 LE. Monitor matching uses the Windows monitor device interface path, **not** a transient `DISPLAY1` number. Moving a monitor to another port/adapter can change that identity; edit and reselect it if needed. There are no bundled presets that assume which screen is your CRT.

Existing presets remain compatible: a missing `WindowsScalePercent` field means **leave Windows scaling unchanged**. Saving with the new editor adds the field. Capture includes the selected monitor's Windows scale when available; if Windows cannot report it, that field stays unchanged. A scale requested by a preset is applied after the display mode/primary-monitor change, verified, and included in timeout/manual restoration.

Windows DPI range handling follows [SetDPI](https://github.com/imniko/SetDPI): a reported current offset outside the supported range is clamped when decoding it, and the maximum can be below the recommended scale. This avoids rejecting valid percentages merely because an offset is stale or the recommendation exceeds the current maximum. **Requested preset percentages are never silently clamped** to a different value. If a switch still fails, copy the affected monitor's `DPI CCD` line from **Read-only diagnostics** and the new error text; these include the raw offsets needed to identify an unrecognized range or a Windows API failure.

## Important limitations and safety

- Windows does not expose a documented public setter for the normal per-monitor scale selector. This feature uses the undocumented CCD DPI packets, validates their reported range, and fails rather than silently selecting a different percentage. Windows updates could change this interface. **Global custom scaling is not supported**: disable it in Windows Settings, or leave Windows scaling unchanged in presets. There are no DPI registry writes, forced sign-outs, or changes to other monitors' scale settings. Some older applications may need a restart to redraw correctly.
- Target monitors must already be active in Windows. This version does not enable disabled/disconnected monitors, disable other displays, create custom timings, or support clone/Surround layouts. All active display paths must be NVIDIA-driven; mixed-GPU configurations are rejected rather than incompletely restored.
- G-SYNC changes **global driver policy**, not every monitor-specific Control Panel checkbox. Saved-policy readback is **not proof that VRR is actually running**. A CRT remains fixed-refresh. Use NVIDIA's G-SYNC indicator and the monitor's own refresh indicator to check your LCD.
- Apply presets **before launching games**. Global changes can be overridden by existing per-game NVIDIA profiles, and running games may need a restart to pick up driver policy. This app deliberately does not rewrite game profiles.
- The driver validates display configurations before any persistent changes. Requested driver edits are staged before applying the display topology. Unsupported settings abort the operation; they are not silently skipped.
- Resolution changes retain the relative positions of display origins; they do not automatically repack the desktop. An incompatible/overlapping arrangement may be rejected by the driver and require an initial adjustment in Windows display settings.
- After applying, the app checks all active display identities, positions, resolutions, primary flags, scaling where requested, and the saved driver values. Refresh readback allows a **0.5 Hz** tolerance for driver timing differences. Fractional input is still sent to NVAPI without integer rounding.
- On an apply/readback error, the app attempts restoration of display, driver, and requested Windows scaling state independently. It restores only driver setting IDs changed by the preset, preserving prior user overrides vs defaults and leaving unrelated settings alone. Do not edit those same global settings or the target monitor's scale in another utility during the transaction/undo period.
- **Rollback is best-effort, not crash protection.** The confirmation countdown works while the process/message loop is running. A hung driver call, killed/crashed process, shutdown, or disconnected monitor can prevent restoration. The snapshot is not persisted across restarts. If restoration fails, an error is shown and **Restore previous settings** can retry. Keep a known-good Windows display configuration and test conservatively, especially with CRT timings.
- The app never requests a driver reload to force a mode through. Some writes may require elevated permissions on a particular installation; if NVAPI reports access denied, close the app and manually run it as administrator. There is no automatic elevation or always-on elevated helper.

## Lightweight design

The tray uses a hidden Win32 window and the Windows message loop. There is **no idle timer, process scanning, background polling, network access, telemetry, or automatic game detection**. Only a user-requested switch starts a worker thread; it exits when the operation completes. A short-lived timer drives the keep/revert dialog. NVIDIA API initialization/unloading is paired around requested operations; startup itself does not initialize NVAPI. The executable statically links the Microsoft C++ runtime.

In a local Release-build tray smoke test, the executable was **449,536 bytes (~439 KiB)**; the idle tray used **9.23 MiB working set / 1.54 MiB private bytes**, accumulated **0 ms of CPU time over a 2-second idle sample**, and had not loaded the NVAPI driver DLL. These are a single startup-only measurement, not guarantees or measurements after a driver operation. Actual working set depends on Windows and NVIDIA's loaded modules. Executable size is not the same thing as RAM use.

## Build

Requirements:

- Visual Studio / Build Tools with **Desktop development with C++**, an x64 MSVC toolchain and a Windows SDK.
- CMake 3.24+.
- Internet on the first configure to download the pinned, MIT-licensed public NVIDIA NVAPI SDK. No network access is needed at runtime.

For Visual Studio 2026:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For Visual Studio 2022, use `-G "Visual Studio 17 2022"` in a separate build directory. Copy `NvidiaQuickSwitch.exe` and `NVIDIA-NVAPI-LICENSE.txt` together when distributing the build. The NVIDIA driver supplies NVAPI's implementation; driver DLLs are not redistributed.

## References

- [NVIDIA public NVAPI SDK](https://github.com/NVIDIA/nvapi), pinned to `70d337db9186e968eab622f7e786de7e437faf3d`.
- [NVIDIA Profile Inspector](https://github.com/Orbmu2k/nvidiaProfileInspector), used as a reference for driver policy behavior. This app does not embed or launch Profile Inspector.
- [SetDPI](https://github.com/imniko/SetDPI), used as a reference for Windows DPI packet/range handling. This app does not embed or launch SetDPI or perform its legacy `AppliedDPI` registry write.
