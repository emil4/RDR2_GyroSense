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

    // Gyro deadzone (rad/s): smoothed pitch/yaw below this value is zeroed out.
    float gyroDeadzone     = 0.015f;

    // Thumbstick look sensitivity (horizontal/vertical) - same high scale as the
    // gyro sensitivities so the stick can compete with the gyro deltas.
    float stickSensitivityX = 1500.0f;
    float stickSensitivityY = 1500.0f;

    // Right-stick deadzone (normalized -1.0..1.0 units): cuts off small deflections
    // near center that would otherwise cause slow diagonal camera crawling.
    float stickDeadzone      = 0.052f;

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

    GetPrivateProfileStringA("Settings", "GyroDeadzone", "0.015", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.gyroDeadzone = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickSensitivityX", "1500.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickSensitivityX = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickSensitivityY", "1500.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickSensitivityY = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "StickDeadzone", "0.052", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.stickDeadzone = std::stof(buf); } catch (...) {}

    g_gyro.showOverlay = GetPrivateProfileIntA("Settings", "ShowOverlay", 1, ".\\RDR2_GyroSense.ini");
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

        // --- 4. Read raw gyro every tick and EMA-smooth it ------------------------
        g_gyro.readSuccess = false;
        if (g_gyro.gamepad && g_gyro.gyroEnabled)
        {
            g_gyro.readSuccess = SDL_GetGamepadSensorData(g_gyro.gamepad, SDL_SENSOR_GYRO, g_gyro.gyroData, 3);
            for (int i = 0; i < 3; ++i)
            {
                g_gyro.smoothedGyro[i] = (g_gyro.gyroData[i] * g_gyro.alpha) + (g_gyro.smoothedGyro[i] * (1.0f - g_gyro.alpha));

                // Deadzone filter: zero out drift on Pitch (0) and Yaw (1) below the threshold.
                if ((i == 0 || i == 1) && std::fabs(g_gyro.smoothedGyro[i]) < g_gyro.gyroDeadzone)
                    g_gyro.smoothedGyro[i] = 0.0f;
            }
        }

        // --- 5. Rotate the gameplay camera directly with smoothed gyro ------------
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

            // Deadzone cut-off: ignore small stick deflections so a worn stick resting
            // near center can't cause slow diagonal camera crawling.
            if (std::fabs(stickX) < g_gyro.stickDeadzone) stickX = 0.0f;
            if (std::fabs(stickY) < g_gyro.stickDeadzone) stickY = 0.0f;

            // Mix BOTH the gyro deltas and the thumbstick input before writing the camera
            // back. Stick deltas use the INI sensitivities (same high scale as the gyro);
            // they are subtracted because the SDL3 right-stick axes come back inverted.
            float newHeading = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING() + (g_gyro.smoothedGyro[1] * g_gyro.gyroSensitivityX * 0.01f) - (stickX * g_gyro.stickSensitivityX * 0.01f); // Yaw -> left/right
            float newPitch   = CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH()  + (g_gyro.smoothedGyro[0] * g_gyro.gyroSensitivityY * 0.01f) - (stickY * g_gyro.stickSensitivityY * 0.01f); // Pitch inverted -> tilt up looks up
            CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(newHeading, 0.1f);
            CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(newPitch, 0.1f);
        }

        // --- 6. On-screen debug overlay ------------------------------------------
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
            const float dzG = (g_gyro.gyroDeadzone > 0.0f ? g_gyro.gyroDeadzone : 0.015f) * 10.0f;
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



