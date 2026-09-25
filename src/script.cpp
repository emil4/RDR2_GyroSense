/*
 * RDR2_GyroSense - main script logic.
 *
 * SDL3 gamepad + gyro reader (SDL2 Python prototype -> SDL3 C API):
 *   SDL_NumJoysticks()                   -> SDL_GetGamepads(int *count)
 *   SDL_GameControllerOpen(index)        -> SDL_OpenGamepad(SDL_JoystickID)
 *   SDL_GameControllerSetSensorEnabled() -> SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true)
 *   SDL_GameControllerGetSensorData()    -> SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, data, 3)
 *
 * SDL3 performs all real HID I/O on its own threads, so the event queue is only
 * drained once per script tick and the cached gyro vector is read right after.
 * No blocking loop and no extra thread - the ScriptHook tick stays freeze-free.
 *
 * NOTE: an active Steam Input layer can block the sensor API; in that case
 * SDL_SetGamepadSensorEnabled() fails and the error is shown on the overlay.
 */

#include "script.h"

#include <cstdlib>
#include <string>

// --- Global state -----------------------------------------------------------
// Gamepad handle, gyro axes, tuning values and overlay status flags.
struct GyroState
{
    SDL_Gamepad*   gamepad    = nullptr;
    SDL_JoystickID gamepadId  = 0;

    // Raw gyro (rad/s) and EMA-smoothed values: [0] pitch, [1] yaw, [2] roll.
    float gyroData[3]     = { 0.0f, 0.0f, 0.0f };
    float smoothedGyro[3] = { 0.0f, 0.0f, 0.0f };

    // EMA factor (lower = smoother, more lag) and split camera sensitivity (X/Y).
    float alpha            = 0.9f;
    float gyroSensitivityX = 1500.0f;
    float gyroSensitivityY = 1500.0f;

    // Gyro deadzones (rad/s) applied to the smoothed values.
    float gyroDeadzoneX = 0.015f; // Yaw (horizontal).
    float gyroDeadzoneY = 0.015f; // Pitch (vertical).

    // Stick sensitivity, kept on the gyro scale so both deltas mix evenly.
    float stickSensitivityX = 1500.0f;
    float stickSensitivityY = 1500.0f;

    // Applied to gyro + stick deltas while scoped (rifle scope / binoculars);
    // below 1.0 for fine precision aiming.
    float zoomMultiplier = 0.2f;

    // Right-stick deadzones (normalized -1.0..1.0) to kill center drift.
    float stickDeadzoneX = 0.052f; // Horizontal.
    float stickDeadzoneY = 0.052f; // Vertical.

    // Master toggle: while false the input pipeline and camera writes are skipped.
    bool modEnabled = true;

    // SDL_GetTicks() timestamp until which the ON/OFF pop-up stays visible.
    uint64_t notificationEndTime = 0;

    // Toggle hotkey virtual-key code (default VK_F2 = 0x71).
    int toggleKey = 0x71;

    // Diagnostics / UI.
    bool        sdlInitOk            = false;
    bool        gamepadOpened        = false;
    bool        gyroEnabled          = false;
    bool        readSuccess          = false;
    bool        lockonDisabled       = false;
    int         showOverlay          = 1;
    std::string sdlError;

    // Camera formula telemetry, refreshed every tick for the debug overlay.
    float currentHeading       = 0.0f;
    float currentPitch         = 0.0f;
    float currentMultiplierVal = 1.0f;
    float gyroContribution     = 0.0f;
    float stickContribution    = 0.0f;
    float finalHeading         = 0.0f;
    float finalPitch           = 0.0f;

    // Virtual angle accumulator: snapshotted from the game camera when aiming
    // starts, then advanced with our own deltas only (breaks the native scope
    // sway feedback loop).
    float virtualHeading      = 0.0f;
    float virtualPitch        = 0.0f;
    bool  wasAimingTransition = false;
};

static GyroState g_gyro;

