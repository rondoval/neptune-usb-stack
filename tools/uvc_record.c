/*
 *----------------------------------------------------------------------------
 *              UVC tools: recording
 *----------------------------------------------------------------------------
 * Implements uvc_record.h: a camera's frames into Matroska files, through the
 * mkv-writer muxer (tools/mkv).
 */

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dos.h>
#include <exec/memory.h>

#include <stdio.h>
#include <string.h>

#include "uvc_record.h"
#include "mjpegdht.h"
#include "mkv.h"

#define USUAL_INTERVAL (UVC_INTERVAL_UNITS / 30) /* should a mode give none */
#define MOVIE_ENDING   ".mkv"
#define MOVIE_NUMBER   "-0000" /* what the name adds to the stem, besides the ending */
#define MOVIE_APP      "Neptune USBVideo"
#define MOVIE_ROOM     (1UL << 20) /* of the muxer's memory, past a quarter more than a frame */
#define MOVIE_ALL      0xffffffffUL /* what waits for the file: all of it */
#define MOVIE_FILE_MOST (0xffffffffUL - (64UL << 20)) /* the muxer counts to 4 GiB: a new file well before */

/* A format a recording takes, and how the muxer stores it */
struct MovieFormat
{
    ULONG mf_FourCC;
    int   mf_Codec;  /* MKV_ */
};

static const struct MovieFormat MovieFormats[] =
{
    { UVFCC_H264, MKV_H264 },
    { UVFCC_H265, MKV_H265 },
    { UVFCC_MJPG, MKV_MJPEG },
    { UVFCC_YUY2, MKV_RAW },
    { UVFCC_NV12, MKV_RAW }
};

/* A recording, in Matroska files of up to 4 GiB; the muxer's memory follows it */
struct UvcMovie
{
    struct mkv        mv_Mux;
    struct mkv_io     mv_Io;
    struct mkv_track  mv_Track;
    ULONG             mv_Memory;    /* the muxer's, in bytes */
    struct UvcCamera *mv_Camera;    /* asked for a key frame while H.264 or H.265 waits for one */
    CONST_STRPTR      mv_Stem;      /* the files' names begin with it */
    ULONG             mv_FrameSize; /* of a raw format's frame: no other is whole */
    BPTR              mv_File;      /* the file being written */
    BOOL              mv_Begun;     /* its first frame is in */
    ULONG             mv_KeyWait;   /* frames left out before it, waiting for a key frame */
    ULONG             mv_ClockKHz;  /* of ivr_PTS, 0 if the camera has no clock */
    ULONG             mv_Interval;  /* of the mode's frames, 100 ns units */
    ULONG             mv_LastPTS;   /* of the last frame in */
    ULONG             mv_LastSeq;
    ULONG             mv_Ms;        /* its time, from the file's first */
    ULONG             mv_Rest;      /* of its clock's ticks, what was left below a ms */
};


/* The muxer's write (struct mkv_io); nonzero when all of it went */
static int movieWrite(void *user, const void *data, uint32_t size)
{
    struct UvcMovie *mv = user;

    return Write(mv->mv_File, (APTR) data, size) == (LONG) size;
}

/* The muxer's seek (struct mkv_io); nonzero when the next write goes there.
   From above 2 GiB Seek() answers a negative old position, so IoErr() tells. */
static int movieSeek(void *user, uint32_t position)
{
    struct UvcMovie *mv = user;

    SetIoErr(0);
    Seek(mv->mv_File, position, OFFSET_BEGINNING);
    return IoErr() == 0;
}

/* The format a recording is of, NULL if it cannot be recorded */
static const struct MovieFormat *movieFormat(ULONG fourcc)
{
    for(ULONG cnt = 0; cnt < sizeof(MovieFormats) / sizeof(MovieFormats[0]); cnt++)
    {
        if(MovieFormats[cnt].mf_FourCC == fourcc)
        {
            return &MovieFormats[cnt];
        }
    }
    return NULL;
}

/* Return TRUE for a format that can be recorded */
BOOL uvcCanRecord(ULONG fourcc)
{
    return movieFormat(fourcc) != NULL;
}

/* Say why the recording stopped */
static void recordingFailed(const struct UvcRecording *rc, int error)
{
    uvcReport((error == MKV_ERR_STREAM) ? "The camera's stream cannot go into '%s': no parameter sets at its key frame" :
                                          "Could not write '%s'", rc->rc_Name);
}

/* Begin the recording's next file, <stem>-NNNN.mkv at the first free number,
   so that none is written over. On failure report the reason and return
   FALSE. */
