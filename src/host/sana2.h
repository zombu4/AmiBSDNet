/*
 * SANA-II network device interface: the subset AmiBSDNet uses
 * (src/host/sana2.c, src/common/probe.c).  Values as in the SANA-II
 * headers of the NDK: downloads/sources/NDK3.2/SANA+RoadshowTCP-IP/
 * include/devices/sana2.h, Include_H/devices/newstyle.h, and wifipi's
 * include/devices/sana2wireless.h for the wireless commands.
 */
#ifndef AMIBSDNET_SANA2_H
#define AMIBSDNET_SANA2_H

#include <exec/types.h>
#include <exec/io.h>
#include <utility/tagitem.h>

#define	SANA2_MAX_ADDR_BITS	128
#define	SANA2_MAX_ADDR_BYTES	((SANA2_MAX_ADDR_BITS + 7) / 8)

struct IOSana2Req {
	struct IORequest ios2_Req;
	ULONG	ios2_WireError;
	ULONG	ios2_PacketType;
	UBYTE	ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];
	UBYTE	ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
	ULONG	ios2_DataLength;
	APTR	ios2_Data;
	APTR	ios2_StatData;
	APTR	ios2_BufferManagement;
};

/* io_Flags */
#define	SANA2IOB_BCAST		6
#define	SANA2IOF_BCAST		(1 << SANA2IOB_BCAST)

/* buffer management tags */
#define	S2_Dummy		(TAG_USER + 0xb0000)
#define	S2_CopyToBuff		(S2_Dummy + 1)
#define	S2_CopyFromBuff		(S2_Dummy + 2)

/* commands */
#define	S2_GETSTATIONADDRESS	(CMD_NONSTD + 1)
#define	S2_CONFIGINTERFACE	(CMD_NONSTD + 2)
#define	S2_ADDMULTICASTADDRESS	(CMD_NONSTD + 5)
#define	S2_DELMULTICASTADDRESS	(CMD_NONSTD + 6)
#define	S2_MULTICAST		(CMD_NONSTD + 7)
#define	S2_BROADCAST		(CMD_NONSTD + 8)
#define	S2_ONEVENT		(CMD_NONSTD + 14)
#define	S2_ONLINE		(CMD_NONSTD + 16)

/* io_Error */
#define	S2ERR_NO_RESOURCES	1
#define	S2ERR_BAD_STATE		4
#define	S2ERR_NOT_SUPPORTED	8
#define	S2ERR_SOFTWARE		9

/* S2_ONEVENT */
#define	S2EVENT_ONLINE		(1UL << 3)
#define	S2EVENT_OFFLINE		(1UL << 4)
#define	S2EVENT_CONNECT		(1UL << 9)
#define	S2EVENT_DISCONNECT	(1UL << 10)

/* SANA-II wireless extensions */
#define	S2_GETSIGNALQUALITY	0xc010
#define	S2_GETNETWORKS		0xc011

/* New Style Device query */
#define	NSCMD_DEVICEQUERY	0x4000
struct NSDeviceQueryResult {
	ULONG	DevQueryFormat;
	ULONG	SizeAvailable;
	UWORD	DeviceType;
	UWORD	DeviceSubType;
	UWORD	*SupportedCommands;
};

#endif /* AMIBSDNET_SANA2_H */
