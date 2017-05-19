#!/bin/sh

make install;
../bin/elogd -p 9090 -c ../elog/elogd.cfg -D;