/*
 *----------------------------------------------------------------------------
 *              UVC tools: the stream process
 *----------------------------------------------------------------------------
 * Implements uvc_preview.h.
 *
 * The memory of the frames is ours, not the session's: reads fill it and
 * VC_ImportPacket() (a decoder's, MJPEG, H.264, H.265) or VC_ImportFrame() (a
 * converter's, the uncompressed formats) lends it to the session, with no
 * copy. So a read never points into memory a session frees, the session can
 * fail and the stream carry on saving, and a stream without a session is the
 * same stream.
 *
 * A read's buffer goes round: at the device, in hand (saved, made ready), at
 * the session, back to the device. Stopping keeps one order: CMD_STOP, reads
 * taken back, session closed, then the buffers freed.
 *
 * The loop empties every source before it waits, and never blocks on a
 * layer: a frame the window cannot take now stays pending, overtaken by the
 * next.
 */

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>

#include <clib/macros.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <libraries/videocodec.h>

#include <string.h>

#include "uvc_preview.h"
#include "uvc_record.h"
#include "mjpegdht.h"

#define STREAM_NAME    "USBVideo stream"
#define STREAM_STACK   16384
#define STREAM_PRI     0
#define STEM_SIZE      256
#define PREVIEW_FRAMES 3    /* one shown, one pending, one the scaler fills */
#define ROW_PIXELS     32   /* the scaler writes rows of whole groups of these */
#define MIN_ROWS       16
#define MIN_WIDTH      128  /* of the window */
#define MIN_HEIGHT     80
#define VC_REVISION    4    /* videocodec.library 0.4: the first with converters' InputFormats */
#define P96_VERSION    2
#define USUAL_INTERVAL (UVC_INTERVAL_UNITS / 30) /* should a camera give none */
#define SILENT_SECS    5    /* of frames sent with no picture back: the VideoCore lacks memory */
#define JPEG_ROOM      MJPEG_STD_DHT_SIZE /* free before an MJPEG frame: SOI and the standard tables go there */

/* Commands */
enum
{
    UVPC_OPEN,
    UVPC_START,
    UVPC_STOP,
    UVPC_RECORD,
    UVPC_SETTINGS,
    UVPC_QUIT
};

struct UvpCommand
{
    struct Message cmd_Msg;
    ULONG          cmd_What;
    APTR           cmd_Data;
    BOOL           cmd_Ok;
};

/* A Picasso96 pixel format the session writes */
struct PixelFormat
{
    RGBFTYPE pf_RGB;
    ULONG    pf_VC;    /* VCF_ */
    ULONG    pf_Bytes; /* per pixel */
};

/* How a camera format is shown: a decoder's packets, or a converter's frames */
struct PreviewPath
{
    ULONG pp_FourCC; /* UVFCC_ */
    ULONG pp_Kind;   /* VCK_ */
    ULONG pp_Codec;  /* VCC_ */
    ULONG pp_Input;  /* a converter's: the camera's frames as a VCF_ */
    ULONG pp_Bytes;  /* a converter's: bytes per pixel of the first plane */
};

struct UvcPreview
{
    /* the caller's, set before the process starts */
    struct MsgPort *up_ReplyPort;
    struct Task    *up_Caller;
    ULONG           up_StopMask;
    ULONG           up_TickMask;
    ULONG           up_Unit;

    /* the process's */
    struct MsgPort        *up_CmdPort;     /* its signal is the session's too */
    struct UvcCamera       up_Camera;
    struct UvcCapture      up_Capture;
    struct UsbVideoModeReq up_Mode;
    UBYTE                  up_Stem[STEM_SIZE];
    struct IBox            up_Clear;       /* what the window keeps off; no width: Intuition places it */
    ULONG                  up_Interval;    /* of frames, 100 ns units; never 0 */
    struct UvpCounts       up_Counts;
    UBYTE                  up_Why[UVC_TEXT_SIZE];

    /* saving */
    struct UvcRecording up_Recording;
    ULONG               up_KeyAsked; /* times the decoder wanted a key frame since the last one came */

    /* the session: a decoder or a converter */
    const struct PreviewPath *up_Path; /* NULL: the mode has no preview */
    struct VCSession         *up_Session;

    /* the window */
    struct Window            *up_Window;
    const struct PixelFormat *up_Format;
    struct IBox               up_Picture; /* where it sits in the window */
    struct VCFrame           *up_Shown;   /* on display, kept for a refresh */
    struct VCFrame           *up_Pending; /* newest picture, not yet on display */
    BOOL                      up_Refresh;
    BOOL                      up_Bars;    /* the bars around the picture want painting */
};

struct Library *P96Base;
struct Library *VideoCodecBase;

static const struct PixelFormat PixelFormats[] =
{
    { RGBFB_R5G6B5PC, VCF_RGB16, 2 },
    { RGBFB_R8G8B8, VCF_RGB24, 3 },
    { RGBFB_B8G8R8, VCF_BGR24, 3 },
    { RGBFB_B8G8R8A8, VCF_BGRA32, 4 },
    { RGBFB_R8G8B8A8, VCF_RGBA32, 4 }
};

static const struct PreviewPath PreviewPaths[] =
{
    { UVFCC_MJPG, VCK_Decoder, VCC_MJPEG, 0, 0 },
    { UVFCC_H264, VCK_Decoder, VCC_H264, 0, 0 },
    { UVFCC_H265, VCK_Decoder, VCC_H265, 0, 0 },
    { UVFCC_YUY2, VCK_Converter, VCC_Raw, VCF_YUYV, 2 },
    { UVFCC_NV12, VCK_Converter, VCC_Raw, VCF_NV12, 1 }
};

