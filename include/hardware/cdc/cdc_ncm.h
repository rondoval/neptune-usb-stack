#ifndef HARDWARE_CDC_NCM_H
#define HARDWARE_CDC_NCM_H

#include <exec/types.h>

#if defined(__GNUC__)
# pragma pack(1)
#endif

#define CDC_NCM_FUNC_DESC_SUBTYPE    0x1a

#define CDC_NCM_NTH16_SIGNATURE      0x484d434eUL /* "NCMH" */
#define CDC_NCM_NDP16_SIGNATURE_NCM0 0x304d434eUL /* "NCM0" */
#define CDC_NCM_NDP16_SIGNATURE_NCM1 0x314d434eUL /* "NCM1" */

/* Class-specific requests (NCM 1.0 Table 6-2) */
#define CDC_NCM_GET_NTB_PARAMETERS   0x80
#define CDC_NCM_SET_NTB_INPUT_SIZE   0x86

/* bmNetworkCapabilities (NCM 1.0 Table 5-2) */
#define CDC_NCM_NCAP_ETH_FILTER      0x01 /* SetEthernetPacketFilter supported */

/* Smallest IN NTB size the host may select (NCM 1.0 Table 6-4) */
#define CDC_NCM_NTB_MIN_IN_SIZE      2048

/* NCM Functional Descriptor (NCM 1.0 Table 5-2) */
struct UsbCDCNcmFunctionalDesc
{
    UBYTE bFunctionLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubtype;
    UWORD bcdNcmVersion;
    UBYTE bmNetworkCapabilities;
};

/* GET_NTB_PARAMETERS reply, little-endian (NCM 1.0 Table 6-3, Errata 1) */
struct UsbCDCNcmNtbParameters
{
    UWORD wLength;
    UWORD bmNtbFormatsSupported;
    ULONG dwNtbInMaxSize;
    UWORD wNdpInDivisor;
    UWORD wNdpInPayloadRemainder;
    UWORD wNdpInAlignment;
    UWORD wReserved;
    ULONG dwNtbOutMaxSize;
    UWORD wNdpOutDivisor;
    UWORD wNdpOutPayloadRemainder;
    UWORD wNdpOutAlignment;
    UWORD wNtbOutMaxDatagrams;
};

struct UsbCDCNcmNtb16Header
{
    ULONG dwSignature;
    UWORD wHeaderLength;
    UWORD wSequence;
    UWORD wBlockLength;
    UWORD wNdpIndex;
};

struct UsbCDCNcmNdp16
{
    ULONG dwSignature;
    UWORD wLength;
    UWORD wNextNdpIndex;
};

struct UsbCDCNcmDatagramPointer16
{
    UWORD wDatagramIndex;
    UWORD wDatagramLength;
};

#if defined(__GNUC__)
# pragma pack()
#endif

#endif
