#!/bin/sh

cd /tmp
find . -name '*.core' | while read c; do
	p=$(basename $c)
	p=${p%.core}
	if [ -f $p ]; then
		prog=$p
	elif [ -f /usr/local/libexec/$p ]; then
		prog=/usr/local/libexec/$p
	elif [ -f /usr/local/bin/$p ]; then
		prog=/usr/local/bin/$p
	else
		prog=""
	fi
	echo
	echo === CORE FILE ===
	echo $p - $prog
	printf 'bt\nquit' | lldb $prog -c $c
done
