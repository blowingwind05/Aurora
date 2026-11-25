# SPDX-License-Identifier: GPL-3.0-only

add_subdirectory(moonlight)
add_subdirectory(sunshine)
add_subdirectory(sunshine/admin)
add_subdirectory(sunshine/host)
add_subdirectory(sunshine/lifecycle)
add_subdirectory(tailscale)
add_subdirectory(moonlight/control)
if(WIN32)
    add_subdirectory(tailscale/system)
endif()
