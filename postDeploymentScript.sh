#!/bin/sh

make install;
/elog-nfs/bin/elogd -p 9090 -c /elog-nfs/elog/elogd.cfg -D;