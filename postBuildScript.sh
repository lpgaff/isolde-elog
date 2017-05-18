#!/bin/sh

make;
make install;
elogd -p 9090 -c /elog-nfs/elog/elogd.cfg -D;