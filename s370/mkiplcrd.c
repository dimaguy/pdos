/*********************************************************************/
/*                                                                   */
/*  This Program Written by Paul Edwards.                            */
/*  Released to the Public Domain                                    */
/*                                                                   */
/*********************************************************************/
/*********************************************************************/
/*                                                                   */
/*  mkiplcrd - make an card deck containing the PDOS OS that can be  */
/*  IPLed                                                            */
/*                                                                   */
/*********************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* sapstart is expecting that 18432 bytes will have been loaded
   before it gets control (when dealing with cards). This is
   256 cards with usable data (72 bytes). We'll call the cards
   that load these cards the IPL4 CCW chain.

   In order to read 256 cards, ie 256 CCWs, you need 256/9 = 28.x = 29
   cards containing CCWs. We'll call the cards that load these cards
   the IPL3 CCW chain.

   In order to read those 29 cards you need 29/9 = 3.x = 4 cards.
   We'll call the card that loads these cards the IPL2 CCW chain.

   In order to read those 4 cards, you need 1 card, which is the
   only card loaded by the IPL1 CCW chain (which normally only
   has 2 CCWs).

   IPL2+3+4 are simply loaded consecutively (with allowance for
   the last card in each IPL chain not having a full set of CCWs),
   so no TIC is required, and the last CCW simply needs to stop chaining.
*/


/* where data (last set of CCWs) is supposed to be loaded */
#define LOADLOC 0
/* sapstart is expecting this much to be loaded - multiple of 72 */
#define CCHUNKSZ 18432
/* load the CCWs at the 1 MiB location */
#define CCWLOC 0x100000


static char ccw[24] =
    "\x00\x00\x00\x00" "\x00\x00\x00\x00" /* IPL PSW - not used */
    "\x02\x10\x00\x00" /* first CCW - read to address 1 MiB */
    "\x60\x00\x00\x50" /* first CCW - ignore length errors, chain, length 80 */
    "\x08\x10\x00\x00" "\x00\x00\x00\x00" /* 2nd CCW - TIC (jump) to 1 MiB */
    ;

int main(int argc, char **argv)
{
    int i;
    FILE *fq;
    FILE *fp;
    char *buf;
    long imgsize;
    int level;
    int numlevels;
    long levellen;

    if (argc <= 1)
    {
        printf("usage: mkiplcrd <output file>\n");
        printf("e.g. mkiplcrd dev10004:\n");
        return (EXIT_FAILURE);
    }
    buf = malloc(18432);
    if (buf == NULL)
    {
        printf("couldn't allocate 18432 bytes\n");
        return (EXIT_FAILURE);
    }
    fq = fopen(*(argv + 1), "wb");
    if (fq == NULL)
    {
        printf("failed to open tape %s\n", *(argv + 1));
        return (EXIT_FAILURE);
    }
    numlevels = 0;
    levellen = CCHUNKSZ;
    while (levellen > 72)
    {
        int rem;

        /* there are 9 CCWs that can fit into a card. Each
           of those CCWs represent the ability to read 72 bytes
           of usable data */
        rem = levellen % (9 * 72);
        levellen /= (9 * 72);
        if (rem != 0)
        {
            /* we are dealing with bytes, and we need another full card */
            levellen += 72;
        }
        numlevels++;
    }
    /* IPL1 has already 
    for (level = 2; level < numlevels; level++)
    {
    }
    loadaddr = CCWLOC + 72;
    currlevellen = 72; /* this is too low, and will be adjusted */
    while (1)
    {
        if (currlevellen == CCHUNKSZ)
        {
            loadaddr = LOADLOC;
        }
        else
        {
            /* we need to know higher level length */
            higherlen = CCHUNKSZ;
            while (1)
            {
                /* there are 9 CCWs that can fit into a card. Each
                   of those CCWs represent the ability to read 72 bytes
                   of usable data */
                rem = higherlen % (9 * 72);
                q = higherlen / (9 * 72);
                q2 = q;
                if (rem != 0)
                {
                    /* we are dealing with bytes, and we need another
                       full card */
                    q2 += 72;
                }
                if (q2 == currlevellen) break;
                higherlen = q2;
            }
        }
        numlevels++;
    }
        }
        z = 0;
        i = 0;
        while (z < currlevellen)
        {
            if (i == 0)
            {
                memset(minicard, '\0', sizeof minicard);
            }
            if ((currlevellen == CCHUNKSZ) && (z == (currlevellen - 72)))
            {
                /* stop chaining */
                minicard[i * 8 + 4] = 0x20;
            }
            else
            {
                minicard[i * 8 + 4] = 0x60;
            }
            minicard[i * 8 + 0] = 0x02;
            minicard[i * 8 + 1] = (loadaddr >> 16) & 0xff;
            minicard[i * 8 + 2] = (loadaddr >> 8) & 0xff;
            minicard[i * 8 + 3] = loadaddr & 0xff;
            minicard[i * 8 + 7] = 0x50;
            i++;
            z += 72;
            if (i == 9)
            {
                writecard(minicard);
                i = 0;
            }
        }
        if (i != 0)
        {
            writecard(minicard);
        }
        if (currlevellen == CCHUNKSZ)
        {
            break;
        }
        currlevellen = higherlen;
    }
    fp = fopen("PDOS.IMG", "rb");
    if (fp == NULL)
    {
        printf("failed to open PDOS.IMG for reading\n");
        return (EXIT_FAILURE);
    }

    fseek(fp, 0, SEEK_END);
    imgsize = ftell(fp);
    printf("imgsize is %ld\n", imgsize);
    rewind(fp);
#if 0
    fwrite(ccw, 1, sizeof ccw, fq);

    i = fread(buf, 1, 18432, fp);
    /* set the IPL PSW to point to where it is located */
    *(int *)(buf + 4) = *(int *)(buf + 8 + 8192);
    fwrite(buf, 1, i + 4, fq);

    while ((i = fread(buf + 4, 1, 18452, fp)) > 0)
    {
        *(short *)buf = i + 4;
        fwrite(buf, 1, i + 4, fq);
    }
    fclose(fp);

    fp = fopen("CONFIG.SYS", "rb");
    if (fp == NULL)
    {
        printf("failed to open CONFIG.SYS for reading\n");
        return (EXIT_FAILURE);
    }

    i = fread(buf + 4, 1, 18452, fp);
    *(short *)buf = i + 4;
    fwrite(buf, 1, i + 4, fq);
    fclose(fp);

    fp = fopen("COMMAND.EXE", "rb");
    if (fp == NULL)
    {
        printf("failed to open COMMAND.EXE for reading\n");
        return (EXIT_FAILURE);
    }
    while ((i = fread(buf + 4, 1, 18452, fp)) > 0)
    {
        *(short *)buf = i + 4;
        fwrite(buf, 1, i + 4, fq);
    }
    fclose(fp);

    if (ferror(fq))
    {
        printf("write error\n");
        return (EXIT_FAILURE);
    }
    fclose(fq);
#endif
    printf("finished writing output file\n");
    return (EXIT_SUCCESS);
}
