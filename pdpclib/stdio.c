/*********************************************************************/
/*                                                                   */
/*  This Program Written by Paul Edwards.                            */
/*  Released to the Public Domain                                    */
/*                                                                   */
/*  Modifications by Dave Edwards, released to the Public Domain     */
/*                                                                   */
/*********************************************************************/
/*********************************************************************/
/*                                                                   */
/*  stdio.c - implementation of stuff in stdio.h                     */
/*                                                                   */
/*  The philosophy of this module is explained here.                 */
/*  There is a static array containing pointers to file objects.     */
/*  This is required in order to close all the files on program      */
/*  termination.                                                     */
/*                                                                   */
/*  In order to give speed absolute priority, so that people don't   */
/*  resort to calling DosRead themselves, there is a special flag    */
/*  in the FILE object called "quickbin".  If this flag is set to 1  */
/*  it means that it is a binary file and there is nothing in the    */
/*  buffer and there are no errors, so don't stuff around, just call */
/*  DosRead.                                                         */
/*                                                                   */
/*  When a buffer exists, which is most of the time, fbuf will point */
/*  to it.  The size of the buffer is given by szfbuf.  upto will    */
/*  point to the next character to be read.  endbuf will point PAST  */
/*  the last valid character in the buffer.  bufStartR represents    */
/*  the position in the file that the first character in the buffer  */
/*  is at.  This is only updated when a new buffer is read in.       */
/*                                                                   */
/*  After file open, for a file being read, bufStartR will actually  */
/*  be a negative number, which if added to the position of upto     */
/*  will get to 0.  On a file being written, bufStartR will be set   */
/*  to 0, and upto will point to the start of the buffer.  The       */
/*  reason for the difference on the read is in order to tell the    */
/*  difference between an empty buffer and a buffer with data in it, */
/*  but which hasn't been used yet.  The alternative would be to     */
/*  either keep track of a flag, or make fopen read in an initial    */
/*  buffer.  But we want to avoid reading in data that no-one has    */
/*  yet requested.                                                   */
/*                                                                   */
/*  The buffer is organized as follows...                            */
/*  What we have is an internal buffer, which is 8 characters        */
/*  longer than the actually used buffer.  E.g. say BUFSIZ is        */
/*  512 bytes, then we actually allocate 520 bytes.  The first       */
/*  2 characters will be junk, the next 2 characters set to NUL,     */
/*  for protection against some backward-compares.  The fourth-last  */
/*  character is set to '\n', to protect against overscan.  The      */
/*  last 3 characters will be junk, to protect against memory        */
/*  violation.  intBuffer is the internal buffer, but everyone       */
/*  refers to fbuf, which is actually set to the &intBuffer[4].      */
/*  Also, szfbuf is the size of the "visible" buffer, not the        */
/*  internal buffer.  The reason for the 2 junk characters at the    */
/*  beginning is to align the buffer on a 4-byte boundary.           */
/*                                                                   */
/*                                                                   */
/*********************************************************************/

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdarg.h"
#include "ctype.h"
#include "errno.h"
#include "float.h"
#include "limits.h"

/* PDOS and MSDOS use the same interface most of the time */
/* Note that PDOS is for the 32-bit version, since the 16-bit
   version uses the MSDOS version since it is compatible with it */
/* linux is pretty similar too */
#if defined(__PDOS__) || defined(__gnu_linux__)
#define __MSDOS__
#endif

#if defined(__MSDOS__) && !defined(__gnu_linux__)
#ifdef __WATCOMC__
#define CTYP __cdecl
#else
#define CTYP
#endif
extern int CTYP __read(int handle, void *buf, size_t len, int *errind);
extern int CTYP __write(int handle, const void *buf, size_t len, int *errind);
extern void CTYP __seek(int handle, long offset, int whence);
extern void CTYP __close(int handle);
extern void CTYP __remove(const char *filename);
extern void CTYP __rename(const char *old, const char *new);
#endif

#ifdef __OS2__
#include <os2.h>
#endif

#ifdef __WIN32__
#include <windows.h>
#endif

#if defined(__MVS__) || defined(__CMS__)
#include "mvssupa.h"
#define FIXED_BINARY 0
#define VARIABLE_BINARY 1
#define FIXED_TEXT 2
#define VARIABLE_TEXT 3
#endif

#if defined(__gnu_linux__)

extern int __open(const char *a, int b, int c);
extern int __write(int a, const void *b, int c);
extern int __read(int a, void *b, int c);

#define O_WRONLY 0x1
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_RDONLY 0x0

static int open(const char *a, int b, int *c)
{
    int ret;
    
    *c = 0;
    if (b)
    {
        ret = __open(a, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    }
    else
    {
        ret = __open(a, O_RDONLY, 0);
    }
    if (ret < 0)
    {
        *c = 1;
    }
    return (ret);
}

#define __open(a,b,c) (open((a),(b),(c)))
#define __write(a,b,c,d) (*(d) = 0, (__write)((a),(b),(c)))
#define __read(a,b,c,d) (*(d) = 0, (__read)((a),(b),(c)))

#endif

static FILE permFiles[3];

#define unused(x) ((void)(x))
#define outch(ch) ((fq == NULL) ? *s++ = (char)ch : putc(ch, fq))
#define inch() ((fp == NULL) ? \
    (ch = (unsigned char)*s++) : (ch = getc(fp)))

/* We need to choose whether we are doing move mode or
   locate mode */
#if !LOCMODE /* move mode */
#define begwrite(stream, len) (lenwrite = (len), dptr = (stream)->asmbuf)
#define finwrite(stream) (__awrite((stream)->hfile, NULL, lenwrite))
#else /* locate mode */
#define begwrite(stream, len) (__awrite((stream)->hfile, &dptr, len))
#define finwrite(stream)
#endif

#if defined(__MVS__) || defined(__CMS__)
static unsigned char *dptr;
static size_t lenwrite;
static int    inseek = 0;
#endif


FILE *stdin = &permFiles[0];
FILE *stdout = &permFiles[1];
FILE *stderr = &permFiles[2];

FILE *__userFiles[__NFILE];
static FILE  *myfile;
static int    spareSpot;
static int    err;
static int    inreopen = 0;

static const char *fnm;
static const char *modus;
static int modeType;

static void dblcvt(double num, char cnvtype, size_t nwidth,
                   int nprecision, char *result);
static int vvprintf(const char *format, va_list arg, FILE *fq, char *s);
static int vvscanf(const char *format, va_list arg, FILE *fp, const char *s);
static void fopen2(void);
static void fopen3(void);
static void findSpareSpot(void);
static void checkMode(void);
static void osfopen(void);

#if !defined(__MVS__) && !defined(__CMS__)
static void fwriteSlow(const void *ptr,
                       size_t size,
                       size_t nmemb,
                       FILE *stream,
                       size_t towrite,
                       size_t *elemWritten);
static void fwriteSlowT(const void *ptr,
                        FILE *stream,
                        size_t towrite,
                        size_t *actualWritten);
static void fwriteSlowB(const void *ptr,
                        FILE *stream,
                        size_t towrite,
                        size_t *actualWritten);
static void freadSlowT(void *ptr,
                       FILE *stream,
                       size_t toread,
                       size_t *actualRead);
static void freadSlowB(void *ptr,
                       FILE *stream,
                       size_t toread,
                       size_t *actualRead);
#endif

static int examine(const char **formt, FILE *fq, char *s, va_list *arg,
                   int chcount);

#ifdef __CMS__
extern void __SVC202 ( char *s202parm, int *code, int *parm );
static void filedef(char *fdddname, char *fnm, int mymode);
static void fdclr(char *ddname);
static char *int_strtok(char *s1, const char *s2);
#define strtok int_strtok
#endif


int printf(const char *format, ...)
{
    va_list arg;
    int ret;

    va_start(arg, format);
    ret = vfprintf(stdout, format, arg);
    va_end(arg);
    return (ret);
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list arg;
    int ret;

    va_start(arg, format);
    ret = vfprintf(stream, format, arg);
    va_end(arg);
    return (ret);
}

int vfprintf(FILE *stream, const char *format, va_list arg)
{
    int ret;

    stream->quickText = 0;
    ret = vvprintf(format, arg, stream, NULL);
    return (ret);
}

FILE *fopen(const char *filename, const char *mode)
{
    fnm = filename;
    modus = mode;
    err = 0;
    findSpareSpot();
    if (!err)
    {
        myfile = malloc(sizeof(FILE));
        if (myfile == NULL)
        {
            err = 1;
        }
        else
        {
            fopen2();
            if (err)
            {
                free(myfile);
            }
        }
    }
    if (err)
    {
        myfile = NULL;
    }
    return (myfile);
}

static void fopen2(void)
{
    checkMode();
    if (!err)
    {
        strcpy(myfile->modeStr, modus);
        osfopen();
        if (!err)
        {
            __userFiles[spareSpot] = myfile;
            myfile->intFno = spareSpot;
            fopen3();
        }
    }
    return;
}

static void fopen3(void)
{
    myfile->intBuffer = malloc(BUFSIZ + 8);
    if (myfile->intBuffer == NULL)
    {
        err = 1;
    }
    else
    {
        myfile->theirBuffer = 0;
        myfile->fbuf = myfile->intBuffer + 2;
        *myfile->fbuf++ = '\0';
        *myfile->fbuf++ = '\0';
        myfile->szfbuf = BUFSIZ;
#if !defined(__MVS__) && !defined(__CMS__)
        myfile->quickText = 0;
#endif
        myfile->noNl = 0;
        myfile->endbuf = myfile->fbuf + myfile->szfbuf;
        *myfile->endbuf = '\n';
#if defined(__MVS__) || defined(__CMS__)
        myfile->upto = myfile->fbuf;
        myfile->szfbuf = myfile->lrecl;
        myfile->endbuf = myfile->fbuf; /* for read only */
#else
        myfile->upto = myfile->endbuf;
#endif
#if defined(__MVS__) || defined(__CMS__)
        myfile->bufStartR = 0;
#else
        myfile->bufStartR = -(long)myfile->szfbuf;
#endif
        myfile->errorInd = 0;
        myfile->eofInd = 0;
        myfile->ungetCh = -1;
        myfile->update = 0;
        if (!inreopen)
        {
            myfile->permfile = 0;
        }
        myfile->isopen = 1;
#if !defined(__MVS__) && !defined(__CMS__)
        if (!myfile->textMode)
        {
            myfile->quickBin = 1;
        }
        else
        {
            myfile->quickBin = 0;
        }
#endif
        myfile->mode = __READ_MODE;
        switch (modeType)
        {
            case 2:
            case 3:
            case 5:
            case 6:
            case 8:
            case 9:
            case 11:
            case 12:
                myfile->bufStartR = 0;
                myfile->upto = myfile->fbuf;
                myfile->mode = __WRITE_MODE;
#if defined(__MVS__) || defined(__CMS__)
                myfile->endbuf = myfile->fbuf + myfile->szfbuf;
#endif
                break;
        }
        switch (modeType)
        {
            case 7:
            case 8:
            case 10:
            case 11:
            case 12:
                myfile->update = 1;
                break;
        }
    }
    return;
}

static void findSpareSpot(void)
{
    int x;

    for (x = 0; x < __NFILE; x++)
    {
        if (__userFiles[x] == NULL)
        {
            break;
        }
    }
    if (x == __NFILE)
    {
        err = 1;
    }
    else
    {
        spareSpot = x;
    }
    return;
}

/* checkMode - interpret mode string */
/* r = 1 */
/* w = 2 */
/* a = 3 */
/* rb = 4 */
/* wb = 5 */
/* ab = 6 */
/* r+ = 7 */
/* w+ = 8 */
/* a+ = 9 */
/* r+b or rb+ = 10 */
/* w+b or wb+ = 11 */
/* a+b or ab+ = 12 */

static void checkMode(void)
{
    if (strncmp(modus, "r+b", 3) == 0)
    {
        modeType = 10;
    }
    else if (strncmp(modus, "rb+", 3) == 0)
    {
        modeType = 10;
    }
    else if (strncmp(modus, "w+b", 3) == 0)
    {
        modeType = 11;
    }
    else if (strncmp(modus, "wb+", 3) == 0)
    {
        modeType = 11;
    }
    else if (strncmp(modus, "a+b", 3) == 0)
    {
        modeType = 12;
    }
    else if (strncmp(modus, "ab+", 3) == 0)
    {
        modeType = 12;
    }
    else if (strncmp(modus, "r+", 2) == 0)
    {
        modeType = 7;
    }
    else if (strncmp(modus, "w+", 2) == 0)
    {
        modeType = 8;
    }
    else if (strncmp(modus, "a+", 2) == 0)
    {
        modeType = 9;
    }
    else if (strncmp(modus, "rb", 2) == 0)
    {
        modeType = 4;
    }
    else if (strncmp(modus, "wb", 2) == 0)
    {
        modeType = 5;
    }
    else if (strncmp(modus, "ab", 2) == 0)
    {
        modeType = 6;
    }
    else if (strncmp(modus, "r", 1) == 0)
    {
        modeType = 1;
    }
    else if (strncmp(modus, "w", 1) == 0)
    {
        modeType = 2;
    }
    else if (strncmp(modus, "a", 1) == 0)
    {
        modeType = 3;
    }
    else
    {
        err = 1;
        return;
    }
    if ((modeType == 4)
        || (modeType == 5)
        || (modeType == 6)
        || (modeType == 10)
        || (modeType == 11)
        || (modeType == 12))
    {
        myfile->textMode = 0;
    }
    else
    {
        myfile->textMode = 1;
    }
    return;
}

