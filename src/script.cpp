/*
 * RDR2_GyroSense - main script logic.
 *
 * Straightforward SDL3 gamepad + gyro reader, ported 1:1 from the working
 * Python (SDL2) prototype:
 *
 *   SDL_GameController*                  -> SDL_Gamepad*
 *   SDL_NumJoysticks()                   -> SDL_GetGamepads(int *count)
 *   SDL_GameControllerOpen(index)        -> SDL_OpenGamepad(SDL_JoystickID)
 *   SDL_GameControllerIsSensorEnabled()  -> SDL_GamepadSensorEnabled(gp, SDL_SENSOR_GYRO)
 *   SDL_GameControllerSetSensorEnabled() -> SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true)
 *   SDL_GameControllerGetSensorData()    -> SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, data, 3)
 *
 * There is no blocking loop anywhere: the SDL event queue is drained exactly
 * once per script tick with a non-blocking `while (SDL_PollEvent(...))`, and
 * the cached gyro vector is read straight off the gamepad right after.
 * SDL3 does all real hardware I/O on its own internal threads (e.g. HIDAPI),
 * so a separate std::thread is not needed - polling the SDL cache from the
 * ScriptHook script thread is safe and freeze-free.
 *
 * NOTE: if Steam Input is active it can block the gamepad sensor API; in that
 * case SDL_SetGamepadSensorEnabled() fails and the error is printed on the
 * debug overlay.
 */

#include "script.h"

#include <cstdlib>
#include <string>

// --- Global state -----------------------------------------------------------
// Flat struct kept in file scope: the gamepad handle, the 3 gyro axes and the
// status flags rendered by the on-screen debug overlay every tick.
struct GyroState
{
    SDL_Gamepad*   gamepad    = nullptr;
    SDL_JoystickID gamepadId  = 0;

    // Raw gyro (rad/s) and EMA-smoothed values - X, Y, Z.
    float gyroData[3]     = { 0.0f, 0.0f, 0.0f };
    float smoothedGyro[3] = { 0.0f, 0.0f, 0.0f };

    // EMA smoothing factor (lower = smoother but more lag) and split camera
    // sensitivity (horizontal/vertical) - loaded from RDR2_GyroSense.ini.
    float alpha            = 1.0f;
    float gyroSensitivityX = 1500.0f;
    float gyroSensitivityY = 1500.0f;

    // Split gyro deadzones (rad/s) per axis: smoothed Pitch / Yaw below the matching
    // threshold is zeroed out before it reaches the camera.
    float gyroDeadzoneX = 0.015f; // Yaw (horizontal) threshold.
    float gyroDeadzoneY = 0.015f; // Pitch (vertical) threshold.

    // Thumbstick look sensitivity (horizontal/vertical) - same high scale as the
    // gyro sensitivities so the stick can compete with the gyro deltas.
    float stickSensitivityX = 1500.0f;
    float stickSensitivityY = 1500.0f;

    // Split right-stick deadzones (normalized -1.0..1.0 units): cut off small
    // deflections near center on each axis that would otherwise cause slow
    // diagonal camera crawling.
    float stickDeadzoneX = 0.052f; // Horizontal (X) threshold.
    float stickDeadzoneY = 0.052f; // Vertical (Y) threshold.

    // Global mod toggle: while false the whole SDL input pipeline and the camera
    // writes are skipped; only the toggle hotkey and the ON/OFF pop-up run.
    bool modEnabled = true;

    // SDL_GetTicks() timestamp until which the ON/OFF pop-up stays on screen.
    uint64_t notificationEndTime = 0;

    // Toggle hotkey virtual-key code (default VK_F2 = 0x71). Loaded from the INI.
    int toggleKey = 0x71;

    // Diagnostics / UI.
    bool        sdlInitOk            = false;
    bool        gamepadOpened        = false;
    bool        gyroEnabled          = false;
    bool        readSuccess          = false;
    bool        lockonDisabled       = false;
    int         showOverlay          = 1;
    std::string sdlError;
};

static GyroState g_gyro;

