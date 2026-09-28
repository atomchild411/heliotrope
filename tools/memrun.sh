#!/bin/sh
# memrun.sh NAME URL [SETTLE] [TIMEOUT]: load URL in heliotrope on :0, wait for
# "loaded" plus SETTLE seconds (default 30), then print memory per process in
# KB: resident, private, malloc heap (the brk region), libWPEWebKit's private
# pages (relocated data), and the rest. Extra environment (HELIOTROPE_*,
# JSC_*) passes through to heliotrope and WebKit. Needs pmapsgi in /var/tmp.
name=$1 url=$2 settle=${3:-30} limit=${4:-900}
lib=/opt/pkgsrc/lib/libWPEWebKit-2.0.so.1.9.10
wki=`ls -i $lib | awk '{print $1}'`
cd /var/tmp
rm -f $name.mlog
DISPLAY=:0 ${HELIOTROPE_BIN:-/opt/pkgsrc/bin/heliotrope} -g 620x640+640+40 "$url" > $name.mlog 2>&1 &
H=$!
i=0
while [ $i -lt $limit ] && kill -0 $H 2>/dev/null && ! egrep "loaded|failed" $name.mlog >/dev/null 2>&1; do
    sleep 5
    i=`expr $i + 5`
done
sleep $settle
echo "$name: loaded after ~${i}s, measured ${settle}s later"
for P in $H `ps -e -o pid,ppid,args | awk -v h=$H '$2 == h && /WPE/ {print $1}'`; do
    n=`ps -p $P -o comm= 2>/dev/null`
    /var/tmp/pmapsgi $P > /var/tmp/$name.$P.map
    awk -v n="$n" -v wki="$wki" '
        $5 == "3813" || $5 == "3811" { if ($2 > big) { big = $2; heap = $4 } }
        $NF ~ ("/" wki "$") { wk += $4 }
        /^total/ { res = $3; priv = $6 }
        END { printf "  %-18s resident %7d  private %7d  heap %7d  webkit %6d  other %7d\n", n, res, priv, heap, wk, priv - heap - wk }
    ' /var/tmp/$name.$P.map
done
grep -v "^heliotrope: loaded" $name.mlog | head -5
kill $H 2>/dev/null
sleep 5