static void osfopen(void)
{
#ifdef __OS2__
    APIRET rc;
    ULONG  action;
    ULONG  newsize = 0;
    ULONG  fileAttr = 0;
    ULONG  openAction = 0;
    ULONG  openMode = 0;

    if ((modeType == 1) || (modeType == 4) || (modeType == 7)
        || (modeType == 10))
    {
        openAction |= OPEN_ACTION_FAIL_IF_NEW;
        openAction |= OPEN_ACTION_OPEN_IF_EXISTS;
    }
    else if ((modeType == 2) || (modeType == 5) || (modeType == 8)
             || (modeType == 11))
    {
        openAction |= OPEN_ACTION_CREATE_IF_NEW;
        openAction |= OPEN_ACTION_REPLACE_IF_EXISTS;
    }
    else if ((modeType == 3) || (modeType == 6) || (modeType == 9)
             || (modeType == 12))
    {
        openAction |= OPEN_ACTION_CREATE_IF_NEW;
        openAction |= OPEN_ACTION_OPEN_IF_EXISTS;
    }
    openMode |= OPEN_SHARE_DENYWRITE;
    if ((modeType == 1) || (modeType == 4))
    {
        openMode |= OPEN_ACCESS_READONLY;
    }
    else if ((modeType == 2) || (modeType == 3) || (modeType == 5)
             || (modeType == 6))
    {
        openMode |= OPEN_ACCESS_WRITEONLY;
    }
    else
    {
        openMode |= OPEN_ACCESS_READWRITE;
    }
    if ((strlen(fnm) == 2)
        && (fnm[1] == ':')
        && (openMode == OPEN_ACCESS_READONLY))
    {
        openMode |= OPEN_FLAGS_DASD;
    }
    rc = DosOpen((PSZ)fnm,
                 &myfile->hfile,
                 &action,
                 newsize,
                 fileAttr,
                 openAction,
                 openMode,
                 NULL);
    if (rc != 0)
    {
        err = 1;
        errno = rc;
    }
#endif
#ifdef __WIN32__
    DWORD dwDesiredAccess = 0;
    DWORD dwShareMode = FILE_SHARE_READ;
    DWORD dwCreationDisposition = 0;
    DWORD dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL;

    if ((modeType == 1) || (modeType == 4) || (modeType == 7)
        || (modeType == 10))
    {
        dwCreationDisposition = OPEN_EXISTING;
    }
    else if ((modeType == 2) || (modeType == 5) || (modeType == 8)
             || (modeType == 11))
    {
        dwCreationDisposition = CREATE_ALWAYS;
    }
    else if ((modeType == 3) || (modeType == 6) || (modeType == 9)
             || (modeType == 12))
    {
        dwCreationDisposition = CREATE_ALWAYS;
    }
    if ((modeType == 1) || (modeType == 4))
    {
        dwDesiredAccess = GENERIC_READ;
    }
    else if ((modeType == 2) || (modeType == 3) || (modeType == 5)
             || (modeType == 6))
    {
        dwDesiredAccess = GENERIC_WRITE;
    }
    else
    {
        dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
    }
    myfile->hfile = CreateFile(fnm,
                               dwDesiredAccess,
                               dwShareMode,
                               NULL,
                               dwCreationDisposition,
                               dwFlagsAndAttributes,
                               NULL);
    if (myfile->hfile == INVALID_HANDLE_VALUE)
    {
        err = 1;
        errno = GetLastError();
    }
#endif
#ifdef __MSDOS__
    int mode;
    int errind;

    if ((modeType == 1) || (modeType == 4))
    {
        mode = 0; /* read */
    }
    else if ((modeType == 2) || (modeType == 5))
    {
        mode = 1; /* write */
    }
    else
    {
        mode = 2; /* update or otherwise unsupported */
        /* because we don't have the ability to update files
           at the moment on MSDOS, just return with an
           error immediately */
        err = 1;
        errno = 2;
        return;
    }
    myfile->hfile = __open(fnm, mode, &errind);
    if (errind)
    {
        err = 1;
        errno = myfile->hfile;
    }
#endif
#if defined(__MVS__) || defined(__CMS__)
    int mode;
    char *p;
    int len;
    char newfnm[FILENAME_MAX];
    char tmpdd[9];

    if ((modeType == 1) || (modeType == 4))
    {
        mode = 0;
    }
    else if ((modeType == 2) || (modeType == 5))
    {
        mode = 1;
    }
    else
    {
        mode = 2;
        
        /* because we don't have the ability to update files
           at the moment on MVS or CMS, just return with an
           error immediately */
        err = 1;
        errno = 2;
        return;
    }

    if (!inseek)
    {
        myfile->dynal = 0;
    }
/* dw */
/* This code needs changing for VM */
    p = strchr(fnm, ':');
    if ((p != NULL)
        && ((strncmp(fnm, "dd", 2) == 0)
            || (strncmp(fnm, "DD", 2) == 0)))
    {
        p++;
    }
    else
/* if we are in here then there is no "dd:" on front of file */
/* if its CMS generate a ddname and issue a filedef for the file */
#if defined(__CMS__)
    {
/* create a DD from the handle number */
        strcpy(newfnm, fnm);
        p = newfnm;
        while (*p != '\0')
        {
            *p = toupper((unsigned char)*p);
            p++;
        }
        sprintf(tmpdd, "PDP%03dHD", spareSpot);
        filedef(tmpdd, newfnm, mode);
        myfile->dynal = 1;
        p = tmpdd;
    }
#elif defined(__MVS__)
    {
        char rawf[FILENAME_MAX]; /* file name without member,
                                    suitable for dynamic allocation */

        sprintf(newfnm, "PDP%03dHD", spareSpot);
        strcpy(tmpdd, newfnm);
        
        /* strip any single quote */
        if (fnm[0] == '\'')
        {
            fnm++;
        }
        strcpy(rawf, fnm);
        
        /* If we have a file such as "'FRED.C(MARY)'" we need to
           convert this into PDP001HD(MARY) and do a dynamic
           allocation of PDP001HD to "FRED.C". We don't yet have
           the ability to add a prefix. So this involves
           extracting the member name and then eliminating the member
           name and any single quotes */
        p = strchr(rawf, '(');
        if (p != NULL)
        {
            *p = '\0';
            p++;
            strcat(newfnm, "(");
            strcat(newfnm, p);
            
            p = strchr(newfnm, ')');
            if (p != NULL)
            {
                *(p + 1) = '\0';
            }
        }
        else
        {
            /* strip any single quote */
            p = strchr(rawf, '\'');
            if (p != NULL)
            {
                *p = '\0';
            }
        }

        /* MVS probably expects uppercase filenames */
        p = rawf;
        while (*p != '\0')
        {
            *p = toupper((unsigned char)*p);
            p++;
        }
        
        /* dynamically allocate file */
        errno = __dynal(strlen(tmpdd), tmpdd, strlen(rawf), rawf);
        if (errno != 0)
        {
            err = 1;
            return;
        }
        myfile->dynal = 1;
        
        p = newfnm;
    }
#else
    {
        p = (char *)fnm;
    }
#endif
    strcpy(myfile->ddname, "        ");
    len = strcspn(p, "(");
    if (len > 8)
    {
        len = 8;
    }
    memcpy(myfile->ddname, p, len);
    p = myfile->ddname;
    while (*p != '\0')
    {
        *p = toupper((unsigned char)*p);
        p++;
    }

    p = strchr(fnm, '(');
    if (p != NULL)
    {
        p++;
        strcpy(myfile->pdsmem, "        ");
        len = strcspn(p, ")");
        if (len > 8)
        {
            len = 8;
        }
        memcpy(myfile->pdsmem, p, len);
        p = myfile->pdsmem;
        while (*p != '\0')
        {
            *p = toupper((unsigned char)*p);
            p++;
        }
        p = myfile->pdsmem;
    }
    myfile->reallyu = 0;
    myfile->reallyt = 0;
    myfile->hfile =
        __aopen(myfile->ddname, mode, &myfile->recfm, &myfile->lrecl, p);
#if !LOCMODE
    /* in move mode, the returned handle is actually a control
       block, and we need to switch, so that we can get the
       assembler buffer */
    if (((int)myfile->hfile > 0) && (mode == 1))
    {
        myfile->oldhfile = myfile->hfile;
        myfile->asmbuf = *(((void **)myfile->hfile) + 1);
        myfile->hfile = *((void **)myfile->hfile);
    }
#endif


    /* errors from MVS __aopen are negative numbers */
    if ((int)myfile->hfile <= 0)
    {
        err = 1;
        errno = -(int)myfile->hfile;
        return;
    }
    if (myfile->recfm == 2)
    {
        myfile->reallyu = 1;
        myfile->quickBin = 0; /* switch off to be on the safe side */

        /* if open for writing, kludge to switch to fixed */
        if (mode == 1)
        {
            myfile->recfm = 0;
        }
        /* if open for reading, kludge to switch to variable */
        else if (mode == 0)
        {
            myfile->recfm = 1;
            myfile->lrecl -= 4; /* lrecl from assembler includes BDW */
        }
    }

    if ((modeType == 4) || (modeType == 5))
    {
        myfile->style = 0; /* binary */
    }
    else
    {
        myfile->style = 2; /* text */
        /* for RECFM=U we use binary mode when reading or writing
           text files as we don't want any translation done. But
           record the fact that it was really text mode */
        if (myfile->reallyu)
        {
            myfile->reallyt = 1;
            myfile->style = 0;
        }
    }
    myfile->style += myfile->recfm;

    if (myfile->style == VARIABLE_TEXT)
    {
        myfile->quickText = 1;
    }
    else
    {
        myfile->quickText = 0;
    }
    if (myfile->style == FIXED_BINARY)
    {
        myfile->quickBin = 1;
    }
    else
    {
        myfile->quickBin = 0;
    }
#endif
    return;
}

int fclose(FILE *stream)
{
#ifdef __OS2__
    APIRET rc;
#endif
#ifdef __WIN32__
    BOOL rc;
#endif

    if (!stream->isopen)
    {
        return (EOF);
    }
    fflush(stream);
#ifdef __OS2__
    rc = DosClose(stream->hfile);
#endif
#ifdef __WIN32__
    rc = CloseHandle(stream->hfile);
#endif
#ifdef __MSDOS__
    __close(stream->hfile);
#endif
#if defined(__MVS__) || defined(__CMS__)
    if ((stream->mode == __WRITE_MODE) && (stream->upto != stream->fbuf))
    {
        if (stream->reallyu)
        {
            size_t last;
            
            last = stream->upto - stream->fbuf;
            begwrite(stream, last);
            memcpy(dptr, stream->fbuf, last);
            finwrite(stream);
        }
        else if (stream->textMode)
        {
            putc('\n', stream);
        }
        else
        {
            size_t remain;
            size_t x;

            remain = stream->endbuf - stream->upto;
            for (x = 0; x < remain; x++)
            {
                putc(0x00, stream);
            }
        }
    }
    __aclose(stream->hfile);
#ifdef __CMS__
    if (stream->dynal && !inseek)
    {
        fdclr(stream->ddname);
    }
#endif
#endif
    if (!stream->theirBuffer)
    {
#if !defined(__MVS__) && !defined(__CMS__) && !defined(__VSE__)
        /* on the PC, permanent files have a static buffer */
        if (!stream->permfile)
#endif
        free(stream->intBuffer);
    }
    if (!stream->permfile && !inreopen)
    {
        __userFiles[stream->intFno] = NULL;
        free(stream);
    }
    else
    {
#if defined(__MVS__) || defined(__CMS__)
        /* if we're not in the middle of freopen ... */
        __userFiles[stream->intFno] = NULL;
        if (!inreopen)
        {
            free(stream);
            /* need to protect against the app closing the file
               which it is allowed to */
            if (stream == stdin)
            {
                stdin = NULL;
            }
            else if (stream == stdout)
            {
                stdout = NULL;
            }
            else if (stream == stderr)
            {
                stderr = NULL;
            }
        }
#else
        stream->isopen = 0;
#endif
    }
#ifdef __OS2__
    if (rc != 0)
    {
        errno = rc;
        return (EOF);
    }
#endif
#ifdef __WIN32__
    if (!rc)
    {
        errno = GetLastError();
        return (EOF);
    }
#endif
    return (0);
}

#if !defined(__MVS__) && !defined(__CMS__)
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t toread;
    size_t elemRead;
    size_t actualRead;
#ifdef __OS2__
    APIRET rc;
    ULONG tempRead;
#endif
#ifdef __WIN32__
    BOOL rc;
    DWORD tempRead;
#endif
#ifdef __MSDOS__
    int errind;
    size_t tempRead;
#endif

    if (nmemb == 1)
    {
        toread = size;
    }
    else if (size == 1)
    {
        toread = nmemb;
    }
    else
    {
        toread = size * nmemb;
    }
    if (toread < stream->szfbuf)
    {
        stream->quickBin = 0;
    }
    if (stream->ungetCh != -1)
    {
        *--stream->upto = (char)stream->ungetCh;
        stream->ungetCh = -1;
    }
    if (!stream->quickBin)
    {
        if (stream->textMode)
        {
            freadSlowT(ptr, stream, toread, &actualRead);
        }
        else
        {
            if (toread <= (stream->endbuf - stream->upto))
            {
                memcpy(ptr, stream->upto, toread);
                actualRead = toread;
                stream->upto += toread;
            }
            else
            {
                freadSlowB(ptr, stream, toread, &actualRead);
            }
        }
        if (nmemb == 1)
        {
            if (actualRead == size)
            {
                elemRead = 1;
            }
            else
            {
                elemRead = 0;
            }
        }
        else if (size == 1)
        {
            elemRead = actualRead;
        }
        else
        {
            if (size == 0)
            {
                elemRead = 0;
            }
            else
            {
                elemRead = actualRead / size;
            }
        }
        return (elemRead);
    }
    else
    {
#ifdef __OS2__
        rc = DosRead(stream->hfile, ptr, toread, &tempRead);
        if (rc != 0)
        {
            actualRead = 0;
            stream->errorInd = 1;
            errno = rc;
        }
        else
        {
            actualRead = tempRead;
        }
#endif
#ifdef __WIN32__
        rc = ReadFile(stream->hfile,
                      ptr,
                      toread,
                      &tempRead,
                      NULL);
        if (!rc)
        {
            actualRead = 0;
            stream->errorInd = 1;
            errno = GetLastError();
        }
        else
        {
            actualRead = tempRead;
        }
#endif
#ifdef __MSDOS__
        tempRead = __read(stream->hfile, ptr, toread, &errind);
        if (errind)
        {
            errno = tempRead;
            actualRead = 0;
            stream->errorInd = 1;
        }
        else
        {
            actualRead = tempRead;
        }
#endif
        if (nmemb == 1)
        {
            if (actualRead == size)
            {
                elemRead = 1;
            }
            else
            {
                elemRead = 0;
                stream->eofInd = 1;
            }
        }
        else if (size == 1)
        {
            elemRead = actualRead;
            if (nmemb != actualRead)
            {
                stream->eofInd = 1;
            }
        }
        else
        {
            if (size == 0)
            {
                elemRead = 0;
            }
            else
            {
                elemRead = actualRead / size;
            }
            if (toread != actualRead)
            {
                stream->eofInd = 1;
            }
        }
        stream->bufStartR += actualRead;
        return (elemRead);
    }
}


/*
while toread has not been satisfied
{
    scan stuff out of buffer, replenishing buffer as required
}
*/

static void freadSlowT(void *ptr,
                       FILE *stream,
                       size_t toread,
                       size_t *actualRead)
{
    int finReading = 0;
    size_t avail;
    size_t need;
    char *p;
    size_t got;
#ifdef __OS2__
    ULONG tempRead;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD tempRead;
    BOOL rc;
#endif
#ifdef __MSDOS__
    size_t tempRead;
    int errind;
#endif

    *actualRead = 0;
    while (!finReading)
    {
        if (stream->upto == stream->endbuf)
        {
#ifdef __OS2__
            rc = DosRead(stream->hfile,
                         stream->fbuf,
                         stream->szfbuf,
                         &tempRead);
            if (rc != 0)
            {
                tempRead = 0;
                stream->errorInd = 1;
                errno = rc;
            }
#endif
#ifdef __WIN32__
            rc = ReadFile(stream->hfile,
                          stream->fbuf,
                          stream->szfbuf,
                          &tempRead,
                          NULL);
            if (!rc)
            {
                tempRead = 0;
                stream->errorInd = 1;
                errno = GetLastError();
            }
#endif
#ifdef __MSDOS__
            tempRead = __read(stream->hfile,
                              stream->fbuf,
                              stream->szfbuf,
                              &errind);
            if (errind)
            {
                errno = tempRead;
                tempRead = 0;
                stream->errorInd = 1;
            }
#endif
            if (tempRead == 0)
            {
                stream->eofInd = 1;
                break;
            }
            stream->bufStartR += (stream->upto - stream->fbuf);
            stream->endbuf = stream->fbuf + tempRead;
            *stream->endbuf = '\n';
            stream->upto = stream->fbuf;
        }
        avail = (size_t)(stream->endbuf - stream->upto) + 1;
        need = toread - *actualRead;
        p = memchr(stream->upto, '\n', avail);
        got = (size_t)(p - stream->upto);
        if (need < got)
        {
            memcpy((char *)ptr + *actualRead, stream->upto, need);
            stream->upto += need;
            *actualRead += need;
        }
        else
        {
            memcpy((char *)ptr + *actualRead, stream->upto, got);
            stream->upto += got;
            *actualRead += got;
            if (p != stream->endbuf)
            {
                if (*(stream->upto - 1) == '\r')
                {
                    *((char *)ptr + *actualRead - 1) = '\n';
                    stream->upto++;
                }
                else if (need != got)
                {
                    *((char *)ptr + *actualRead) = '\n';
                    *actualRead += 1;
                    stream->upto++;
                }
            }
            else
            {
                if (*(stream->upto - 1) == '\r')
                {
                    *actualRead -= 1;
                }
            }
        }
        if (*actualRead == toread)
        {
            finReading = 1;
        }
    }
    return;
}

