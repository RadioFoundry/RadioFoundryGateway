#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# The Pluto bundle's scripts on this host (tools/pluto/bundle/): which program run.sh starts when the flash and the
# SD card's RDOF partition both hold one, where an update goes, and the rollback; install.sh's RDOF partition plan and
# MBR entry. The flash and the partition are directories here; the programs are shell scripts, packed with xz.
#   sh host-tests/pluto_bundle.sh <repo root>
set -u
B=$1/tools/pluto/bundle
W=$(mktemp -d "${TMPDIR:-/tmp}/rdof-plbundle.XXXXXX")
trap 'rm -rf "$W"' EXIT
FAIL=0
no() { echo "FAIL: $*"; FAIL=1; }

# a fake program: answers --help as rdofradio does, notes its name in ran, then runs act.N (N: runs so far), all in
# the case's directory (RDOF_RUN_ROOT, from run.sh's environment)
prog() {
    cat > $W/$1 << EOF
#!/bin/sh
[ "\$1" = --help ] && { echo "usage: rdofradio"; exit 0; }
R=\$RDOF_RUN_ROOT
echo $1 >> \$R/ran
N=\$(wc -l < \$R/ran | tr -d ' ')
RC=0
[ \$N -gt 6 ] && touch \$R/mnt/jffs2/rdof/disabled
[ -f \$R/act.\$N ] && . \$R/act.\$N
exit \$RC
EOF
    xz -c $W/$1 > $W/$1.xz
}
# a new case: the flash and the partition empty
fresh() {
    R=$W/$1
    mkdir -p $R/mnt/jffs2/rdof $R/sd/rdof $R/tmp/rdof
    : > $R/mnt/jffs2/rdof/flags
    : > $R/ran
}
# image $1 in place $2 (flash or sd) with sequence number $3
have() {
    case $2 in flash) P=$R/mnt/jffs2/rdof ;; *) P=$R/sd/rdof ;; esac
    cp $W/$1.xz $P/rdofradio.xz
    echo $3 > $P/seq
}
act() { echo "$2" > $R/act.$1; }
ota() { act $1 "cp $W/$2.xz $R/tmp/rdof/rdofradio.ota"; }
stop() { act $1 "touch $R/mnt/jffs2/rdof/disabled"; }
# run.sh until a program stops it: $1 KB of room in the flash (1000), $2 the SD card's partition (a directory)
run() { RDOF_RUN_ROOT=$R RDOF_RUN_SD=${2:-$R/sd} RDOF_RUN_PAUSE=0 RDOF_RUN_ROOM_KB=${1:-1000} sh $B/run.sh; }
ran() { [ "$(tr '\n' ' ' < $R/ran)" = "$2 " ] || no "$1: ran $(tr '\n' ' ' < $R/ran), not $2"; }
is() { # place $2 holds program $3 with sequence number $4
    case $2 in flash) P=$R/mnt/jffs2/rdof ;; *) P=$R/sd/rdof ;; esac
    if [ -z "$3" ]; then
        [ -f $P/rdofradio.xz ] && no "$1: $2 still holds a program"
        return 0
    fi
    xz -dc $P/rdofradio.xz 2> /dev/null | cmp -s - $W/$3 || no "$1: $2 doesn't hold $3"
    [ "$(cat $P/seq 2> /dev/null)" = "$4" ] || no "$1: $2's sequence number $(cat $P/seq 2> /dev/null), not $4"
}

for p in A B C; do prog $p; done

# the newer runs: the SD card's
fresh newer; have A flash 1; have B sd 2; stop 1; run; ran newer B
# the flash's newer
fresh older; have A flash 3; have B sd 2; stop 1; run; ran older A
# the newer won't unpack: the other one
fresh broken; have A flash 1; echo junk > $R/sd/rdof/rdofradio.xz; echo 2 > $R/sd/rdof/seq
stop 1; run; ran broken A
grep -q "doesn't unpack" $R/tmp/rdof/log || no "broken: not in the log"
# no number at all (an install from before): the flash's
fresh legacy; have A flash 1; rm $R/mnt/jffs2/rdof/seq; stop 1; run; ran legacy A

