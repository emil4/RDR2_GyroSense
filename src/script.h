/*
 * RDR2_GyroSense - shared declarations: ScriptHookRDR2 SDK headers + SDL3.
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <cmath>

#include "inc/types.h"
#include "inc/natives.h"
#include "inc/main.h"
#include "inc/SDL3/SDL.h"

// Entry point invoked by ScriptHookRDR2 on its script thread.
void ScriptMain();
