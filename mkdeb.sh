#!/bin/sh

set -e

trap clean INT TERM EXIT

clean() {
    rm -f debian/control
}

if [ -z "$C100" ]
then
    echo "Please set C100 env variable to 1 (C100) or 0 (C200)"
    exit 1
fi

if [ "$C100" = 0 ]
then
    export ver="200"
else
    export ver="100"
fi

sed s/@@/${ver}/ debian/control.in > debian/control

echo
echo Building c${ver} deb package
echo

# original version
v=`head -1 debian/changelog |cut -d\( -f2|cut -d\) -f1`

dpkg-buildpackage -us -uc -b

mv ../obed_"$v"_amd64.deb ../obed_"$v"-c"$ver"_amd64.deb
