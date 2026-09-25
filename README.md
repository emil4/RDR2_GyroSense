# RDR2 GyroSense (v0.1) - Archived & Open Source

This is a C++ ASI mod for Red Dead Redemption 2 that implements hardware-level hybrid motion aiming (Gyroscope + Right Thumbstick) via SDL3, completely bypassing the RAGE engine's virtual layer restrictions and preventing the annoying UI button flickering caused by Steam's gyro-to-mouse emulation.

> ⚠️ **PROJECT STATUS: ARCHIVED / ABANDONED**
> I have lost interest in this project and am releasing the source code "as is" to the community. I will **NOT** be finishing the backlog or updating this mod further. Feel free to fork this repository, fix bugs, or continue development.

## 🛠️ Installation & Usage
1. Install **ScriptHookRDR2** (by Alexander Blade).
2. Copy `RDR2_GyroSense.asi` and `RDR2_GyroSense.ini` into your main RDR2 directory (where `RDR2.exe` is located).
3. **Disable Steam Input** (!!! REQUIRED ON LAUNCH, OTHERWISE THE MOD WILL NOT DETECT THE GAMEPAD !!!).
4. Launch the game.
5. Re-enable Steam Input via the Steam Overlay afterward if you need standard button bindings.

## ⚙️ Configuration & Inverted Y-Axis Hack
All parameters (Sensitivities, Deadzones, Blending) are configured via `RDR2_GyroSense.ini`.
* **Invert Right Stick Y-Axis**: If you play with inverted camera controls, set a negative value for the vertical sensitivity in your INI file (e.g., `StickSensitivityY=-1500.0`).

## ❌ Known Issues & Open Backlog (Unfinished Tasks)
The following tasks are documented for anyone who wants to fork the project and continue development. **These will likely never be finished by the original author:**

### High Priority
- **Task #1: Angle Normalization (\(\pm180^{\circ}\) Wrap)** — Implement a proper circular wrap-around (`while > 180` / `while < -180`) for the horizontal accumulator (`virtualHeading`) to prevent coordinates from overflowing during infinite 360-degree rotations.
- **Task #3: Code Cleanup & Blending Config** — Move the hardcoded camera blending factor into the INI file (`CamBlending=1.0`) and clean up duplicated fallback values between `GyroState` structure initialization and `LoadSettings()`.
- **Task #4: Crouching Vertical Axis Freeze** — Investigate why the vertical axis (Pitch) freezes or gets overridden by the engine animations when Arthur is crouching (`PED::IS_PED_CROUCHING`), and implement a bypass.
- **Task #14: Native Scope Sway Evaluation** — Since `CamBlending` is now set to 1.0f, test if the feedback loop is naturally broken. If so, completely strip the delta-correction workaround (`Proposal #1`) to restore natural sniper breathing without leg animation jitters.

### Medium Priority
- **Task #5: Scripted Mission & Dead Eye QTE Breakage** — The gyro turns off during specific cinematic QTE sequences (e.g., the train robbery in "Pouring Forth Oil" or when fighting off the lion in Rhodes). Update the `IsPlayerCombatAiming` helper to detect forced/scripted Dead Eye states.
- **Task #15: Mounted Maxim/Gatling Guns Support** — Gyro tracking completely stops when using static machine guns. Update the combat helper to check for vehicle/mounted controls (`INPUT_VEH_ATTACK`).
- **Task #16: First-Person Mode Separate Sensitivity** — Add independent INI scaling values for when the player manually switches the gameplay camera to full 1st-person perspective, adjusting for FOV shifts.
- **Task #17: Thumbstick Inversion Option** — Add a dedicated `InvertStickY=1` configuration flag to the INI file to handle inverted axis natively instead of relying on the negative sensitivity multiplier hack.

### Low Priority
- **Task #6: Steam Input Formatting in INI** — Convert abstract raw multiplier units into standard Steam Input values (e.g., Dots per 360° or Degrees per second).
- **Task #10: WinAPI Raw HID Exclusive Hook** — Attempt to exclusively grab the DualSense controller mesh via WinAPI `CreateFileA` on boot to bypass Steam Input automatically, eliminating the need to manually toggle it during launch.
- **Task #12: Live Debug Telemetry Hotkey** — Add an in-game hotkey (e.g., **F4**) to toggle the real-time visual sliders and graph lines on/off without editing the INI file.
- **Task #13: Refactor & Comment Stripping** — Strip massive explanatory lecture comments inside `script.cpp` down to tight engineering notes.
