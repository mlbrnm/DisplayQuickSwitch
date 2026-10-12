# Display Quick Switch

A native **Windows x64 tray application** for manually switching display presets on NVIDIA, AMD, Intel and mixed-GPU systems. Written in C++20/Win32; no .NET runtime, browser engine, resident service, or Profile Inspector installation is required. The tray icon uses **#007AFF** blue.

I created this tool because I found keeping track of all the settings while switching monitors or display modes to be a hassle. I'm one of those crazy people who uses a 4K OLED TV as a monitor so I often like to play at alternate resolutions such as 3840x1600 or 3440x1440 ultrawide, and I also sometimes use a small CRT monitor that requires very different settings from a modern display.

<div align="center">
  <img width="772" height="527" alt="Screenshot 2026-10-12 115608" src="https://github.com/user-attachments/assets/599f9b1e-b10a-4999-bb2b-ab4f29573ecd" />
</div>

_Resolution, refresh rate, primary-monitor selection, monitor layouts and Windows scaling do not require an AMD/Intel SDK. Driver-specific controls are shown only when their backend and runtime capabilities are available; unavailable features are listed in a grey notice in the preset editor. **AMD driver controls are an optional build**, because ADLX has separate SDK licensing and distribution/EULA requirements (see below). NVIDIA and Intel backends are included in the default build._

## Run

1. Launch the executable and find the icon in the system tray (possibly inside the overflow menu).
2. Right-click → **Manage presets…** → **New…**.
3. Select the target monitor. **Make this the primary monitor** is checked by default.
4. Optionally choose **Capture current settings**, then edit the values you want.
5. Save, then select the preset from the tray menu.
6. Choose **Keep** within 20 seconds. Otherwise, the app attempts to restore the previous display and global driver settings.

Starting the application, creating/editing presets, and opening diagnostics **do not apply settings**. Capture is read-only. Nothing is automatically applied on startup.

### Monitor layout presets

The tray has two independent preset lists, separated by a divider:

- **Display settings**: the existing presets change a target display's mode/settings and optionally make it primary. They do not enable or disable monitors.
- **Monitor layouts**: choose which connected monitors are enabled and which enabled monitor is primary. They do not edit driver policy or Windows DPI settings.

Choose **Manage monitor layouts… → New…**, check the monitors to enable, select the primary monitor, and save. The editor includes connected displays that are currently disabled. **Capture current layout** copies the current enabled/primary state. Layouts use the same 20-second Keep/Revert confirmation and **Restore previous settings** action; the shared undo remembers the last successful switch of either kind.

For example, create `Monitor only`, `CRT only`, and `Both — Monitor primary` layouts, then apply a display-settings preset after selecting a layout. At least one monitor must be enabled, and the primary must be enabled. An enabled monitor that is disconnected causes the entire switch to fail before applying changes; an absent monitor marked disabled is harmless. New monitors not recorded in an older layout keep their current enabled state—edit/save the layout to include them explicitly.

### Included settings
<img width="1378" height="980" alt="Screenshot 2026-10-12 115639" src="https://github.com/user-attachments/assets/499e4fa1-d2e3-41e2-9ec0-d1b9700f42c6" />

- Optional primary-monitor selection in display-settings presets. Other active monitors remain enabled; their desktop positions are translated so the new primary is at `(0, 0)`.
- Resolution and refresh rate, with refresh values stored in millihertz (`59.94`, `119.88`, etc.).
- GPU scaling where supported by the selected vendor backend: centered/no scaling, aspect ratio, full-screen/stretch, integer scaling.
- Per-monitor **Windows scaling** (text/apps/UI): leave unchanged, or a standard percentage from 100% to 500%, subject to the monitor's supported range. This is separate from GPU scaling.
- NVIDIA global G-SYNC policy, AMD per-display FreeSync (optional ADLX build), or Intel per-display Arc Sync where supported. These are **not interchangeable policies**: only NVIDIA has the full-screen/windowed selector here.
- Vendor-specific V-SYNC: application-controlled, off, on. NVIDIA updates Direct3D and OpenGL policies; AMD ADLX's Wait for Vertical Refresh is an **OpenGL** policy; Intel uses supported gaming flip modes.
- GPU frame limiter: disabled or a cap within the runtime driver range/step (up to 1023 FPS in the preset format). NVIDIA uses NVAPI, AMD uses FRTC (optional ADLX build), Intel uses IGCL Frame Limit.
- **Restore previous settings** after a confirmed switch. One undo snapshot is retained in memory; the next successful switch replaces it.
- Separate monitor layout presets for enabling/disabling connected monitors and choosing the primary.
<img width="1443" height="720" alt="Screenshot 2026-10-12 115719" src="https://github.com/user-attachments/assets/32a2d3f1-b9b7-4ec1-820c-5c3fc732f5fb" />