static void freadSlowB(void *ptr,
                       FILE *stream,
                       size_t toread,
                       size_t *actualRead)
{
    size_t avail;
#ifdef __OS2__
    ULONG tempRead;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD tempRead;
    BOOL rc;
#endif
#ifdef __MSDOS__
    size_t tempRead;
    int errind;
#endif

    avail = (size_t)(stream->endbuf - stream->upto);
    memcpy(ptr, stream->upto, avail);
    *actualRead = avail;
    stream->bufStartR += (stream->endbuf - stream->fbuf);
    if (toread >= stream->szfbuf)
    {
        stream->upto = stream->endbuf;
        stream->quickBin = 1;
#ifdef __OS2__
        rc = DosRead(stream->hfile,
                     (char *)ptr + *actualRead,
                     toread - *actualRead,
                     &tempRead);
        if (rc != 0)
        {
            tempRead = 0;
            stream->errorInd = 1;
            errno = rc;
        }
#endif
#ifdef __WIN32__
            rc = ReadFile(stream->hfile,
                          (char *)ptr + *actualRead,
                          toread - *actualRead,
                          &tempRead,
                          NULL);
            if (!rc)
            {
                tempRead = 0;
                stream->errorInd = 1;
                errno = GetLastError();
            }
#endif
#ifdef __MSDOS__
        tempRead = __read(stream->hfile,
                          (char *)ptr + *actualRead,
                          toread - *actualRead,
                          &errind);
        if (errind)
        {
            errno = tempRead;
            tempRead = 0;
            stream->errorInd = 1;
        }
#endif
        else if (tempRead != (toread - *actualRead))
        {
            stream->eofInd = 1;
        }
        *actualRead += tempRead;
        stream->bufStartR += tempRead;
    }
    else
    {
        size_t left;

        stream->upto = stream->fbuf;
#ifdef __OS2__
        rc = DosRead(stream->hfile,
                     stream->fbuf,
                     stream->szfbuf,
                     &tempRead);
        left = toread - *actualRead;
        if (rc != 0)
        {
            tempRead = 0;
            stream->errorInd = 1;
            errno = rc;
        }
#endif
#ifdef __WIN32__
        rc = ReadFile(stream->hfile,
                      stream->fbuf,
                      stream->szfbuf,
                      &tempRead,
                      NULL);
        left = toread - *actualRead;
        if (!rc)
        {
            tempRead = 0;
            stream->errorInd = 1;
            errno = GetLastError();
        }
#endif
#ifdef __MSDOS__
        tempRead = __read(stream->hfile,
                          stream->fbuf,
                          stream->szfbuf,
                          &errind);
        left = toread - *actualRead;
        if (errind)
        {
            errno = tempRead;
            tempRead = 0;
            stream->errorInd = 1;
        }
#endif
        else if (tempRead < left)
        {
            stream->eofInd = 1;
        }
        stream->endbuf = stream->fbuf + tempRead;
        *stream->endbuf = '\n';
        avail = (size_t)(stream->endbuf - stream->upto);
        if (avail > left)
        {
            avail = left;
        }
        memcpy((char *)ptr + *actualRead,
               stream->upto,
               avail);
        stream->upto += avail;
        *actualRead += avail;
    }
    return;
}
#endif

#if !defined(__MVS__) && !defined(__CMS__)
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t towrite;
    size_t elemWritten;
#ifdef __OS2__
    ULONG actualWritten;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD actualWritten;
    BOOL rc;
#endif
#ifdef __MSDOS__
    size_t actualWritten;
    int errind;
#endif

    if (nmemb == 1)
    {
        towrite = size;
    }
    else if (size == 1)
    {
        towrite = nmemb;
    }
    else
    {
        towrite = size * nmemb;
    }
    if (towrite < stream->szfbuf)
    {
        stream->quickBin = 0;
        if ((stream->bufTech == _IONBF) && !stream->textMode)
        {
            stream->quickBin = 1;
        }
    }
    if (!stream->quickBin)
    {
        fwriteSlow(ptr, size, nmemb, stream, towrite, &elemWritten);
        return (elemWritten);
    }
    else
    {
#ifdef __OS2__
        rc = DosWrite(stream->hfile, (VOID *)ptr, towrite, &actualWritten);
        if (rc != 0)
        {
            stream->errorInd = 1;
            actualWritten = 0;
            errno = rc;
        }
#endif
#ifdef __WIN32__
        rc = WriteFile(stream->hfile, ptr, towrite, &actualWritten, NULL);
        if (!rc)
        {
            stream->errorInd = 1;
            actualWritten = 0;
            errno = GetLastError();
        }
#endif
#ifdef __MSDOS__
        actualWritten = __write(stream->hfile,
                                ptr,
                                towrite,
                                &errind);
        if (errind)
        {
            stream->errorInd = 1;
            actualWritten = 0;
            errno = actualWritten;
        }
#endif
        if (nmemb == 1)
        {
            if (actualWritten == size)
            {
                elemWritten = 1;
            }
            else
            {
                elemWritten = 0;
            }
        }
        else if (size == 1)
        {
            elemWritten = actualWritten;
        }
        else
        {
            elemWritten = actualWritten / size;
        }
        stream->bufStartR += actualWritten;
        return (elemWritten);
    }
}

static void fwriteSlow(const void *ptr,
                       size_t size,
                       size_t nmemb,
                       FILE *stream,
                       size_t towrite,
                       size_t *elemWritten)
{
    size_t actualWritten;

    /* Normally, on output, there will never be a situation where
       the write buffer is full, but it hasn't been written out.
       If we find this to be the case, then it is because we have
       done an fseek, and didn't know whether we were going to do
       a read or a write after it, so now that we know, we switch
       the buffer to being set up for write.  We could use a flag,
       but I thought it would be better to just put some magic
       code in with a comment */
    if (stream->upto == stream->endbuf)
    {
        stream->bufStartR += (stream->endbuf - stream->fbuf);
        stream->upto = stream->fbuf;
        stream->mode = __WRITE_MODE;
    }
    if ((stream->textMode) || (stream->bufTech == _IOLBF))
    {
        fwriteSlowT(ptr, stream, towrite, &actualWritten);
    }
    else
    {
        fwriteSlowB(ptr, stream, towrite, &actualWritten);
    }
    if (nmemb == 1)
    {
        if (actualWritten == size)
        {
            *elemWritten = 1;
        }
        else
        {
            *elemWritten = 0;
        }
    }
    else if (size == 1)
    {
        *elemWritten = actualWritten;
    }
    else
    {
        *elemWritten = actualWritten / size;
    }
    return;
}


/* can still be called on binary files, if the binary file is
   line buffered  */

static void fwriteSlowT(const void *ptr,
                        FILE *stream,
                        size_t towrite,
                        size_t *actualWritten)
{
    char *p;
    char *tptr;
    char *oldp;
    size_t diffp;
    size_t rem;
    int fin;
#ifdef __OS2__
    ULONG tempWritten;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD tempWritten;
    BOOL rc;
#endif
#ifdef __MSDOS__
    size_t tempWritten;
    int errind;
#endif

    *actualWritten = 0;
    tptr = (char *)ptr;
    p = tptr;
    oldp = p;
    p = (char *)memchr(oldp, '\n', towrite - (size_t)(oldp - tptr));
    while (p != NULL)
    {
        diffp = (size_t)(p - oldp);
        fin = 0;
        while (!fin)
        {
            rem = (size_t)(stream->endbuf - stream->upto);
            if (diffp < rem)
            {
                memcpy(stream->upto, oldp, diffp);
                stream->upto += diffp;
                *actualWritten += diffp;
                fin = 1;
            }
            else
            {
                memcpy(stream->upto, oldp, rem);
                oldp += rem;
                diffp -= rem;
#ifdef __OS2__
                rc = DosWrite(stream->hfile,
                              stream->fbuf,
                              stream->szfbuf,
                              &tempWritten);
                if (rc != 0)
                {
                    stream->errorInd = 1;
                    errno = rc;
                    return;
                }
#endif
#ifdef __WIN32__
                rc = WriteFile(stream->hfile,
                               stream->fbuf,
                               stream->szfbuf,
                               &tempWritten,
                               NULL);
                if (!rc)
                {
                    stream->errorInd = 1;
                    errno = GetLastError();
                    return;
                }
#endif
#ifdef __MSDOS__
                tempWritten = __write(stream->hfile,
                                      stream->fbuf,
                                      stream->szfbuf,
                                      &errind);
                if (errind)
                {
                    stream->errorInd = 1;
                    return;
                }
#endif
                else
                {
                    *actualWritten += rem;
                    stream->upto = stream->fbuf;
                    stream->bufStartR += tempWritten;
                }
            }
        }
        rem = (size_t)(stream->endbuf - stream->upto);
        if (rem < 3)
        {
#ifdef __OS2__
            rc = DosWrite(stream->hfile,
                          stream->fbuf,
                          (size_t)(stream->upto - stream->fbuf),
                          &tempWritten);
            if (rc != 0)
            {
                stream->errorInd = 1;
                errno = rc;
                return;
            }
#endif
#ifdef __WIN32__
            rc = WriteFile(stream->hfile,
                           stream->fbuf,
                           (size_t)(stream->upto - stream->fbuf),
                           &tempWritten,
                           NULL);
            if (!rc)
            {
                stream->errorInd = 1;
                errno = GetLastError();
                return;
            }
#endif
#ifdef __MSDOS__
            tempWritten = __write(stream->hfile,
                                  stream->fbuf,
                                  (size_t)(stream->upto - stream->fbuf),
                                  &errind);
            if (errind)
            {
                stream->errorInd = 1;
                errno = tempWritten;
                return;
            }
#endif
            stream->upto = stream->fbuf;
            stream->bufStartR += tempWritten;
        }
#ifndef __gnu_linux__
        if (stream->textMode)
        {
            memcpy(stream->upto, "\r\n", 2);
            stream->upto += 2;
        }
        else
#endif
        {
            memcpy(stream->upto, "\n", 1);
            stream->upto += 1;
        }
        *actualWritten += 1;
        oldp = p + 1;
        p = (char *)memchr(oldp, '\n', towrite - (size_t)(oldp - tptr));
    }

    if ((stream->bufTech == _IOLBF)
        && (stream->upto != stream->fbuf)
        && (oldp != tptr))
    {
#ifdef __OS2__
        rc = DosWrite(stream->hfile,
                      stream->fbuf,
                      (size_t)(stream->upto - stream->fbuf),
                      &tempWritten);
        if (rc != 0)
        {
            stream->errorInd = 1;
            errno = rc;
            return;
        }
#endif
#ifdef __WIN32__
        rc = WriteFile(stream->hfile,
                       stream->fbuf,
                       (size_t)(stream->upto - stream->fbuf),
                       &tempWritten,
                       NULL);
        if (!rc)
        {
            stream->errorInd = 1;
            errno = GetLastError();
            return;
        }
#endif
#ifdef __MSDOS__
        tempWritten = __write(stream->hfile,
                              stream->fbuf,
                              (size_t)(stream->upto - stream->fbuf),
                              &errind);
        if (errind)
        {
            stream->errorInd = 1;
            errno = tempWritten;
            return;
        }
#endif
        stream->upto = stream->fbuf;
        stream->bufStartR += tempWritten;
    }

    diffp = towrite - *actualWritten;
    while (diffp != 0)
    {
        rem = (size_t)(stream->endbuf - stream->upto);
        if (diffp < rem)
        {
            memcpy(stream->upto, oldp, diffp);
            stream->upto += diffp;
            *actualWritten += diffp;
        }
        else
        {
            memcpy(stream->upto, oldp, rem);
#ifdef __OS2__
            rc = DosWrite(stream->hfile,
                          stream->fbuf,
                          stream->szfbuf,
                          &tempWritten);
            if (rc != 0)
            {
                stream->errorInd = 1;
                errno = rc;
                return;
            }
#endif
#ifdef __WIN32__
            rc = WriteFile(stream->hfile,
                           stream->fbuf,
                           stream->szfbuf,
                           &tempWritten,
                           NULL);
            if (!rc)
            {
                stream->errorInd = 1;
                errno = GetLastError();
                return;
            }
#endif
#ifdef __MSDOS__
            tempWritten = __write(stream->hfile,
                                  stream->fbuf,
                                  stream->szfbuf,
                                  &errind);
            if (errind)
            {
                stream->errorInd = 1;
                errno = tempWritten;
                return;
            }
#endif
            else
            {
                *actualWritten += rem;
                stream->upto = stream->fbuf;
            }
            stream->bufStartR += tempWritten;
            oldp += rem;
        }
        diffp = towrite - *actualWritten;
    }
    if ((stream->bufTech == _IONBF)
        && (stream->upto != stream->fbuf))
    {
#ifdef __OS2__
        rc = DosWrite(stream->hfile,
                      stream->fbuf,
                      (size_t)(stream->upto - stream->fbuf),
                      &tempWritten);
        if (rc != 0)
        {
            stream->errorInd = 1;
            errno = rc;
            return;
        }
#endif
#ifdef __WIN32__
        rc = WriteFile(stream->hfile,
                       stream->fbuf,
                       (size_t)(stream->upto - stream->fbuf),
                       &tempWritten,
                       NULL);
        if (!rc)
        {
            stream->errorInd = 1;
            errno = GetLastError();
            return;
        }
#endif
#ifdef __MSDOS__
        tempWritten = __write(stream->hfile,
                              stream->fbuf,
                              (size_t)(stream->upto - stream->fbuf),
                              &errind);
        if (errind)
        {
            stream->errorInd = 1;
            errno = tempWritten;
            return;
        }
#endif
        stream->upto = stream->fbuf;
        stream->bufStartR += tempWritten;
    }
    return;
}

/* whilst write requests are smaller than a buffer, we do not turn
   on quickbin */

static void fwriteSlowB(const void *ptr,
                        FILE *stream,
                        size_t towrite,
                        size_t *actualWritten)
{
    size_t spare;
#ifdef __OS2__
    ULONG tempWritten;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD tempWritten;
    BOOL rc;
#endif
#ifdef __MSDOS__
    size_t tempWritten;
    int errind;
#endif

    spare = (size_t)(stream->endbuf - stream->upto);
    if (towrite < spare)
    {
        memcpy(stream->upto, ptr, towrite);
        *actualWritten = towrite;
        stream->upto += towrite;
        return;
    }
    memcpy(stream->upto, ptr, spare);
#ifdef __OS2__
    rc = DosWrite(stream->hfile,
                  stream->fbuf,
                  stream->szfbuf,
                  &tempWritten);
    if (rc != 0)
    {
        stream->errorInd = 1;
        errno = rc;
        return;
    }
#endif
#ifdef __WIN32__
    rc = WriteFile(stream->hfile,
                   stream->fbuf,
                   stream->szfbuf,
                   &tempWritten,
                   NULL);
    if (!rc)
    {
        stream->errorInd = 1;
        errno = GetLastError();
        return;
    }
#endif
#ifdef __MSDOS__
    tempWritten = __write(stream->hfile,
                          stream->fbuf,
                          stream->szfbuf,
                          &errind);
    if (errind)
    {
        stream->errorInd = 1;
        errno = tempWritten;
        return;
    }
#endif
    *actualWritten = spare;
    stream->upto = stream->fbuf;
    stream->bufStartR += tempWritten;
    if (towrite > stream->szfbuf)
    {
        stream->quickBin = 1;
#ifdef __OS2__
        rc = DosWrite(stream->hfile,
                      (char *)ptr + *actualWritten,
                      towrite - *actualWritten,
                      &tempWritten);
        if (rc != 0)
        {
            stream->errorInd = 1;
            errno = rc;
            return;
        }
#endif
#ifdef __WIN32__
        rc = WriteFile(stream->hfile,
                       (char *)ptr + *actualWritten,
                       towrite - *actualWritten,
                       &tempWritten,
                       NULL);
        if (!rc)
        {
            stream->errorInd = 1;
            errno = GetLastError();
            return;
        }
#endif
#ifdef __MSDOS__
        tempWritten = __write(stream->hfile,
                              (char *)ptr + *actualWritten,
                              towrite - *actualWritten,
                              &errind);
        if (errind)
        {
            stream->errorInd = 1;
            errno = tempWritten;
            return;
        }
#endif
        *actualWritten += tempWritten;
        stream->bufStartR += tempWritten;
    }
    else
    {
        memcpy(stream->fbuf,
               (char *)ptr + *actualWritten,
               towrite - *actualWritten);
        stream->upto += (towrite - *actualWritten);
        *actualWritten = towrite;
    }
    stream->bufStartR += *actualWritten;
    return;
}
#endif

