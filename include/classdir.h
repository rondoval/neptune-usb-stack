#ifndef NEPTUNE_CLASSDIR_H
#define NEPTUNE_CLASSDIR_H
/*
 * One definition of "load every class driver in the class drawer", shared by
 * USBStart, USBAddClasses and NeptuneUSB.
 *
 * A class already in the stack is left alone, so this is safe to run on a
 * stack that came up from ROM with some classes resident, or more than once.
 * It only loads: binding the new classes to devices is the caller's
 * psdClassScan() (or psdParseCfg(), which ends in one).
 *
 * The includer provides exec, dos and neptune.library protos; `ps` is the
 * neptune.library base (NEPTUNE_BASE_NAME is `ps` tree-wide).
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/exall.h>
#include <string.h>
#include <nep_paths.h>  /* NEP_CLASSDRAWER, NEP_CLASSSUFFIX */

#define NEP_CLASSNAMEMAX 128

/* Returns FALSE if the drawer could not be read. `verbose` prints one line
   per file, as USBAddClasses does. */
static inline BOOL psdAddClassDir(struct Library *ps, BOOL verbose)
{
    UBYTE buf[1024];
    UBYTE sbuf[NEP_CLASSNAMEMAX];
    struct ExAllControl *exall;
    struct ExAllData *exdata;
    struct List *puclist;
    struct Node *puc;
    BPTR lock;
    ULONG ents;
    ULONG namelen;
    BOOL exready;
    BOOL loaded;

    if(!(exall = AllocDosObject(DOS_EXALLCONTROL, NULL)))
    {
        return(FALSE);
    }
    if(!(lock = Lock(NEP_CLASSDRAWER, ACCESS_READ)))
    {
        FreeDosObject(DOS_EXALLCONTROL, exall);
        return(FALSE);
    }

    exall->eac_LastKey = 0;
    exall->eac_MatchString = NULL;
    exall->eac_MatchFunc = NULL;
    do
    {
        exready = ExAll(lock, (struct ExAllData *) buf, 1024, ED_NAME, exall);
        exdata = (struct ExAllData *) buf;
        ents = exall->eac_Entries;
        for(; ents--; exdata = exdata->ed_Next)
        {
            /* Class drivers only: icons, other stacks' classes (Poseidon's are
               "<name>.class") and anything else in the drawer are skipped. */
            namelen = strlen((const char *) exdata->ed_Name);
            if((namelen <= sizeof(NEP_CLASSSUFFIX) - 1) ||
               stricmp((const char *) &exdata->ed_Name[namelen - (sizeof(NEP_CLASSSUFFIX) - 1)],
                       NEP_CLASSSUFFIX))
            {
                continue;
            }
            psdSafeRawDoFmt(sbuf, NEP_CLASSNAMEMAX, NEP_CLASSDRAWER "/%s", exdata->ed_Name);

            psdGetAttrs(PGA_STACK, NULL, PA_ClassList, &puclist, TAG_END);
            Forbid();
            puc = puclist->lh_Head;
            while(puc->ln_Succ)
            {
                if(!strncmp(puc->ln_Name, exdata->ed_Name, strlen(puc->ln_Name)))
                {
                    break;
                }
                puc = puc->ln_Succ;
            }
            loaded = (puc->ln_Succ != NULL);
            Permit();
            if(loaded)
            {
                if(verbose)
                {
                    Printf("Skipping class %s...\n", exdata->ed_Name);
                }
                continue;
            }

            if(verbose)
            {
                Printf("Adding class %s...", exdata->ed_Name);
            }
            if(psdAddClass(sbuf, 0))
            {
                if(verbose)
                {
                    PutStr("okay!\n");
                }
            } else {
                if(verbose)
                {
                    PutStr("failed!\n");
                }
            }
        }
    } while(exready);

    UnLock(lock);
    FreeDosObject(DOS_EXALLCONTROL, exall);
    return(TRUE);
}

#endif /* NEPTUNE_CLASSDIR_H */
