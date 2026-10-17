#!/bin/sh
# Starts the Linux build you made (make linux or make linux-compat).
cd "$(dirname "$0")"
for exe in build/linux/ds2rb build/linux-compat/ds2rb; do
    if [ -x "$exe" ]; then exec "$exe" --data data "$@"; fi
done
echo "No Linux build found. See README.md: make setup recomp linux" >&2
exit 1
