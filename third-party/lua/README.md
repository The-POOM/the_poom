# Lua (third-party)

Vendored Lua source used by POOM to enable scriptable firmware extensions and automation features.

## Overview

- **Upstream**: Lua 5.4.6 (released 02 May 2023)
- **License**: MIT (see `LICENSE`)
- **Purpose**: Provides a lightweight, embeddable scripting engine for device automation

This component builds the Lua 5.4 VM and standard libraries as an ESP-IDF component, enabling execution of Lua scripts stored on the SD card.

## Features

- Full Lua 5.4 standard library support
- SD card script execution
- C/Lua bindings layer for native firmware access
- FreeRTOS task-based execution engine
- Error handling and stack traceability
- Optimized for embedded systems with memory constraints

## Component Integration

### Basic Usage

```c
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

lua_State *L = luaL_newstate();
luaL_openlibs(L);
// Register custom functions...
luaL_dofile(L, "/sdcard/script.lua");
lua_close(L);
```

### Including in Your Application

Add to `CMakeLists.txt`:
```cmake
REQUIRES lua
```

## SD Script Execution

Scripts are loaded from the SD card mount point (default: `/sdcard/main.lua`).

Example script:
```lua
print("Script started")
App.my_function(123)
```