- Optional **Start with Windows**, using only the current user's Run registry entry. Off by default; no scheduled task or service.

Blank resolution/refresh/FPS fields and **Leave unchanged** options do not override those settings. FPS `0` explicitly disables the limiter. Resolution must have both dimensions supplied, or both blank.

Presets and bounded error logs are stored in:

```text
%LOCALAPPDATA%\DisplayQuickSwitch\presets.ini
%LOCALAPPDATA%\DisplayQuickSwitch\layouts.ini
%LOCALAPPDATA%\DisplayQuickSwitch\errors.log
```

Existing `presets.ini` and `layouts.ini` files in `%LOCALAPPDATA%\NvidiaQuickSwitch` are copied on first use when the corresponding new file does not exist. Originals are not deleted and new files are never overwritten by migration. Toggle **Start with Windows** to register the new executable and remove the old startup entry. The legacy single-instance mutex is intentionally retained so old and new versions cannot apply settings concurrently.

The editor saves presets atomically as UTF-16 LE. Monitor matching uses the Windows monitor device interface path, **not** a transient `DISPLAY1` number. Moving a monitor to another port/adapter can change that identity; edit and reselect it if needed. The legacy INI field name `Gsync` is retained for compatibility; on AMD/Intel its values `0` and `1` mean per-display VRR off/on. An imported NVIDIA windowed-policy request is rejected on other vendors, not silently translated. Saving through the editor clears unavailable driver fields, as indicated in its notice; applying an unedited preset with unavailable driver requests fails **before display changes**.

### Driver API support and scope

| Feature | NVIDIA NVAPI | AMD ADLX (optional) | Intel IGCL |
| --- | --- | --- | --- |
| Resolution/refresh/primary/layout/DPI | Supported | Supported without ADLX | Supported without IGCL |
| GPU scaling | Per-display; all-NVIDIA active topology required | Per-display, GPU scaling + scaling mode | Per-display if supported |
| Integer scaling | Driver validates requested mode | Per-display if supported | **Adapter-wide** retro scaling if supported |
| Variable refresh | Global G-SYNC off/full-screen/windowed policy | Per-display FreeSync on/off | Per-display Arc Sync off/recommended profile |
| V-SYNC | Global Direct3D + OpenGL policies | GPU-wide OpenGL Wait for Vertical Refresh | GPU-wide gaming flip modes if supported |
| Max FPS | Global limiter, 0–1023 | GPU-wide FRTC, driver range/step | GPU-wide Frame Limit, driver range/step |

Capabilities depend on hardware, monitor and installed driver, not just the vendor name. Intel displays are matched using the Windows adapter LUID and target ID. The optional AMD backend uses unambiguous EDID matching and **disables driver controls for missing/duplicate EDIDs**, rather than risk changing the wrong display/GPU. Windows-only settings remain usable in that case. The app does not implement AMD Enhanced Sync or Radeon Chill, Intel Adaptive Sync Plus tuning, or custom Arc Sync profiles (existing custom-profile values are retained for rollback).

NVIDIA global policy controls also work for NVIDIA targets in mixed-GPU layouts, using Windows for the display modeset. NVIDIA GPU scaling remains unavailable in those mixed layouts. Basic display capture/apply does not initialize AMD/Intel APIs unless their driver settings are requested (read-only capability probes/capture may initialize them).

