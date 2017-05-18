#!/bin/sh

make;
make intsall;
elogd -p 9090 -c /elog-nfs/elog/elogd.cfg -D;