// --- On-screen text helper ---------------------------------------------------
// Renders one line of text on the game overlay using the UIDEBUG natives.
static void DrawText(const std::string& text, float x, float y, int r, int g, int b, float scale = 0.20f)
{
    UIDEBUG::_BG_SET_TEXT_SCALE(scale, scale);
    UIDEBUG::_BG_SET_TEXT_COLOR(r, g, b, 255);
    UIDEBUG::_BG_DISPLAY_TEXT(MISC::VAR_STRING(10, "LITERAL_STRING", text.c_str()), x, y);
}

// --- Settings ----------------------------------------------------------------
// Converts the ToggleKey INI value into a Windows virtual-key code. Accepts
// "F1".."F24", a hexadecimal code ("0x71") or a plain integer VK code ("113").
// Anything unparseable falls back to VK_F2 (0x71).
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

    // "0xNN" hexadecimal VK code.
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    {
        char* end = nullptr;
        long  vk  = std::strtol(value + 2, &end, 16);
        if (end && *end == '\0' && vk > 0 && vk <= 0xFF)
            return static_cast<int>(vk);
        return fallback;
    }

    // Plain decimal VK code.
    char* end = nullptr;
    long  vk  = std::strtol(value, &end, 10);
    if (end && *end == '\0' && vk > 0 && vk <= 0xFF)
        return static_cast<int>(vk);

    return fallback;
}

