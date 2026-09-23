#!/usr/bin/env bash
set -e

PIO=~/.platformio/penv/bin/pio

cd "$(dirname "$0")"
"$PIO" run -t upload
"$PIO" device monitor
