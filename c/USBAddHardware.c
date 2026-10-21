/*
** USBAddHardware: attach or remove a host-controller device by hand.
**
** Based on Poseidon's AddUSBHardware by Chris Hodges <chrisly@platon42.de>.
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
#include <hwmatch.h>
#include <nep_otherstack.h>

#define ARGS_DEVICE   0
#define ARGS_UNIT     1
#define ARGS_QUIET    2
#define ARGS_REMOVE   3
#define ARGS_ALL      4
#define ARGS_SIZEOF   5

static const char *template = "DEVICE,UNIT/N,QUIET/S,REMOVE/S,ALL/S";
const char *version = NEP_VER("USBAddHardware");
static IPTR ArgsArray[ARGS_SIZEOF];
static struct RDArgs *ArgsHook = NULL;

//extern struct DOSBase *DOSBase;

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

/* The hardware entry for this device/unit, or NULL - used by both the ADD and the
 * REMOVE path, so there is one answer to "which controller is that?".
 *
 * Identity comes from <hwmatch.h>, the same predicate the library's own
 * pFindHardware() compares with, so a prefs entry like
 * DEVS:USBHardware/xhci-context.device matches a bare xhci-context.device.
 */
static struct Node *findHardware(struct Library *ps, STRPTR devname, ULONG unit)
{
    struct List *phwlist;
    struct Node *phw;

    psdLockReadPBase();
    psdGetAttrs(PGA_STACK, NULL, PA_HardwareList, &phwlist, TAG_END);
    for(phw = phwlist->lh_Head; phw->ln_Succ; phw = phw->ln_Succ)
    {
        STRPTR cmpdevname = NULL;
        ULONG cmpunit = 0;

        psdGetAttrs(PGA_HARDWARE, phw,
                    HA_DeviceName, &cmpdevname,
                    HA_DeviceUnit, &cmpunit,
                    TAG_END);
        if(psdHwMatch(cmpdevname, cmpunit, devname, unit))
        {
            break;
        }
    }
    psdUnlockPBase();

    return phw->ln_Succ ? phw : NULL;
}

int main(int argc, char *argv[])
{
    struct Library *ps;
    char *errmsg = NULL;
    struct List *phwlist;
    struct Node *phw;
    ULONG unit;
    STRPTR devname = NULL;
    ULONG cmpunit;
    STRPTR cmpdevname;

    if(!(ArgsHook = ReadArgs(template, ArgsArray, NULL)))
    {
        fail("Wrong arguments!\n");
    }

    if((!ArgsArray[ARGS_DEVICE]) && (!(ArgsArray[ARGS_REMOVE] && ArgsArray[ARGS_ALL])))
    {
        fail("DEVICE argument is mandatory except for REMOVE ALL!\n");
    }
    
    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        unit = 0;
        if(ArgsArray[ARGS_DEVICE])
        {
            devname = (STRPTR) ArgsArray[ARGS_DEVICE];
        }
        if(ArgsArray[ARGS_UNIT])
        {
            unit = *((ULONG *) ArgsArray[ARGS_UNIT]);
        }
        if(ArgsArray[ARGS_REMOVE])
        {
            if(ArgsArray[ARGS_ALL])
            {
                /* No matching involved: take the head until the list is empty. */
                for(;;)
                {
                    psdLockReadPBase();
                    psdGetAttrs(PGA_STACK, NULL, PA_HardwareList, &phwlist, TAG_END);
                    phw = phwlist->lh_Head;
                    if(!phw->ln_Succ)
                    {
                        psdUnlockPBase();
                        break;
                    }
                    psdGetAttrs(PGA_HARDWARE, phw,
                                HA_DeviceName, &cmpdevname,
                                HA_DeviceUnit, &cmpunit,
                                TAG_END);
                    psdUnlockPBase();
                    if(!ArgsArray[ARGS_QUIET])
                    {
                        Printf("Removing hardware %s, unit %ld...\n", cmpdevname, cmpunit);
                    }
                    psdRemHardware(phw);
                }
            }
            else if((phw = findHardware(ps, devname, unit)))
            {
                if(!ArgsArray[ARGS_QUIET])
                {
                    Printf("Removing hardware %s, unit %ld...\n", devname, unit);
                }
                psdRemHardware(phw);
            }
        } else {
            UWORD other = nepPoseidonLoaded();
            if(other)
            {
                Printf(NEP_OTHERSTACK_TXT "\n", (LONG) other);
                CloseLibrary(ps);
                FreeArgs(ArgsHook);
                return RETURN_WARN;
            }
            do
            {
                /* Skipping rather than failing: with ALL this walks on to the next
                   unit, and the loop still terminates on the first unit that is
                   neither present nor addable. */
                if(findHardware(ps, devname, unit))
                {
                    if(!ArgsArray[ARGS_QUIET])
                    {
                        Printf("Hardware %s, unit %ld is already added, skipping.\n",
                               devname, unit);
                    }
                    unit++;
                    continue;
                }
                if(!ArgsArray[ARGS_QUIET])
                {
                    Printf("Adding hardware %s, unit %ld...", devname, unit);
                }
                if((phw = psdAddHardware(devname, unit)))
                {
                    if(!ArgsArray[ARGS_QUIET])
                    {
                        if(psdEnumerateHardware(phw))
                        {
                            PutStr("okay!\n");
                        } else {
                            PutStr("enumeration failed!\n");
                        }
                    } else {
                        psdEnumerateHardware(phw);
                    }
                } else {
                    if(!ArgsArray[ARGS_QUIET])
                    {
                        PutStr("failed!\n");
                    }
                    errmsg = "";
                    break;
                }
                unit++;
            } while(ArgsArray[ARGS_ALL]);
            psdClassScan();
        }
        CloseLibrary(ps);
    } else {
        errmsg = "Unable to open neptune.library\n";
    }
    fail(errmsg);
    return(0); // never gets here, just to shut the compiler up
}