/*
 * Words
 */

/* Return a VCERR_ as text */
static const char *sessionErrorText(LONG err)
{
    switch(err)
    {
        case VCERR_NOMEM:       return "not enough memory";
        case VCERR_DEVMEM:      return "the VideoCore is out of memory: raise gpu_mem in config.txt";
        case VCERR_NOBACKEND:   return "no videocodec backend shows this format";
        case VCERR_UNSUPPORTED: return "the hardware cannot do this";
        case VCERR_STREAM:      return "the decoder refused the stream";
        case VCERR_TRANSPORT:   return "the link to the VideoCore failed";
    }
    return "the VideoCore failed";
}

/* The caller wants to know: the stream ended, or the preview did */
static void tellCaller(struct UvcPreview *up)
{
    Signal(up->up_Caller, up->up_StopMask);
}

/*
 * The window
 */

/* The width of the widest picture of the mode's shape that fits a box */
static ULONG fitWidth(const struct UvcPreview *up, ULONG boxw, ULONG boxh)
{
    ULONG videow = up->up_Mode.uvq_Width;
    ULONG videoh = up->up_Mode.uvq_Height;

    return (videoh * boxw / videow > boxh) ? videow * boxh / videoh : boxw;
}

/* The picture's size and place in the window's inner area, kept to the
   mode's aspect. Its width is a whole number of groups, so the scaler writes
   full rows of exactly that width. FALSE when the window is too small. */
static BOOL fitPicture(const struct UvcPreview *up, struct IBox *pic)
{
    const struct Window *win = up->up_Window;
    LONG innerw = win->Width - win->BorderLeft - win->BorderRight;
    LONG innerh = win->Height - win->BorderTop - win->BorderBottom;

    if((innerw < ROW_PIXELS) || (innerh < MIN_ROWS))
    {
        return FALSE;
    }

    ULONG width = fitWidth(up, innerw, innerh) & ~(ULONG) (ROW_PIXELS - 1);
    ULONG height = (up->up_Mode.uvq_Height * width / up->up_Mode.uvq_Width) & ~1UL;

    if(!width || !height)
    {
        return FALSE;
    }
    pic->Width = width;
    pic->Height = height;
    pic->Left = win->BorderLeft + (innerw - (LONG) width) / 2;
    pic->Top = win->BorderTop + (innerh - (LONG) height) / 2;
    return TRUE;
}

/* Black in a box, if it is not empty */
static void fillBox(struct RastPort *rp, LONG left, LONG top, LONG right, LONG bottom)
{
    if((left <= right) && (top <= bottom))
    {
        p96RectFill(rp, left, top, right, bottom, 0);
    }
}

/* Black around the picture, or everywhere before the first one */
static void fillBars(const struct UvcPreview *up)
{
    const struct Window *win = up->up_Window;
    struct RastPort *rp = win->RPort;
    const struct IBox *pic = &up->up_Picture;
    LONG left = win->BorderLeft;
    LONG top = win->BorderTop;
    LONG right = win->Width - win->BorderRight - 1;
    LONG bottom = win->Height - win->BorderBottom - 1;
    LONG picright = pic->Left + pic->Width - 1;
    LONG picbottom = pic->Top + pic->Height - 1;

    if(!up->up_Shown)
    {
        fillBox(rp, left, top, right, bottom);
        return;
    }
    fillBox(rp, left, top, right, pic->Top - 1);
    fillBox(rp, left, picbottom + 1, right, bottom);
    fillBox(rp, left, pic->Top, pic->Left - 1, picbottom);
    fillBox(rp, picright + 1, pic->Top, right, picbottom);
}

/* A frame into the window: Picasso96 copies it, clipped by the layers */
static void drawFrame(const struct UvcPreview *up, const struct VCFrame *frame)
{
    struct RenderInfo ri;

    ri.Memory = (UBYTE *) frame->Data + frame->Offset[0];
    ri.BytesPerRow = (WORD) frame->Pitch[0];
    ri.pad = 0;
    ri.RGBFormat = up->up_Format->pf_RGB;
    p96WritePixelArray(&ri, 0, 0, up->up_Window->RPort, up->up_Picture.Left, up->up_Picture.Top,
                       up->up_Picture.Width, up->up_Picture.Height);
}

/* The pending picture onto the display, if the layer is free this moment.
   A held menu or a dragged window can lock it for as long as the user
   likes: the stream must not wait for that. */
static void showPending(struct UvcPreview *up)
{
    struct Layer *layer;

    if(!up->up_Pending)
    {
        return;
    }
    layer = up->up_Window->RPort->Layer;
    if(!AttemptLockLayerRom(layer))
    {
        return;
    }
    drawFrame(up, up->up_Pending);
    UnlockLayerRom(layer);
    if(up->up_Shown)
    {
        VC_ReleaseFrame(up->up_Session, up->up_Shown);
    }
    up->up_Shown = up->up_Pending;
    up->up_Pending = NULL;
    up->up_Counts.uc_Shown++;
}

/* What only the free layer allows: bars, and a refresh of what was covered.
   The layer is let go again before BeginRefresh(): it must not be held
   across an Intuition call. That call can still wait, if a lock comes
   between the two, and only then. */
