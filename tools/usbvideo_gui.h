#ifndef USBVIDEO_GUI_H
#define USBVIDEO_GUI_H
/*
 *----------------------------------------------------------------------------
 *              USBVideo: the window
 *----------------------------------------------------------------------------
 * Choose a camera and a mode, run the stream, save from it. The camera itself
 * is the stream process's (uvc_preview.h), so nothing done here can hold a
 * stream up. MUI is opened here: USBVideo in the Shell runs without it.
 */

#include <exec/types.h>

/* Run the window until it closes, on the camera of that unit if there is one;
   return the DOS return code. version is the $VER string. */
LONG usbVideoGui(ULONG unit, CONST_STRPTR version);

#endif /* USBVIDEO_GUI_H */
