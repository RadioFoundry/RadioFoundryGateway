#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The Pluto bundle's run.sh: on a kernel without cdc_acm, the generic usb-serial driver takes a USB radio's IDs; on
# one with it, or without usb-serial, nothing is written. Against a made-up /sys (new_id a FIFO: each write a line,
# as sysfs takes them). Argument: tools/pluto/bundle/run.sh.
RUN=$1
W=$(mktemp -d "${TMPDIR:-/tmp}/rdof-usbserial.XXXXXX")
trap 'rm -rf "$W"' EXIT
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

# the writes run.sh makes to new_id, one a line
writes() {
    S=$W/$1
    mkdir -p $S/bus/usb-serial/drivers/generic
    mkfifo $S/bus/usb-serial/drivers/generic/new_id
    cat $S/bus/usb-serial/drivers/generic/new_id > $W/$1.got &
    R=$!
    exec 3> $S/bus/usb-serial/drivers/generic/new_id # held open: the reader sees every write, ends when it closes
    sh "$RUN" --usb-serial $S || echo "run.sh --usb-serial exited $?"
    exec 3>&-
    wait $R
    cat $W/$1.got
}

# the stock Pluto kernel: generic usb-serial, no cdc_acm
got=$(writes stock)
[ "$got" = "1fc9 0094" ] || fail "stock kernel: new_id got '$got'"

# a kernel with cdc_acm: left to it
mkdir -p $W/acm/bus/usb/drivers/cdc_acm
got=$(writes acm)
[ -z "$got" ] || fail "with cdc_acm: new_id got '$got'"

# no usb-serial at all: nothing, and no error
mkdir -p $W/none/bus/usb/drivers
sh "$RUN" --usb-serial $W/none || fail "no usb-serial: exited $?"

[ $fails -eq 0 ] && echo "pluto usb serial: ok" || exit 1
