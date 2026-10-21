/*
** USBAddClasses: load the classes in DEVS:USBClasses, or remove them, by hand.
**
** Based on Poseidon's AddUSBClasses by Chris Hodges <chrisly@platon42.de>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <exec/exec.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
#include <dos/exall.h>
#include <libraries/neptune.h>
#include <proto/neptune.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <neptune_version.h>
#include <classdir.h>
#include <nep_otherstack.h>

#define ARGS_QUIET      0
#define ARGS_REMOVE     1
#define ARGS_SIZEOF     2

static const char *template = "QUIET/S,REMOVE/S";
const char *version = NEP_VER("USBAddClasses") ", (C) The AROS Development Team";
static IPTR ArgsArray[ARGS_SIZEOF];
static struct RDArgs *ArgsHook = NULL;

void fail(char *str)
{
    if(ArgsHook)
    {
        FreeArgs(ArgsHook);
        ArgsHook = NULL;
    }
    if(str)
    {
        PutStr(str);
        exit(20);
    }
    exit(0);
}

int main(int argc, char *argv[])
{
    struct Library      *ps;
    STRPTR              errmsg = NULL;
    struct List         *puclist;

    if(!(ArgsHook = ReadArgs(template, ArgsArray, NULL)))
    {
        fail("Wrong arguments!\n");
    }
    
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        if(ArgsArray[ARGS_REMOVE])
        {
            psdLockWritePBase();
            psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &puclist, TAG_END);
            while(puclist->lh_Head->ln_Succ)
            {
                if(!ArgsArray[ARGS_QUIET])
                {
                    Printf("Removing class %s...\n", puclist->lh_Head->ln_Name);
                }
                psdUnlockPBase();
                psdRemClass(puclist->lh_Head);
                psdLockWritePBase();
            }
            psdUnlockPBase();
        } else {
            UWORD other = nepPoseidonLoaded();
            if(other)
            {
                Printf(NEP_OTHERSTACK_TXT "\n", (LONG) other);
                CloseLibrary(ps);
                FreeArgs(ArgsHook);
                return RETURN_WARN;
            }
            if(psdAddClassDir(ps, !ArgsArray[ARGS_QUIET]))
            {
                psdClassScan();
            } else {
                errmsg = "Failed to lock " NEP_CLASSDRAWER ".\n";
            }
        }
        CloseLibrary(ps);
    } else {
        errmsg = "Unable to open neptune.library\n";
    }
    fail(errmsg);
    return(0); // never gets here, just to shut the compiler up
}