// --- On-screen text helper ---------------------------------------------------
// One line of overlay text via the UIDEBUG natives.
static void DrawText(const std::string& text, float x, float y, int r, int g, int b, float scale = 0.20f)
{
    UIDEBUG::_BG_SET_TEXT_SCALE(scale, scale);
    UIDEBUG::_BG_SET_TEXT_COLOR(r, g, b, 255);
    UIDEBUG::_BG_DISPLAY_TEXT(MISC::VAR_STRING(10, "LITERAL_STRING", text.c_str()), x, y);
}

// --- Settings ----------------------------------------------------------------
// ToggleKey INI value -> Windows virtual-key code: "F1".."F24", hex "0x71" or
// decimal "113". Unparseable input falls back to VK_F2 (0x71).
static int ParseToggleKey(const char* value, int fallback = 0x71)
{
    if (!value || !value[0])
        return fallback;

    // "F1".."F24" -> VK_F1 (0x70) .. VK_F24 (0x87).
    if (value[0] == 'F' || value[0] == 'f')
    {
        char* end = nullptr;
        long  fn  = std::strtol(value + 1, &end, 10);
        if (end && *end == '\0' && fn >= 1 && fn <= 24)
            return 0x70 + static_cast<int>(fn - 1);
    }

    // Hex VK code.
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    {
        char* end = nullptr;
        long  vk  = std::strtol(value + 2, &end, 16);
        if (end && *end == '\0' && vk > 0 && vk <= 0xFF)
            return static_cast<int>(vk);
        return fallback;
    }

    // Decimal VK code.
    char* end = nullptr;
    long  vk  = std::strtol(value, &end, 10);
    if (end && *end == '\0' && vk > 0 && vk <= 0xFF)
        return static_cast<int>(vk);

    return fallback;
}

// Reads RDR2_GyroSense.ini from the game directory; missing keys keep the
// defaults declared in GyroState.
static void LoadSettings()
{
    char buf[32];

    GetPrivateProfileStringA("Settings", "Alpha", "0.9", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.alpha = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "GyroSensitivityX", "1500.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.gyroSensitivityX = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "GyroSensitivityY", "1500.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.gyroSensitivityY = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "GyroDeadzoneX", "0.015", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.gyroDeadzoneX = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "GyroDeadzoneY", "0.015", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.gyroDeadzoneY = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickSensitivityX", "1500.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickSensitivityX = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickSensitivityY", "1500.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickSensitivityY = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickDeadzoneX", "0.052", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickDeadzoneX = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickDeadzoneY", "0.052", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickDeadzoneY = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "ZoomMultiplier", "0.2", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.zoomMultiplier = std::stof(buf); } catch (...) {}
    // Write the resolved value back so the key materializes in the INI on first run.
    WritePrivateProfileStringA("Settings", "ZoomMultiplier", buf, ".\\RDR2_GyroSense.ini");

    g_gyro.showOverlay = GetPrivateProfileIntA("Settings", "ShowOverlay", 1, ".\\RDR2_GyroSense.ini");

    char toggleBuf[16];
    GetPrivateProfileStringA("Settings", "ToggleKey", "F2", toggleBuf, sizeof(toggleBuf), ".\\RDR2_GyroSense.ini");
    g_gyro.toggleKey = ParseToggleKey(toggleBuf);
}

// --- Combat aiming -----------------------------------------------------------
// True only while aiming a ranged weapon (never during NPC interaction / melee).
bool IsPlayerCombatAiming(Ped playerPed)
{
    Hash weaponHash = 0;
    WEAPON::GET_CURRENT_PED_WEAPON(playerPed, &weaponHash, true, 0, true);
    bool hasRangedWeapon = WEAPON::IS_WEAPON_A_GUN(weaponHash)
                        || WEAPON::IS_WEAPON_BOW(weaponHash)
                        || WEAPON::_IS_WEAPON_THROWABLE(weaponHash)
                        || WEAPON::_IS_WEAPON_LASSO(weaponHash)
                        || WEAPON::_IS_WEAPON_BINOCULARS(weaponHash);
  return (PLAYER::IS_PLAYER_FREE_AIMING(PLAYER::PLAYER_ID()) || PAD::IS_CONTROL_PRESSED(0, 32)) && hasRangedWeapon;
}

