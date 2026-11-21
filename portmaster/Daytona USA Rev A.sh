#!/bin/bash
# Direct launcher for Daytona USA Revision A 1994 (daytona) on PortMaster
GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
exec "$GAMEDIR/Daytona USA.sh" --romset daytona --autostart "$@"