static void repairWindow(struct UvcPreview *up)
{
    struct Window *win = up->up_Window;
    struct Layer *layer = win->RPort->Layer;

    if((!up->up_Bars && !up->up_Refresh) || !AttemptLockLayerRom(layer))
    {
        return;
    }
    if(up->up_Bars)
    {
        fillBars(up);
        up->up_Bars = FALSE;
    }
    UnlockLayerRom(layer);
    if(up->up_Refresh)
    {
        BeginRefresh(win);
        fillBars(up);
        if(up->up_Shown)
        {
            drawFrame(up, up->up_Shown);
        }
        EndRefresh(win, TRUE);
        up->up_Refresh = FALSE;
    }
}

/* The format to write for a screen of that format and size per pixel: its
   own, so the copy is a plain copy, else one of the same size, else any of
   them: Picasso96 converts while copying */
static const struct PixelFormat *chooseFormat(RGBFTYPE rgb, ULONG bytes, ULONG written)
{
    const struct PixelFormat *any = NULL;
    const struct PixelFormat *like = NULL;

    for(ULONG cnt = 0; cnt < sizeof(PixelFormats) / sizeof(PixelFormats[0]); cnt++)
    {
        const struct PixelFormat *pf = &PixelFormats[cnt];

        if(!(written & VCF_MASK(pf->pf_VC)))
        {
            continue;
        }
        if(pf->pf_RGB == rgb)
        {
            return pf;
        }
        if(!like && (pf->pf_Bytes == bytes))
        {
            like = pf;
        }
        any = any ? any : pf;
    }
    return like ? like : any;
}

/* Return how much of box a window of width x height at left, top covers */
static ULONG covered(const struct IBox *box, LONG left, LONG top, LONG width, LONG height)
{
    LONG across = MIN(left + width, box->Left + box->Width) - MAX(left, (LONG) box->Left);
    LONG down = MIN(top + height, box->Top + box->Height) - MAX(top, (LONG) box->Top);

    return ((across > 0) && (down > 0)) ? across * down : 0;
}

/* Set left and top to where a window of width x height covers least of
   up_Clear: beside it on one of its sides, moved back onto the screen */
static void placeWindow(const struct UvcPreview *up, const struct Screen *scr, LONG width, LONG height, LONG *left, LONG *top)
{
    const struct IBox *box = &up->up_Clear;
    const LONG spots[][2] =
    {
        { box->Left + box->Width, box->Top },
        { box->Left - width, box->Top },
        { box->Left, box->Top + box->Height },
        { box->Left, box->Top - height }
    };
    LONG firsty = scr->BarHeight + 1;
    ULONG least = ~0UL;

    for(ULONG cnt = 0; cnt < sizeof(spots) / sizeof(spots[0]); cnt++)
    {
        LONG x = MAX(MIN(spots[cnt][0], scr->Width - width), 0);
        LONG y = MAX(MIN(spots[cnt][1], scr->Height - height), firsty);
        ULONG cover = covered(box, x, y, width, height);

        if(cover < least)
        {
            least = cover;
            *left = x;
            *top = y;
        }
    }
}

/* Open the preview window on the default public screen: the mode's size, at
   most half the screen's. FALSE with the reason kept. */
static BOOL openWindow(struct UvcPreview *up, ULONG written)
{
    struct Screen *scr = LockPubScreen(NULL);

    if(!scr)
    {
        uvcReport("No preview: no public screen");
        return FALSE;
    }

    struct BitMap *bm = scr->RastPort.BitMap;

    if(!p96GetBitMapAttr(bm, P96BMA_ISP96))
    {
        UnlockPubScreen(NULL, scr);
        uvcReport("No preview: the screen is not a Picasso96 screen");
        return FALSE;
    }
    up->up_Format = chooseFormat((RGBFTYPE) p96GetBitMapAttr(bm, P96BMA_RGBFORMAT),
                                 p96GetBitMapAttr(bm, P96BMA_BYTESPERPIXEL), written);
    if(!up->up_Format)
    {
        UnlockPubScreen(NULL, scr);
        uvcReport("No preview: the VideoCore writes no format Picasso96 knows");
        return FALSE;
    }

    ULONG videow = up->up_Mode.uvq_Width;
    ULONG videoh = up->up_Mode.uvq_Height;
    ULONG innerw = fitWidth(up, MIN(videow, (ULONG) scr->Width / 2), MIN(videoh, (ULONG) scr->Height / 2));
    ULONG innerh = videoh * innerw / videow;

    BOOL placed = up->up_Clear.Width != 0;
    LONG left = 0;
    LONG top = 0;

    if(placed)
    {
        placeWindow(up, scr, innerw + scr->WBorLeft + scr->WBorRight,
                    innerh + scr->WBorTop + scr->Font->ta_YSize + 1 + scr->WBorBottom, &left, &top);
    }
    up->up_Window = OpenWindowTags(NULL, WA_PubScreen, (IPTR) scr, WA_Title, (IPTR) "USBVideo preview",
                                   placed ? WA_Left : TAG_IGNORE, left, placed ? WA_Top : TAG_IGNORE, top,
                                   WA_InnerWidth, innerw, WA_InnerHeight, innerh,
                                   WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                                   WA_SizeGadget, TRUE, WA_SizeBBottom, TRUE, WA_SimpleRefresh, TRUE,
                                   WA_AutoAdjust, TRUE, WA_MinWidth, MIN_WIDTH, WA_MinHeight, MIN_HEIGHT,
                                   WA_MaxWidth, ~0UL, WA_MaxHeight, ~0UL,
                                   WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW,
                                   TAG_DONE);
    UnlockPubScreen(NULL, scr);
    if(!up->up_Window)
    {
        uvcReport("No preview: the window did not open");
        return FALSE;
    }
    if(!fitPicture(up, &up->up_Picture))
    {
        uvcReport("No preview: the window is too small for a picture");
        return FALSE;
    }
    up->up_Bars = TRUE;
    return TRUE;
}

