/*
 *----------------------------------------------------------------------------
 *                          USBVideo - UVC tool for Neptune
 *----------------------------------------------------------------------------
 * usbvideo.device command-line client: prints camera info and the mode
 * table, captures frames to files.
 *
 *   USBVideo                        unit 0: camera info and mode table
 *   USBVideo UNIT=1                 another unit
 *   USBVideo TO=RAM:shot            one frame in the default mode,
 *                                  RAM:shot-0001.jpg
 *   USBVideo WIDTH=1280 HEIGHT=720 FPS=30 FRAMES=10 TO=RAM:cam
 *                                  10 frames in that mode, RAM:cam-0001.jpg
 *                                  .. RAM:cam-0010.jpg
 *   USBVideo FORMAT=YUY2 FRAMES=10 TO=RAM:cam
 *                                  raw frames, RAM:cam-0001.yuy2 ...
 *   USBVideo FORMAT=H264 WIDTH=1280 HEIGHT=720 FRAMES=300 TO=RAM:cam
 *                                  H.264 elementary stream, RAM:cam.h264.
 *                                  BITRATE (bit/s) and KEYFRAME (key frame
 *                                  interval, ms) override the encoder's
 *                                  defaults
 *
 * Mode parameters not given come from the default mode (the settings
 * window's, else the camera's). Ctrl-C stops; frames written are kept.
 */

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dos.h>
#include <devices/usbvideodevice.h>

#include <neptune_version.h>

#include "uvc_client.h"

/* ReadArgs indices, in template order; those after ARGS_TO are capture
   parameters and need TO */
enum
{
    ARGS_UNIT,
    ARGS_TO,
    ARGS_FORMAT,
    ARGS_WIDTH,
    ARGS_HEIGHT,
    ARGS_FPS,
    ARGS_FRAMES,
    ARGS_BITRATE,
    ARGS_KEYFRAME,
    ARGS_SIZEOF
};

const char USBVideo_prgname[] = "USBVideo";
static const char *template = "UNIT/N/K,TO/K,FORMAT/K,WIDTH/N/K,HEIGHT/N/K,FPS/N/K,FRAMES/N/K,BITRATE/N/K,KEYFRAME/N/K";
const char USBVideo_version[] = NEP_VER("USBVideo");
static LONG ArgsArray[ARGS_SIZEOF];

/* Return the value of a /N argument, or none if it is absent */
static ULONG numberArgument(ULONG arg, ULONG none)
{
    return ArgsArray[arg] ? *((ULONG *) ArgsArray[arg]) : none;
}

/* Set the requested mode (NSCMD_UV_SETMODE) and print the negotiated one.
   Print the reason and return FALSE if it fails, or if BITRATE/KEYFRAME
   were given and the mode's encoder ignored them. */
static BOOL setMode(struct UvcCamera *cm, struct UsbVideoModeReq *uvq)
{
    ULONG bitrate = uvq->uvq_BitRate;
    ULONG keyinterval = uvq->uvq_KeyInterval;
    LONG err = uvcCommand(cm->cm_Request, NSCMD_UV_SETMODE, uvq, sizeof(*uvq));

    if(err)
    {
        Printf("Could not set that mode: %s\n", uvcErrorText(err));
        if(err == UVERR_BADMODE)
        {
            PutStr("USBVideo without TO lists the modes it has.\n");
        }
        return FALSE;
    }
    uvcPrintMode(uvq);
    uvcPrintEncoder(uvq);
    PutStr("\n");
    if((bitrate && !uvq->uvq_BitRate) || (keyinterval && !uvq->uvq_KeyInterval))
    {
        PutStr("BITRATE and KEYFRAME are for H264 modes whose encoder takes them, and this one does not.\n");
        return FALSE;
    }
    return TRUE;
}

/* Return why a capture ended early, as text; NULL if it completed */
static STRPTR whyStopped(const struct UvcCapture *cp)
{
    if(cp->cp_Interrupted)
    {
        return "Ctrl-C";
    }
    if(cp->cp_NotSaved)
    {
        return "could not save";
    }
    if(cp->cp_Error)
    {
        return uvcErrorText(cp->cp_Error);
    }
    return NULL;
}