static BOOL fileBegin(struct UvcRecording *rc)
{
    struct UvcMovie *mv = rc->rc_Movie;
    ULONG number = 0;
    BPTR lock;

    do
    {
        snprintf((char *) rc->rc_Name, UVC_NAME_SIZE, "%s-%04lu%s", (const char *) mv->mv_Stem, ++number, MOVIE_ENDING);
        if((lock = Lock(rc->rc_Name, SHARED_LOCK)))
        {
            UnLock(lock);
        }
    } while(lock);
    if(!(mv->mv_File = Open(rc->rc_Name, MODE_NEWFILE)))
    {
        uvcReport("Could not open '%s' for writing", rc->rc_Name);
        return FALSE;
    }
    mkv_open(&mv->mv_Mux, &mv->mv_Io, &mv->mv_Track, mv + 1, mv->mv_Memory);
    mv->mv_Begun = FALSE;
    mv->mv_KeyWait = 0;
    mv->mv_Ms = 0;
    mv->mv_Rest = 0;
    if(!rc->rc_Files++)
    {
        strcpy((char *) rc->rc_First, (const char *) rc->rc_Name);
    }
    return TRUE;
}

/* Finish the file being written: its last writes, and away with it if
   nothing went in. FALSE if those writes failed, with the reason reported. */
static BOOL fileEnd(struct UvcRecording *rc)
{
    struct UvcMovie *mv = rc->rc_Movie;
    int error = mkv_close(&mv->mv_Mux);

    Close(mv->mv_File);
    if(!mv->mv_Mux.pictures)
    {
        DeleteFile(rc->rc_Name);
    }
    if(error == MKV_ERR_WRITE)
    {
        recordingFailed(rc, error);
        return FALSE;
    }
    return TRUE;
}

/* Start recording, in the mode set on cm, into Matroska files named from the
   stem (fileBegin()). If there is no stem, the format cannot be recorded or
   the file cannot be opened, report the reason, leave the recording closed
   and return FALSE. */
BOOL uvcStartRecording(struct UvcRecording *rc, struct UvcCamera *cm, CONST_STRPTR stem,
                       const struct UsbVideoModeReq *uvq)
{
    ULONG fourcc = uvq->uvq_FourCC;
    ULONG interval = uvq->uvq_Interval ? uvq->uvq_Interval : USUAL_INTERVAL;
    ULONG memory = uvq->uvq_MaxFrameSize + (uvq->uvq_MaxFrameSize >> 2) + MOVIE_ROOM;
    ULONG fps = (UVC_INTERVAL_UNITS + (interval >> 1)) / interval;

    rc->rc_Movie = NULL;
    rc->rc_Saved = 0;
    rc->rc_Files = 0;
    if(!stem || !stem[0])
    {
        uvcReport("No file name to save to");
        return FALSE;
    }
    if(strlen(stem) + sizeof(MOVIE_NUMBER MOVIE_ENDING) > UVC_NAME_SIZE)
    {
        uvcReport("The name '%s' is too long", stem);
        return FALSE;
    }
    if(!uvcCanRecord(fourcc))
    {
        uvcReport("Only H264, H265, MJPG, YUY2 and NV12 can be recorded");
        return FALSE;
    }

    struct UvcMovie *mv = AllocVec(sizeof(*mv) + memory, MEMF_ANY | MEMF_CLEAR);
    struct UsbVideoInfo uvi; /* cm_Info is from the open, and the clock comes with a mode */

    if(!mv)
    {
        uvcReport("Not enough memory to record: %ld bytes", memory);
        return FALSE;
    }
    mv->mv_Io.user = mv;
    mv->mv_Io.write = movieWrite;
    mv->mv_Io.seek = movieSeek;
    mv->mv_Track.codec = movieFormat(fourcc)->mf_Codec;
    mv->mv_Track.fourcc = fourcc;
    mv->mv_Track.width = uvq->uvq_Width;
    mv->mv_Track.height = uvq->uvq_Height;
    mv->mv_Track.fps = fps ? fps : 1;
    mv->mv_Track.app = MOVIE_APP;
    mv->mv_Memory = memory;
    mv->mv_Camera = cm;
    mv->mv_Stem = stem;
    mv->mv_FrameSize = uvq->uvq_MaxFrameSize;
    mv->mv_Interval = interval;
    if(!uvcCommand(cm->cm_Request, NSCMD_UV_GETINFO, &uvi, sizeof(uvi)))
    {
        mv->mv_ClockKHz = uvi.uvi_ClockFrequency / 1000;
    }
    rc->rc_Movie = mv;
    if(!fileBegin(rc))
    {
        FreeVec(mv);
        rc->rc_Movie = NULL;
        return FALSE;
    }
    return TRUE;
}

/* Go on in a new file; on failure report the reason, end the recording and
   return FALSE */
static BOOL fileNext(struct UvcRecording *rc)
{
    struct UvcMovie *mv = rc->rc_Movie;

    if(!fileEnd(rc) || !fileBegin(rc))
    {
        FreeVec(mv);
        rc->rc_Movie = NULL;
        return FALSE;
    }
    return TRUE;
}

/* An H.264 or H.265 file begins at a key frame: ask the camera for one, once
   a second of frames while none comes. A camera with no way to ask, or one
   that refuses, sends one when its own interval says so. */
