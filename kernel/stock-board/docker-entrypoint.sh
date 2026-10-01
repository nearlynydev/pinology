#!/bin/sh
set -eu
action=${1:-run}
if [ "$#" -gt 0 ]; then shift; fi
case "$action" in
  run) exec /opt/pat-python/bin/python /opt/ds223/runtime/instance.py run "$@" --qemu /opt/ds223/bin/qemu-system-aarch64 --flash --restart-on-guest-reset --console-socket ;;
  checkpoint) exec /opt/pat-python/bin/python /opt/ds223/runtime/checkpoint.py "$@" ;;
  *) exec /opt/pat-python/bin/python /opt/ds223/runtime/instance.py "$action" "$@" ;;
esac
