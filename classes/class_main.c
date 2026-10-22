/*
 * Shared class-driver library skeleton (romtag + LVO funcTable + Lib vectors),
 * reused by every Neptune *.class. Hand-written replacement for AROS genmodule.
 *
 * Parameterised per class via -D (see each class's CMakeLists):
 *   CLASS_NAME           "hub.usbclass"            -- the resident/library name
 *   CLASS_PRI            -45                     -- romtag rt_Pri (NEP_CLASS_PRI)
 *   CLASS_BASETYPE_NAME  NepHubBase              -- struct tag of the libbase
 *   CLASS_INCLUDE        "hub.h"                 -- header defining that struct
 *   HAS_LIBOPEN          (defined if the class has a libOpen hook)
 * The version pair is the distribution's (NEPTUNE_VERSION/REVISION, global); the
 * $VER id-string is VERSION_STRING (built from it and CLASS_NAME in class_version.h).
 *
 * Every class libbase starts with `struct Library` (so the std vectors use a
 * `struct Library *` cast) and has a `BPTR nh_SegList;` field (ROM-able expunge -
 * no static). It implements the usbclass ABI: usbGetAttrsA/usbSetAttrsA/usbDoMethodA.
 */

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>                  /* BPTR (LibInit's seglist) */
#include <utility/tagitem.h>          /* struct TagItem (the usbclass ABI vectors) */

#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)

#include <proto/exec.h>

#include CLASS_INCLUDE                 /* defines struct CLASS_BASETYPE_NAME */

#define CLASS_BASE struct CLASS_BASETYPE_NAME

/* VERSION_STRING - the romtag's $VER id-string. One definition, shared with the
   class bodies (which pull it via common.h). */
#include "class_version.h"

/* the class's kept hooks (in <class>.class.c) */
extern int libInit(CLASS_BASE *base);
/* TRUE = the class has torn itself down; the skeleton unlinks the library node and frees the
   base.  FALSE = it cannot go now (its embedded device is open elsewhere, a patch cannot be
   taken back) and has torn nothing down; it is asked again later. */
extern int libExpunge(CLASS_BASE *base);
#ifdef HAS_LIBOPEN
extern int libOpen(CLASS_BASE *base);
#endif
#ifdef HAS_LIBCLOSE
extern int libClose(CLASS_BASE *base);    /* hook checks lib_OpenCnt==0 itself */
#endif

/* The usbclass ABI vectors (in <class>.class.c).  Only their addresses are used here, for
   funcTable[] - but the declarations must still match the definitions: under LTO the compiler
   sees both TUs, and a placeholder (void) prototype is -Wlto-type-mismatch (and undefined
   behaviour).  The ABI is fixed; only the a6 libbase type varies, which is what CLASS_BASE is. */
extern LONG usbGetAttrsA(ULONG type asm("d0"), APTR usbstruct asm("a0"),
                         struct TagItem *tags asm("a1"), CLASS_BASE *nh asm("a6"));
extern LONG usbSetAttrsA(ULONG type asm("d0"), APTR usbstruct asm("a0"),
                         struct TagItem *tags asm("a1"), CLASS_BASE *nh asm("a6"));
extern LONG usbDoMethodA(ULONG methodid asm("d0"), IPTR *methoddata asm("a1"),
                         CLASS_BASE *nh asm("a6"));

/* Optional per-class extra library vectors beyond the usbclass ABI. A class that
   exports its own functions (e.g. camdusbmidi.usbclass's usbCAMDOpenPort/ClosePort)
   provides a header via -DCLASS_VECTORS_HDR="..." that declares them (address-only,
   like the usbclass externs above) and #defines CLASS_EXTRA_VECTORS to the funcTable
   entries (reserved slots use the LibNull below). Absent for every other class. */
#ifdef CLASS_VECTORS_HDR
#include CLASS_VECTORS_HDR
#endif

extern const UBYTE endOfCode;
static const APTR initTable[4];

LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void) { return -1; }

static void freeBase(CLASS_BASE *base)
{
    struct Library *lib = (struct Library *)base;
    ULONG size = (ULONG)lib->lib_NegSize + lib->lib_PosSize;
    FreeMem((APTR)((ULONG)base - lib->lib_NegSize), size);
}

static CLASS_BASE *LibInit(CLASS_BASE *base    asm("d0"),
                           BPTR         seglist asm("a0"),
                           struct ExecBase *sysbase asm("a6"))
{
    (void)sysbase;                     /* exec base comes from $4 (EXEC_BASE_NAME) */
    base->nh_SegList = seglist;
    ((struct Library *)base)->lib_Revision = NEPTUNE_REVISION;
    if(libInit(base))
        return base;
    freeBase(base);
    return NULL;
}

static CLASS_BASE *LibOpen(CLASS_BASE *base asm("a6"))
{
    struct Library *lib = (struct Library *)base;
    lib->lib_OpenCnt++;
    lib->lib_Flags &= ~LIBF_DELEXP;
#ifdef HAS_LIBOPEN
    if(!libOpen(base)) {
        lib->lib_OpenCnt--;
        return NULL;
    }
#endif
    return base;
}

static BPTR LibExpunge(CLASS_BASE *base asm("a6"))
{
    struct Library *lib = (struct Library *)base;
    BPTR seglist;
    /* An opener, or the class itself, says not now: the class stays whole, and goes when
       its last opener closes or at the next flush. */
    if(lib->lib_OpenCnt > 0 || !libExpunge(base)) {
        lib->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seglist = base->nh_SegList;
    Remove(&lib->lib_Node);
    freeBase(base);
    return seglist;
}

static BPTR LibClose(CLASS_BASE *base asm("a6"))
{
    struct Library *lib = (struct Library *)base;
    lib->lib_OpenCnt--;
#ifdef HAS_LIBCLOSE
    /* decrement first: the hook tests OpenCnt == 0 to mean "last close" */
    libClose(base);
#endif
    if(lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return LibExpunge(base);
    return 0;
}

static ULONG LibNull(void) { return 0; }

/* LVO table: 4 std vectors then the usbclass ABI (same for every class), then any
   per-class extra vectors (CLASS_EXTRA_VECTORS, gated), then -1. */
static const APTR funcTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)usbGetAttrsA,
    (APTR)usbSetAttrsA,
    (APTR)usbDoMethodA,
#ifdef CLASS_EXTRA_VECTORS
    CLASS_EXTRA_VECTORS
#endif
    (APTR)-1
};

static const APTR initTable[4] = {
    (APTR)sizeof(CLASS_BASE),
    (APTR)funcTable,
    (APTR)0,
    (APTR)LibInit
};

static const char libName[]     = CLASS_NAME;
static const char libIdString[] = VERSION_STRING;

const struct Resident romTag __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&romTag,
    (APTR)&endOfCode,
    RTF_AUTOINIT | RTF_COLDSTART,
    NEPTUNE_VERSION,
    NT_LIBRARY,
    CLASS_PRI,
    (char *)libName,
    (char *)libIdString,
    (APTR)initTable
};
