/*
    Copyright (C) 2019, The AROS Development Team. All rights reserved.
*/


#include <exec/types.h>
#include <exec/libraries.h>

#include "debug.h"

#include "arosx.class.h"   /* pulls in common.h -> <proto/exec.h> (with EXEC_BASE_NAME) */
#include "include/arosx.h"

BOOL AROSXClass_SendEvent(struct AROSXClassBase * arosxb, ULONG ehmt, APTR param1, APTR param2);

BPTR axLibExpunge(struct AROSXBase * base asm("a6"));

static const UBYTE libarosx[] = "arosx.library";

struct AROSXBase * axLibInit(struct AROSXBase * base asm("d0"), BPTR seglist asm("a0"), struct ExecBase * SysBase asm("a6")) {
    
    KPRINTF(10, ("[AROSXLib] axLibInit base: 0x%08lx seglist: 0x%08lx SysBase: 0x%08lx\n", base, seglist, SysBase));

    base->arosx_LibNode.lib_Node.ln_Type = NT_LIBRARY;
    base->arosx_LibNode.lib_Node.ln_Name = (UBYTE*)libarosx;
    base->arosx_LibNode.lib_Flags        = LIBF_SUMUSED|LIBF_CHANGED;
    base->arosx_LibNode.lib_Version      = 0;
    base->arosx_LibNode.lib_Revision     = 1;
    base->arosx_LibNode.lib_IdString     = (UBYTE*)libarosx;

    /* Store segment, don't have one... */
    //base->arosx_LibNode.np_SegList = seglist;
    
    return(base);

}

struct AROSXBase * (axLibOpen)(ULONG version asm("d0"), struct AROSXBase * base asm("a6")) {

    ++base->arosx_LibNode.lib_OpenCnt;
    base->arosx_LibNode.lib_Flags &= ~LIBF_DELEXP;

    return base;
    
}

BPTR (axLibClose)(struct AROSXBase * base asm("a6")) {

    BPTR ret;
    ret = BNULL;

    if(--base->arosx_LibNode.lib_OpenCnt == 0) {
        if(base->arosx_LibNode.lib_Flags & LIBF_DELEXP)
        {
            KPRINTF(10, ("[AROSXLib] axLibClose: calling expunge...\n"));
            ret = axLibExpunge(base);
        }
    }

    return(ret);

}

BPTR (axLibExpunge)(struct AROSXBase * base asm("a6")) {

    BPTR ret;

    /*
        CHECME: Our memory belongs to arosx.usbclass, make sure we free only allocated memory
    */

    KPRINTF(10, ("[AROSXLib] axLibExpunge base: 0x%08lx\n", base));

    ret = BNULL;

    if(base->arosx_LibNode.lib_OpenCnt == 0)
    {
        KPRINTF(10, ("[AROSXLib] axLibExpunge: Unloading...\n"));

        //ret = base->np_SegList;

        KPRINTF(10, ("[AROSXLib] axLibExpunge: removing library node 0x%08lx\n", &base->arosx_LibNode.lib_Node));
        Remove(&base->arosx_LibNode.lib_Node);

        KPRINTF(10, ("[AROSXLib] axLibExpunge: FreeMem()...\n"));
        FreeMem((char *) base - base->arosx_LibNode.lib_NegSize, (ULONG) (base->arosx_LibNode.lib_NegSize + base->arosx_LibNode.lib_PosSize));

        KPRINTF(10, ("[AROSXLib] axLibExpunge: Unloading done! arosx.library expunged!\n"));

        return(ret);
    }
    else
    {
        KPRINTF(20, ("[AROSXLib] axLibExpunge: Could not expunge, LIBF_DELEXP set!\n"));
        base->arosx_LibNode.lib_Flags |= LIBF_DELEXP;
    }

    return(BNULL);

}

IPTR (axLibReserved)(struct AROSXBase * base asm("a6")) {
    return (IPTR)NULL;
}

struct AROSX_EventHook * (AROSX_AddEventHandler)(struct MsgPort * mp asm("a1"), ULONG msgmask asm("d0"), struct AROSXBase * base asm("a6"))
{

    struct AROSXClassBase *arosxb;
    arosxb = base->arosxb;

    struct AROSXClassController *arosxc;

    struct AROSX_EventHook *eh = NULL;

    KPRINTF(10, ("AROSX_AddEventHandler(0x%08lx, 0x%08lx)\n", mp, msgmask));

