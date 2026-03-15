#!/bin/sh
cd "$(dirname "$0")"

if [ ! -d build-sim ]; then
    mkdir build-sim
    cd build-sim
    ../tools/configure --target=ipod6g --type=s
else
    cd build-sim
fi

make -j$(sysctl -n hw.ncpu) && make install

# Install theme/font files from ~/Temp
# for z in ~/Temp/rockbox-fonts-*.zip ~/Temp/SNARTY.zip ~/Temp/adwaitapod_dark_simplified.zip ~/Temp/themify.zip; do
#     [ -f "$z" ] && unzip -o "$z" -d simdisk/
# done