static int vvprintf(const char *format, va_list arg, FILE *fq, char *s)
{
    int fin = 0;
    int vint;
    double vdbl;
    unsigned int uvint;
    const char *vcptr;
    int chcount = 0;
    size_t len;
    char numbuf[50];
    char *nptr;
    int *viptr;

    while (!fin)
    {
        if (*format == '\0')
        {
            fin = 1;
        }
        else if (*format == '%')
        {
            format++;
            if (*format == 'd')
            {
                vint = va_arg(arg, int);
                if (vint < 0)
                {
                    uvint = -vint;
                }
                else
                {
                    uvint = vint;
                }
                nptr = numbuf;
                do
                {
                    *nptr++ = (char)('0' + uvint % 10);
                    uvint /= 10;
                } while (uvint > 0);
                if (vint < 0)
                {
                    *nptr++ = '-';
                }
                do
                {
                    nptr--;
                    outch(*nptr);
                    chcount++;
                } while (nptr != numbuf);
            }
            else if (strchr("eEgGfF", *format) != NULL && *format != 0)
            {
                vdbl = va_arg(arg, double);
                dblcvt(vdbl, *format, 0, 6, numbuf);   /* 'e','f' etc. */
                len = strlen(numbuf);
                if (fq == NULL)
                {
                    memcpy(s, numbuf, len);
                    s += len;
                }
                else
                {
                    fputs(numbuf, fq);
                }
                chcount += len;
            }
            else if (*format == 's')
            {
                vcptr = va_arg(arg, const char *);
                if (vcptr == NULL)
                {
                    vcptr = "(null)";
                }
                if (fq == NULL)
                {
                    len = strlen(vcptr);
                    memcpy(s, vcptr, len);
                    s += len;
                    chcount += len;
                }
                else
                {
                    fputs(vcptr, fq);
                    chcount += strlen(vcptr);
                }
            }
            else if (*format == 'c')
            {
                vint = va_arg(arg, int);
                outch(vint);
                chcount++;
            }
            else if (*format == 'n')
            {
                viptr = va_arg(arg, int *);
                *viptr = chcount;
            }
            else if (*format == '%')
            {
                outch('%');
                chcount++;
            }
            else
            {
                int extraCh;

                extraCh = examine(&format, fq, s, &arg, chcount);
                chcount += extraCh;
                if (s != NULL)
                {
                    s += extraCh;
                }
            }
        }
        else
        {
            outch(*format);
            chcount++;
        }
        format++;
    }
    return (chcount);
}

static int examine(const char **formt, FILE *fq, char *s, va_list *arg,
                   int chcount)
{
    int extraCh = 0;
    int flagMinus = 0;
    int flagPlus = 0;
    int flagSpace = 0;
    int flagHash = 0;
    int flagZero = 0;
    int width = 0;
    int precision = -1;
    int half = 0;
    int lng = 0;
    int specifier = 0;
    int fin;
    long lvalue;
    short int hvalue;
    int ivalue;
    unsigned long ulvalue;
    double vdbl;
    char *svalue;
    char work[50];
    int x;
    int y;
    int rem;
    const char *format;
    int base;
    int fillCh;
    int neg;
    int length;
    size_t slen;

    unused(chcount);
    format = *formt;
    /* processing flags */
    fin = 0;
    while (!fin)
    {
        switch (*format)
        {
            case '-': flagMinus = 1;
                      break;
            case '+': flagPlus = 1;
                      break;
            case ' ': flagSpace = 1;
                      break;
            case '#': flagHash = 1;
                      break;
            case '0': flagZero = 1;
                      break;
            case '*': width = va_arg(*arg, int);
                      if (width < 0)
                      {
                          flagMinus = 1;
                          width = -width;
                      }
                      break;
            default:  fin = 1;
                      break;
        }
        if (!fin)
        {
            format++;
        }
        else
        {
            if (flagSpace && flagPlus)
            {
                flagSpace = 0;
            }
            if (flagMinus)
            {
                flagZero = 0;
            }
        }
    }

    /* processing width */
    if (isdigit((unsigned char)*format))
    {
        while (isdigit((unsigned char)*format))
        {
            width = width * 10 + (*format - '0');
            format++;
        }
    }

    /* processing precision */
    if (*format == '.')
    {
        format++;
        if (*format == '*')
        {
            precision = va_arg(*arg, int);
            format++;
        }
        else
        {
            precision = 0;
            while (isdigit((unsigned char)*format))
            {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }
    }

    /* processing h/l/L */
    if (*format == 'h')
    {
        /* all environments should promote shorts to ints,
           so we should be able to ignore the 'h' specifier.
           It will create problems otherwise. */
        /* half = 1; */
    }
    else if (*format == 'l')
    {
        lng = 1;
    }
    else if (*format == 'L')
    {
        lng = 1;
    }
    else
    {
        format--;
    }
    format++;

    /* processing specifier */
    specifier = *format;

    if (strchr("dxXuiop", specifier) != NULL && specifier != 0)
    {
        if (precision < 0)
        {
            precision = 1;
        }
#if defined(__MSDOS__) && !defined(__PDOS__) && !defined(__gnu_linux__)
        if (specifier == 'p')
        {
            lng = 1;
        }
#endif
        if (lng)
        {
            lvalue = va_arg(*arg, long);
        }
        else if (half)
        {
            hvalue = va_arg(*arg, short);
            if (specifier == 'u') lvalue = (unsigned short)hvalue;
            else lvalue = hvalue;
        }
        else
        {
            ivalue = va_arg(*arg, int);
            if (specifier == 'u') lvalue = (unsigned int)ivalue;
            else lvalue = ivalue;
        }
        ulvalue = (unsigned long)lvalue;
        if ((lvalue < 0) && ((specifier == 'd') || (specifier == 'i')))
        {
            neg = 1;
            ulvalue = -lvalue;
        }
        else
        {
            neg = 0;
        }
#if defined(__MSDOS__) && !defined(__PDOS__) && !defined(__gnu_linux__)
        if (!lng)
        {
            ulvalue &= 0xffff;
        }
#endif
        if ((specifier == 'X') || (specifier == 'x') || (specifier == 'p'))
        {
            base = 16;
        }
        else if (specifier == 'o')
        {
            base = 8;
        }
        else
        {
            base = 10;
        }
        if (specifier == 'p')
        {
#if defined(__MSDOS__) && !defined(__PDOS__) && !defined(__gnu_linux__)
            precision = 9;
#else
            precision = 8;
#endif
        }
        x = 0;
        while (ulvalue > 0)
        {
            rem = (int)(ulvalue % base);
            if (rem < 10)
            {
                work[x] = (char)('0' + rem);
            }
            else
            {
                if ((specifier == 'X') || (specifier == 'p'))
                {
                    work[x] = (char)('A' + (rem - 10));
                }
                else
                {
                    work[x] = (char)('a' + (rem - 10));
                }
            }
            x++;
#if defined(__MSDOS__) && !defined(__PDOS__) && !defined(__gnu_linux__)
            if ((x == 4) && (specifier == 'p'))
            {
                work[x] = ':';
                x++;
            }
#endif
            ulvalue = ulvalue / base;
        }
#if defined(__MSDOS__) && !defined(__PDOS__) && !defined(__gnu_linux__)
        if (specifier == 'p')
        {
            while (x < 5)
            {
                work[x] = (x == 4) ? ':' : '0';
                x++;
            }
        }
#endif
        while (x < precision)
        {
            work[x] = '0';
            x++;
        }
        if (neg)
        {
            work[x++] = '-';
        }
        else if (flagPlus)
        {
            work[x++] = '+';
        }
        if (flagZero)
        {
            fillCh = '0';
        }
        else
        {
            fillCh = ' ';
        }
        y = x;
        if (!flagMinus)
        {
            while (y < width)
            {
                outch(fillCh);
                extraCh++;
                y++;
            }
        }
        if (flagHash && (toupper((unsigned char)specifier) == 'X'))
        {
            outch('0');
            outch('x');
            extraCh += 2;
        }
        x--;
        while (x >= 0)
        {
            outch(work[x]);
            extraCh++;
            x--;
        }
        if (flagMinus)
        {
            while (y < width)
            {
                outch(fillCh);
                extraCh++;
                y++;
            }
        }
    }
    else if (strchr("eEgGfF", specifier) != NULL && specifier != 0)
    {
        if (precision < 0)
        {
            precision = 6;
        }
        vdbl = va_arg(*arg, double);
        dblcvt(vdbl, specifier, width, precision, work);   /* 'e','f' etc. */
        slen = strlen(work);
        if (fq == NULL)
        {
            memcpy(s, work, slen);
            s += slen;
        }
        else
        {
            fputs(work, fq);
        }
        extraCh += slen;
    }
    else if (specifier == 's')
    {
        if (precision < 0)
        {
            precision = 1;
        }
        svalue = va_arg(*arg, char *);
        fillCh = ' ';
        if (precision > 1)
        {
            char *p;

            p = memchr(svalue, '\0', precision);
            if (p != NULL)
            {
                length = (int)(p - svalue);
            }
            else
            {
                length = precision;
            }
        }
        else
        {
            length = strlen(svalue);
        }
        if (!flagMinus)
        {
            if (length < width)
            {
                extraCh += (width - length);
                for (x = 0; x < (width - length); x++)
                {
                    outch(fillCh);
                }
            }
        }
        for (x = 0; x < length; x++)
        {
            outch(svalue[x]);
        }
        extraCh += length;
        if (flagMinus)
        {
            if (length < width)
            {
                extraCh += (width - length);
                for (x = 0; x < (width - length); x++)
                {
                    outch(fillCh);
                }
            }
        }
    }
    *formt = format;
    return (extraCh);
}

int fputc(int c, FILE *stream)
{
    char buf[1];

#if !defined(__MVS__) && !defined(__CMS__)
    stream->quickBin = 0;
    if ((stream->upto < (stream->endbuf - 2))
        && (stream->bufTech != _IONBF))
    {
        if (stream->textMode)
        {
            if (c == '\n')
            {
                if (stream->bufTech == _IOFBF)
                {
                    *stream->upto++ = '\r';
                    *stream->upto++ = '\n';
                }
                else
                {
                    buf[0] = (char)c;
                    if (fwrite(buf, 1, 1, stream) != 1)
                    {
                        return (EOF);
                    }
                }
            }
            else
            {
                *stream->upto++ = (char)c;
            }
        }
        else
        {
            *stream->upto++ = (char)c;
        }
    }
    else
#endif
    {
        buf[0] = (char)c;
        if (fwrite(buf, 1, 1, stream) != 1)
        {
            return (EOF);
        }
    }
    return (c);
}

#if !defined(__MVS__) && !defined(__CMS__)
int fputs(const char *s, FILE *stream)
{
    size_t len;
    size_t ret;

    len = strlen(s);
    ret = fwrite(s, len, 1, stream);
    if (ret != 1) return (EOF);
    else return (0);
}
#endif

int remove(const char *filename)
{
    int ret;
#ifdef __OS2__
    APIRET rc;
#endif
#ifdef __WIN32__
    BOOL rc;
#endif

#ifdef __OS2__
    rc = DosDelete((PSZ)filename);
    if (rc != 0)
    {
        ret = 1;
        errno = rc;
    }
    else
    {
        ret = 0;
    }
#endif
#ifdef __WIN32__
    rc = DeleteFile(filename);
    if (!rc)
    {
        ret = 1;
        errno = GetLastError();
    }
    else
    {
        ret = 0;
    }
#endif
#ifdef __MSDOS__
    __remove(filename);
    ret = 0;
#endif
#ifdef __MVS__
    char buf[FILENAME_MAX + 50];
    char *p;
    
    sprintf(buf, " DELETE %s", filename);
    p = buf;
    while (*p != '\0')
    {
       *p = toupper((unsigned char)*p);
       p++;
    }
    ret = __idcams(strlen(buf), buf);
#endif
    return (ret);
}

int rename(const char *old, const char *new)
{
    int ret;
#ifdef __OS2__
    APIRET rc;
#endif
#ifdef __WIN32__
    BOOL rc;
#endif

#ifdef __OS2__
    rc = DosMove((PSZ)old, (PSZ)new);
    if (rc != 0)
    {
        ret = 1;
        errno = rc;
    }
    else
    {
        ret = 0;
    }
#endif
#ifdef __WIN32__
    rc = MoveFile(old, new);
    if (!rc)
    {
        ret = 1;
        errno = GetLastError();
    }
    else
    {
        ret = 0;
    }
#endif
#ifdef __MSDOS__
    __rename(old, new);
    ret = 0;
#endif
#ifdef __MVS__
    char buf[FILENAME_MAX + FILENAME_MAX + 50];
    char *p;
    
    sprintf(buf, " ALTER %s NEWNAME(%s)", old, new);
    p = buf;
    while (*p != '\0')
    {
       *p = toupper((unsigned char)*p);
       p++;
    }
    ret = __idcams(strlen(buf), buf);
#endif
    return (ret);
}

int sprintf(char *s, const char *format, ...)
{
    va_list arg;
    int ret;

    va_start(arg, format);
    ret = vsprintf(s, format, arg);
    va_end(arg);
    return (ret);
}

int vsprintf(char *s, const char *format, va_list arg)
{
    int ret;

    ret = vvprintf(format, arg, NULL, s);
    if (ret >= 0)
    {
        *(s + ret) = '\0';
    }
    return (ret);
}

/*

In fgets, we have the following possibilites...

1. we found a genuine '\n' that terminated the search.
2. we hit the '\n' at the endbuf.
3. we hit the '\n' sentinel.

*/
#if !defined(__MVS__) && !defined(__CMS__)
char *fgets(char *s, int n, FILE *stream)
{
    char *p;
    register char *t;
    register char *u = s;
    int c;
    int processed;
#ifdef __OS2__
    ULONG actualRead;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD actualRead;
    BOOL rc;
#endif
#ifdef __MSDOS__
    size_t actualRead;
    int errind;
#endif

    if (stream->quickText)
    {
        p = stream->upto + n - 1;
        t = stream->upto;
        if (p < stream->endbuf)
        {
            c = *p;
            *p = '\n';
#if defined(__OS2__) || defined(__WIN32__)
            if (n < 8)
            {
#endif
                while ((*u++ = *t++) != '\n') ; /* tight inner loop */
#if defined(__OS2__) || defined(__WIN32__)
            }
            else
            {
                register unsigned int *i1;
                register unsigned int *i2;
                register unsigned int z;

                i1 = (unsigned int *)t;
                i2 = (unsigned int *)u;
                while (1)
                {
                    z = *i1;
                    if ((z & 0xffU) == '\n') break;
                    z >>= 8;
                    if ((z & 0xffU) == '\n') break;
                    z >>= 8;
                    if ((z & 0xffU) == '\n') break;
                    z >>= 8;
                    if ((z & 0xffU) == '\n') break;
                    *i2++ = *i1++;
                }
                t = (char *)i1;
                u = (char *)i2;
                while ((*u++ = *t++) != '\n') ;
            }
#endif
            *p = (char)c;
            if (t <= p)
            {
                if (*(t - 2) == '\r') /* t is protected, u isn't */
                {
                    *(u - 2) = '\n';
                    *(u - 1) = '\0';
                }
                else
                {
                    *u = '\0';
                }
                stream->upto = t;
                return (s);
            }
            else
            {
                processed = (int)(t - stream->upto) - 1;
                stream->upto = t - 1;
                u--;
            }
        }
        else
        {
            while ((*u++ = *t++) != '\n') ; /* tight inner loop */
            if (t <= stream->endbuf)
            {
                if (*(t - 2) == '\r') /* t is protected, u isn't */
                {
                    *(u - 2) = '\n';
                    *(u - 1) = '\0';
                }
                else
                {
                    *u = '\0';
                }
                stream->upto = t;
                return (s);
            }
            else
            {
                processed = (int)(t - stream->upto) - 1;
                stream->upto = t - 1;
                u--;
            }
        }
    }
    else
    {
        processed = 0;
    }

    if (n < 1)
    {
        return (NULL);
    }
    if (n < 2)
    {
        *u = '\0';
        return (s);
    }
    if (stream->ungetCh != -1)
    {
        processed++;
        *u++ = (char)stream->ungetCh;
        stream->ungetCh = -1;
    }
    while (1)
    {
        t = stream->upto;
        p = stream->upto + (n - processed) - 1;
        if (p < stream->endbuf)
        {
            c = *p;
            *p = '\n';
        }
        if (stream->noNl)
        {
            while (((*u++ = *t) != '\n') && (*t++ != '\r')) ;
            if (*(u - 1) == '\n')
            {
                t++;
            }
            else
            {
                u--;
                while ((*u++ = *t++) != '\n') ;
            }
        }
        else
        {
            while ((*u++ = *t++) != '\n') ; /* tight inner loop */
        }
        if (p < stream->endbuf)
        {
            *p = (char)c;
        }
        if (((t <= p) && (p < stream->endbuf))
           || ((t <= stream->endbuf) && (p >= stream->endbuf)))
        {
            if (stream->textMode)
            {
                if (stream->noNl)
                {
                    if ((*(t - 1) == '\r') || (*(t - 1) == '\n'))
                    {
                        *(u - 1) = '\0';
                    }
                    else
                    {
                        *u = '\0';
                    }
                }
                else if (*(t - 2) == '\r') /* t is protected, u isn't */
                {
                    *(u - 2) = '\n';
                    *(u - 1) = '\0';
                }
                else
                {
                    *u = '\0';
                }
            }
            stream->upto = t;
            if (stream->textMode)
            {
                stream->quickText = 1;
            }
            return (s);
        }
        else if (((t > p) && (p < stream->endbuf))
                 || ((t > stream->endbuf) && (p >= stream->endbuf)))
        {
            int leave = 1;

            if (stream->textMode)
            {
                if (t > stream->endbuf)
                {
                    if ((t - stream->upto) > 1)
                    {
                        if (*(t - 2) == '\r') /* t is protected, u isn't */
                        {
                            processed -= 1; /* preparation for add */
                        }
                    }
                    leave = 0;
                }
                else
                {
                    if ((*(t - 2) == '\r') && (*(t - 1) == '\n'))
                    {
                        *(u - 2) = '\n';
                        *(u - 1) = '\0';
                    }
                    else
                    {
                        t--;
                        *(u - 1) = '\0';
                    }
                }
            }
            else if (t > stream->endbuf)
            {
                leave = 0;
            }
            else
            {
                *u = '\0';
            }
            if (leave)
            {
                stream->upto = t;
                if (stream->textMode)
                {
                    stream->quickText = 1;
                }
                return (s);
            }
        }
        processed += (int)(t - stream->upto) - 1;
        u--;
        stream->bufStartR += (stream->endbuf - stream->fbuf);
#ifdef __OS2__
        rc = DosRead(stream->hfile, stream->fbuf, stream->szfbuf, &actualRead);
        if (rc != 0)
        {
            actualRead = 0;
            stream->errorInd = 1;
            errno = rc;
        }
#endif
#ifdef __WIN32__
        rc = ReadFile(stream->hfile,
                      stream->fbuf,
                      stream->szfbuf,
                      &actualRead,
                      NULL);
        if (!rc)
        {
            actualRead = 0;
            stream->errorInd = 1;
            errno = GetLastError();
        }
#endif
#ifdef __MSDOS__
        actualRead = __read(stream->hfile,
                            stream->fbuf,
                            stream->szfbuf,
                            &errind);
        if (errind)
        {
            errno = actualRead;
            actualRead = 0;
            stream->errorInd = 1;
        }
#endif
        stream->endbuf = stream->fbuf + actualRead;
        *stream->endbuf = '\n';
        if (actualRead == 0)
        {
            *u = '\0';
            if ((u - s) <= 1)
            {
                stream->eofInd = 1;
                return (NULL);
            }
            else
            {
                return (s);
            }
        }
        stream->upto = stream->fbuf;
    }
}
#endif

int ungetc(int c, FILE *stream)
{
    if ((stream->ungetCh != -1) || (c == EOF))
    {
        return (EOF);
    }
    stream->ungetCh = (unsigned char)c;
    stream->quickText = 0;
    stream->quickBin = 0;
    return ((unsigned char)c);
}

int fgetc(FILE *stream)
{
    unsigned char x[1];
    size_t ret;

    ret = fread(x, 1, 1, stream);
    if (ret == 0)
    {
        return (EOF);
    }
    return ((int)x[0]);
}

int fseek(FILE *stream, long int offset, int whence)
{
    long oldpos;
    long newpos;
#ifdef __OS2__
    ULONG retpos;
    APIRET rc;
#endif
#ifdef __WIN32__
    DWORD retpos;
#endif

    oldpos = stream->bufStartR + (stream->upto - stream->fbuf);
    if (stream->mode == __WRITE_MODE)
    {
        fflush(stream);
    }
    if (whence == SEEK_SET)
    {
        newpos = offset;
    }
    else if (whence == SEEK_CUR)
    {
        newpos = oldpos + offset;
    }

    if (whence == SEEK_END)
    {
        char buf[1000];

        while (fread(buf, sizeof buf, 1, stream) == 1)
        {
            /* do nothing */
        }
    }
    else if ((newpos >= stream->bufStartR)
        && (newpos < (stream->bufStartR + (stream->endbuf - stream->fbuf)))
        && (stream->update || (stream->mode == __READ_MODE)))
    {
        stream->upto = stream->fbuf + (size_t)(newpos - stream->bufStartR);
    }
    else
    {
#ifdef __OS2__
        rc = DosSetFilePtr(stream->hfile, newpos, FILE_BEGIN, &retpos);
        if ((rc != 0) || (retpos != newpos))
        {
            errno = rc;
            return (-1);
        }
        else
        {
            stream->endbuf = stream->fbuf + stream->szfbuf;
            stream->upto = stream->endbuf;
            stream->bufStartR = newpos - stream->szfbuf;
        }
#endif
#ifdef __WIN32__
        retpos = SetFilePointer(stream->hfile, newpos, NULL, FILE_BEGIN);
        if (retpos != newpos)
        {
            errno = GetLastError();
            return (-1);
        }
        else
        {
            stream->endbuf = stream->fbuf + stream->szfbuf;
            stream->upto = stream->endbuf;
            stream->bufStartR = newpos - stream->szfbuf;
        }
#endif
#ifdef __MSDOS__
        __seek(stream->hfile, newpos, whence);
        stream->endbuf = stream->fbuf + stream->szfbuf;
        stream->upto = stream->endbuf;
        stream->bufStartR = newpos - stream->szfbuf;
#endif
#if defined(__MVS__) || defined(__CMS__)
        char fnm[FILENAME_MAX];
        long int x;
        size_t y;
        char buf[1000];

        oldpos = ftell(stream);
        if (newpos < oldpos)
        {
            strcpy(fnm, "dd:");
            strcat(fnm, stream->ddname);
            inseek = 1;
            freopen(fnm, stream->modeStr, stream);
            inseek = 0;
            oldpos = 0;
        }
        y = (newpos - oldpos) % sizeof buf;
        fread(buf, y, 1, stream);
        for (x = oldpos + y; x < newpos; x += sizeof buf)
        {
            fread(buf, sizeof buf, 1, stream);
        }
#endif
    }
    stream->quickBin = 0;
    stream->quickText = 0;
    stream->ungetCh = -1;
    return (0);
}

long int ftell(FILE *stream)
{
    return (stream->bufStartR + (stream->upto - stream->fbuf));
}

int fsetpos(FILE *stream, const fpos_t *pos)
{
    fseek(stream, *pos, SEEK_SET);
    return (0);
}

int fgetpos(FILE *stream, fpos_t *pos)
{
    *pos = ftell(stream);
    return (0);
}

void rewind(FILE *stream)
{
    fseek(stream, 0L, SEEK_SET);
    return;
}

void clearerr(FILE *stream)
{
    stream->errorInd = 0;
    stream->eofInd = 0;
    return;
}

void perror(const char *s)
{
    if ((s != NULL) && (*s != '\0'))
    {
        printf("%s: ", s);
    }
    if (errno == 0)
    {
        printf("No error has occurred\n");
    }
    else
    {
        printf("An error has occurred\n");
    }
    return;
}

/*
NULL + F = allocate, setup
NULL + L = allocate, setup
NULL + N = ignore, return success
buf  + F = setup
buf  + L = setup
buf  + N = ignore, return success
*/

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    char *mybuf;

#if defined(__MVS__) || defined(__CMS__)
    /* don't allow mucking around with buffers on MVS or CMS */
    return (0);
#endif

    if (mode == _IONBF)
    {
        stream->bufTech = mode;
        return (0);
    }
    if (buf == NULL)
    {
        if (size < 2)
        {
            return (-1);
        }
        mybuf = malloc(size + 8);
        if (mybuf == NULL)
        {
            return (-1);
        }
    }
    else
    {
        if (size < 10)
        {
            return (-1);
        }
        mybuf = buf;
        stream->theirBuffer = 1;
        size -= 8;
    }
    if (!stream->permfile)
    {
        free(stream->intBuffer);
    }
    stream->intBuffer = mybuf;
    stream->fbuf = stream->intBuffer + 2;
    *stream->fbuf++ = '\0';
    *stream->fbuf++ = '\0';
    stream->szfbuf = size;
    stream->endbuf = stream->fbuf + stream->szfbuf;
    *stream->endbuf = '\n';
    if (stream->mode == __WRITE_MODE)
    {
        stream->upto = stream->fbuf;
    }
    else
    {
        stream->upto = stream->endbuf;
    }
    stream->bufTech = mode;
    if (!stream->textMode && (stream->bufTech == _IOLBF))
    {
        stream->quickBin = 0;
    }
    return (0);
}