/*
 * The session
 */

/* How a camera format is shown, NULL if it cannot be */
static const struct PreviewPath *previewPath(ULONG fourcc)
{
    for(ULONG cnt = 0; cnt < sizeof(PreviewPaths) / sizeof(PreviewPaths[0]); cnt++)
    {
        if(PreviewPaths[cnt].pp_FourCC == fourcc)
        {
            return &PreviewPaths[cnt];
        }
    }
    return NULL;
}

BOOL uvpCanPreview(ULONG fourcc)
{
    return previewPath(fourcc) != NULL;
}

/* Open what a preview needs, if that is not done. FALSE with the reason kept
   if it cannot be: a later start looks again, and says so again. */
static BOOL openLibraries(struct UvcPreview *up)
{
    if(VideoCodecBase)
    {
        return TRUE;
    }
    if(!P96Base && !(P96Base = OpenLibrary(P96NAME, P96_VERSION)))
    {
        uvcReport("No preview: it needs Picasso96");
    }
    else if(!(VideoCodecBase = OpenLibrary(VIDEOCODEC_NAME, 0)))
    {
        uvcReport("No preview: it needs " VIDEOCODEC_NAME);
    }
    else if(!VideoCodecBase->lib_Version && (VideoCodecBase->lib_Revision < VC_REVISION))
    {
        CloseLibrary(VideoCodecBase);
        VideoCodecBase = NULL;
        uvcReport("No preview: " VIDEOCODEC_NAME " is too old");
    }
    return VideoCodecBase != NULL;
}

/* The pixel formats a session of the path can be asked for, VCF_MASK()s:
   what the rows for its codec and kind write that sessions can use (for a
   converter, those that read the camera's format), and what a converter the
   library puts behind such a decoder adds (an H.265 decoder's column
   pictures need one); 0 if there is no such row */
static ULONG sessionFormats(const struct PreviewPath *pp)
{
    ULONG input = pp->pp_Input ? VCF_MASK(pp->pp_Input) : 0;
    ULONG written = 0;

    for(ULONG index = 0; ; index++)
    {
        struct VCCodecInfo info;

        info.Size = sizeof(info);
        if(VC_QueryCodec(index, &info))
        {
            return written;
        }
        if((info.Codec == pp->pp_Codec) && (info.Kind == pp->pp_Kind) && (info.Flags & VCCF_Session) &&
           (!input || (info.InputFormats & input)))
        {
            written |= info.OutputFormats;
            if(VC_HAS_FIELD(info.Size, struct VCCodecInfo, ChainOutputFormats))
            {
                written |= info.ChainOutputFormats;
            }
        }
    }
}

/* Close the session, then the window it draws for. The frames go with the
   session; the buffers it had are in hand again. */
static void closePreview(struct UvcPreview *up)
{
    if(up->up_Session)
    {
        VC_CloseSession(up->up_Session);
        up->up_Session = NULL;
    }
    up->up_Shown = NULL;
    up->up_Pending = NULL;
    if(up->up_Window)
    {
        CloseWindow(up->up_Window);
        up->up_Window = NULL;
    }
}

/* Lend a read's buffer of that many bytes to the session: a decoder's packet
   or a converter's frame, each knowing its read. FALSE if it is refused.
   The two point at each other for the buffer's whole life: sendFrame()
   finds the object from the read, and the item that gives the object back
   (VCIT_PacketReturned, VCIT_FrameReturned) finds the read to send to the
   device again. */
static BOOL importRead(struct UvcPreview *up, struct UvcRead *rd, ULONG bytes)
{
    if(up->up_Path->pp_Input)
    {
        /* a frame's size is the picture's, which the session was opened
           with (VCA_Width, VCA_Height, VCA_InputFormat, VCA_InputPitch) */
        struct VCFrame *frame = VC_ImportFrame(up->up_Session, VCS_Input, rd->rd_Buffer);

        if(frame)
        {
            frame->UserData = rd;
        }
        rd->rd_UserData = frame;
    } else {
        /* a packet holds as much as the buffer: an MJPEG's room in front of
           the largest frame */
        struct VCPacket *packet = VC_ImportPacket(up->up_Session, VCS_Input, rd->rd_Buffer, bytes);

        if(packet)
        {
            packet->UserData = rd;
        }
        rd->rd_UserData = packet;
    }
    return rd->rd_UserData != NULL;
}

/* Open the window and a session for the mode in force, and lend it the
   reads' buffers of that many bytes. FALSE with the reason kept. */
