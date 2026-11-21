#!/bin/bash
# Direct launcher for Daytona USA Deluxe '93 (daytona93) on PortMaster
GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
exec "$GAMEDIR/Daytona USA.sh" --romset daytona93 --autostart "$@"
