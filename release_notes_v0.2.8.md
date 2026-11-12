# Henge v0.2.8 - Volume Parity, Smooth Scrolling, Metal Magenta & Dock Glitch Fixes

### 1. Volume Level Synchronization (Hardware Buttons to macOS)
- **1:1 Level Matching**: Fixed an issue where pressing iPad hardware volume buttons moved macOS volume by a tiny fraction instead of matching the iPad slider level.
- **MacSpeaker Fix**: `MacSpeaker.swift` now always pushes the exact scalar volume level (`henge_vm_speaker_host_volume`) on every button press and gesture.
- **Henge Agent CoreAudio Integration**: `henge-agent` on macOS now directly controls system volume via `kAudioHardwareServiceDeviceProperty_VirtualMasterVolume` and `kAudioDevicePropertyVolumeScalar` on the default output device, keeping macOS volume in lockstep with iPad volume.
- **Protocol Bump**: Added `HV_AGENT_SINCE_VOLUME_LEVEL = 40` and `hv_share_volume_level`.

### 2. Smooth Scrolling & Momentum Tuning
- **Trackpad Sensitivity Calibrated**: Reduced `scrollScale` from `3.0` to `1.2` in `Trackpad.swift` to stop runaway scrolling.
- **Hardware Scroll Damping**: Added `0.6x` calibration factor to `Hardware.swift` for Magic Keyboard and external mice.
- **Momentum Friction**: In `MacWheel.swift` and `henge-agent.m`, clamped top glide speed to 6,000-8,000 points/sec and tuned deceleration friction (`pow(0.995, dt * 1000)`), delivering a natural, smooth stop without overshooting.

### 3. Metal GPU Driver Improvements & Glitch Elimination
- **Antigravity / Chromium Magenta Bug**:
  - Added format 261 (`MTLPixelFormatX32_Stencil8`) to `hv_pvg_stencil_format` in `devices/pvg_command.h`.
  - Fixed pipeline depth/stencil pairing validation in `hv_pvg_pipeline_problem` for companion stencil views (formats 260, 255, 261, 262).
  - Allowed texture view format compatibility across combined depth/stencil formats in `hv_pvg_view_format_ok`.
- **Dock Notification Bubble Glitch**:
  - Corrected Metal linear buffer length calculation in `devices/pvg_metal.m` and `devices/pvg_validate.h` so the last row requires only its pixels without trailing row padding (`(height - 1) * row + width * bpp`).
  - Fixed dock notification bubbles (105x105 surface 8) and small UI textures being improperly rejected.
  - Preserved color swizzle on non-render-targets.
  - Initialized fallback stand-in textures with `replaceRegion:withBytes:` from guest surface memory instead of leaving them blank.

### 4. Performance & Hitch Reduction
- **Removed Artificial Presenter Sleep**: Eliminated `nanosleep` inside `presentLoop` in `devices/pvg_metal.m` which caused dropped frames and stuttering.
- **Faster Drawable Retry**: Reduced retry interval from 100ms to 16ms (single frame interval) when a drawable is temporarily held, eliminating 100-200ms hitches.
- **Elevated Mapper QoS**: Set `mapper_q` to `QOS_CLASS_USER_INTERACTIVE` to prevent texture uploads from being stalled on efficiency cores.
