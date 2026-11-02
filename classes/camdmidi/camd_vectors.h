/* camd_vectors.h - camdusbmidi.usbclass's extra library vectors, injected into the
 * shared class skeleton's funcTable via -DCLASS_VECTORS_HDR (see classes/class_main.c).
 *
 * The original genmodule .conf declared, after the 3 usbclass vectors:
 *     .skip 7
 *     APTR usbCAMDOpenPort(xmitfc,recvfct,userdata,idstr,port) (A0,A1,A2,A3,D0)   // LVO -90
 *     VOID usbCAMDClosePort(adapter)                           (A0)               // LVO -96
 * so the embedded CAMD MIDI driver (camd/camd_driver.c) can OpenLibrary the class and
 * call these two vectors. The 7 reserved slots map to LibNull (a harmless no-op).
 * usbCAMDOpenPort returns the struct CAMDAdapter slot the port got; usbCAMDClosePort
 * takes that slot back (not port + id: after an unplug the id no longer names a binding).
 */
/* Declared with the real signatures, not address-only placeholders: under LTO the compiler
 * sees this TU and camdusbmidi.class.c together, and a mismatched prototype is
 * -Wlto-type-mismatch (and undefined behaviour).  CLASS_BASE is in scope -- class_main.c
 * defines it before including this header. */
extern APTR usbCAMDOpenPort(APTR xmitfct asm("a0"), APTR recvfct asm("a1"),
                            APTR userdata asm("a2"), STRPTR idstr asm("a3"),
                            ULONG port asm("d0"), CLASS_BASE *nh asm("a6"));
extern void usbCAMDClosePort(APTR adapter asm("a0"), CLASS_BASE *nh asm("a6"));

#define CLASS_EXTRA_VECTORS \
    (APTR)LibNull,(APTR)LibNull,(APTR)LibNull,(APTR)LibNull,(APTR)LibNull,(APTR)LibNull,(APTR)LibNull, \
    (APTR)usbCAMDOpenPort,(APTR)usbCAMDClosePort,
