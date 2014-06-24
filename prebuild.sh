#!/bin/bash

cd ..
git branch | grep "2.2" && git pull || git checkout --track origin/release/2.2
./configure --disable-yasm
#make clean
make
