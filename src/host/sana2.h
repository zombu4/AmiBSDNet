/*
 * SANA-II network device interface: the subset AmiBSDNet uses.
 * Written from the SANA-II specification (Commodore-Amiga, 1992); the
 * values are fixed by that specification and shared by every driver.
 */
#ifndef AMIBSDNET_SANA2_H
#define AMIBSDNET_SANA2_H

#include <exec/types.h>
#include <exec/io.h>
#include <utility/tagitem.h>

#define	SANA2_MAX_ADDR_BITS	128
#define	SANA2_MAX_ADDR_BYTES	(SANA2_MAX_ADDR_BITS / 8)

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

struct Sana2DeviceQuery {
	ULONG	SizeAvailable;
	ULONG	SizeSupplied;
	ULONG	DevQueryFormat;
	ULONG	DeviceLevel;
	UWORD	AddrFieldSize;		/* bits */
	ULONG	MTU;
	ULONG	BPS;
	ULONG	HardwareType;
};

/* io_Flags */
#define	SANA2IOB_RAW		7
#define	SANA2IOB_BCAST		6
#define	SANA2IOB_MCAST		5
#define	SANA2IOF_RAW		(1 << SANA2IOB_RAW)
#define	SANA2IOF_BCAST		(1 << SANA2IOB_BCAST)
#define	SANA2IOF_MCAST		(1 << SANA2IOB_MCAST)

/* OpenDevice() flags */
#define	SANA2OPF_MINE		(1 << 0)
#define	SANA2OPF_PROM		(1 << 1)

/* buffer management tags */
#define	S2_Dummy		(TAG_USER + 0xb0000)
#define	S2_CopyToBuff		(S2_Dummy + 1)
#define	S2_CopyFromBuff		(S2_Dummy + 2)
#define	S2_PacketFilter		(S2_Dummy + 3)

#define	S2WireType_Ethernet	1

/* commands */
#define	S2_DEVICEQUERY		(CMD_NONSTD + 0)
#define	S2_GETSTATIONADDRESS	(CMD_NONSTD + 1)
#define	S2_CONFIGINTERFACE	(CMD_NONSTD + 2)
#define	S2_ADDMULTICASTADDRESS	(CMD_NONSTD + 5)
#define	S2_DELMULTICASTADDRESS	(CMD_NONSTD + 6)
#define	S2_MULTICAST		(CMD_NONSTD + 7)
#define	S2_BROADCAST		(CMD_NONSTD + 8)
#define	S2_GETGLOBALSTATS	(CMD_NONSTD + 13)
#define	S2_ONEVENT		(CMD_NONSTD + 14)
#define	S2_READORPHAN		(CMD_NONSTD + 15)
#define	S2_ONLINE		(CMD_NONSTD + 16)
#define	S2_OFFLINE		(CMD_NONSTD + 17)

/* io_Error */
#define	S2ERR_NO_ERROR		0
#define	S2ERR_NO_RESOURCES	1
#define	S2ERR_BAD_ARGUMENT	3
#define	S2ERR_BAD_STATE		4
#define	S2ERR_BAD_ADDRESS	5
#define	S2ERR_MTU_EXCEEDED	6
#define	S2ERR_NOT_SUPPORTED	8
#define	S2ERR_SOFTWARE		9
#define	S2ERR_OUTOFSERVICE	10
#define	S2ERR_TX_FAILURE	11

/* ios2_WireError */
#define	S2WERR_GENERIC_ERROR	0
#define	S2WERR_NOT_CONFIGURED	1
#define	S2WERR_UNIT_ONLINE	2
#define	S2WERR_UNIT_OFFLINE	3
#define	S2WERR_IS_CONFIGURED	15

/* S2_ONEVENT */
#define	S2EVENT_ERROR		(1 << 0)
#define	S2EVENT_TX		(1 << 1)
#define	S2EVENT_RX		(1 << 2)
#define	S2EVENT_ONLINE		(1 << 3)
#define	S2EVENT_OFFLINE		(1 << 4)

#endif /* AMIBSDNET_SANA2_H */
