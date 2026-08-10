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
    float alpha        = 1.0f;
    float sensitivityX = 100.0f;
    float sensitivityY = 130.0f;

    // Diagnostics / UI.
    bool        sdlInitOk     = false;
    bool        gamepadOpened = false;
    bool        gyroEnabled   = false;
    bool        readSuccess   = false;
    int         showOverlay   = 1;
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

    GetPrivateProfileStringA("Settings", "SensitivityX", "50.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.sensitivityX = std::stof(buf); } catch (...) {}

    GetPrivateProfileStringA("Settings", "SensitivityY", "75.0", buf, sizeof(buf), ".\\RDR2_GyroSense.ini");
    try { g_gyro.sensitivityY = std::stof(buf); } catch (...) {}

    g_gyro.showOverlay = GetPrivateProfileIntA("Settings", "ShowOverlay", 1, ".\\RDR2_GyroSense.ini");
}

// --- Combat aiming -----------------------------------------------------------
// Reconstructs the ranged-combat state hierarchy from Shtivi's RDR2-DualSense
// updateTriggers(): gun, bow, throwables and mounted weapons (Gatling / Maxim /
// cannons). Returns true ONLY while the player actively aims a ready combat
// weapon (or while reloading one) - never during NPC interactions or melee.
bool IsPlayerCombatAiming(Ped playerPed)
{
    Hash weaponHash = 0;
    WEAPON::GET_CURRENT_PED_WEAPON(playerPed, &weaponHash, true, 0, true);

    Hash mountedWeapon = 0;
    WEAPON::GET_CURRENT_PED_VEHICLE_WEAPON(playerPed, &mountedWeapon);

    bool hasRangedWeapon =
        WEAPON::IS_WEAPON_A_GUN(weaponHash) ||
        WEAPON::IS_WEAPON_BOW(weaponHash) ||
        WEAPON::_IS_WEAPON_THROWABLE(weaponHash) ||
        mountedWeapon == -628784915 ||  // Gatling
        mountedWeapon == -1193642378 || // Maxim
        mountedWeapon == 1609145491 ||  // normal cannon
        mountedWeapon == -1829236809;   // automatic cannon

    bool isReloading = PAD::IS_CONTROL_PRESSED(0, 32) && PED::IS_PED_RELOADING(playerPed);
    bool aimingInput = PAD::IS_CONTROL_PRESSED(0, 32) || isReloading;

    return hasRangedWeapon && aimingInput;
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

        // --- 4. Read raw gyro every tick and EMA-smooth it ------------------------
        g_gyro.readSuccess = false;
        if (g_gyro.gamepad && g_gyro.gyroEnabled)
        {
            g_gyro.readSuccess = SDL_GetGamepadSensorData(g_gyro.gamepad, SDL_SENSOR_GYRO, g_gyro.gyroData, 3);
            for (int i = 0; i < 3; ++i)
                g_gyro.smoothedGyro[i] = (g_gyro.gyroData[i] * g_gyro.alpha) + (g_gyro.smoothedGyro[i] * (1.0f - g_gyro.alpha));
        }

        // --- 5. Rotate the gameplay camera directly with smoothed gyro ------------
        // Gyro only acts during weapon combat aiming (see IsPlayerCombatAiming) -
        // never during NPC interactions or melee.
        bool isAiming = IsPlayerCombatAiming(PLAYER::PLAYER_PED_ID());
        if (isAiming && g_gyro.readSuccess)
        {
            float newHeading = CAM::GET_GAMEPLAY_CAM_RELATIVE_HEADING() + (g_gyro.smoothedGyro[1] * g_gyro.sensitivityX * 0.01f); // Yaw -> left/right
            float newPitch   = CAM::GET_GAMEPLAY_CAM_RELATIVE_PITCH()  + (g_gyro.smoothedGyro[0] * g_gyro.sensitivityY * 0.01f); // Pitch inverted -> tilt up looks up
            CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(newHeading, 1.0f);
            CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(newPitch, 1.0f);
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

            DrawText(
                "Gyro (smoothed) X: " + std::to_string(g_gyro.smoothedGyro[0]) +
                " | Y: " + std::to_string(g_gyro.smoothedGyro[1]) +
                " | Z: " + std::to_string(g_gyro.smoothedGyro[2]),
                0.05f, 0.09f, 0, 255, 0);

            if (!g_gyro.sdlError.empty())
            {
                DrawText("SDL Error: " + g_gyro.sdlError, 0.05f, 0.11f, 255, 0, 0);
            }
            else if (!g_gyro.gamepad)
            {
                DrawText("No gamepad detected - connect one", 0.05f, 0.11f, 255, 255, 0);
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



