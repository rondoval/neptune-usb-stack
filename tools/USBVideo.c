/*
 *----------------------------------------------------------------------------
 *                          USBVideo - UVC tool for Neptune
 *----------------------------------------------------------------------------
 * usbvideo.device client: in the Shell it prints camera info and the mode
 * table and captures frames to files; from its icon, or with GUI, it opens a
 * window for all of that with a live preview.
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
 *   USBVideo PREVIEW FORMAT=H264 WIDTH=1280 HEIGHT=720
 *                                  a window that shows the stream, decoded
 *                                  by videocodec.library, until Ctrl-C or the
 *                                  window closes; with TO it saves too
 *   USBVideo GUI                   the window (needs MUI); UNIT picks the camera
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
#include "uvc_preview.h"
#include "usbvideo_gui.h"

/* ReadArgs indices, in template order; those after ARGS_TO are capture
   parameters and need TO, or PREVIEW */
enum
{
    ARGS_UNIT,
    ARGS_GUI,
    ARGS_PREVIEW,
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
static const char *template = "UNIT/N/K,GUI/S,PREVIEW/S,TO/K,FORMAT/K,WIDTH/N/K,HEIGHT/N/K,FPS/N/K,FRAMES/N/K,BITRATE/N/K,KEYFRAME/N/K";
const char USBVideo_version[] = NEP_VER("USBVideo");
static LONG ArgsArray[ARGS_SIZEOF];

unsigned long __stack = 32768; /* for MUI: libnix gives it to main() */

#define STOP_SIGNAL SIGBREAKF_CTRL_F /* the stream ended by itself; a stray one costs a look */

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

/* Print what a preview's stream did */
static void printCounts(const struct UvpCounts *uc)
{
    Printf("%ld frames came, %ld were lost, %ld kept from the decoder\n", uc->uc_Frames, uc->uc_Lost, uc->uc_Held);
    Printf("%ld sent to the decoder, %ld pictures back, %ld shown, %ld saved\n", uc->uc_Sent, uc->uc_Pictures,
           uc->uc_Shown, uc->uc_Saved);
    Printf("The decoder held back %ld frames at the last picture, %ld at most\n", uc->uc_Lag, uc->uc_LagMost);
}

/* Run the stream in a process of its own until Ctrl-C, or until it ends by
   itself; return the DOS return code */
static LONG runPreview(struct UvcPreview *up, struct UsbVideoModeReq *uvq, STRPTR stem)
{
    if(!uvpStart(up, uvq, stem))
    {
        Printf("%s\n", uvpWhy(up));
        return RETURN_FAIL;
    }
    uvcPrintMode(uvpMode(up));
    PutStr("\n");
    if(uvpWhy(up)[0])
    {
        Printf("%s\n", uvpWhy(up));
    }
    if(stem && !uvpRecord(up, TRUE))
    {
        Printf("%s\n", uvpWhy(up));
    }
    while(uvpRunning(up) && !(Wait(STOP_SIGNAL | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C))
    {
        if(uvpWhy(up)[0])
        {
            Printf("%s\n", uvpWhy(up));
        }
    }
    uvpStop(up);
    printCounts(uvpCounts(up));
    return RETURN_OK;
}

/* Open the unit from a stream process and run the preview; return the DOS
   return code */
static LONG preview(ULONG unit, struct UsbVideoModeReq *uvq, STRPTR stem)
{
    UBYTE why[UVC_TEXT_SIZE];
    struct UvcPreview *up = uvpOpen(unit, STOP_SIGNAL, 0, why);

    if(!up)
    {
        Printf("%s\n", why);
        return RETURN_FAIL;
    }
    uvcPrintCamera(uvpCamera(up));

    LONG rc = runPreview(up, uvq, stem);

    uvpClose(up);
    return rc;
}

/* Print the camera, then save frames from it, or list its modes if there is
   no file name; return the DOS return code */
static LONG listOrSave(ULONG unit, struct UsbVideoModeReq *uvq, ULONG frames, STRPTR stem)
{
    struct UvcCamera cm;
    LONG rc = RETURN_FAIL;

    if(uvcOpenCamera(&cm, unit))
    {
        uvcPrintCamera(&cm);
        if(stem)
        {
            rc = saveFrames(&cm, uvq, frames, stem);
        } else {
            uvcPrintModes(&cm);
            rc = RETURN_OK;
        }
    }
    uvcCloseCamera(&cm);
    return rc;
}

int main(int argc, char *argv[])
{
    if(!argc)
    {
        return usbVideoGui(0, (CONST_STRPTR) USBVideo_version); /* from Workbench */
    }

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

    ULONG unit = numberArgument(ARGS_UNIT, 0);
    LONG rc = RETURN_FAIL;

    if(howtosave && !stem && !ArgsArray[ARGS_PREVIEW])
    {
        PutStr("Add TO=<name> to save in that mode. Without TO, USBVideo lists the modes.\n");
    }
    else if(ArgsArray[ARGS_GUI])
    {
        rc = usbVideoGui(unit, (CONST_STRPTR) USBVideo_version);
    }
    else if(ArgsArray[ARGS_PREVIEW])
    {
        if((uvq.uvq_FourCC == UVFCC_H264) && !uvq.uvq_KeyInterval)
        {
            uvq.uvq_KeyInterval = UVP_KEY_INTERVAL;
        }
        rc = preview(unit, &uvq, stem);
    } else {
        rc = listOrSave(unit, &uvq, numberArgument(ARGS_FRAMES, 1), stem);
    }
    FreeArgs(rdargs);
    return rc;
}
