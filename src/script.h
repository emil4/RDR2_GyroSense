/*
 * RDR2_GyroSense - shared declarations.
 *
 * Aggregates the ScriptHookRDR2 SDK headers and SDL3.
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <cmath>

#include "inc/types.h"
#include "inc/natives.h"
#include "inc/main.h"
#include "inc/SDL3/SDL.h"

// Script entry point invoked by ScriptHookRDR2 on a dedicated script thread.
void ScriptMain();