// --- Gamepad discovery -------------------------------------------------------
// Opens the first connected gamepad and enables its gyro sensor.
static void OpenFirstGamepad()
{
    // Release the previously opened gamepad.
    if (g_gyro.gamepad)
    {
        SDL_CloseGamepad(g_gyro.gamepad);
        g_gyro.gamepad = nullptr;
    }
    g_gyro.gamepadId     = 0;
    g_gyro.gamepadOpened = false;
    g_gyro.gyroEnabled   = false;
    g_gyro.sdlError.clear();

    int gamepadCount = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepadCount);
    for (int i = 0; i < gamepadCount; ++i)
    {
        if (!SDL_IsGamepad(gamepads[i]))
        {
            continue; // Not gamepad capable.
        }

        g_gyro.gamepad = SDL_OpenGamepad(gamepads[i]);
        if (g_gyro.gamepad)
        {
            g_gyro.gamepadId     = gamepads[i];
            g_gyro.gamepadOpened = true;
            break;
        }
    }
    SDL_free(gamepads);

    // Enable the gyro explicitly (fails under Steam Input).
    if (g_gyro.gamepad && SDL_GamepadHasSensor(g_gyro.gamepad, SDL_SENSOR_GYRO))
    {
        g_gyro.gyroEnabled = SDL_SetGamepadSensorEnabled(g_gyro.gamepad, SDL_SENSOR_GYRO, true);
        if (!g_gyro.gyroEnabled)
            g_gyro.sdlError = SDL_GetError();
    }
    else if (g_gyro.gamepad)
    {
        g_gyro.sdlError = "Gamepad has no gyro (SDL_GamepadHasSensor = false)";
    }
}

