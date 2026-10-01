#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/dsm" menu
