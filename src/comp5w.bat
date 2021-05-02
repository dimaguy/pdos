gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib pdos.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib bos.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib fat.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib patmat.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib memmgr.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib protintp.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib exeload.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib process.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib int21.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib int80.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib log.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib helper.c
gcc386 -S -Os -fno-common -DNOVM -D__PDOS386__ -D__32BIT__ -I../pdpclib vgatext.c

as386 -o pdos.o pdos.s
del pdos.s
as386 -o bos.o bos.s
del bos.s
as386 -o fat.o fat.s
del fat.s
as386 -o patmat.o patmat.s
del patmat.s
as386 -o memmgr.o memmgr.s
del memmgr.s
as386 -o protintp.o protintp.s
del protintp.s
as386 -o exeload.o exeload.s
del exeload.s
as386 -o process.o process.s
del process.s
as386 -o int21.o int21.s
del int21.s
as386 -o int80.o int80.s
del int80.s
as386 -o log.o log.s
del log.s
as386 -o helper.o helper.s
del helper.s
as386 -o vgatext.o vgatext.s
del vgatext.s

as386 -o strt32.o strt32.s
as386 -o support.o support.s
as386 -o protints.o protints.s
as386 -o pdoss.o pdoss.s

del os.a
ar386 -r os.a bos.o fat.o exeload.o
ar386 -r os.a process.o
ar386 -r os.a int21.o int80.o log.o helper.o
ar386 -r os.a memmgr.o patmat.o support.o protintp.o protints.o pdoss.o
ar386 -r os.a vgatext.o

ld386 -N -s -e start -o pdos.exe strt32.o pdos.o os.a ../pdpclib/pdos.a
del os.a