static BOOL openPreview(struct UvcPreview *up, ULONG bytes)
{
    const struct PreviewPath *pp = up->up_Path;
    ULONG written = sessionFormats(pp);

    if(!written)
    {
        uvcReport("No preview: %s", sessionErrorText(VCERR_NOBACKEND));
        return FALSE;
    }
    if(!openWindow(up, written))
    {
        return FALSE;
    }

    LONG err = 0;
    BOOL convert = pp->pp_Input != 0;
    /* a decoder's packets hold whole pictures; a converter's frames are the camera's pictures as they come */
    struct TagItem tags[] =
    {
        { VCA_Codec, pp->pp_Codec },
        { VCA_Kind, pp->pp_Kind },
        { VCA_SigMask, 1UL << up->up_CmdPort->mp_SigBit },
        { convert ? TAG_IGNORE : VCA_Framed, TRUE },
        { convert ? VCA_Width : TAG_IGNORE, up->up_Mode.uvq_Width },
        { convert ? VCA_Height : TAG_IGNORE, up->up_Mode.uvq_Height },
        { convert ? VCA_InputFormat : TAG_IGNORE, pp->pp_Input },
        { convert ? VCA_InputPitch : TAG_IGNORE, up->up_Mode.uvq_Width * pp->pp_Bytes },
        { VCA_FrameRateNum, UVC_INTERVAL_UNITS },
        { VCA_FrameRateDen, up->up_Interval },
        { VCA_OutputFormat, up->up_Format->pf_VC },
        { VCA_OutputWidth, up->up_Picture.Width },
        { VCA_OutputHeight, up->up_Picture.Height },
        { VCA_OutputPitch, up->up_Picture.Width * up->up_Format->pf_Bytes },
        { VCA_OutputCount, PREVIEW_FRAMES },
        { VCA_ErrorPtr, (IPTR) &err },
        { TAG_DONE, 0 }
    };

    up->up_Session = VC_OpenSessionTagList(tags);
    if(!up->up_Session)
    {
        uvcReport("No preview: %s", sessionErrorText(err));
        return FALSE;
    }
    UVC_EACH_READ(rd, &up->up_Capture)
    {
        if(!importRead(up, rd, bytes))
        {
            uvcReport("No preview: the VideoCore did not take the buffers");
            return FALSE;
        }
    }
    return TRUE;
}

/* The decoder is lost, the stream is not: every buffer it had goes back to
   the device. */
static void dropPreview(struct UvcPreview *up, LONG err)
{
    uvcReport("No more preview: %s", sessionErrorText(err));
    closePreview(up);
    uvcSendReads(&up->up_Capture);
    tellCaller(up);
}

/* The stream's rate, whole frames a second, at least 1 */
static ULONG framesPerSecond(const struct UvcPreview *up)
{
    ULONG fps = UVC_INTERVAL_UNITS / up->up_Interval;

    return fps ? fps : 1;
}

/* The camera should send a key frame: ask it, once a second of frames
   while none comes. A camera with no way to ask, or one that refuses, sends
   one when its own interval says so. */
static void askKeyFrame(struct UvcPreview *up)
{
    if(!(up->up_KeyAsked++ % framesPerSecond(up)))
    {
        uvcCommand(up->up_Camera.cm_Request, NSCMD_UV_KEYFRAME, NULL, 0);
    }
}

/* A camera's frame of len bytes that lies JPEG_ROOM bytes into buffer
   becomes a JPEG that begins at buffer, without moving it: the room and the
   frame's own two SOI bytes are written over, ending just before the
   frame's next marker. What fills them is a new SOI, then the standard
   Huffman tables if the frame has none (they may come before the frame
   header), else a comment of the same length, so the sizes come out alike.
   The comment's bytes are what the room held, uvcMakeReads() cleared it, or
   old tables. Return the JPEG's length; 0 if the frame is not to be decoded,
   left as it was. */
static ULONG jpegFill(UBYTE *buffer, ULONG len)
{
    static const UBYTE comment[] = { 0xff, 0xd8, 0xff, 0xfe, (JPEG_ROOM - 2) >> 8, (JPEG_ROOM - 2) & 0xff };
    const UBYTE *frame = buffer + JPEG_ROOM;
    ULONG end = uvcJpegLength(frame, len);

    if(!end)
    {
        return 0;
    }
    if(mjpegMissingDHT(frame, end))
    {
        buffer[0] = 0xff;
        buffer[1] = 0xd8;
        memcpy(&buffer[2], MJPEGStdDHT, MJPEG_STD_DHT_SIZE);
    } else {
        memcpy(buffer, comment, sizeof(comment));
    }
    return end + JPEG_ROOM;
}

/* Send a frame to the session. FALSE if it does not go there: the buffer is
   still in hand, unless the session was lost and took it along. A decoder
   gives its input back at once; a converter only once its picture is out,
   which a window changing size stops for a while, so it gets a frame only
   while UVC_KEEP_READS stay at the device: a picture is lost, never a
   frame. */
