/*
 * neptune.library - library skeleton (romtag, init/open/close/expunge, LVO
 * funcTable). Hand-written replacement for AROS genmodule. Bebbo gcc, freestanding.
 *
 * The real init/open/expunge logic lives in neptune.library.c (libInit/libOpen/
 * libExpunge); this file only wraps them in the standard Exec library vectors and
 * publishes the jump table in neptune.sfd order.
 */

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/execbase.h>

#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>

#include "neptune_intern.h"          /* struct PsdBase */

#define LIBRARY_PRIORITY (-44)

extern const char libname[];          /* "neptune.library", in neptune.library.c */
static const char libIdString[] = VERSION_STRING;

/* the real hooks (in neptune.library.c) */
extern int libInit(struct PsdBase *ps);
extern int libOpen(struct PsdBase *ps);
extern int libExpunge(struct PsdBase *ps);

/* forward decls */
extern const UBYTE endOfCode;
static const APTR initTable[4];

/* Refuse to run if someone tries to execute the library as a program. */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void) { return -1; }

static void freeBase(struct PsdBase *base)
{
    ULONG size = (ULONG)base->ps_Library.lib_NegSize + base->ps_Library.lib_PosSize;
    FreeMem((APTR)((ULONG)base - base->ps_Library.lib_NegSize), size);
}

static struct PsdBase *LibInit(struct PsdBase *base   asm("d0"),
                               BPTR            seglist asm("a0"),
                               struct ExecBase *sysbase asm("a6"))
{
    (void)sysbase;
    base->ps_SegList = seglist;
    base->ps_Library.lib_Revision = NEPTUNE_REVISION;

    if(libInit(base))
        return base;

    freeBase(base);
    return NULL;
}

struct PsdBase *LibOpen(struct PsdBase *base asm("a6"))
{
    base->ps_Library.lib_OpenCnt++;
    base->ps_Library.lib_Flags &= ~LIBF_DELEXP;
    if(!libOpen(base)) {
        base->ps_Library.lib_OpenCnt--;
        return NULL;
    }
    return base;
}

/**
 * LibExpunge() - Free the library, or defer while it cannot go.
 * @base: library base (A6)
 *
 * libExpunge() tears down and Remove()s the lib node, or refuses with nothing torn down.
 *
 * Return: the seglist for exec to unload, 0 if the library stays.
 */
BPTR LibExpunge(struct PsdBase *base asm("a6"))
{
    if(base->ps_Library.lib_OpenCnt > 0 || !libExpunge(base)) {
        base->ps_Library.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    BPTR seglist = base->ps_SegList;
    freeBase(base);
    return seglist;
}

BPTR LibClose(struct PsdBase *base asm("a6"))
{
    if(--base->ps_Library.lib_OpenCnt == 0 &&
       (base->ps_Library.lib_Flags & LIBF_DELEXP))
        return LibExpunge(base);
    return 0;
}

ULONG LibNull(void) { return 0; }

/* The LVO jump table lives in neptune.library.c, below the psd* definitions - see the
   comment there.  The four std vectors above are exported for it. */
extern const APTR psdFuncTable[];

static const APTR initTable[4] = {
    (APTR)sizeof(struct PsdBase),
    (APTR)psdFuncTable,
    (APTR)0,
    (APTR)LibInit
};

const struct Resident romTag __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&romTag,
    (APTR)&endOfCode,
    RTF_AUTOINIT | RTF_COLDSTART,
    NEPTUNE_VERSION,
    NT_LIBRARY,
    LIBRARY_PRIORITY,
    (char *)libname,
    (char *)libIdString,
    (APTR)initTable
};
