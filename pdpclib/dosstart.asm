; dosstart.asm - startup code for C programs for DOS
;
; This program written by Paul Edwards
; Released to the public domain

; For an executable, at entry, the CS:IP will be set
; correctly as specified in the executable header (offset
; 0x14 - 2 bytes - initial IP, and 0x16 - 2 bytes,
; an offset segment). DS and ES will both be set to point to the
; PSP. SS will be set correct as specified in the
; executable header (offset 0x0e - 2 bytes - an offset
; segment), and SP will be set to the stack length as
; specified in the executable header (offset 0x10 - 2 bytes).

; For a COM file, there is no header at all, so the values
; that are set are - all of CS, DS, ES, SS are set to point
; to the PSP and the IP is 0x100, ie at the end of the PSP,
; which means you need both an "org 100h" in your assembler
; code plus you need to link that object code first. You
; must also use the tiny memory model and you may use the
; exe2bin utility or your linker may be able to produce a
; COM file itself.

% .model memodel, c

extrn __start:proc

extrn _end:byte
extrn _edata:byte

public __psp
public __envptr
public __osver

ifndef MAKECOM
.stack 1000h
endif

.data

banner  db  "PDPCLIB"
__psp   dd  ?
__envptr dd ?
__osver dw ?

.code

ifdef MAKECOM
org 100h
endif

top:

public __asmstart

__asmstart proc

; add some nops to create a cs-addressable save area, and also create a
; bit of an eyecatcher

nop
nop
nop
nop

; push the psp now, ready for calling start

if @DataSize
push ds
endif
; in small etc memory model, this will be a NULL
mov ax, 0
push ax

; determine how much memory is needed. The stack pointer points
; to the top. Work out what segment that is, then subtract the
; starting segment (the PSP), and you have your answer.

mov ax, sp
mov cl, 4
shr ax, cl ; get sp into pages
mov bx, ss
add ax, bx
add ax, 2 ; safety margin because we've done some pushes etc
mov bx, es
sub ax, bx ; subtract the psp segment

; free initially allocated memory

mov bx, ax
mov ah, 4ah
int 21h

; It appears that in the tiny memory model, you are still required
; to set ds to the same as cs yourself, presumably because ds is
; pointing to the PSP while cs is probably pointing to the beginning
; of the executable. DGROUP may also get the correct value, presumably
; zero. es is set to ds a bit later. And you need to set ss to that
; value too

if @Model eq 1
mov dx, cs
else
mov dx,DGROUP
endif

mov ds,dx

; In tiny, small and medium memory models, you need to set
; ss to ds (MSDOS will have set them to different values
; when it loaded the executable).
; ds and ss are the same so that
; near pointers can refer to either stack or data and still work

if @DataSize
else

mov bx,ss
mov ax,ds
sub bx,ax
mov cl,4
shl bx,cl

mov bp, sp
add bp, bx
mov ss, dx
mov sp, bp
; And that null PSP thing needs to be redone
mov ax, 0
push ax

endif


; we are responsible for clearing our own BSS
; in Watcom at least, the BSS is at the end of the DGROUP
; which can be referenced as _end, and the end of the
; DATA section is referenced as _edata
; We can use rep stos to clear the memory, which initializes
; using the byte in al, starting at es:di, for a length of cx

push es
mov cx, offset DGROUP:_end
mov di, offset DGROUP:_edata
sub cx, di
mov es, dx ; we still have that set from above
mov al, 0
rep stosb
pop es

mov ah,30h
int 21h
xchg al,ah
mov [__osver],ax

mov word ptr __psp, 0
mov word ptr [__psp + 2], es
mov word ptr __envptr, 0
mov dx, es:[02ch]
mov word ptr [__envptr + 2], dx
mov dx, ds
mov es, dx

; we have already pushed the pointer to psp
call __start
if @DataSize
add sp, 4  ; delete psp from stack
else
add sp, 2
endif

push ax

; how do I get rid of the warning about "instruction can be compacted
; with override"?  The answer is certainly NOT to change the "far" to
; "near".
call __exita
add sp, 2
ret
__asmstart endp

public __exita
__exita proc rc:word
mov ax, rc
mov ah,4ch
int 21h ; terminate
ret
__exita endp


public __main
__main proc
ret
__main endp


end top