static BOOL sendFrame(struct UvcPreview *up, struct UvcRead *rd)
{
    struct IOVideoReq *ivr = rd->rd_Req;
    struct UvpCounts *uc = &up->up_Counts;
    ULONG length = ivr->ivr_Std.io_Actual;
    VCTime pts = (VCTime) ivr->ivr_Sequence * (up->up_Interval / 10);

    if(up->up_Mode.uvq_FourCC == UVFCC_MJPG)
    {
        length = jpegFill(rd->rd_Buffer, length);
    }
    else if(up->up_Path->pp_Input &&
            ((length != up->up_Mode.uvq_MaxFrameSize) || (uvcReadsAtDevice(&up->up_Capture) < UVC_KEEP_READS)))
    {
        length = 0; /* a converter takes whole pictures only, and only so many */
    }
    if(!length)
    {
        uc->uc_Held++;
        return FALSE;
    }

    LONG err;

    if(up->up_Path->pp_Input)
    {
        struct VCFrame *frame = rd->rd_UserData;

        frame->Pts = pts;
        frame->Flags = 0;
        err = VC_SendFrame(up->up_Session, frame);
    } else {
        struct VCPacket *packet = rd->rd_UserData;

        packet->Length = length;
        packet->Flags = VCPF_FrameStart | VCPF_FrameEnd;
        packet->Pts = pts;
        err = VC_SendPacket(up->up_Session, packet);
    }

    if(err)
    {
        dropPreview(up, err);
        return FALSE;
    }
    uc->uc_Sent++;

    /* the VideoCore gives no error for this: it just never answers */
    if(!uc->uc_Pictures && (uc->uc_Sent == SILENT_SECS * framesPerSecond(up)))
    {
        uvcReport("No picture from the VideoCore: raise gpu_mem in config.txt");
        tellCaller(up);
    }
    return TRUE;
}

/* The latest picture of a batch is the one to show: an older one still
   pending goes straight back, so a slow window costs pictures, not delay */
static void pictureIn(struct UvcPreview *up, struct VCFrame *frame)
{
    struct UvpCounts *uc = &up->up_Counts;

    if(up->up_Pending)
    {
        VC_ReleaseFrame(up->up_Session, up->up_Pending);
    }
    up->up_Pending = frame;
    uc->uc_Pictures++;
    uc->uc_Lag = uc->uc_Sent - uc->uc_Pictures;
    if(uc->uc_Lag > uc->uc_LagMost)
    {
        uc->uc_LagMost = uc->uc_Lag;
    }
}

/* Take what the session has */
static void pollSession(struct UvcPreview *up)
{
    struct VCItem item;

    while(up->up_Session && VC_Poll(up->up_Session, &item))
    {
        switch(item.Type)
        {
            case VCIT_PacketReturned:
                uvcSendRead(item.u.Packet->UserData);
                break;

            case VCIT_FrameReturned:
                uvcSendRead(item.u.Frame->UserData);
                break;

            case VCIT_Frame:
                pictureIn(up, item.u.Frame);
                break;

            case VCIT_Event:
                if(item.u.Event.Event == VCEV_Error)
                {
                    dropPreview(up, item.u.Event.Status);
                }
                break;
        }
    }
}

/*
 * The stream
 */

/* Stop the stream, if it runs, the one order: see the head of this file */
static void stopStream(struct UvcPreview *up)
{
    uvcHaltCapture(&up->up_Capture);
    closePreview(up);
    uvcStopCapture(&up->up_Capture);
    uvcStopRecording(&up->up_Recording);
}

/* Set the mode, get the reads ready, preview if there can be one, start.
   FALSE with the reason kept and nothing running. */
static BOOL startStream(struct UvcPreview *up, const struct UsbVideoModeReq *uvq)
{
    struct UvcCapture *cp = &up->up_Capture;
    struct UvcCamera *cm = &up->up_Camera;

    up->up_Why[0] = 0;
    memset(&up->up_Counts, 0, sizeof(up->up_Counts));
    up->up_KeyAsked = 0;
    up->up_Mode = *uvq;

    if(uvcSetMode(cm, &up->up_Mode))
    {
        return FALSE;
    }

    up->up_Path = previewPath(up->up_Mode.uvq_FourCC);

    BOOL preview = up->up_Path && openLibraries(up);
    ULONG room = (up->up_Mode.uvq_FourCC == UVFCC_MJPG) ? JPEG_ROOM : 0;
    ULONG size = up->up_Mode.uvq_MaxFrameSize;

    up->up_Interval = up->up_Mode.uvq_Interval ? up->up_Mode.uvq_Interval : USUAL_INTERVAL;
    if(!up->up_Path)
    {
        uvcReport("No preview: only MJPEG, H.264, H.265, YUY2 and NV12 can be shown");
    }

    /* the buffers go to the VPU */
    BOOL ok = uvcMakeReads(cp, cm, room, size, preview ? MEMF_FAST : MEMF_ANY);

    if(ok)
    {
        if(preview && !openPreview(up, room + size))
        {
            closePreview(up);
        }
        ok = uvcStartStream(cp);
    }
    if(!ok)
    {
        stopStream(up);
    }
    return ok;
}

/* Record a frame, if a recording runs; a failed one ends, the caller told */
static void recordFrame(struct UvcPreview *up, const struct UvcRead *rd)
{
    if(uvcRecordFrame(&up->up_Recording, rd->rd_Req))
    {
        up->up_Counts.uc_Saved = up->up_Recording.rc_Saved;
        return;
    }
    uvcStopRecording(&up->up_Recording);
    tellCaller(up);
}