void ScriptMain()
{
    // --- 1. SDL initialization -------------------------------------------------
    // GAMEPAD implies JOYSTICK + EVENTS; SENSOR covers the IMU subsystem.
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR))
    {
        g_gyro.sdlError = SDL_GetError();
        scriptWait(MAXDWORD); // SDL3.dll missing / init failed: stay inert.
        return;
    }
    g_gyro.sdlInitOk = true;

    // --- 2. Load INI settings, open the first gamepad ---------------------------
    LoadSettings();
    OpenFirstGamepad();

    // --- 3. Main script tick loop ----------------------------------------------
    while (true)
    {
        // Toggle hotkey: handled unconditionally so the mod can always be turned
        // off again. The low bit of GetAsyncKeyState is the press transition.
        if (GetAsyncKeyState(g_gyro.toggleKey) & 1)
        {
            g_gyro.modEnabled = !g_gyro.modEnabled;
            g_gyro.notificationEndTime = SDL_GetTicks() + 2000; // 2 s pop-up.
        }

        // Track the live gameplay camera every tick, even with the mod OFF, so the
        // overlay keeps showing the engine's own camera values.
        g_gyro.currentHeading = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING();
        g_gyro.currentPitch   = CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH();

        // Loop-scope diagnostics consumed by the overlay section below.
        bool  isAiming     = false;
        float stickX       = 0.0f;
        float stickY       = 0.0f;
        bool  isUsingScope = false;
        // Raw right-stick axes (-32768..32767) read here so the overlay shows them
        // even while a different camera path is active.
        int16_t rawStickX = SDL_GetGamepadAxis(g_gyro.gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        int16_t rawStickY = SDL_GetGamepadAxis(g_gyro.gamepad, SDL_GAMEPAD_AXIS_RIGHTY);

        if (g_gyro.modEnabled)
        {
            // Drain the SDL queue once per tick; only the refreshed internal cache
            // is used.
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                // No event handling yet.
            }

            // Hotplug: re-open the gamepad if it was unplugged or not present yet.
            if (!g_gyro.gamepad || !SDL_GamepadConnected(g_gyro.gamepad))
            {
                OpenFirstGamepad();
            }

            // F2 toggles lock-on suppression (floor-drift investigation).
            if (GetAsyncKeyState(VK_F2) & 1)
            {
                g_gyro.lockonDisabled = !g_gyro.lockonDisabled;
            }

            // Suppress lock-on while disabled.
            if (g_gyro.lockonDisabled)
            {
                PLAYER::SET_PLAYER_LOCKON(PLAYER::PLAYER_ID(), FALSE);
            }

            // --- 4. Read raw gyro every tick and EMA-smooth it --------------------
            g_gyro.readSuccess = false;
            if (g_gyro.gamepad && g_gyro.gyroEnabled)
            {
                g_gyro.readSuccess = SDL_GetGamepadSensorData(g_gyro.gamepad, SDL_SENSOR_GYRO, g_gyro.gyroData, 3);
                for (int i = 0; i < 3; ++i)
                {
                    g_gyro.smoothedGyro[i] = (g_gyro.gyroData[i] * g_gyro.alpha) + (g_gyro.smoothedGyro[i] * (1.0f - g_gyro.alpha));
                }
            }

            // --- 5. Rotate the gameplay camera with the smoothed gyro ------------
            // Gyro only acts during weapon combat aiming (see IsPlayerCombatAiming).
            isAiming = IsPlayerCombatAiming(PLAYER::PLAYER_PED_ID());

            // Scope detection: official _IS_PLAYER_IN_SCOPE (0x04D7F33640662FA2)
            // plus binoculars; selects the camera path used below.
            Hash weaponHash = 0;
            WEAPON::GET_CURRENT_PED_WEAPON(PLAYER::PLAYER_PED_ID(), &weaponHash, true, 0, true);
            bool isSniperScope = invoke<BOOL>(0x04D7F33640662FA2, PLAYER::PLAYER_ID()) != 0;
            isUsingScope = isSniperScope || (weaponHash == MISC::GET_HASH_KEY("WEAPON_BINOCULARS"));

            // Normalized stick deflection (-1.0f..1.0f), filled in below.
            stickX = 0.0f;
            stickY = 0.0f;

            if (isAiming && g_gyro.readSuccess)
            {
                stickX = rawStickX / 32767.0f;
                stickY = rawStickY / 32767.0f;

                // Stick deadzones: ignore small center deflections.
                if (std::fabs(stickX) < g_gyro.stickDeadzoneX) stickX = 0.0f;
                if (std::fabs(stickY) < g_gyro.stickDeadzoneY) stickY = 0.0f;

                // Gyro deadzones on the smoothed values (per axis).
                if (std::fabs(g_gyro.smoothedGyro[1]) < g_gyro.gyroDeadzoneX) g_gyro.smoothedGyro[1] = 0.0f; // Yaw -> X axis.
                if (std::fabs(g_gyro.smoothedGyro[0]) < g_gyro.gyroDeadzoneY) g_gyro.smoothedGyro[0] = 0.0f; // Pitch -> Y axis.

                // Scoped: scale both deltas down for fine aiming.
                float currentMultiplier = isUsingScope ? g_gyro.zoomMultiplier : 1.0f;

                // Formula terms captured for the overlay.
                g_gyro.currentMultiplierVal = currentMultiplier;
                g_gyro.gyroContribution     = g_gyro.smoothedGyro[1] * g_gyro.gyroSensitivityX * 0.01f * currentMultiplier;
                g_gyro.stickContribution    = stickX * g_gyro.stickSensitivityX * 0.01f * currentMultiplier;

                if (isUsingScope)
                {
                    // --- Scope / binoculars: virtual angle accumulator ---------------
                    // Snapshot the game camera once, then integrate our deltas only
                    // (breaks the native sway feedback loop).
                    if (!g_gyro.wasAimingTransition)
                    {
                        g_gyro.virtualHeading = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING();
                        g_gyro.virtualPitch   = CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH();
                        g_gyro.wasAimingTransition = true;
                    }

                    // Resync when the engine rotated the body (delta > 2 deg) so the
                    // camera cannot lag behind the forced alignment.
                    float liveHeading  = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING();
                    float headingDelta = std::fabs(g_gyro.virtualHeading - liveHeading);
                    if (headingDelta > 2.0f)
                    {
                        g_gyro.virtualHeading = liveHeading;
                    }

                    // Mix formula: gyro delta minus (inverted) stick delta.
                    g_gyro.virtualHeading += g_gyro.gyroContribution - g_gyro.stickContribution;
                    g_gyro.virtualPitch   += (g_gyro.smoothedGyro[0] * g_gyro.gyroSensitivityY * 0.01f * currentMultiplier) - (stickY * g_gyro.stickSensitivityY * 0.01f * currentMultiplier);

                    // Wrap heading into -180..180.
                    while (g_gyro.virtualHeading > 180.0f)  g_gyro.virtualHeading -= 360.0f;
                    while (g_gyro.virtualHeading < -180.0f) g_gyro.virtualHeading += 360.0f;

                    // Clamp pitch so the camera cannot flip over.
                    if (g_gyro.virtualPitch > 75.0f) g_gyro.virtualPitch = 75.0f;
                    else if (g_gyro.virtualPitch < -75.0f) g_gyro.virtualPitch = -75.0f;

                    g_gyro.finalHeading = g_gyro.virtualHeading;
                    g_gyro.finalPitch   = g_gyro.virtualPitch;

                    // Feed the integrated coordinates back; blend 1.0f applies them
                    // without the easing lag that reads as harsh camera motion.
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(g_gyro.virtualHeading, 1.0f);
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(g_gyro.virtualPitch, 1.0f);
                }
                else
                {
                    // --- 3rd-person aiming: direct relative math, no accumulator -------
                    // currentMultiplier is 1.0f here (no scope), so it is omitted.
                    float newHeading = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING() + (g_gyro.smoothedGyro[1] * g_gyro.gyroSensitivityX * 0.01f) - (stickX * g_gyro.stickSensitivityX * 0.01f); // Yaw -> left/right
                    float newPitch   = CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH()  + (g_gyro.smoothedGyro[0] * g_gyro.gyroSensitivityY * 0.01f) - (stickY * g_gyro.stickSensitivityY * 0.01f); // Pitch inverted -> tilt up looks up

                    g_gyro.finalHeading = newHeading;
                    g_gyro.finalPitch   = newPitch;

                    CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(newHeading, 0.1f);
                    CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(newPitch, 0.1f);

                    // Reset so entering a scope later takes a fresh snapshot.
                    g_gyro.wasAimingTransition = false;
                }
            }
        }
        else
        {
            // Mod OFF: report the 0.0f baseline instead of stale telemetry.
            // currentHeading/currentPitch are still refreshed above.
            g_gyro.gyroContribution  = 0.0f;
            g_gyro.stickContribution = 0.0f;
            g_gyro.finalHeading      = 0.0f;
            g_gyro.finalPitch        = 0.0f;
        }

        // --- 6. Debug overlay (rendered with the mod OFF as well) -----------------
        if (g_gyro.showOverlay)
        {
            DrawText(
                "Gamepad Type: " + std::string(g_gyro.gamepad ? SDL_GetGamepadStringForType(SDL_GetGamepadType(g_gyro.gamepad)) : "none") +
                " | SDL: " + std::to_string(g_gyro.sdlInitOk ? 1 : 0) +
                " | Pad: " + std::to_string(g_gyro.gamepadOpened ? 1 : 0) +
                " | ID: " + std::to_string(g_gyro.gamepadId),
                0.05f, 0.05f, 255, 255, 255);

            DrawText(
                "G_Enabled: " + std::to_string(g_gyro.gamepad ? SDL_GamepadSensorEnabled(g_gyro.gamepad, SDL_SENSOR_GYRO) : 0) +
                " | Read: " + std::to_string(g_gyro.readSuccess ? 1 : 0) +
                " | Aiming: " + std::to_string(isAiming ? 1 : 0),
                0.05f, 0.07f, 255, 255, 255);

            // Green line: live gyro values (Pitch = X, Yaw = Y).
            // slider() draws a bar scaled to the gyro deadzone and is kept for axis
            // diagnostics.
            const float dzG = (g_gyro.gyroDeadzoneX > 0.0f ? g_gyro.gyroDeadzoneX : 0.015f) * 10.0f;
            const float sliderScale = 1.0f / dzG;
            auto slider = [](float value, float scale) -> std::string
            {
                const int width = 15, center = width / 2;
                float t = value * scale;
                if (t > 1.0f) t = 1.0f;
                else if (t < -1.0f) t = -1.0f;
                int pos = center + static_cast<int>(t * (width - center - 1) + (t >= 0.0f ? 0.5f : -0.5f));
                std::string bar(width, ' ');
                bar[pos] = '|';
                return "[" + bar + "]";
            };
            // DrawText(
            //     std::string("Pitch: ") + slider(g_gyro.smoothedGyro[0], sliderScale) +
            //     "  Yaw: " + slider(g_gyro.smoothedGyro[1], sliderScale),
            //     0.05f, 0.09f, 0, 255, 0);
            DrawText(
                std::string("Pitch: ") + (g_gyro.smoothedGyro[0] >= 0.0f ? "+" : "") + std::to_string(g_gyro.smoothedGyro[0]) +
                             "  Yaw: " + (g_gyro.smoothedGyro[1] >= 0.0f ? "+" : "") + std::to_string(g_gyro.smoothedGyro[1]),
                0.05f, 0.09f, 0, 255, 0);

            // Orange line: raw right-stick values (-32768..32767).
            DrawText(
                std::string("rawStickX: ") + (rawStickX >= 0.0f ? "+" : "") + std::to_string(rawStickX) +
                " | Y: " + (rawStickY >= 0.0f ? "+" : "") + std::to_string(rawStickY),
                0.05f, 0.11f, 255, 165, 0);

            // --- Camera formula breakdown (Yaw / Pitch) ---
            DrawText("--- Camera Formula Breakdown (Yaw / Pitch) ---", 0.05f, 0.13f, 255, 255, 255);
            DrawText(
                std::string("CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING() = ") + (g_gyro.currentHeading >= 0.0f ? "+" : "") + std::to_string(g_gyro.currentHeading),
                0.05f, 0.15f, 0, 255, 255);
            DrawText(
                std::string("CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH()   = ") + (g_gyro.currentPitch >= 0.0f ? "+" : "") + std::to_string(g_gyro.currentPitch),
                0.05f, 0.165f, 173, 216, 230);
            DrawText(
                std::string("g_gyro.smoothedGyro[1] Contribution = ") + (g_gyro.gyroContribution >= 0.0f ? "+" : "") + std::to_string(g_gyro.gyroContribution),
                0.05f, 0.18f, 0, 255, 0);
            DrawText(
                std::string("stickX Contribution = ") + (g_gyro.stickContribution >= 0.0f ? "+" : "") + std::to_string(g_gyro.stickContribution),
                0.05f, 0.195f, 255, 165, 0);
            DrawText(
                std::string("FINAL newHeading = ") + (g_gyro.finalHeading >= 0.0f ? "+" : "") + std::to_string(g_gyro.finalHeading),
                0.05f, 0.21f, 255, 255, 0);
            DrawText(
                std::string("FINAL newPitch   = ") + (g_gyro.finalPitch >= 0.0f ? "+" : "") + std::to_string(g_gyro.finalPitch),
                0.05f, 0.225f, 255, 255, 0);

            // SDL / gamepad errors below the numeric block to keep the layout scannable.
            if (!g_gyro.sdlError.empty())
            {
                DrawText("SDL Error: " + g_gyro.sdlError, 0.05f, 0.25f, 255, 0, 0);
            }
            else if (!g_gyro.gamepad)
            {
                DrawText("No gamepad detected - connect one", 0.05f, 0.25f, 255, 255, 0);
            }
        }

        // --- Mod ON/OFF pop-up notification --------------------------------------
        if (SDL_GetTicks() < g_gyro.notificationEndTime)
        {
            if (g_gyro.modEnabled)
                DrawText("RDR2 GyroSense: ON", 0.5f, 0.2f, 0, 255, 0);
            else
                DrawText("RDR2 GyroSense: OFF", 0.5f, 0.2f, 255, 0, 0);
        }

        // Not aiming: next aim starts with a fresh accumulator snapshot.
        if (!isAiming)
        {
            g_gyro.wasAimingTransition = false;
        }

        scriptWait(0);
    }

    // --- 7. Cleanup (script termination) -----------------------------------------
    if (g_gyro.gamepad)
    {
        SDL_CloseGamepad(g_gyro.gamepad);
        g_gyro.gamepad = nullptr;
    }
    SDL_Quit();
}



