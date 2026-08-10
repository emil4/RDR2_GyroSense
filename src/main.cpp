/*
 * RDR2_GyroSense - DLL entry point.
 *
 * Registers the plugin script with ScriptHookRDR2 on DLL attach and
 * unregisters it on detach.
 */

#include "script.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        // Hand the script entry point over to the ScriptHook.
        scriptRegister(hModule, ScriptMain);
        break;

    case DLL_PROCESS_DETACH:
        // Unregister the script when the plugin is unloaded.
        scriptUnregister(hModule);
        break;
    }

    return TRUE;
}