/* A read came back with a frame: record it, decode it */
static void frameIn(struct UvcPreview *up, struct UvcRead *rd)
{
    struct UvcCapture *cp = &up->up_Capture;
    struct UvpCounts *uc = &up->up_Counts;
    struct IOVideoReq *ivr = rd->rd_Req;
    ULONG len = ivr->ivr_Std.io_Actual;
    BOOL stream = uvcIsAnnexB(up->up_Mode.uvq_FourCC);
    BOOL key = stream && uvcIsKeyFrame(up->up_Mode.uvq_FourCC, ivr->ivr_Std.io_Data, len);
    ULONG lost = uvcLost(cp);

    uc->uc_Frames++;
    uc->uc_Bytes += len;
    if(!(uc->uc_Frames % framesPerSecond(up)))
    {
        Signal(up->up_Caller, up->up_TickMask);
    }
    if(key)
    {
        up->up_KeyAsked = 0;
    }
    else if(stream && (lost != uc->uc_Lost))
    {
        askKeyFrame(up); /* after a hole the decoder gives rubbish until one comes */
    }
    uc->uc_Lost = lost;
    recordFrame(up, rd);
    if((!up->up_Session || !sendFrame(up, rd)) && !rd->rd_AtDevice)
    {
        uvcSendRead(rd);
    }
}

/* Take every read that came back */
static void takeFrames(struct UvcPreview *up)
{
    struct UvcCapture *cp = &up->up_Capture;
    struct UvcRead *rd;

    while(cp->cp_Streaming && (rd = uvcTakeFrame(cp)))
    {
        frameIn(up, rd);
    }
    if(cp->cp_Streaming && cp->cp_Error)
    {
        uvcReport("The stream ended: %s", uvcErrorText(cp->cp_Error));
        stopStream(up);
        tellCaller(up);
    }
}

/* The window changed size: the session's output takes the new picture size
   and makes new frames, all held ones gone with the old. Same size (a width
   rounded to the same group, say): the session would free nothing, so nothing
   is to be forgotten. A size the scaler cannot do would end the session, so
   only what fitPicture() gives goes there. */
static void resizePicture(struct UvcPreview *up)
{
    struct IBox pic;

    if(!fitPicture(up, &pic))
    {
        return; /* the old picture stays until it fits again */
    }

    BOOL same = (pic.Width == up->up_Picture.Width) && (pic.Height == up->up_Picture.Height);

    up->up_Picture = pic;
    up->up_Bars = TRUE;
    if(same)
    {
        return;
    }
    up->up_Shown = NULL;
    up->up_Pending = NULL;

    LONG err = VC_SetAttrsTags(up->up_Session, VCA_OutputWidth, pic.Width, VCA_OutputHeight, pic.Height,
                               VCA_OutputPitch, pic.Width * up->up_Format->pf_Bytes, TAG_DONE);

    if(err)
    {
        dropPreview(up, err);
    }
}

/* Take the window's messages. Closing it ends the stream. They come after the
   session's: a resize then works on the frames of that moment. */
static void serveWindow(struct UvcPreview *up)
{
    struct IntuiMessage *msg;
    BOOL closed = FALSE;
    BOOL resized = FALSE;

    if(!up->up_Window)
    {
        return;
    }
    while((msg = (struct IntuiMessage *) GetMsg(up->up_Window->UserPort)))
    {
        switch(msg->Class)
        {
            case IDCMP_CLOSEWINDOW:
                closed = TRUE;
                break;

            case IDCMP_NEWSIZE:
                resized = TRUE;
                break;

            case IDCMP_REFRESHWINDOW:
                up->up_Refresh = TRUE;
                break;
        }
        ReplyMsg((struct Message *) msg);
    }
    if(closed)
    {
        stopStream(up);
        tellCaller(up);
    }
    else if(resized)
    {
        resizePicture(up);
    }
}

/* Do a command. FALSE if it failed, with the reason kept. */
static BOOL obey(struct UvcPreview *up, struct UvpCommand *cmd)
{
    switch(cmd->cmd_What)
    {
        case UVPC_START:
            return !uvpRunning(up) && startStream(up, cmd->cmd_Data);

        case UVPC_STOP:
            stopStream(up);
            return TRUE;

        case UVPC_RECORD:
            uvcStopRecording(&up->up_Recording);
            return !cmd->cmd_Data ||
                   (uvpRunning(up) && uvcStartRecording(&up->up_Recording, &up->up_Camera, up->up_Stem, &up->up_Mode));

        case UVPC_SETTINGS:
        {
            LONG err = uvcCommand(up->up_Camera.cm_Request, NSCMD_UV_SETTINGS, NULL, 0);

            if(err)
            {
                uvcReport("No camera settings window: %s", uvcErrorText(err));
            }
            return !err;
        }
    }
    return FALSE;
}

/* Answer the commands that came. Return the QUIT, which is answered last of
   all; NULL if there is none. */
static struct UvpCommand *serveCommands(struct UvcPreview *up)
{
    struct UvpCommand *cmd;

    while((cmd = (struct UvpCommand *) GetMsg(up->up_CmdPort)))
    {
        if(cmd->cmd_What == UVPC_QUIT)
        {
            return cmd;
        }
        cmd->cmd_Ok = obey(up, cmd);
        ReplyMsg(&cmd->cmd_Msg);
    }
    return NULL;
}

/* The signals that wake the loop */
static ULONG wakeMask(const struct UvcPreview *up)
{
    ULONG mask = (1UL << up->up_CmdPort->mp_SigBit) | (1UL << up->up_Camera.cm_Port->mp_SigBit);

    if(up->up_Window)
    {
        mask |= 1UL << up->up_Window->UserPort->mp_SigBit;
    }
    return mask;
}

/* Run until the QUIT comes; return it. Every source is emptied before the
   Wait(), and no DoIO() on the camera comes after its port is, while frames
   flow: a DoIO() can eat the port's signal. */
