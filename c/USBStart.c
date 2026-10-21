/*
** USBStart starts the stack: it is the one command the startup needs.
**
** What should run comes from ENVARC:Sys/neptune.prefs (on first start, imported
** from poseidon.prefs and saved). Where the prefs say nothing, defaults stand in:
** every class in DEVS:USBClasses, and the host controller named on the command
** line. USBAddClasses and USBAddHardware do those two jobs by hand; they are not
** needed at startup.
**
** Based on Poseidon's PsdStackLoader by Chris Hodges <chrisly@platon42.de>.
*/

#include <string.h>
#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <libraries/neptune.h>
#include <proto/neptune.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <neptune_version.h>
#include <classdir.h>
#include <nep_otherstack.h>

#define ARGS_DEVICE   0
#define ARGS_UNIT     1
#define ARGS_SIZEOF   2

static const char *template = "DEVICE,UNIT/N";
const char *nep_version = NEP_VER("USBStart");

/* TRUE if no host controller is attached to the stack. */
static BOOL noHardware(struct Library *ps)
{
    struct List *phwlist;
    BOOL none;

    psdLockReadPBase();
    psdGetAttrs(PGA_STACK, NULL, PA_HardwareList, &phwlist, TAG_END);
    none = (phwlist->lh_Head->ln_Succ == NULL);
    psdUnlockPBase();

    return(none);
}

int main(void)
{
    IPTR ArgsArray[ARGS_SIZEOF] = { 0, 0 };
    struct RDArgs *ArgsHook;
    struct Library *ps;
    int ret = RETURN_OK;

    if(!(ArgsHook = ReadArgs(template, ArgsArray, NULL)))
    {
        PutStr("Wrong arguments!\n");
        return(RETURN_FAIL);
    }

    if((ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        UWORD other = nepPoseidonLoaded();
        if(other)
        {
            /* The startup line runs >NIL:, so the stack log is where this is read.
               RETURN_WARN: below the default FailAt, the rest of the startup runs. */
            psdAddErrorMsg(RETURN_ERROR, "USBStart", NEP_OTHERSTACK_TXT, (LONG) other);
            Printf(NEP_OTHERSTACK_TXT "\n", (LONG) other);
            CloseLibrary(ps);
            FreeArgs(ArgsHook);
            return RETURN_WARN;
        }

        /* No prefs file is not an error: the defaults below cover it. */
        psdLoadCfgFromDisk(NULL);

        /* "The prefs exist" does not mean "the prefs say what to run". The
           controller and class lists are written by NeptuneUSB only; prefs saved
           from a class settings window carry neither. So the defaults are
           keyed on what the config lists, not on whether a file was found. */
        APTR pic = psdFindCfgForm(NULL, IFFFORM_STACKCFG);

        if(!(pic && psdFindCfgForm(pic, IFFFORM_USBCLASS)))
        {
            if(!psdAddClassDir(ps, FALSE))
            {
                PutStr("Failed to lock " NEP_CLASSDRAWER ".\n");
                ret = RETURN_WARN;
            }
        }

        /* A stack started from ROM has its controller already; so has one whose
           prefs list it, once they are applied below. */
        if(ArgsArray[ARGS_DEVICE] && noHardware(ps) &&
           !(pic && psdFindCfgForm(pic, IFFFORM_UHWDEVICE)))
        {
            STRPTR devname = (STRPTR) ArgsArray[ARGS_DEVICE];
            ULONG unit = ArgsArray[ARGS_UNIT] ? *((ULONG *) ArgsArray[ARGS_UNIT]) : 0;
            APTR phw = psdAddHardware(devname, unit);

            if(phw && !psdEnumerateHardware(phw))
            {
                psdRemHardware(phw);
                phw = NULL;
            }
            if(!phw)
            {
                Printf("Unable to start %s unit %ld.\n", devname, unit);
                ret = RETURN_WARN;
            }
        }

        /* Applies what the prefs list and ends in the class scan. That scan is
           the first one from a process, which is what hands the keyboard and
           mouse of a ROM-started stack over from the boot classes to hid.class
           - so the classes have to be loaded before this point. */
        psdParseCfg();

        CloseLibrary(ps);
    } else {
        PutStr("Unable to open neptune.library\n");
        ret = RETURN_FAIL;
    }

    FreeArgs(ArgsHook);
    return(ret);
}
