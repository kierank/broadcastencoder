#!/bin/sh

mask=`numactl -s | grep ^nodebind -|cut -d\  -f2-|sed 's/ *$//g'|rev|cut -d\  -f1`
i=`echo $(($1 & $mask))`
exec numactl -m $i -N $i "`dirname $0`"/obed $1