static struct UvpCommand *streamLoop(struct UvcPreview *up)
{
    for(;;)
    {
        struct UvpCommand *quit = serveCommands(up);

        if(quit)
        {
            return quit;
        }
        takeFrames(up);
        pollSession(up);
        serveWindow(up);
        if(up->up_Window)
        {
            showPending(up);
            repairWindow(up);
        }
        Wait(wakeMask(up));
    }
}

/* The process: all it owns is made here, so its signals are its own */
static void streamProcess(void)
{
    struct Process *me = (struct Process *) FindTask(NULL);

    WaitPort(&me->pr_MsgPort);

    struct UvpCommand *cmd = (struct UvpCommand *) GetMsg(&me->pr_MsgPort);
    struct UvcPreview *up = cmd->cmd_Data;

    me->pr_WindowPtr = (APTR) -1; /* a full disk is an error, not a requester no one may see */
    uvcReportTo = up->up_Why;
    up->up_CmdPort = CreateMsgPort();
    if(up->up_CmdPort)
    {
        cmd->cmd_Ok = uvcOpenCamera(&up->up_Camera, up->up_Unit);
    } else {
        uvcReport("Not enough memory");
    }
    if(cmd->cmd_Ok)
    {
        ReplyMsg(&cmd->cmd_Msg);
        cmd = streamLoop(up);
        stopStream(up);
    }
    uvcCloseCamera(&up->up_Camera);
    DeleteMsgPort(up->up_CmdPort);
    CloseLibrary(VideoCodecBase);
    VideoCodecBase = NULL;
    CloseLibrary(P96Base);
    P96Base = NULL;
    uvcReportTo = NULL;

    /* the caller may unload this code once it has the reply: Forbid() holds
       until this process is gone */
    Forbid();
    ReplyMsg(&cmd->cmd_Msg);
}

/*
 * The caller's side
 */

/* Send a command and wait for its answer */
static BOOL command(struct UvcPreview *up, ULONG what, APTR data)
{
    struct UvpCommand cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_Msg.mn_ReplyPort = up->up_ReplyPort;
    cmd.cmd_Msg.mn_Length = sizeof(cmd);
    cmd.cmd_What = what;
    cmd.cmd_Data = data;
    PutMsg(up->up_CmdPort, &cmd.cmd_Msg);
    WaitPort(up->up_ReplyPort);
    GetMsg(up->up_ReplyPort);
    return cmd.cmd_Ok;
}

struct UvcPreview *uvpOpen(ULONG unit, ULONG stopmask, ULONG tickmask, UBYTE *why)
{
    struct UvcPreview *up = AllocVec(sizeof(*up), MEMF_PUBLIC | MEMF_CLEAR);
    struct Process *proc = NULL;

    strcpy((char *) why, "Not enough memory");
    if(!up)
    {
        return NULL;
    }
    up->up_Caller = FindTask(NULL);
    up->up_StopMask = stopmask;
    up->up_TickMask = tickmask;
    up->up_Unit = unit;
    up->up_ReplyPort = CreateMsgPort();
    if(up->up_ReplyPort)
    {
        proc = CreateNewProcTags(NP_Entry, (IPTR) streamProcess, NP_Name, (IPTR) STREAM_NAME,
                                 NP_StackSize, STREAM_STACK, NP_Priority, STREAM_PRI, TAG_DONE);
    }
    if(proc)
    {
        up->up_CmdPort = &proc->pr_MsgPort; /* until the process has a port of its own */
        if(command(up, UVPC_OPEN, up))
        {
            return up;
        }
        strcpy((char *) why, (const char *) up->up_Why);
    }
    DeleteMsgPort(up->up_ReplyPort);
    FreeVec(up);
    return NULL;
}

void uvpClose(struct UvcPreview *up)
{
    command(up, UVPC_QUIT, NULL);
    DeleteMsgPort(up->up_ReplyPort);
    FreeVec(up);
}

const struct UvcCamera *uvpCamera(const struct UvcPreview *up)
{
    return &up->up_Camera;
}

const struct UvpCounts *uvpCounts(const struct UvcPreview *up)
{
    return &up->up_Counts;
}

const struct UsbVideoModeReq *uvpMode(const struct UvcPreview *up)
{
    return &up->up_Mode;
}

CONST_STRPTR uvpWhy(const struct UvcPreview *up)
{
    return up->up_Why;
}

BOOL uvpRunning(const struct UvcPreview *up)
{
    return up->up_Capture.cp_Streaming;
}

struct Window *uvpWindow(const struct UvcPreview *up)
{
    return up->up_Window;
}

void uvpKeepClear(struct UvcPreview *up, const struct IBox *box)
{
    up->up_Clear = *box;
}

BOOL uvpStart(struct UvcPreview *up, const struct UsbVideoModeReq *uvq, STRPTR stem)
{
    up->up_Stem[0] = 0;
    if(stem)
    {
        strncpy((char *) up->up_Stem, (const char *) stem, sizeof(up->up_Stem) - 1);
    }
    return command(up, UVPC_START, (APTR) uvq);
}

void uvpStop(struct UvcPreview *up)
{
    command(up, UVPC_STOP, NULL);
}

BOOL uvpRecord(struct UvcPreview *up, BOOL on)
{
    return command(up, UVPC_RECORD, (APTR) (IPTR) on);
}

BOOL uvpSettings(struct UvcPreview *up)
{
    return command(up, UVPC_SETTINGS, NULL);
}