/* Print the capture summary (saved, lost, early stop); return the DOS
   return code */
static LONG reportCapture(const struct UvcCapture *cp)
{
    ULONG saved = cp->cp_Files.fl_Saved;
    ULONG lost = cp->cp_Missed + cp->cp_TooLong;
    STRPTR stopped = whyStopped(cp);

    if(saved)
    {
        uvcPrintSaved(&cp->cp_Files);
    } else {
        PutStr("Nothing was saved.\n");
    }
    if(lost)
    {
        Printf("%ld %s lost on the way\n", lost, (lost == 1) ? "frame was" : "frames were");
    }
    if(stopped)
    {
        Printf("Stopped early: %s\n", stopped);
    }
    if(!saved)
    {
        return RETURN_FAIL;
    }
    return (stopped || lost) ? RETURN_WARN : RETURN_OK;
}

/* Set the mode, capture frames frames into files named from stem; return
   the DOS return code */
static LONG saveFrames(struct UvcCamera *cm, struct UsbVideoModeReq *uvq, ULONG frames, STRPTR stem)
{
    struct UvcCapture cp;

    if(!setMode(cm, uvq))
    {
        return RETURN_FAIL;
    }
    if(uvcStartCapture(&cp, cm, uvq, stem))
    {
        while((cp.cp_Files.fl_Saved < frames) && uvcNextFrame(&cp))
        {
            /* uvcNextFrame() writes each frame */
        }
    }
    uvcStopCapture(&cp);
    return reportCapture(&cp);
}

int main(int argc, char *argv[])
{
    struct RDArgs *rdargs = ReadArgs(template, ArgsArray, NULL);

    if(!rdargs)
    {
        PrintFault(IoErr(), (STRPTR) USBVideo_prgname);
        return RETURN_FAIL;
    }

    STRPTR stem = (STRPTR) ArgsArray[ARGS_TO];
    ULONG fps = numberArgument(ARGS_FPS, 0);
    struct UsbVideoModeReq uvq =
    {
        .uvq_FourCC = uvcParseFourCC((STRPTR) ArgsArray[ARGS_FORMAT]),
        .uvq_Width = numberArgument(ARGS_WIDTH, 0),
        .uvq_Height = numberArgument(ARGS_HEIGHT, 0),
        .uvq_Interval = fps ? UVC_INTERVAL_UNITS / fps : 0,
        .uvq_BitRate = numberArgument(ARGS_BITRATE, 0),
        .uvq_KeyInterval = numberArgument(ARGS_KEYFRAME, 0)
    };
    BOOL howtosave = FALSE;

    /* Was any capture parameter given (FORMAT, WIDTH, HEIGHT, FPS, FRAMES,
       BITRATE, KEYFRAME)? ReadArgs() leaves an argument that was not typed
       at 0. The enum puts every capture parameter after ARGS_TO, so the loop
       finds a new one without change. They need TO; without it the user is
       told so instead of getting the mode list with the parameters ignored. */
    for(ULONG arg = ARGS_TO + 1; arg < ARGS_SIZEOF; arg++)
    {
        howtosave |= (ArgsArray[arg] != 0);
    }
    if(howtosave && !stem)
    {
        PutStr("Add TO=<name> to save in that mode. Without TO, USBVideo lists the modes.\n");
        FreeArgs(rdargs);
        return RETURN_FAIL;
    }

    struct UvcCamera cm;
    LONG rc = RETURN_FAIL;

    if(uvcOpenCamera(&cm, numberArgument(ARGS_UNIT, 0)))
    {
        uvcPrintCamera(&cm);
        if(stem)
        {
            rc = saveFrames(&cm, &uvq, numberArgument(ARGS_FRAMES, 1), stem);
        } else {
            uvcPrintModes(&cm);
            rc = RETURN_OK;
        }
    }
    uvcCloseCamera(&cm);
    FreeArgs(rdargs);
    return rc;
}
