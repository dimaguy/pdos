rem wasmr -zq -zcm -Dmemodel=tiny fat32.asm
rem tasm -m2 -Dmemodel=tiny fat32.asm
rem tlink -t -x fat32,fat32.com,,,

wasm -zq -zcm -Dmemodel=tiny fat32.asm
wlink File fat32.obj Name fat32.com Form dos com Option quiet

rem as86 -Dmemodel=tiny -o fat32.o fat32.asm
rem ld86 -o fat32.com --oformat msdos fat32.o

rem ml -c -Dmemodel=tiny fat32.asm
rem wlink File fat32.obj Name fat32.com Form dos com Option quiet