int setbuf(FILE *stream, char *buf)
{
    int ret;

    if (buf == NULL)
    {
        ret = setvbuf(stream, NULL, _IONBF, 0);
    }
    else
    {
        ret = setvbuf(stream, buf, _IOFBF, BUFSIZ);
    }
    return (ret);
}

FILE *freopen(const char *filename, const char *mode, FILE *stream)
{
    inreopen = 1;
    fclose(stream);

    myfile = stream;
    fnm = filename;
    modus = mode;
    err = 0;
    spareSpot = stream->intFno;
    fopen2();
    if (err && !stream->permfile)
    {
        __userFiles[stream->intFno] = NULL;
        free(stream);
    }
#if defined(__MVS__) || defined(__CMS__)
    else if (err)
    {
        free(stream);
        /* need to protect against the app closing the file
           which it is allowed to */
        if (stream == stdin)
        {
            stdin = NULL;
        }
        else if (stream == stdout)
        {
            stdout = NULL;
        }
        else if (stream == stderr)
        {
            stderr = NULL;
        }
    }
#endif
    inreopen = 0;
    if (err)
    {
        return (NULL);
    }
    return (stream);
}

int fflush(FILE *stream)
{
#if !defined(__MVS__) && !defined(__CMS__)
#ifdef __OS2__
    APIRET rc;
    ULONG actualWritten;
#endif
#ifdef __WIN32__
    BOOL rc;
    DWORD actualWritten;
#endif
#ifdef __MSDOS__
    int errind;
    size_t actualWritten;
#endif

    if ((stream->upto != stream->fbuf) && (stream->mode == __WRITE_MODE))
    {
#ifdef __OS2__
        rc = DosWrite(stream->hfile,
                     (VOID *)stream->fbuf,
                     (size_t)(stream->upto - stream->fbuf),
                     &actualWritten);
        if (rc != 0)
        {
            stream->errorInd = 1;
            errno = rc;
            return (EOF);
        }
#endif
#ifdef __WIN32__
        rc = WriteFile(stream->hfile,
                       stream->fbuf,
                       (size_t)(stream->upto - stream->fbuf),
                       &actualWritten,
                       NULL);
        if (!rc)
        {
            stream->errorInd = 1;
            errno = GetLastError();
            return (EOF);
        }
#endif
#ifdef __MSDOS__
        actualWritten = __write(stream->hfile,
                                stream->fbuf,
                                (size_t)(stream->upto - stream->fbuf),
                                &errind);
        if (errind)
        {
            stream->errorInd = 1;
            errno = actualWritten;
            return (EOF);
        }
#endif
        stream->bufStartR += actualWritten;
        stream->upto = stream->fbuf;
    }
#endif
    return (0);
}

char *tmpnam(char *s)
{
#if defined(__MVS__) || defined(__CMS__)
    static char buf[] = "dd:ZZZZZZZA";
#else
    static char buf[] = "ZZZZZZZA.$$$";
#endif

#if defined(__MVS__) || defined(__CMS__)
    buf[10]++;
#else
    buf[7]++;
#endif
    if (s == NULL)
    {
        return (buf);
    }
    strcpy(s, buf);
    return (s);
}

FILE *tmpfile(void)
{
#if defined(__MVS__) || defined(__CMS__)
    return (fopen("dd:ZZZZZZZA", "wb+"));
#else
    return (fopen("ZZZZZZZA.$$$", "wb+"));
#endif
}

int fscanf(FILE *stream, const char *format, ...)
{
    va_list arg;
    int ret;

    va_start(arg, format);
    ret = vvscanf(format, arg, stream, NULL);
    va_end(arg);
    return (ret);
}

int scanf(const char *format, ...)
{
    va_list arg;
    int ret;

    va_start(arg, format);
    ret = vvscanf(format, arg, stdin, NULL);
    va_end(arg);
    return (ret);
}

int sscanf(const char *s, const char *format, ...)
{
    va_list arg;
    int ret;

    va_start(arg, format);
    ret = vvscanf(format, arg, NULL, s);
    va_end(arg);
    return (ret);
}

