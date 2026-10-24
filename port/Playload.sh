#!/bin/bash
# PORTMASTER: playload.zip, Playload.sh
# Playload player: runs Playload game URLs (kitao.dev/playload) saved as .url files.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/playload"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# On plumOS, run at the performance governor like its own game launchers do,
# and restore the previous governors on exit. Devices without plumOS's
# cpu-control helper (e.g. Pixel2) get the same done through sysfs.
CPU_CONTROL=/storage/plumos/bin/plumos-cpu-control
CPU_SNAPSHOT=/run/plumos/playload-cpu.snapshot
if [ -x "$CPU_CONTROL" ] && sh "$CPU_CONTROL" snapshot "$CPU_SNAPSHOT"; then
  sh "$CPU_CONTROL" apply performance
  trap 'sh "$CPU_CONTROL" restore "$CPU_SNAPSHOT"' EXIT
else
  saved_governors=""
  for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -w "$policy/scaling_governor" ] || continue
    grep -qw performance "$policy/scaling_available_governors" 2>/dev/null || continue
    saved_governors="$saved_governors $policy=$(cat "$policy/scaling_governor")"
    echo performance > "$policy/scaling_governor"
  done
  trap 'for kv in $saved_governors; do echo "${kv#*=}" > "${kv%%=*}/scaling_governor"; done' EXIT
fi

pm_platform_helper "$GAMEDIR/playload.${DEVICE_ARCH}"
./playload.${DEVICE_ARCH} "${@:-games}"

pm_finish