# an update that fits the flash goes there, numbered past both
fresh fits; have A flash 1; have B sd 2; ota 1 C; stop 2; run; ran fits "B C"; is fits flash C 3; is fits sd B 2
# one that doesn't goes on the SD card; the flash keeps its own
fresh big; have A flash 1; ota 1 C; stop 2; run 0; ran big "A C"; is big flash A 1; is big sd C 2
grep -q "updated: in $R/sd/rdof" $R/tmp/rdof/log || no "big: not in the log"
# no room anywhere: the program as it was
fresh noroom; have A flash 1; ota 1 C; stop 2; run 0 $R/nosd; ran noroom "A A"; is noroom flash A 1
grep -q "update failed" $R/tmp/rdof/log || no "noroom: not in the log"

# the update on the SD card ends in error at once: the one before goes back there, and runs
fresh rollsd; have A flash 1; ota 1 C; act 2 "RC=1"; stop 3; run 0; ran rollsd "A C A"; is rollsd sd A 2
grep -q "put back" $R/tmp/rdof/log || no "rollsd: not in the log"
# in the flash, while the SD card's ran: the SD card's goes in the flash
fresh rollflash; have A flash 1; have B sd 2; ota 1 C; act 2 "RC=1"; stop 3; run; ran rollflash "B C B"
is rollflash flash B 3; is rollflash sd B 2
# install.sh's partition plan: the Pluto's card (labeled 128 MB, says 3.76 GiB; boot partition 242 + 244238 sectors)
mbr() { # entry 1: FAT32 LBA at $1, $2 sectors
    printf '\200\003\066\000\014\067\050\017' > $W/mbr
    for v in $1 $2; do
        for i in 1 2 3 4; do printf "\\$(printf %03o $((v % 256)))" >> $W/mbr; v=$((v / 256)); done
    done
    head -c 48 /dev/zero >> $W/mbr
    printf '\125\252' >> $W/mbr
    { head -c 446 /dev/zero; cat $W/mbr; } > $W/mbr.full && mv $W/mbr.full $W/mbr
}
plan() { sh $B/install.sh --sd-test $W/mbr $1 2>&1; }
mbr 242 244238
[ "$(plan 7880704)" = "245760 14336" ] || no "plan: $(plan 7880704)"
[ "$(wc -c < $W/mbr.new | tr -d ' ')" = 512 ] || no "mbr: $(wc -c < $W/mbr.new) octets"
cmp -s -n 462 $W/mbr $W/mbr.new || no "mbr: entry 1 or the code before it changed"
[ "$(od -An -tx1 -j462 -N16 $W/mbr.new | tr -d ' \n')" = 00feffff0cfeffff00c0030000380000 ] ||
    no "mbr: entry 2 $(od -An -tx1 -j462 -N16 $W/mbr.new)"
[ "$(od -An -v -tx1 -j478 -N34 $W/mbr.new | tr -d ' \n')" = "$(printf '0%.0s' $(seq 64))55aa" ] ||
    no "mbr: entries 3-4 or the signature changed"
# a card no bigger than its boot partition, or past it by under 4 MiB: none
case "$(plan 250000)" in "no room"*) ;; *) no "small card: $(plan 250000)" ;; esac
mbr 2048 260096 # ends at 128 MiB
case "$(plan 7880704)" in "no room"*) ;; *) no "full card: $(plan 7880704)" ;; esac
# a second partition already: none
mbr 242 244238
printf '\014' | dd of=$W/mbr bs=1 seek=466 conv=notrunc 2> /dev/null
case "$(plan 7880704)" in *"partitions after"*) ;; *) no "two partitions: $(plan 7880704)" ;; esac
# not FAT
mbr 242 244238
printf '\203' | dd of=$W/mbr bs=1 seek=450 conv=notrunc 2> /dev/null
case "$(plan 7880704)" in *"isn't FAT"*) ;; *) no "not FAT: $(plan 7880704)" ;; esac
# a bigger boot partition (a 1 GB card's 500 MiB): up to 511 MiB
mbr 2048 1021952
[ "$(plan 7880704)" = "1024000 22528" ] || no "500 MiB: $(plan 7880704)"

[ $FAIL = 0 ] && echo "pluto bundle: ok"
exit $FAIL
