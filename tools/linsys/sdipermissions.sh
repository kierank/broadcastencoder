#!/bin/bash

if [[ $EUID -ne 0 ]];
then
   echo "This script must be run as root" 1>&2
   exit 1
fi

sysvideo="/sys/class/sdivideo"
sysaudio="/sys/class/sdiaudio"

for i in `seq 0 16`;
    do
        if [ -c "/dev/sdivideorx$i" ]
        then
            echo "Setting permissions for card $i" 1>&2
            chmod 774 "/dev/sdivideorx$i"
            chmod 774 "/dev/sdiaudiorx$i"
            chmod 776 "${sysvideo}/sdivideorx$i/buffers"
            chmod 776 "${sysvideo}/sdivideorx$i/bufsize"
            chmod 776 "${sysvideo}/sdivideorx$i/mode"
            chmod -f 776 "${sysvideo}/sdivideorx$i/vanc"
            chmod 776 "${sysaudio}/sdiaudiorx$i/buffers"
            chmod 776 "${sysaudio}/sdiaudiorx$i/bufsize"
            chmod 776 "${sysaudio}/sdiaudiorx$i/channels"
            chmod 776 "${sysaudio}/sdiaudiorx$i/sample_size"
        else
            if [ "$i" = "0" ]
            then
                echo "No Linsys/DVEO SDI cards found" 1>&2
            fi
            exit 1
        fi
    done
