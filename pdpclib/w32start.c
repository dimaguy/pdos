/*********************************************************************/
/*                                                                   */
/*  This Program Written by Paul Edwards.                            */
/*  Released to the Public Domain                                    */
/*                                                                   */
/*********************************************************************/
/*********************************************************************/
/*                                                                   */
/*  w32start - startup code for WIN32                                */
/*                                                                   */
/*********************************************************************/

/* This is the main entry point of a console mode executable */

#include <stdlib.h>

void mainCRTStartup(void)
{
    __start(0);
    return;
}

void __main(void)
{
    return;
}

/* This function is insufficient to replace the GNU function */
#if 0
void *_alloca(int size)
{
    return (malloc(size));
}
#endif
