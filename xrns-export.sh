#!/bin/sh
if [ -z "$2" ] ; then
	echo "usage: $0 <song.xrns> <song.xrns.inc.c>"
	exit 1
fi
unzip -p $1 Song.xml | $(dirname $0)/song-xml-to-inc-c.py $1 $2
