rem wasmr -zq -zcm -Dmemodel=tiny mbr.asm
rem tasm -m2 -Dmemodel=tiny mbr.asm
rem tlink -t -x mbr,mbr.com,,,

rem ml -c -Dmemodel=tiny mbr.asm

rem wasm is incorrectly treating warnings as errors
rem so be careful if you see any warnings
rem wasm -zq -zcm -Dmemodel=tiny mbr.asm
rem wlink File mbr.obj Name mbr.com Form dos com Option quiet

as86 -Dmemodel=tiny -o mbr.o mbr.asm
ld86 -o mbr.com --oformat msdos mbr.o