/* vvscanf - the guts of the input scanning */
/* several mods by Dave Edwards */
static int vvscanf(const char *format, va_list arg, FILE *fp, const char *s)
{
    int ch;
    int fin = 0;
    int cnt = 0;
    char *cptr;
    int *iptr;
    unsigned int *uptr;
    long *lptr;
    unsigned long *luptr;
    short *hptr;
    unsigned short *huptr;
    double *dptr;
    float *fptr;
    long startpos;
    const char *startp;
    int skipvar; /* nonzero if we are skipping this variable */
    int modlong;   /* nonzero if "l" modifier found */
    int modshort;   /* nonzero if "h" modifier found */
    int informatitem;  /* nonzero if % format item started */
           /* informatitem is 1 if we have processed "%l" but not the
              type letter (s,d,e,f,g,...) yet. */

    if (fp != NULL)
    {
        startpos = ftell(fp);
    }
    else
    {
        startp = s;
    }
    inch();
    informatitem = 0;   /* init */
    if ((fp != NULL && ch == EOF) || (fp == NULL && ch == 0)) return EOF;
                               /* initially at EOF or end of string */
    while (!fin)
    {
        if (*format == '\0')
        {
            fin = 1;
        }
        else if (*format == '%' || informatitem)
        {
            if(*format=='%')   /* starting a format item */
            {
                format++;
                modlong=0;   /* init */
                modshort=0;
                skipvar = 0;
                if (*format == '*')
                {
                    skipvar = 1;
                    format++;
                }
            }
            if (*format == '%')   /* %% */
            {
                if (ch != '%') return (cnt);
                inch();
                informatitem=0;
            }
            else if (*format == 'l')
            {
                /* Type modifier: l  (e.g. %ld or %lf) */
                modlong=1;
                informatitem=1;
            }
            else if (*format == 'h')
            {
                /* Type modifier: h (short int) */
                modshort=1;
                informatitem=1;
            }
            else    /* process a type character: */
            {
                informatitem=0;   /* end of format item */
                if (*format == 's')
                {
                    if (!skipvar)
                    {
                        cptr = va_arg(arg, char *);
                    }
                    /* Skip leading whitespace: */
                    while (ch>=0 && isspace(ch)) inch();
                    if ((fp != NULL && ch == EOF) || (fp == NULL && ch == 0))
                                         /* at EOF or end of string */
                    {
                        fin = 1;
                        if (!skipvar)
                        {
                            *cptr = 0;   /* give a null string */
                        }
                    }
                    else
                    {
                        for(;;)
                        {
                            if (isspace(ch)) break;
                            if ((fp != NULL && ch == EOF)
                                || (fp == NULL && ch == 0))
                            {
                                fin = 1;
                                break;
                            }
                            if (!skipvar)
                            {
                                *cptr++ = (char)ch;
                            }
                            inch();
                        }
                        if (!skipvar)
                        {
                            *cptr = '\0';
                        }
                        cnt++;
                    }
                }
                else if (*format == '[')
                {
                    int reverse = 0;
                    int found;
                    const char *first;
                    const char *last;
                    size_t size;
                    size_t mcnt = 0;
                    
                    if (!skipvar)
                    {
                        cptr = va_arg(arg, char *);
                    }
                    format++;
                    if (*format == '^')
                    {
                        reverse = 1;
                        format++;
                    }
                    if (*format == '\0') break;
                    first = format;
                    format++;
                    last = strchr(format, ']');                    
                    if (last == NULL) return (cnt);
                    size = (size_t)(last - first);
                    while (1)
                    {
                        /* note that C90 doesn't require special
                           processing for '-' so it hasn't been
                           added */
                        found = (memchr(first, ch, size) != NULL);
                        if (found && reverse) break;
                        if (!found && !reverse) break;
                        if (!skipvar)
                        {
                            *cptr++ = (char)ch;
                        }
                        mcnt++;
                        inch();
                        /* if at EOF or end of string, bug out */
                        if ((fp != NULL && ch == EOF) 
                            || (fp == NULL && ch == 0))
                        {
                            break;
                        }
                    }
                    if (mcnt > 0)
                    {
                        if (!skipvar)
                        {
                            *cptr++ = '\0';
                        }
                        cnt++;
                    }
                    else
                    {
                        break;
                    }
                    format = last + 1;
                }
                else if (*format == 'c')
                {
                    if (!skipvar)
                    {
                        cptr = va_arg(arg, char *);
                    }
                    if ((fp != NULL && ch == EOF)
                        || (fp == NULL && ch == 0)) fin = 1;
                                          /* at EOF or end of string */
                    else
                    {
                        if (!skipvar)
                        {
                            *cptr = ch;
                        }
                        cnt++;
                        inch();
                    }
                }
                else if (*format == 'n')
                {
                    uptr = va_arg(arg, unsigned int *);
                    if (fp != NULL)
                    {
                        *uptr = (unsigned int)(ftell(fp) - startpos);
                    }
                    else
                    {
                        *uptr = (unsigned int)(startp - s);
                    }                    
                }
                else if (*format == 'd' || *format == 'u'
                         || *format == 'x' || *format == 'o'
                         || *format == 'p'
                         || *format == 'i')                         
                {
                    int neg = 0;
                    unsigned long x = 0;
                    int undecided = 0;
                    int base = 10;
                    int mcnt = 0;

                    if (*format == 'x') base = 16;
                    else if (*format == 'p') base = 16;
                    else if (*format == 'o') base = 8;
                    else if (*format == 'i') base = 0;
                    if (!skipvar)
                    {
                        if ((*format == 'd') || (*format == 'i'))
                        {
                            if (modlong) lptr = va_arg(arg, long *);
                            else if (modshort) hptr = va_arg(arg, short *);
                            else iptr = va_arg(arg, int *);
                        }
                        else
                        {
                            if (modlong) luptr = va_arg(arg, unsigned long *);
                            else if (modshort) huptr =
                                     va_arg(arg, unsigned short *);
                            else uptr = va_arg(arg, unsigned int *);
                        }
                    }
                    /* Skip leading whitespace: */
                    while (ch>=0 && isspace(ch)) inch();
                    if (ch == '-')
                    {
                        neg = 1;
                        inch();
                    }
                    else if(ch == '+') inch();
                    
                    /* this logic is the same as strtoul so if you
                       change this, change that one too */

                    if (base == 0)
                    {
                        undecided = 1;
                    }
                    while (!((fp != NULL && ch == EOF)
                             || (fp == NULL && ch == 0)))
                    {
                        if (isdigit((unsigned char)ch))
                        {
                            if (base == 0)
                            {
                                if (ch == '0')
                                {
                                    base = 8;
                                }
                                else
                                {
                                    base = 10;
                                    undecided = 0;
                                }
                            }
                            x = x * base + (ch - '0');
                            inch();
                        }
/* DOS has a ':' in the pointer - skip that */
#if defined(__MSDOS__) && !defined(__PDOS__) && !defined(__gnu_linux__)
                        else if ((*format == 'p') && (ch == ':'))
                        {
                            inch();
                        }
#endif
                        else if (isalpha((unsigned char)ch))
                        {
                            if ((ch == 'X') || (ch == 'x'))
                            {
                                if ((base == 0) || ((base == 8) && undecided))
                                {
                                    base = 16;
                                    undecided = 0;
                                    inch();
                                }
                                else if (base == 16)
                                {
                                    /* hex values are allowed to have an 
                                       optional 0x */
                                    inch();
                                }
                                else
                                {
                                    break;
                                }
                            }
                            else if (base <= 10)
                            {
                                break;
                            }
                            else
                            {
                                x = x * base + 
                                    (toupper((unsigned char)ch) - 'A') + 10;
                                inch();
                            }
                        }
                        else
                        {
                            break;
                        }
                        mcnt++;
                    }
                    
                    /* end of strtoul logic */
                    
                    /* If we didn't get any characters, don't go any
                       further */
                    if (mcnt == 0)
                    {
                        break;
                    }

                    
                    if (!skipvar)
                    {
                        if ((*format == 'd') || (*format == 'i'))
                        {
                            long lval;
                            
                            if (neg)
                            {
                                lval = (long)-x;
                            }
                            else
                            {
                                lval = (long)x;
                            }
                            if (modlong) *lptr=lval;
                                /* l modifier: assign to long */
                            else if (modshort) *hptr = (short)lval;
                                /* h modifier */
                            else *iptr=(int)lval;
                        }
                        else
                        {
                            if (modlong) *luptr = (unsigned long)x;
                            else if (modshort) *huptr = (unsigned short)x;
                            else *uptr = (unsigned int)x;
                        }
                    }
                    cnt++;
                }
                else if (*format=='e' || *format=='f' || *format=='g' ||
                         *format=='E' || *format=='G')
                {
                    /* Floating-point (double or float) input item */
                    int negsw1,negsw2,dotsw,expsw,ndigs1,ndigs2,nfdigs;
                    int ntrailzer,expnum,expsignsw;
                    double fpval,pow10;

                    if (!skipvar)                    
                    {
                        if (modlong) dptr = va_arg(arg, double *);
                        else fptr = va_arg(arg, float *);
                    }
                    negsw1=0;   /* init */
                    negsw2=0;
                    dotsw=0;
                    expsw=0;
                    ndigs1=0;
                    ndigs2=0;
                    nfdigs=0;
                    ntrailzer=0;  /* # of trailing 0's unaccounted for */
                    expnum=0;
                    expsignsw=0;  /* nonzero means done +/- on exponent */
                    fpval=0.0;
                    /* Skip leading whitespace: */
                    while (ch>=0 && isspace(ch)) inch();
                    if (ch=='-')
                    {
                        negsw1=1;
                        inch();
                    }
                    else if (ch=='+') inch();
                    while (ch>0)
                    {
                        if (ch=='.' && dotsw==0 && expsw==0) dotsw=1;
                        else if (isdigit(ch))
                        {
                            if(expsw)
                            {
                                ndigs2++;
                                expnum=expnum*10+(ch-'0');
                            }
                            else
                            {
                                /* To avoid overflow or loss of precision,
                                   skip leading and trailing zeros unless
                                   really needed. (Automatic for leading
                                   0's, since 0.0*10.0 is 0.0) */
                                ndigs1++;
                                if (dotsw) nfdigs++;
                                if (ch=='0' && fpval!=0.)
                                {
                                    /* Possible trailing 0 */
                                    ntrailzer++;
                                }
                                else
                                {
                                    /* Account for any preceding zeros */
                                    while (ntrailzer>0)
                                    {
                                        fpval*=10.;
                                        ntrailzer--;
                                    }
                                    fpval=fpval*10.0+(ch-'0');
                                }
                            }
                        }
                        else if ((ch=='e' || ch=='E') && expsw==0) expsw=1;
                        else if ((ch=='+' || ch=='-') && expsw==1
                                 && ndigs2==0 && expsignsw==0)
                        {
                            expsignsw=1;
                            if (ch=='-') negsw2=1;
                        }
                        else break;   /* bad char: end of input item */
                        inch();
                    }
                    if ((fp != NULL && ch == EOF)
                        || (fp == NULL && ch == 0)) fin=1;
                    /* Check for a valid fl-pt value: */
                    if (ndigs1==0 || (expsw && ndigs2==0)) return(cnt);
                    /* Complete the fl-pt value: */
                    if (negsw2) expnum=-expnum;
                    expnum+=ntrailzer-nfdigs;
                    if (expnum!=0 && fpval!=0.0)
                    {
                        negsw2=0;
                        if (expnum<0)
                        {
                            expnum=-expnum;
                            negsw2=1;
                        }
                        /* Multiply or divide by 10.0**expnum, using
                           bits of expnum (fast method) */
                        pow10=10.0;
                        for (;;)
                        {
                            if (expnum & 1)     /* low-order bit */
                            {
                                if (negsw2) fpval/=pow10;
                                else fpval*=pow10;
                            }
                            expnum>>=1;   /* shift right 1 bit */
                            if (expnum==0) break;
                            pow10*=pow10;   /* 10.**n where n is power of 2 */
                        }
                    }
                    if (negsw1) fpval=-fpval;
                    if (!skipvar)
                    {
                        /* l modifier: assign to double */
                        if (modlong) *dptr=fpval;
                        else *fptr=(float)fpval;
                    }
                    cnt++;
                }
            }
        }
        else if (isspace((unsigned char)(*format)))
        {
            /* Whitespace char in format string: skip next whitespace
               chars in input data. This supports input of multiple
               data items. */
            while (ch>=0 && isspace(ch))
            {
                inch();
            }
        }
        else   /* some other character in the format string */
        {
            if (ch != *format) return (cnt);
            inch();
        }
        format++;
        if ((fp != NULL && ch == EOF) || (fp == NULL && ch == 0)) fin = 1;
            /* EOF */
    }
    if (fp != NULL) ungetc(ch, fp);
    return (cnt);
}

char *gets(char *s)
{
    char *ret;

    stdin->quickText = 0;
    stdin->noNl = 1;
    ret = fgets(s, INT_MAX, stdin);
    stdin->noNl = 0;
    stdin->quickText = 1;
    return (ret);
}

int puts(const char *s)
{
    int ret;

    ret = fputs(s, stdout);
    if (ret == EOF)
    {
        return (ret);
    }
    return (putc('\n', stdout));
}

/* The following functions are implemented as macros */

#undef getchar
#undef putchar
#undef getc
#undef putc
#undef feof
#undef ferror

int getc(FILE *stream)
{
    return (fgetc(stream));
}

int putc(int c, FILE *stream)
{
    return (fputc(c, stream));
}

int getchar(void)
{
    return (getc(stdin));
}

int putchar(int c)
{
    return (putc(c, stdout));
}

int feof(FILE *stream)
{
    return (stream->eofInd);
}

int ferror(FILE *stream)
{
    return (stream->errorInd);
}

#if 0
Design of MVS i/o routines

in/out function rec-type mode   method
in     fread    fixed    bin    loop reading, remember remainder
in     fread    fixed    text   loop reading + truncing, remember rem
in     fread    var      bin    loop reading (+ len), remember remainder
in     fread    var      text   loop reading (+ len), remember remainder
in     fgets    fixed    bin    read, scan, remember remainder
in     fgets    fixed    text   read, trunc, remember remainder
in     fgets    var      bin    read, scan, rr
in     fgets    var      text   read, rr
in     fgetc    fixed    bin    read, rr
in     fgetc    fixed    text   read, trunc, rr
in     fgetc    var      bin    read, rr
in     fgetc    var      text   read, rr

out    fwrite   fixed    bin    loop doing put, rr
out    fwrite   fixed    text   search newline, copy + pad, put, rr
out    fwrite   var      bin    if nelem != 1 copy to max lrecl
out    fwrite   var      text   loop search nl, put, rr
out    fputs    fixed    bin    loop doing put, rr
out    fputs    fixed    text   search newline, copy + pad, put, rr
out    fputs    var      bin    put
out    fputs    var      text   search newline, put, copy rem
out    fputc    fixed    bin    copy to rr until rr == lrecl
out    fputc    fixed    text   copy to rr until newline, then pad
out    fputc    var      bin    copy to rr until rr == lrecl
out    fputc    var      text   copy to rr until newline

optimize for fread on binary files (read matching record length),
especially fixed block files, and fgets on text files, especially
variable blocked files.

binary, variable block files are not a file type supported by this
library as part of the conforming implementation.  Instead, they
are considered to be record-oriented processing, similar to unix
systems reading data from a pipe, where you can read less bytes
than requested, without reaching EOF.  ISO 7.9.8.1 does not give you
the flexibility of calling either of these things conforming.
Basically, the C standard does not have a concept of operating
system maintained length binary records, you have to do that
yourself, e.g. by writing out the lengths yourself.  You can do
this in a fixed block dataset on MVS, and if you are concerned
about null-padding at the end of your data, use a lrecl of 1
(and suffer the consequences!).  You could argue that this
non-conformance should only be initiated if fopen has a parameter
including ",type=record" or whatever.  Another option would
be to make VB binary records include the record size as part of
the stream.  Hmmm, sounds like that is the go actually.

fread: if quickbin, if read elem size == lrecl, doit
fgets: if variable record + no remainder
       if buffer > record size, copy + add newline
#endif