    if(mp) {
        if((eh = AllocVec(sizeof(struct AROSX_EventHook), (MEMF_CLEAR|MEMF_ANY)))) {
            eh->eh_MsgPort = mp;
            eh->eh_MsgMask = msgmask;
            ObtainSemaphore(&arosxb->event_lock);
            AddTail(&base->arosxb->event_port_list, &eh->eh_Node);
            ReleaseSemaphore(&arosxb->event_lock);

            /*
                Send connect events from those controllers that are already connected and inluded in the mask
            */

            ObtainSemaphore(&arosxb->arosxc_lock);

            arosxc = arosxb->arosxc_0;
            if( arosxc && (((msgmask>>28) & 1) && (arosxc->status.connected)) ) {
                AROSXClass_SendEvent(arosxb, ((1<<28) | ((arosxc->controller_type)<<20) | AROSX_EHMF_CONNECT), (APTR)1, (APTR)2);
                KPRINTF(10, ("Sent connect event for 0\n"));
            }

            arosxc = arosxb->arosxc_1;
            if( arosxc && (((msgmask>>28) & 2) && (arosxc->status.connected)) ) {
                AROSXClass_SendEvent(arosxb, ((2<<28) | ((arosxc->controller_type)<<20) | AROSX_EHMF_CONNECT), (APTR)1, (APTR)2);
                KPRINTF(10, ("Sent connect event for 1\n"));
            }

            arosxc = arosxb->arosxc_2;
            if( arosxc && (((msgmask>>28) & 4) && (arosxc->status.connected)) ) {
                AROSXClass_SendEvent(arosxb, ((4<<28) | ((arosxc->controller_type)<<20) | AROSX_EHMF_CONNECT), (APTR)1, (APTR)2);
                KPRINTF(10, ("Sent connect event for 2\n"));
            }

            arosxc = arosxb->arosxc_3;
            if( arosxc && (((msgmask>>28) & 8) && (arosxc->status.connected)) ) {
                AROSXClass_SendEvent(arosxb, ((8<<28) | ((arosxc->controller_type)<<20) | AROSX_EHMF_CONNECT), (APTR)1, (APTR)2);
                KPRINTF(10, ("Sent connect event for 3\n"));
            }

            ReleaseSemaphore(&arosxb->arosxc_lock);

        }

    }

    return(eh);
}

void (AROSX_RemEventHandler)(struct AROSX_EventHook * eh asm("a0"), struct AROSXBase * base asm("a6"))
{

    struct AROSXClassBase *arosxb;
    arosxb = base->arosxb;

    struct Message *msg;

    KPRINTF(10, ("AROSX_RemEventHandler(0x%08lx)\n", eh));
    if(!eh) {
        return;
    }

    ObtainSemaphore(&arosxb->event_lock);
    Remove(&eh->eh_Node);
    while((msg = GetMsg(eh->eh_MsgPort))) {
        ReplyMsg(msg);
    }
    ReleaseSemaphore(&arosxb->event_lock);

    KPRINTF(10, ("AROSX_RemEventHandler garbage collector\n"));

    struct AROSX_EventNote *en;
    while((en = (struct AROSX_EventNote *) GetMsg(&arosxb->event_reply_port))) {
        KPRINTF(10, ("    Free AROSX_EventNote(0x%08lx)\n", en));
        FreeVec(en);
    }

    FreeVec(eh);

}




static const APTR libFuncTable[] = {
    (APTR) axLibOpen,
    (APTR) axLibClose,
    (APTR) axLibExpunge,
    (APTR) axLibReserved,
    (APTR) AROSX_AddEventHandler,
    (APTR) AROSX_RemEventHandler,
    (APTR) -1,
};

struct AROSXBase * AROSXInit(void) {

    struct AROSXBase *lib;

    KPRINTF(10,("AROSXInit\n"));

    if((lib = (struct AROSXBase *)MakeLibrary((APTR) libFuncTable, NULL, (APTR) axLibInit, sizeof(struct AROSXBase), NULL))) {
        Forbid();
        AddLibrary((struct Library *)lib);
        lib->arosx_LibNode.lib_OpenCnt++;
        Permit();
    } else {
        KPRINTF(20, ("failed to create arosx.library\n"));
    }

    KPRINTF(10,("AROSXInit base 0x%08lx\n", lib));
    KPRINTF(10,("AROSXInit done\n"));

    return lib;
}
