/*********************************************************************/
/*                                                                   */
/*  This Program Written By Paul Edwards.                            */
/*  Released to the public domain.                                   */
/*                                                                   */
/*********************************************************************/

#ifndef MEMMGR_INCLUDED
#define MEMMGR_INCLUDED

#include <stddef.h>

/* #define __MEMMGR_INTEGRITY 1 */

typedef struct memmgrn {
#ifdef __MEMMGR_INTEGRITY
    int eyecheck1;
#endif
    struct memmgrn *next;
    struct memmgrn *prev;
    int fixed;
    size_t size;
    int allocated;
    int id;
#ifdef __MEMMGR_INTEGRITY
    int eyecheck2;
#endif
    size_t filler; /* we add this so that *(p - size_t) is writable */
} MEMMGRN;

typedef struct {
    MEMMGRN *start;
} MEMMGR;

/* What boundary we want the memmgr control block to be a multiple of */
#define MEMMGR_ALIGN 8

#define MEMMGRN_SZ \
  ((sizeof(MEMMGRN) % MEMMGR_ALIGN == 0) ? \
   sizeof(MEMMGRN) : \
   ((sizeof(MEMMGRN) / MEMMGR_ALIGN + 1) * MEMMGR_ALIGN))

/* what we would like chunks of memory, including room for the
   control block, to be as a minimum */
#define MEMMGR_MINFREE_INTERNAL 64

#define MEMMGR_MINFREE (MEMMGRN_SZ < MEMMGR_MINFREE_INTERNAL ? \
    MEMMGR_MINFREE_INTERNAL - MEMMGRN_SZ : MEMMGRN_SZ)

#define memmgrDefaults __mmDef
#define memmgrInit __mmInit
#define memmgrTerm __mmTerm
#define memmgrSupply __mmSupply
#define memmgrAllocate __mmAlloc
#define memmgrFree __mmFree
#define memmgrFreeId __mmFId
#define memmgrMaxSize __mmMaxSize
#define memmgrIntegrity __mmIntegrity
#define memmgrRealloc __mmRealloc

void memmgrDefaults(MEMMGR *memmgr);
void memmgrInit(MEMMGR *memmgr);
void memmgrTerm(MEMMGR *memmgr);
void memmgrSupply(MEMMGR *memmgr, void *buffer, size_t szbuf);
void *memmgrAllocate(MEMMGR *memmgr, size_t bytes, int id);
void memmgrFree(MEMMGR *memmgr, void *ptr);
void memmgrFreeId(MEMMGR *memmgr, int id);
size_t memmgrMaxSize(MEMMGR *memmgr);
void memmgrIntegrity(MEMMGR *memmgr);
int memmgrRealloc(MEMMGR *memmgr, void *ptr, size_t newsize);

extern MEMMGR __memmgr;

#endif