static void waitKeyFrame(struct UvcMovie *mv)
{
    if(!(mv->mv_KeyWait++ % mv->mv_Track.fps))
    {
        uvcCommand(mv->mv_Camera->cm_Request, NSCMD_UV_KEYFRAME, NULL, 0);
    }
}

/* Return the time of a frame going in, in ms from the file's first. The
   camera's clock says when it took the picture, whatever the light does to
   its rate; without one, frames count at the mode's rate. */
static ULONG movieTime(struct UvcMovie *mv, const struct IOVideoReq *ivr)
{
    BOOL clock = (ivr->ivr_Flags & IVRF_PTS) && mv->mv_ClockKHz;
    ULONG perms = clock ? mv->mv_ClockKHz : UVC_INTERVAL_UNITS / 1000;
    ULONG ticks = clock ? ivr->ivr_PTS - mv->mv_LastPTS : (ivr->ivr_Sequence - mv->mv_LastSeq) * mv->mv_Interval;

    if(mv->mv_Begun && ((LONG) ticks > 0))
    {
        mv->mv_Rest += ticks % perms;
        mv->mv_Ms += ticks / perms + mv->mv_Rest / perms;
        mv->mv_Rest %= perms;
    }
    mv->mv_LastPTS = ivr->ivr_PTS;
    mv->mv_LastSeq = ivr->ivr_Sequence;
    return mv->mv_Ms;
}

/* Return how much of a frame goes in: H.264 and H.265 as they are, a JPEG
   up to its EOI, a raw frame only if it is whole; 0 to leave it out */
static ULONG frameLength(const struct UvcMovie *mv, const UBYTE *frame, ULONG len)
{
    switch(mv->mv_Track.codec)
    {
        case MKV_MJPEG:
            return uvcJpegLength(frame, len);

        case MKV_RAW:
            return (len == mv->mv_FrameSize) ? len : 0;

        default:
            return len;
    }
}

/* Put the frame a read brought into the recording, and what waits into the
   file; nothing if no recording is open. Close to 4 GiB it goes on in a new
   file. H.264 and H.265 can only begin a file at a key frame, asked for
   while they wait, and a frame with nothing in it is left out; neither counts as
   saved. On failure report the reason and return FALSE. */
BOOL uvcRecordFrame(struct UvcRecording *rc, const struct IOVideoReq *ivr)
{
    struct UvcMovie *mv = rc->rc_Movie;

    if(!mv)
    {
        return TRUE;
    }

    struct mkv *mux = &mv->mv_Mux;
    const UBYTE *frame = ivr->ivr_Std.io_Data;
    ULONG len = frameLength(mv, frame, ivr->ivr_Std.io_Actual);
    BOOL key = uvcIsKeyFrame(mv->mv_Track.fourcc, frame, len);

    if(len && mv->mv_Begun && (mkv_bytes(mux) + len + MJPEG_STD_DHT_SIZE > MOVIE_FILE_MOST) && !fileNext(rc))
    {
        return FALSE;
    }
    if(!len)
    {
        return TRUE;
    }
    if(!mv->mv_Begun && !key)
    {
        waitKeyFrame(mv);
        return TRUE;
    }

    ULONG ms = movieTime(mv, ivr);
    ULONG dht = (mv->mv_Track.codec == MKV_MJPEG) ? mjpegMissingDHT(frame, len) : 0;

    mv->mv_Begun = TRUE;
    mkv_picture_begin(mux, ms, key);
    mkv_picture_bytes(mux, frame, dht);
    if(dht)
    {
        mkv_picture_bytes(mux, MJPEGStdDHT, MJPEG_STD_DHT_SIZE);
    }
    mkv_picture_bytes(mux, frame + dht, len - dht);

    int error = mkv_picture_end(mux);

    if(!error)
    {
        error = mkv_flush(mux, MOVIE_ALL);
    }
    if(error)
    {
        recordingFailed(rc, error);
        return FALSE;
    }
    rc->rc_Saved++;
    return TRUE;
}

/* Finish the file, if one is open */
void uvcStopRecording(struct UvcRecording *rc)
{
    if(rc->rc_Movie)
    {
        fileEnd(rc);
        FreeVec(rc->rc_Movie);
        rc->rc_Movie = NULL;
    }
}

/* Print the frames recorded and the files' names; nothing if none */
void uvcPrintSaved(const struct UvcRecording *rc)
{
    if(!rc->rc_Saved)
    {
        return;
    }
    Printf("%ld %s saved ", rc->rc_Saved, (rc->rc_Saved == 1) ? "frame" : "frames");
    if(rc->rc_Files > 1)
    {
        Printf("in %ld files, %s to %s\n", rc->rc_Files, rc->rc_First, rc->rc_Name);
    } else {
        Printf("as %s\n", rc->rc_Name);
    }
}