#if defined(__MVS__) || defined(__CMS__)
char *fgets(char *s, int n, FILE *stream)
{
    unsigned char *eptr;
    size_t len;
    int cnt;
    int c;

    if (stream->quickText)
    {
        if (__aread(stream->hfile, &dptr) != 0)
        {
            stream->eofInd = 1;
            stream->quickText = 0;
            return (NULL);
        }
        len = ((dptr[0] << 8) | dptr[1]) - 4;
        if ((len == 1) && (dptr[4] == ' '))
        {
            len = 0;
        }
        if (n > (len + 1))
        {
            memcpy(s, dptr + 4, len);
            memcpy(s + len, "\n", 2);
            stream->bufStartR += len + 1;
            return (s);
        }
        else
        {
            memcpy(stream->fbuf, dptr + 4, len);
            stream->upto = stream->fbuf;
            stream->endbuf = stream->fbuf + len;
            *(stream->endbuf++) = '\n';
            stream->quickText = 0;
        }
    }

    if (stream->eofInd)
    {
        return (NULL);
    }

    switch (stream->style)
    {
        case FIXED_TEXT:
            if ((stream->endbuf == stream->fbuf)
                && (n > (stream->lrecl + 2)))
            {
                if (__aread(stream->hfile, &dptr) != 0)
                {
                    stream->eofInd = 1;
                    return (NULL);
                }
                eptr = dptr + stream->lrecl - 1;
                while ((*eptr == ' ') && (eptr >= dptr))
                {
                    eptr--;
                }
                eptr++;
                memcpy(s, dptr, eptr - dptr);
                memcpy(s + (eptr - dptr), "\n", 2);
                stream->bufStartR += (eptr - dptr) + 1;
                return (s);
            }
            break;

        default:
            break;

    }

    /* Ok, the obvious optimizations have been done,
       so now we switch to the slow generic version */

    n--;
    cnt = 0;
    while (cnt < n)
    {
        c = getc(stream);
        if (c == EOF) break;
        s[cnt] = c;
        if (c == '\n') break;
        cnt++;
    }
    if (cnt < n) s[cnt++] = '\n';
    s[cnt] = '\0';
    return (s);
}

int fputs(const char *s, FILE *stream)
{
    const char *p;
    size_t len;

    if (stream->quickText)
    {
        p = strchr(s, '\n');
        if (p != NULL)
        {
            len = p - s;
            if (len > stream->lrecl)
            {
                len = stream->lrecl;
            }
            begwrite(stream, len);
            memcpy(dptr + 4, s, len);
            dptr[0] = (len + 4) >> 8;
            dptr[1] = (len + 4) & 0xff;
            dptr[2] = 0;
            dptr[3] = 0;
            finwrite(stream);
            stream->bufStartR += (len + 1);
            if (*(p + 1) == '\0')
            {
                return (len + 1);
            }
            s = p + 1;
            stream->quickText = 0;
        }
    }
    switch (stream->style)
    {
        case FIXED_TEXT:
            len = strlen(s);
            if (len > 0)
            {
                len--;
                if (((strchr(s, '\n') - s) == len)
                    && (stream->upto == stream->fbuf)
                    && (len <= stream->lrecl))
                {
                    begwrite(stream, stream->lrecl);
                    memcpy(dptr, s, len);
                    memset(dptr + len, ' ', stream->szfbuf - len);
                    finwrite(stream);
                    stream->bufStartR += len;
                }
                else
                {
                    fwrite(s, len + 1, 1, stream);
                }
            }
            break;

        default:
            len = strlen(s);
            fwrite(s, len, 1, stream);
            break;
    }
    return (0);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t bytes;
    size_t sz;
    char *p;
    int x;

    if (stream->quickBin)
    {
        if ((nmemb == 1) && (size == stream->lrecl))
        {
            begwrite(stream, stream->lrecl);
            memcpy(dptr, ptr, size);
            finwrite(stream);
            stream->bufStartR += size;
            return (1);
        }
        else
        {
            stream->quickBin = 0;
        }
    }
    switch (stream->style)
    {
        case FIXED_BINARY:
            bytes = nmemb * size;
            /* if we've exceed our buffer we need to write out
               a record - but if we haven't written any data to
               our internal buffer yet, don't bother going through
               this code, it'll be handled later. */
            if (((stream->endbuf - stream->upto) <= bytes)
                && (stream->upto != stream->fbuf))
            {
                /* ready to write a record - request some space
                   from MVS */
                begwrite(stream, stream->lrecl);
                sz = stream->endbuf - stream->upto;
                memcpy(dptr, stream->fbuf, stream->szfbuf - sz);
                memcpy(dptr + stream->szfbuf - sz, ptr, sz);
                finwrite(stream);
                ptr = (char *)ptr + sz;
                bytes -= sz;
                stream->upto = stream->fbuf;
                stream->bufStartR += stream->szfbuf;
            }
            /* At this point, the internal buffer is empty if the
               number of bytes to write is still greater than the
               internal buffer. In which case, start writing directly
               to an MVS-provided area. */
            while (bytes >= stream->szfbuf)
            {
                begwrite(stream, stream->lrecl);
                memcpy(dptr, ptr, stream->szfbuf);
                finwrite(stream);
                ptr = (char *)ptr + stream->szfbuf;
                bytes -= stream->szfbuf;
                stream->bufStartR += stream->szfbuf;
            }
            /* any remainder needs to go into the internal buffer */
            memcpy(stream->upto, ptr, bytes);
            stream->upto += bytes;
            break;

        case VARIABLE_BINARY:
            bytes = nmemb * size;            
            while (bytes > 0)
            {
                int fulllen;
                
                if (stream->errorInd) break;
                sz = stream->upto - stream->fbuf;
                if (sz < 4)
                {
                    if ((bytes + sz) < 4)
                    {
                        memcpy(stream->upto, ptr, bytes);
                        stream->upto += bytes;
                        bytes = 0;
                        break; 
                    }
                    else
                    {
                        memcpy(stream->upto, ptr, 4 - sz);
                        ptr = (char *)ptr + (4 - sz);
                        bytes -= (4 - sz);
                        stream->upto += (4 - sz);
                        sz = 4;
                        if (memcmp(stream->fbuf + 2, "\0\0", 2) != 0)
                        {
                            stream->errorInd = 1;
                            break;
                        }
                        fulllen = (stream->fbuf[0] << 8) | stream->fbuf[1];
                        if (fulllen == 0)                        
                        {
                            /* here we allow for the possibility that
                               they are copying a data source that has
                               terminating NULs added - so long as all
                               remaining charactes are NUL, it will be
                               allowed. Otherwise we rely on the above
                               validation to catch a problem - checking
                               2 bytes at a time, which shouldn't be a
                               problem since this is only at the end of
                               the file */
                            stream->upto = stream->fbuf + 2;
                            continue;
                        }
                        else if (fulllen < 4)
                        {
                            stream->errorInd = 1;
                            break;
                        }
                    }
                }
                
                /* we have 4 bytes, validated */
                fulllen = (stream->fbuf[0] << 8) | stream->fbuf[1];
                
                /* If we have enough data, write it out */
                if ((sz + bytes) >= fulllen)
                {
                    /* silently truncate long records to give
                       user more flexibility */
                    if (fulllen > stream->lrecl)
                    {
                        stream->fbuf[0] = stream->lrecl >> 8;
                        stream->fbuf[1] = stream->lrecl & 0xff;
                        begwrite(stream, stream->lrecl);
                        if (sz >= stream->lrecl)
                        {
                            memcpy(dptr, stream->fbuf, stream->lrecl);
                        }
                        else
                        {
                            memcpy(dptr, stream->fbuf, sz);
                            memcpy(dptr + sz, ptr, stream->lrecl - sz);
                        }
                    }
                    else
                    {
                        begwrite(stream, fulllen);
                        memcpy(dptr, stream->fbuf, sz);
                        memcpy(dptr + sz, ptr, fulllen - sz);
                    }
                    finwrite(stream);
                    stream->bufStartR += fulllen;
                    stream->upto = stream->fbuf;
                    bytes -= (fulllen - sz);
                    ptr = (char *)ptr + (fulllen - sz);
                }
                
                /* less data than required, store it, without
                   overflowing our buffer */
                else if ((sz + bytes) > stream->lrecl)
                {
                    memcpy(stream->upto,
                           ptr,
                           stream->lrecl - sz);
                    /* here we allow upto to exceed our buffer.
                       shouldn't be a problem as we never write
                       to that memory. alternative is to make
                       BUFSIZ 64k. */
                    stream->upto += bytes;
                    ptr = (char *)ptr + bytes;
                    bytes = 0;
                }
                
                /* enough room to fit data */
                else
                {
                    memcpy(stream->upto, ptr, bytes);
                    stream->upto += bytes;
                    ptr = (char *)ptr + bytes;
                    bytes = 0;
                }
            }
            break;

        case FIXED_TEXT:
            bytes = nmemb * size;
            p = memchr(ptr, '\n', bytes);
            if (p != NULL)
            {
                sz = p - (char *)ptr;
                bytes -= sz + 1;
                if (stream->upto == stream->fbuf)
                {
                    if (sz > stream->lrecl)
                    {
                        sz = stream->lrecl;
                    }
                    begwrite(stream, stream->lrecl);
                    memcpy(dptr, ptr, sz);
                    memset(dptr + sz, ' ', stream->szfbuf - sz);
                    finwrite(stream);
                    stream->bufStartR += sz;
                }
                else
                {
                    if (((stream->upto - stream->fbuf) + sz) > stream->lrecl)
                    {
                        sz = stream->lrecl - (stream->upto - stream->fbuf);
                    }
                    memcpy(stream->upto, ptr, sz);
                    sz += (stream->upto - stream->fbuf);
                    begwrite(stream, stream->lrecl);
                    memcpy(dptr, stream->fbuf, sz);
                    memset(dptr + sz, ' ', stream->lrecl - sz);
                    finwrite(stream);
                    stream->bufStartR += sz;
                    stream->upto = stream->fbuf;
                }
                ptr = (char *)p + 1;
                if (bytes > 0)
                {
                    p = memchr(ptr, '\n', bytes);
                    while (p != NULL)
                    {
                        sz = p - (char *)ptr;
                        bytes -= sz + 1;
                        if (sz > stream->lrecl)
                        {
                            sz = stream->lrecl;
                        }
                        begwrite(stream, stream->lrecl);
                        memcpy(dptr, ptr, sz);
                        memset(dptr + sz, ' ', stream->szfbuf - sz);
                        finwrite(stream);
                        stream->bufStartR += sz;
                        ptr = p + 1;
                        p = memchr(ptr, '\n', bytes);
                    }
                    if (bytes > 0)
                    {
                        sz = bytes;
                        if (sz > stream->lrecl)
                        {
                            sz = stream->lrecl;
                        }
                        memcpy(stream->upto, ptr, sz);
                        stream->upto += sz;
                        bytes = 0;
                    }
                }
            }
            else /* p == NULL */
            {
                if (((stream->upto - stream->fbuf) + bytes) > stream->lrecl)
                {
                    bytes = stream->lrecl - (stream->upto - stream->fbuf);
                }
                memcpy(stream->upto, ptr, bytes);
                stream->upto += bytes;
            }
            break;

        case VARIABLE_TEXT:
            stream->quickText = 0;
            bytes = nmemb * size;
            p = memchr(ptr, '\n', bytes);
            if (p != NULL)
            {
                sz = p - (char *)ptr;
                bytes -= sz + 1;
                if (stream->upto == stream->fbuf)
                {
                    if (sz > stream->lrecl)
                    {
                        sz = stream->lrecl;
                    }
                    begwrite(stream, (sz == 0) ? 5 : sz + 4);
                    if(sz == 0)
                    {
                        dptr[0] = 0;
                        dptr[1] = 5;
                        dptr[2] = 0;
                        dptr[3] = 0;
                        dptr[4] = ' ';
                        finwrite(stream);
                        /* note that the bufStartR needs to reflect
                           just the newline, and not the dummy space
                           we added */                        
                        stream->bufStartR += 1;
                    }
                    else
                    {
                        dptr[0] = (sz + 4) >> 8;
                        dptr[1] = (sz + 4) & 0xff;
                        dptr[2] = 0;
                        dptr[3] = 0;
                        memcpy(dptr + 4, ptr, sz);
                        finwrite(stream);
                        stream->bufStartR += (sz + 1);
                    }
                }
                else
                {
                    if (((stream->upto - stream->fbuf) + sz) > stream->lrecl)
                    {
                        sz = stream->lrecl - (stream->upto - stream->fbuf);
                    }
                    memcpy(stream->upto, ptr, sz);
                    sz += (stream->upto - stream->fbuf);
                    begwrite(stream, (sz == 0) ? 5 : sz + 4);
                    if(sz == 0)
                    {
                        dptr[0] = 0;
                        dptr[1] = 5;
                        dptr[2] = 0;
                        dptr[3] = 0;
                        dptr[4] = ' ';
                        finwrite(stream);
                        stream->bufStartR += 1;
                    }
                    else
                    {
                        dptr[0] = (sz + 4) >> 8;
                        dptr[1] = (sz + 4) & 0xff;
                        dptr[2] = 0;
                        dptr[3] = 0;
                        memcpy(dptr + 4, stream->fbuf, sz);
                        finwrite(stream);
                        stream->bufStartR += (sz + 1);
                    }
                    stream->upto = stream->fbuf;
                }
                ptr = (char *)p + 1;
                if (bytes > 0)
                {
                    p = memchr(ptr, '\n', bytes);
                    while (p != NULL)
                    {
                        sz = p - (char *)ptr;
                        bytes -= sz + 1;
                        if (sz > stream->lrecl)
                        {
                            sz = stream->lrecl;
                        }
                        begwrite(stream, (sz == 0) ? 5 : sz + 4);
                        if(sz == 0)
                        {
                            dptr[0] = 0;
                            dptr[1] = 5;
                            dptr[2] = 0;
                            dptr[3] = 0;
                            dptr[4] = ' ';
                            finwrite(stream);
                            stream->bufStartR += 1;
                        }
                        else
                        {
                            dptr[0] = (sz + 4) >> 8;
                            dptr[1] = (sz + 4) & 0xff;
                            dptr[2] = 0;
                            dptr[3] = 0;
                            memcpy(dptr + 4, ptr, sz);
                            finwrite(stream);
                            stream->bufStartR += (sz + 1);
                        }
                        ptr = p + 1;
                        p = memchr(ptr, '\n', bytes);
                    }
                    if (bytes > 0)
                    {
                        sz = bytes;
                        if (sz > stream->lrecl)
                        {
                            sz = stream->lrecl;
                        }
                        memcpy(stream->upto, ptr, sz);
                        stream->upto += sz;
                        bytes = 0;
                    }
                }
            }
            else /* p == NULL */
            {
                if (((stream->upto - stream->fbuf) + bytes) > stream->lrecl)
                {
                    bytes = stream->lrecl - (stream->upto - stream->fbuf);
                }
                memcpy(stream->upto, ptr, bytes);
                stream->upto += bytes;
            }
            break;
    }
    return (nmemb);
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t bytes;
    size_t read;
    size_t totalread;
    size_t extra;
    unsigned char *eptr;

    if (stream->quickBin)
    {
        if ((nmemb == 1) && (size == stream->lrecl))
        {
            if (__aread(stream->hfile, &dptr) != 0)
            {
                stream->eofInd = 1;
                stream->quickBin = 0;
                return (0);
            }
            memcpy(ptr, dptr, size);
            return (1);
        }
        else
        {
            stream->quickBin = 0;
        }
    }
    if (stream->eofInd)
    {
        return (0);
    }

    /* If we have an unget character, then write it into
       the buffer in advance */
    if (stream->ungetCh != -1)
    {
        stream->upto--;
        *stream->upto = stream->ungetCh;
        stream->ungetCh = -1;
    }

    switch (stream->style)
    {
        case FIXED_TEXT:
            bytes = nmemb * size;
            read = stream->endbuf - stream->upto;
            if (read > bytes)
            {
                memcpy(ptr, stream->upto, bytes);
                stream->upto += bytes;
                totalread = bytes;
            }
            else
            {
                memcpy(ptr, stream->upto, read);
                stream->bufStartR += (stream->endbuf - stream->fbuf);
                stream->upto = stream->endbuf = stream->fbuf;
                totalread = read;
            }

            while (totalread < bytes)
            {
                if (__aread(stream->hfile, &dptr) != 0)
                {
                    stream->eofInd = 1;
                    break;
                }

                eptr = dptr + stream->lrecl - 1;
                while ((*eptr == ' ') && (eptr >= dptr))
                {
                    eptr--;
                }

                read = eptr + 1 - dptr;

                if ((totalread + read) >= bytes)
                {
                    extra = (totalread + read) - bytes;
                    read -= extra;
                    memcpy(stream->fbuf, dptr + read, extra);
                    stream->endbuf = stream->fbuf + extra;
                    *stream->endbuf++ = '\n';
                }

                memcpy((char *)ptr + totalread, dptr, read);
                totalread += read;
                stream->bufStartR += read;
                if (totalread < bytes)
                {
                    *((char *)ptr + totalread) = '\n';
                    totalread++;
                    stream->bufStartR++;
                }
            }
            return ((size == 0) ? 0 : (totalread / size));
            break;

        case FIXED_BINARY:
            bytes = nmemb * size;
            read = stream->endbuf - stream->upto;
            if (read > bytes)
            {
                memcpy(ptr, stream->upto, bytes);
                stream->upto += bytes;
                totalread = bytes;
            }
            else
            {
                memcpy(ptr, stream->upto, read);
                stream->bufStartR += (stream->endbuf - stream->fbuf);
                stream->upto = stream->endbuf = stream->fbuf;
                totalread = read;
            }

            while (totalread < bytes)
            {
                if (__aread(stream->hfile, &dptr) != 0)
                {
                    stream->eofInd = 1;
                    break;
                }

                read = stream->lrecl;

                if ((totalread + read) > bytes)
                {
                    extra = (totalread + read) - bytes;
                    read -= extra;
                    memcpy(stream->fbuf, dptr + read, extra);
                    stream->endbuf = stream->fbuf + extra;
                }

                memcpy((char *)ptr + totalread, dptr, read);
                totalread += read;
                stream->bufStartR += read;
            }
            return ((size == 0) ? 0 : (totalread / size));
            break;

        case VARIABLE_TEXT:
            bytes = nmemb * size;
            read = stream->endbuf - stream->upto;
            if (read > bytes)
            {
                memcpy(ptr, stream->upto, bytes);
                stream->upto += bytes;
                totalread = bytes;
            }
            else
            {
                memcpy(ptr, stream->upto, read);
                stream->bufStartR += (stream->endbuf - stream->fbuf);
                stream->upto = stream->endbuf = stream->fbuf;
                totalread = read;
            }

            while (totalread < bytes)
            {
                if (__aread(stream->hfile, &dptr) != 0)
                {
                    stream->eofInd = 1;
                    break;
                }

                read = (dptr[0] << 8) | dptr[1];
                read -= 4;
                dptr += 4;
                if ((read == 1) && (dptr[0] == ' '))
                {
                    read = 0;
                }

                if ((totalread + read) >= bytes)
                {
                    extra = (totalread + read) - bytes;
                    read -= extra;
                    memcpy(stream->fbuf, dptr + read, extra);
                    stream->endbuf = stream->fbuf + extra;
                    *stream->endbuf++ = '\n';
                }

                memcpy((char *)ptr + totalread, dptr, read);
                totalread += read;
                stream->bufStartR += read;
                if (totalread < bytes)
                {
                    *((char *)ptr + totalread) = '\n';
                    totalread++;
                    stream->bufStartR++;
                }
            }
            return ((size == 0) ? 0 : (totalread / size));
            break;

        case VARIABLE_BINARY:
            bytes = nmemb * size;
            read = stream->endbuf - stream->upto;
            if (read > bytes)
            {
                memcpy(ptr, stream->upto, bytes);
                stream->upto += bytes;
                totalread = bytes;
            }
            else
            {
                memcpy(ptr, stream->upto, read);
                stream->bufStartR += (stream->endbuf - stream->fbuf);
                stream->upto = stream->endbuf = stream->fbuf;
                totalread = read;
            }

            while (totalread < bytes)
            {
                if (__aread(stream->hfile, &dptr) != 0)
                {
                    stream->eofInd = 1;
                    break;
                }

                read = (dptr[0] << 8) | dptr[1];
                
                if (stream->reallyu)
                {
                    /* skip over the RDW */
                    dptr += 4;
                    read -= 4;
                    if (stream->reallyt)
                    {
                        unsigned char *p;
                        
                        /* get rid of any trailing NULs in text mode */
                        p = memchr(dptr, '\0', read);
                        if (p != NULL)
                        {
                            read = p - dptr;
                        }
                    }
                }

                if ((totalread + read) > bytes)
                {
                    extra = (totalread + read) - bytes;
                    read -= extra;
                    memcpy(stream->fbuf, dptr + read, extra);
                    stream->endbuf = stream->fbuf + extra;
                }

                memcpy((char *)ptr + totalread, dptr, read);
                totalread += read;
                stream->bufStartR += read;
            }
            return ((size == 0) ? 0 : (totalread / size));
            break;

        default:
            break;
    }
    return (0);
}