// Loads RDR2_GyroSense.ini from the game directory. Missing keys or a missing
// file fall back to the defaults below (also mirrored in GyroState).
static void LoadSettings()
{
    char buf[32];

    GetPrivateProfileStringA("Settings", "Alpha", "0.001", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
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

    g_gyro.showOverlay = GetPrivateProfileIntA("Settings", "ShowOverlay", 1, ".\\RDR2_GyroSense.ini");

    char toggleBuf[16];
    GetPrivateProfileStringA("Settings", "ToggleKey", "F2", toggleBuf, sizeof(toggleBuf), ".\\RDR2_GyroSense.ini");
    g_gyro.toggleKey = ParseToggleKey(toggleBuf);
}

// --- Combat aiming -----------------------------------------------------------
// Simple and stable: gun/bow in hand + aiming input. Returns true only during
// weapon combat aiming - never during NPC interactions or melee.
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
// Scan all connected gamepads, open the first valid one and explicitly enable
// its gyro, mirroring the Python prototype.
static void OpenFirstGamepad()
{
    // Close a previously opened gamepad first.
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
            continue; // Not gamepad-capable, skip it.
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

    // Explicitly enable the gyro on the opened gamepad.
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
        // Global mod toggle hotkey - processed unconditionally so the mod can
        // always be switched off (and back on). Edge-detection: the low bit of
        // GetAsyncKeyState is set only on the press transition.
        if (GetAsyncKeyState(g_gyro.toggleKey) & 1)
        {
            g_gyro.modEnabled = !g_gyro.modEnabled;
            g_gyro.notificationEndTime = SDL_GetTicks() + 2000; // 2-second pop-up.
        }

        if (g_gyro.modEnabled)
        {
            // Pump the SDL input cache exactly once per tick. Non-blocking: returns
            // false immediately when the queue is empty.
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                // Nothing to react to yet - we only need SDL's cached state refreshed.
            }

            // Hotplug safety: re-open a gamepad if it was unplugged / not there yet.
            if (!g_gyro.gamepad || !SDL_GamepadConnected(g_gyro.gamepad))
            {
                OpenFirstGamepad();
            }

            // F2 toggles lock-on suppression (investigating the floor-drifting issue).
            if (GetAsyncKeyState(VK_F2) & 1)
            {
                g_gyro.lockonDisabled = !g_gyro.lockonDisabled;
            }

            // Suppress lock-on mechanics while disabled (engine handles it naturally otherwise).
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

            // --- 5. Rotate the gameplay camera directly with smoothed gyro --------
            // Gyro only acts during weapon combat aiming (see IsPlayerCombatAiming) -
            // never during NPC interactions or melee.
            bool isAiming = IsPlayerCombatAiming(PLAYER::PLAYER_PED_ID());

            // Normalized right-stick deflection (-1.0f..1.0f) read straight off the SDL3
            // gamepad hardware. Declared here so the debug overlay can show the values;
            // they are refreshed from SDL inside the aiming block below.
            float stickX = 0.0f;
            float stickY = 0.0f;

            if (isAiming && g_gyro.readSuccess)
            {
                // Raw right-stick axes from SDL3, normalized to the -1.0f..1.0f range.
                int16_t rawStickX = SDL_GetGamepadAxis(g_gyro.gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
                int16_t rawStickY = SDL_GetGamepadAxis(g_gyro.gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
                stickX = rawStickX / 32767.0f;
                stickY = rawStickY / 32767.0f;

                // Split deadzone cut-offs per axis: ignore small stick deflections so a
                // worn stick resting near center can't cause slow diagonal crawling.
                if (std::fabs(stickX) < g_gyro.stickDeadzoneX) stickX = 0.0f;
                if (std::fabs(stickY) < g_gyro.stickDeadzoneY) stickY = 0.0f;

                // Split gyro deadzone cut-offs per axis: zero out slow Pitch/Yaw drift
                // below the matching threshold before it reaches the camera.
                if (std::fabs(g_gyro.smoothedGyro[1]) < g_gyro.gyroDeadzoneX) g_gyro.smoothedGyro[1] = 0.0f; // Yaw -> X axis.
                if (std::fabs(g_gyro.smoothedGyro[0]) < g_gyro.gyroDeadzoneY) g_gyro.smoothedGyro[0] = 0.0f; // Pitch -> Y axis.

                // Mix BOTH the gyro deltas and the thumbstick input before writing the
                // camera back. Stick deltas use the INI sensitivities (same high scale as
                // the gyro); they are subtracted because the SDL3 right-stick axes come
                // back inverted.
                float newHeading = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING() + (g_gyro.smoothedGyro[1] * g_gyro.gyroSensitivityX * 0.01f) - (stickX * g_gyro.stickSensitivityX * 0.01f); // Yaw -> left/right
                float newPitch   = CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH()  + (g_gyro.smoothedGyro[0] * g_gyro.gyroSensitivityY * 0.01f) - (stickY * g_gyro.stickSensitivityY * 0.01f); // Pitch inverted -> tilt up looks up
                CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(newHeading, 0.1f);
                CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(newPitch, 0.1f);
            }

            // --- 6. On-screen debug overlay --------------------------------------
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

                // Green gyro line: live text sliders for Pitch (X) and Yaw (Y). The bar
                // scale is tied to the deadzone so the central no-movement region stays
                // clearly visible while the indicator tracks real-time deflection.
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
                DrawText(
                    std::string("Pitch: ") + slider(g_gyro.smoothedGyro[0], sliderScale) +
                    "  Yaw: " + slider(g_gyro.smoothedGyro[1], sliderScale),
                    0.05f, 0.09f, 0, 255, 0);

                // Orange line: raw SDL3 right-stick diagnostics (normalized -1.0f..1.0f).
                DrawText(
                    std::string("Stick X: ") + (stickX >= 0.0f ? "+" : "") + std::to_string(stickX) +
                    " | Y: " + (stickY >= 0.0f ? "+" : "") + std::to_string(stickY),
                    0.05f, 0.11f, 255, 165, 0);

                // Pushed down to make room for the orange stick line above.
                if (!g_gyro.sdlError.empty())
                {
                    DrawText("SDL Error: " + g_gyro.sdlError, 0.05f, 0.13f, 255, 0, 0);
                }
                else if (!g_gyro.gamepad)
                {
                    DrawText("No gamepad detected - connect one", 0.05f, 0.13f, 255, 255, 0);
                }
            }
        } // end if (g_gyro.modEnabled)

        // --- Mod ON/OFF pop-up notification --------------------------------------
        if (SDL_GetTicks() < g_gyro.notificationEndTime)
        {
            if (g_gyro.modEnabled)
                DrawText("RDR2 GyroSense: ON", 0.5f, 0.2f, 0, 255, 0);
            else
                DrawText("RDR2 GyroSense: OFF", 0.5f, 0.2f, 255, 0, 0);
        }

        scriptWait(0);
    }

    // --- 7. Cleanup (reached only when the script is terminated) -----------------
    if (g_gyro.gamepad)
    {
        SDL_CloseGamepad(g_gyro.gamepad);
        g_gyro.gamepad = nullptr;
    }
    SDL_Quit();
}