Windows DPI range handling follows [SetDPI](https://github.com/imniko/SetDPI): a reported current offset outside the supported range is clamped when decoding it, and the maximum can be below the recommended scale. This avoids rejecting valid percentages merely because an offset is stale or the recommendation exceeds the current maximum. **Requested preset percentages are never silently clamped** to a different value. If a switch still fails, copy the affected monitor's `DPI CCD` line from **Read-only diagnostics** and the new error text; these include the raw offsets needed to identify an unrecognized range or a Windows API failure.

## Important limitations and safety

- Windows does not expose a documented public setter for the normal per-monitor scale selector. This feature uses the undocumented CCD DPI packets, validates their reported range, and fails rather than silently selecting a different percentage. Windows updates could change this interface. **Global custom scaling is not supported**: disable it in Windows Settings, or leave Windows scaling unchanged in presets. There are no DPI registry writes, forced sign-outs, or changes to other monitors' scale settings. Some older applications may need a restart to redraw correctly.
- Display-settings presets require an active target; use a monitor layout first to enable a connected but disabled display. Disconnected monitors cannot be enabled. Custom timings and clone/Surround layouts are not supported. All-NVIDIA presets with driver-feature requests retain the NVAPI topology path; other/mixed topologies and basic display-only presets use Windows CCD with fractional rational refresh requests and readback verification. If Windows cannot retain the requested mode/rate or another display's state, the transaction fails and attempts rollback.
- Monitor layouts preserve the modes and relative positions of displays that remain enabled, translating positions around the new primary. Windows restores saved modes/topology for re-enabled displays when available, otherwise chooses a workable mode; newly enabled displays are placed to avoid overlap. A layout does not specify resolution/refresh for an inactive screen—apply a display-settings preset afterward if needed. No DRS or DPI settings are written, although Windows/driver reactivation may restore monitor-specific state as part of a modeset. The previous enabled set, modes, and positions are retained for best-effort rollback.
- G-SYNC changes **global driver policy**, not every monitor-specific Control Panel checkbox. Saved-policy readback is **not proof that VRR is actually running**. A CRT remains fixed-refresh. Use NVIDIA's G-SYNC indicator and the monitor's own refresh indicator to check your LCD.
- Apply presets **before launching games**. GPU-wide changes can be overridden by existing per-game profiles, and running games may need a restart to pick up driver policy. A vendor API's saved policy is not a guarantee of support in every rendering API/game. This app deliberately does not rewrite game profiles.
- Display configurations are validated before display writes. Requested driver values are checked/snapshotted before applying the display topology (NVIDIA additionally stages writes in its DRS session). AMD/Intel do not offer the same DRS staging mechanism; an apply-time rejection triggers rollback. Unsupported requests abort the operation; they are not silently skipped.
- Resolution changes retain the relative positions of display origins; they do not automatically repack the desktop. An incompatible/overlapping arrangement may be rejected by the driver and require an initial adjustment in Windows display settings.
- After applying, the app checks all active display identities, positions, resolutions, primary flags, scaling where requested, and the saved driver values. Refresh readback allows a **0.5 Hz** tolerance for driver timing differences. Fractional input is sent as millihertz to NVAPI or as a rational to Windows CCD, without integer-Hz rounding. Windows topology is checked again after AMD/Intel driver edits, because some scaling setters perform their own modeset.
- On an apply/readback error, the app attempts restoration of display, driver, and requested Windows scaling state independently. NVIDIA restores only driver setting IDs changed by the preset, preserving prior user overrides vs defaults. AMD/Intel restore raw values of requested settings, including inactive caps/custom VRR values where exposed; their APIs do not provide NVIDIA's override-versus-default metadata. Intel integer scaling affects the entire adapter. Do not edit the same settings in another utility during the transaction/undo period.
- **Rollback is best-effort, not crash protection.** The confirmation countdown works while the process/message loop is running. A hung driver call, killed/crashed process, shutdown, or disconnected monitor can prevent restoration. The snapshot is not persisted across restarts. If restoration fails, an error is shown and **Restore previous settings** can retry. Keep a known-good Windows display configuration and test conservatively, especially with CRT timings.
- The app never requests a driver reload to force a mode through. Some writes may require elevated permissions on a particular installation; if NVAPI reports access denied, close the app and manually run it as administrator. There is no automatic elevation or always-on elevated helper.

## Lightweight design

The tray uses a hidden Win32 window and the Windows message loop. There is **no idle timer, process scanning, background polling, network access, telemetry, or automatic game detection**. Only a user-requested switch starts a worker thread; it exits when the operation completes. A short-lived timer drives the keep/revert dialog. Startup does not initialize vendor APIs. NVIDIA initialization/unloading is paired around requested operations. AMD/Intel read-only probes/capture close their SDK sessions afterward; driver-changing transactions retain their SDK sessions with the in-memory undo snapshot until it is replaced or the app exits. Driver DLLs are loaded from the Windows system directory, never from the current working directory. The executable statically links the Microsoft C++ runtime.

On an Nvidia system, the executable uses only 3.5MB of system memory while running.

## Build

Requirements:

- Visual Studio / Build Tools with **Desktop development with C++**, an x64 MSVC toolchain and a Windows SDK.
- CMake 3.24+.
- Internet on the first configure to download the pinned NVIDIA NVAPI and Intel IGCL SDKs (MIT licensed). No network access is needed at runtime.

For Visual Studio 2026:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For Visual Studio 2022, use `-G "Visual Studio 17 2022"` in a separate build directory. Copy `DisplayQuickSwitch.exe`, `NVIDIA-NVAPI-LICENSE.txt` and `INTEL-IGCL-LICENSE.txt` together when distributing the default build. Installed drivers supply the implementations; driver DLLs are not redistributed. Tests use fake NVAPI, CCD, DPI and IGCL entry points and do not change real display or driver settings. AMD support is compile-checked but needs validation on real AMD hardware; fake IGCL tests also cannot establish compatibility with every real Intel driver.

### Optional AMD ADLX backend

The public [ADLX SDK license agreement](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/blob/32b5a740d42295c5dfe9026b9f52683da0f3af91/ADLX%20SDK%20License%20Agreement.pdf) is **not MIT** and includes requirements concerning SDK use, distribution and an end-user license agreement. Merely copying its license file is not sufficient to satisfy those requirements. The project license is not changed by this work, and ADLX is **disabled by default**. Review its terms and obtain any necessary permission/legal advice before enabling it or distributing an ADLX-enabled executable. No AMD SDK headers or binaries are vendored into this repository.

After addressing those requirements, enable it in a separate build directory:

```powershell
cmake -S . -B build-amd -G "Visual Studio 18 2026" -A x64 -DQS_ENABLE_AMD_ADLX=ON
cmake --build build-amd --config Release
```

This adds the pinned ADLX headers at build time and copies `AMD-ADLX-SDK-LICENSE.pdf` beside the executable as a reference. The installed AMD driver must provide `amdadlx64.dll` and the requested capabilities. Without this build option, AMD users see the licensing/build notice instead of driver controls; resolution, refresh, primary, layouts and Windows scaling still work.

## References

- [NVIDIA public NVAPI SDK](https://github.com/NVIDIA/nvapi), pinned to `70d337db9186e968eab622f7e786de7e437faf3d`.
- [Intel Graphics Control Library](https://github.com/intel/drivers.gpu.control-library), pinned to `b6c462933502e13d1537dd5024949a51be30e63d` (3D features, scaling and Arc Sync APIs).
- [AMD ADLX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX), optional, pinned to `32b5a740d42295c5dfe9026b9f52683da0f3af91` (FreeSync, scaling, Wait for Vertical Refresh and FRTC APIs; separate SDK license).
- [Windows SetDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig), used for vendor-independent modes/layouts.
- [NVIDIA Profile Inspector](https://github.com/Orbmu2k/nvidiaProfileInspector), used as a reference for driver policy behavior. This app does not embed or launch Profile Inspector.
- [SetDPI](https://github.com/imniko/SetDPI), used as a reference for Windows DPI packet/range handling. This app does not embed or launch SetDPI or perform its legacy `AppliedDPI` registry write.