#endif

/*
   Following code issues a FILEDEF for CMS
*/

#ifdef __CMS__
static void filedef(char *fdddname, char *fnm, int mymode)
{
    char s202parm [800];

    int code;
    int parm;
    char *fname;
    char *ftype;
    char *fmode;
    char *p;
    int console;

/* 
    Skip leading blanks because sometimes people do that in CMS
*/
    while (fnm[0] == ' ') fnm++;

/*
    first parse the file name
*/
    console = 0;
    if( fnm[0] == '*') console = 1;
    while ( NULL != (p = strchr(fnm, '.')) )*p=' '; /* replace . with   */
    fname =  strtok(fnm, " ");
    ftype =  strtok(NULL, " ");
    if (ftype == NULL) ftype = "";
    fmode =  strtok(NULL, " ");
    if (fmode == NULL) fmode = "";
    

/*
 Now build the SVC 202 string
*/
    memcpy ( &s202parm[0] , "FILEDEF ", 8);
    memcpy ( &s202parm[8] , fdddname, 8);
    if(console)
    {
        memcpy ( &s202parm[16] , "TERMINAL", 8);
        memcpy ( &s202parm[24] , "(       " , 8 );
        memcpy ( &s202parm[32] , "RECFM   " , 8 );
        memcpy ( &s202parm[40] , "V       " , 8 );
        memcpy ( &s202parm[48] , "LRECL   " , 8 );
        memcpy ( &s202parm[56] , "80      " , 8 );
        s202parm[64]=s202parm[65]=s202parm[66]=s202parm[67]=
            s202parm[68]=s202parm[69]=s202parm[70]=s202parm[71]=0xff;
    }
    else
    {
        memcpy ( &s202parm[16] , "DISK    ", 8);
/*
  Clear PARMS area
*/
        memcpy ( &s202parm[24] , "        " , 8);
        memcpy ( &s202parm[32] , "        " , 8);
        if (mymode)
        {
            memcpy ( &s202parm[40] , "A1      " , 8);
            if (fmode[0] != '\0')
            {
                memcpy ( &s202parm[40] , fmode, strlen(fmode));
            }
        }
        else
        {
            memcpy ( &s202parm[40] , "*       " , 8);
            memcpy ( &s202parm[40] , fmode , strlen(fmode) );
        }

        memcpy ( &s202parm[24] , fname ,
                 ( strlen(fname) > 8 ) ? 8 : strlen(fname)  );
        memcpy ( &s202parm[32] , ftype ,
                 ( strlen(ftype) >8 ) ? 8 : strlen(ftype) );
        if ( mymode )
        {
             memcpy ( &s202parm[48] , "(       " , 8 );
             memcpy ( &s202parm[56] , "RECFM   " , 8 );
             memcpy ( &s202parm[64] , "V       " , 8 );
             memcpy ( &s202parm[72] , "LRECL   " , 8 );
             memcpy ( &s202parm[80] , "2000    " , 8 );
             if (modeType == 5)
             {
                 memcpy ( &s202parm[64] , "F       " , 8 );
                 memcpy ( &s202parm[80] , "800     " , 8 );
             }
             s202parm[88]=s202parm[89]=s202parm[90]=s202parm[91]=
                 s202parm[92]=s202parm[93]=s202parm[94]=s202parm[95]=0xff;
        }
        else
        {
             s202parm[48]=s202parm[49]=s202parm[50]=s202parm[51]=
                 s202parm[52]=s202parm[53]=s202parm[54]=s202parm[55]=0xff;
        }
    }
    __SVC202 ( s202parm, &code, &parm );
}

static void fdclr(char *ddname)
{
    char s202parm [800];
    int code;
    int parm;

    /* build the SVC 202 string */
    memcpy( &s202parm[0] , "FILEDEF ", 8);
    memcpy( &s202parm[8] , ddname, 8);
    memcpy( &s202parm[16] , "CLEAR   ", 8);
    memset( &s202parm[24], 0xff, 8);

    __SVC202 ( s202parm, &code, &parm );
    return;
}

static char *int_strtok(char *s1, const char *s2)
{
    static char *old = NULL;
    char *p;
    size_t len;
    size_t remain;

    if (s1 != NULL) old = s1;
    if (old == NULL) return (NULL);
    p = old;
    len = strspn(p, s2);
    remain = strlen(p);
    if (remain <= len) { old = NULL; return (NULL); }
    p += len;
    len = strcspn(p, s2);
    remain = strlen(p);
    if (remain <= len) { old = NULL; return (p); }
    *(p + len) = '\0';
    old = p + len + 1;
    return (p);
}

#endif


/*

 The truely cludged piece of code was concocted by Dave Wade

 His erstwhile tutors are probably turning in their graves.

 It is however placed in the Public Domain so that any one
 who wishes to improve is free to do so

*/

static void dblcvt(double num, char cnvtype, size_t nwidth,
            int nprecision, char *result)
{
    double b,round;
    int i,j,exp,pdigits,format;
    char sign, work[45];

    /* save original data & set sign */

    if ( num < 0 )
    {
        b = -num;
        sign = '-';
    }
    else
    {
        b = num;
        sign = ' ';
    }

    /*
      Now scale to get exponent
    */

    exp = 0;
    if( b > 1.0 )
    {
        while ((b >= 10.0) && (exp < 35))
        {
            ++exp;
            b=b / 10.0;
        }
    }
    else if ( b == 0.0 )
    {
        exp=0;
    }
    /* 1.0 will get exp = 0 */
    else if ( b < 1.0 )
    {
        while ((b < 1.0) && (exp > -35))
        {
            --exp;
            b=b*10.0;
        }
    }
    if ((exp <= -35) || (exp >= 35))
    {
        exp = 0;
        b = 0.0;
    }
    
    /*
      now decide how to print and save in FORMAT.
         -1 => we need leading digits
          0 => print in exp
         +1 => we have digits before dp.
    */

    switch (cnvtype)
    {
        case 'E':
        case 'e':
            format = 0;
            break;
        case 'f':
        case 'F':
            if ( exp >= 0 )
            {
                format = 1;
            }
            else
            {
                format = -1;
            }
            break;
        default:
            /* Style e is used if the exponent from its
               conversion is less than -4 or greater than
               or equal to the precision.
            */
            if ( exp >= 0 )
            {
                if ( nprecision > exp )
                {
                    format=1;
                }
                else
                {
                    format=0;
                }
            }
            else
            {
                /*  if ( nprecision > (-(exp+1) ) ) { */
                if ( exp >= -4)
                {
                    format=-1;
                }
                else
                {
                    format=0;
                }
            }
            break;
    }
    /*
    Now round
    */
    switch (format)
    {
        case 0:    /* we are printing in standard form */
            if (nprecision < DBL_MANT_DIG) /* we need to round */
            {
                j = nprecision;
            }
            else
            {
                j=DBL_MANT_DIG;
            }
            round = 1.0/2.0;
            i = 0;
            while (++i <= j)
            {
                round = round/10.0;
            }
            b = b + round;
            if (b >= 10.0)
            {
                b = b/10.0;
                exp = exp + 1;
            }
            break;

        case 1:      /* we have a number > 1  */
                         /* need to round at the exp + nprecisionth digit */
                if (exp + nprecision < DBL_MANT_DIG) /* we need to round */
                {
                    j = exp + nprecision;
                }
                else
                {
                    j = DBL_MANT_DIG;
                }
                round = 0.5;
                i = 0;
                while (i++ < j)
                {
                    round = round/10;
                }
                b = b + round;
                if (b >= 10.0)
                {
                    b = b/10.0;
                    exp = exp + 1;
                }
                break;

        case -1:   /* we have a number that starts 0.xxxx */
            if (nprecision < DBL_MANT_DIG) /* we need to round */
            {
                j = nprecision + exp + 1;
            }
            else
            {
                j = DBL_MANT_DIG;
            }
            round = 5.0;
            i = 0;
            while (i++ < j)
            {
                round = round/10;
            }
            if (j >= 0)
            {
                b = b + round;
            }
            if (b >= 10.0)
            {
                b = b/10.0;
                exp = exp + 1;
            }
            if (exp >= 0)
            {
                format = 1;
            }
            break;
    }
    /*
       Now extract the requisite number of digits
    */

    if (format==-1)
    {
        /*
             Number < 1.0 so we need to print the "0."
             and the leading zeros...
        */
        result[0]=sign;
        result[1]='0';
        result[2]='.';
        result[3]=0x00;
        while (++exp)
        {
            --nprecision;
            strcat(result,"0");
        }
        i=b;
        --nprecision;
        work[0] = (char)('0' + i % 10);
        work[1] = 0x00;
        strcat(result,work);

        pdigits = nprecision;

        while (pdigits-- > 0)
        {
            b = b - i;
            b = b * 10.0;
            i = b;
            work[0] = (char)('0' + i % 10);
            work[1] = 0x00;
            strcat(result,work);
        }
    }
    /*
       Number >= 1.0 just print the first digit
    */
    else if (format==+1)
    {
        i = b;
        result[0] = sign;
        result[1] = '\0';
        work[0] = (char)('0' + i % 10);
        work[1] = 0x00;
        strcat(result,work);
        nprecision = nprecision + exp;
        pdigits = nprecision ;

        while (pdigits-- > 0)
        {
            if ( ((nprecision-pdigits-1)==exp)  )
            {
                strcat(result,".");
            }
            b = b - i;
            b = b * 10.0;
            i = b;
            work[0] = (char)('0' + i % 10);
            work[1] = 0x00;
            strcat(result,work);
        }
    }
    /*
       printing in standard form
    */
    else
    {
        i = b;
        result[0] = sign;
        result[1] = '\0';
        work[0] = (char)('0' + i % 10);
        work[1] = 0x00;
        strcat(result,work);
        strcat(result,".");

        pdigits = nprecision;

        while (pdigits-- > 0)
        {
            b = b - i;
            b = b * 10.0;
            i = b;
            work[0] = (char)('0' + i % 10);
            work[1] = 0x00;
            strcat(result,work);
        }
    }

    if (format==0)
    { /* exp format - put exp on end */
        work[0] = 'E';
        if ( exp < 0 )
        {
            exp = -exp;
            work[1]= '-';
        }
        else
        {
            work[1]= '+';
        }
        work[2] = (char)('0' + (exp/10) % 10);
        work[3] = (char)('0' + exp % 10);
        work[4] = 0x00;
        strcat(result, work);
    }
    else
    {
        /* get rid of trailing zeros for g specifier */
        if (cnvtype == 'G' || cnvtype == 'g')
        {
            char *p;
            
            p = strchr(result, '.');
            if (p != NULL)
            {
                p++;
                p = p + strlen(p) - 1;                
                while (*p != '.' && *p == '0')
                {
                    *p = '\0';
                    p--;
                }
                if (*p == '.')
                {
                    *p = '\0';
                }
            }
        }
     }
    /* printf(" Final Answer = <%s> fprintf gives=%g\n",
                result,num); */
    /*
     do we need to pad
    */
    if(result[0] == ' ')strcpy(work,result+1); else strcpy(work,result);
    pdigits=nwidth-strlen(work);
    result[0]= 0x00;
    while(pdigits>0)
    {
        strcat(result," ");
        pdigits--;
    }
    strcat(result,work);
    return;
}
