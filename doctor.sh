#!/bin/sh
exec /usr/bin/python3 -I "$(cd "$(dirname "$0")" && pwd)/primebridge.py" doctor "$@"
