/*
 *  app_main.cpp - The application's main.
 *
 *  Copyright (c) 2015 Martin Glueck <martin@mangari.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3 as
 *  published by the Free Software Foundation.
 */

#include <sblib/eib.h>
#include <sblib/eib/bus.h>
#include <sblib/eib/apci.h>
#include <sblib/internal/iap.h>
#include <string.h>
#include <sblib/io_pin_names.h>
#include <sblib/serial.h>
#include <BCUUpdate.h>
#include <crc.h>
#include <boot_descriptor_block.h>

//#define ENABLE_EMULATION

#ifdef ENABLE_EMULATION
//#define SERIAL_DEBUG
#define RUN_OR_EMULATE(code) (emulation & 0x0F ? IAP_SUCCESS : code)
#else
#define RUN_OR_EMULATE(code) code
#endif


/**
 * Updater protocol:
 *   We miss-use the memory write EIB frames. Miss-use because we do not transmitt the address in each request
 *   to have more frame left for the actal data transmission:
 *     BYTES of teh EIB telegram:
 *       8    CMD Nummer (see enum below)
 *       9-x  CMD dependent
 *
 *    UPD_ERASE_SECTOR
 *      9    Number of the sector which should be erased
 *           if the erasing was successful a T_ACK_PDU will be returned, otherwise a T_NACK_PDU
 *    UPD_SEND_DATA
 *      9-   the actual data which will be copied into a RAM buffer for later use
 *           If the RAM buffer is not yet full a T_ACK_PDU will be returned, otherwise a T_NACK_PDU
 *           The address of the RAM buffer will be automatically incremented.
 *           After a Program or Boot Desc Aupdate, the RAM buffer address will be reseted.
 *    UPD_PROGRAM
 *      9-12 How many bytes of the RMA Buffer should be programmed. Be aware that ths value nees to be one of the following
 *           256, 512, 1024, 4096 (required by the IAP of the LPC11xx devices)
 *     13-16 Flash address the data should be programmed to
 *     16-19 The CRC of the data downloaded via the UPD_SEND_DATA commands. If the CRC does not match the
 *           programming returns an error
 *    UPD_UPDATE_BOOT_DESC
 *    UPD_PROGRAM
 *      9-12 The CRC of the data downloaded via the UPD_SEND_DATA commands. If the CRC does not match the
 *           programming returns an error
 *        13 Which boot block should be used
 *    UPD_REQ_DATA
 *      ???
 *    UPD_GET_LAST_ERROR
 *      Returns the reason why the last memory write PDU had a T_NACK_PDU
 *
 *    Workflow:
 *      - erase the sector which needs to be programmed (UPD_ERASE_SECTOR)
 *      - download the data via UPD_SEND_DATA telegrams
 *      - program the transmitted to into the FLASH  (UPD_PROGRAM)
 *      - repeat the above steps until the whole application has been downloaded
 *      - download the boot descriptor block via UPD_SEND_DATA telegrams
 *      - update the boot descriptor block so that the bootloader is able to start the new
 *        application (UPD_UPDATE_BOOT_DESC)
 *      - restart the board (UPD_RESTART)
 */
enum
{
	UPD_ERASE_SECTOR     = 0
,   UPD_SEND_DATA        = 1
,   UPD_PROGRAM          = 2
,   UPD_UPDATE_BOOT_DESC = 3
,   UPD_REQ_DATA         = 10
,   UPD_GET_LAST_ERROR   = 20
,   UPD_SEND_LAST_ERROR  = 21
,   UPD_UNLOCK_DEVICE    = 30
,   UPD_REQUEST_UID      = 31
,   UPD_RESPONSE_UID     = 32
,   UPD_APP_VERSION_REQUEST  = 33
,   UPD_APP_VERSION_RESPONSE  = 34
,   UPD_SET_EMULATION    = 100
};

#define DEVICE_LOCKED   ((unsigned int ) 0x5AA55AA5)
#define DEVICE_UNLOCKED ((unsigned int ) ~DEVICE_LOCKED)
#define ADDRESS2SECTOR(a) ((a + 4095) / 4096)

enum UPD_Status
{
      UDP_UNKONW_COMMAND = 0x100       //<! received command is not defined
    , UDP_CRC_EROR                     //<! CRC calculated on the device
								       //<! and by the updater don't match
    , UPD_ADDRESS_NOT_ALLOWED_TO_FLASH //<! specifed address cannot be programmed
    , UPD_SECTOR_NOT_ALLOWED_TO_ERASE  //<! the specified sector cannot be erased
    , UPD_RAM_BUFFER_OVERFLOW          //<! internal buffer for storing the data
	                                   //<! would overflow
    , UPD_WRONG_DESCRIPTOR_BLOCK       //<! the boot descriptor block does not exist
    , UPD_APPLICATION_NOT_STARTABLE    //<! the programmed application is not startable
    , UPD_DEVICE_LOCKED                //<! the device is still locked
	, UPD_UID_MISSMATCH                //<! UID sent to unlock the device is invalid
    , UDP_NOT_IMPLEMENTED  = 0xFFFF    //<! this command is not yet implemented
};

unsigned char ramBuffer[4096];

/*
 * a direct cast does not work due to possible miss aligned addresses.
 * therefore a good old conversion has to be performed
 */
unsigned int streamToUIn32(unsigned char * buffer)
{
    return buffer[0] << 24 | buffer [1] << 16 | buffer [2] << 8 | buffer [3];
}

/* the following two symbols are used to protect the updater from
 * killing itself with a new application downloaded over the bus
 */
/* the vector table marks the beginning of the updater application */
extern const unsigned int __vectors_start__;
/* the _etext symbol marks the end of the used flash area */
extern const unsigned int _etext;

static bool _prepareReturnTelegram(unsigned int count, unsigned char cmd)
{
    bcu->sendTelegram[5] = 0x63 + count;
    bcu->sendTelegram[6] = 0x42;
    bcu->sendTelegram[7] = 0x40 | count;
    bcu->sendTelegram[8] = 0;
    bcu->sendTelegram[9] = cmd;
    return true;
}

/*
 * Checks if the requested sector is allowed to be erased.
 */
inline bool sectorAllowedToErease(unsigned int sectorNumber)
{
    if (sectorNumber == 0) return false; // bootloader sector
    return !(  (sectorNumber >= ADDRESS2SECTOR(__vectors_start__))
            && (sectorNumber <= ADDRESS2SECTOR(_etext))
            );
}

/*
 * Checks if the address range is allowed to be programmed
 */
inline bool addressAllowedToProgram(unsigned int start, unsigned int length)
{
    unsigned int end = start + length;
    return !(  (start >= __vectors_start__)
            && (end   <= _etext)
            );
}

unsigned char handleMemoryRequests(int apciCmd, bool * sendTel, unsigned char * data)
{
    unsigned int count = data[0] & 0x0f;
	unsigned int address;
	static unsigned int ramLocation;
	static unsigned int deviceLocked = DEVICE_LOCKED;
	unsigned int crc = 0xFFFFFFFF;
	static unsigned int lastError = 0;
#ifdef ENABLE_EMULATION
	static unsigned int emulation = 0;
#endif

    digitalWrite(PIN_INFO, !digitalRead(PIN_INFO));
    switch (data [2])
    {
    case UPD_UNLOCK_DEVICE:
#ifdef SERIAL_DEBUG
        if (emulation & 0xF0)
        {
            serial.print("REUL ");
            serial.print(streamToUIn32(data + 3 + 0), HEX, 8);
            serial.print(streamToUIn32(data + 3 + 4), HEX, 8);
            serial.print(streamToUIn32(data + 3 + 8), HEX, 8);
            serial.println();
        }
#endif
    	if (!((BcuUpdate *) bcu)->progPinStatus())
    	{   // the operator has physical access to the device -> we unlock it
    		deviceLocked = DEVICE_UNLOCKED;
			lastError = IAP_SUCCESS;
    	}
    	else
    	{   // we need to ensure that only authorized operators can
    		// update the application
    		// as a simple method we use the unique ID of the CPU itself
    		// only if this UUID is known, the device will be unlocked
    		byte uid[4*32];
    		if (IAP_SUCCESS == iapReadUID(uid))
    		{
    			for (unsigned int i = 0; i < 12; i++)
    			{
    				if (data [i + 3] != uid[i])
    				{
    					lastError = UPD_UID_MISSMATCH;
    				}
    			}
    		}
    		if (lastError != UPD_UID_MISSMATCH)
    		{
        		deviceLocked = DEVICE_UNLOCKED;
    			lastError = IAP_SUCCESS;
    		}
    	}
    	break;
    case UPD_REQUEST_UID:
#ifdef SERIAL_DEBUG
        if (emulation & 0xF0)
        {
            serial.println("REUI");
        }
#endif
    	if (!((BcuUpdate *) bcu)->progPinStatus())
    	{   // the operator has physical access to the device -> we unlock it
    		byte uid[4*4];
    		lastError = iapReadUID(uid);
    		if (lastError == IAP_SUCCESS)
    		{
                * sendTel = _prepareReturnTelegram(12, UPD_RESPONSE_UID);
                memcpy(bcu->sendTelegram +10, uid, 12);
    		}
            break;
    	}
    	else
    		lastError = UPD_DEVICE_LOCKED;
    	break;
    case UPD_APP_VERSION_REQUEST:
        unsigned char * appversion;
        appversion = getAppVersion
	    	    ( (AppDescriptionBlock *)
    		    		(FIRST_SECTOR - (1 + data[3]) * BOOT_BLOCK_SIZE)
				);
        if (((unsigned int) appversion) < 0x50000)
        {
            * sendTel = _prepareReturnTelegram(12, UPD_APP_VERSION_RESPONSE);
			memcpy( bcu->sendTelegram +10
				  , appversion
				  , 12
				  );
			lastError = IAP_SUCCESS;
        }
        else
        	lastError = UPD_APPLICATION_NOT_STARTABLE;
    	break;
    case UPD_ERASE_SECTOR:
#ifdef SERIAL_DEBUG
        if (emulation & 0xF0)
        {
            serial.print("SCER ");
            serial.print(data [3], HEX, 2);
            serial.println();
        }
#endif
        if (deviceLocked == DEVICE_UNLOCKED)
        {
			if (sectorAllowedToErease(data [3]))
			{
				lastError = RUN_OR_EMULATE(iapEraseSector(data [3]));
			}
			else
				lastError = UPD_SECTOR_NOT_ALLOWED_TO_ERASE;
        }
        else
        	lastError = UPD_DEVICE_LOCKED;
        ramLocation = 0;
        break;
    case UPD_SEND_DATA:
        if (deviceLocked == DEVICE_UNLOCKED)
        {
			if ((ramLocation + count) < sizeof(ramBuffer))
			{
				memcpy((void *)& ramBuffer[ramLocation], data + 3, count);
				crc          = crc32(crc, data + 3, count);
	#ifdef SERIAL_DEBUG
				if (emulation & 0xF0)
				{
					serial.print("DSND ");
					serial.print(count, HEX, 4);
					serial.print(" ");
					serial.print(ramLocation, HEX, 4);
					serial.println();
				}
	#endif
				ramLocation += count;
				lastError    = IAP_SUCCESS;
			}
			else
				lastError = UPD_RAM_BUFFER_OVERFLOW;
        }
        else
        	lastError = UPD_DEVICE_LOCKED;
        break;
    case UPD_PROGRAM:
        if (deviceLocked == DEVICE_UNLOCKED)
        {
			count        = streamToUIn32(data + 3);
			address      = streamToUIn32(data + 3 + 4);
			if (addressAllowedToProgram(address, count))
			{
				crc = crc32(0xFFFFFFFF, ramBuffer, count);
	#ifdef SERIAL_DEBUG
				if (emulation & 0xF0)
				{
					serial.print("PROG ");
					serial.print(count, HEX, 8);
					serial.print(" ");
					serial.print(address, HEX, 8);
					serial.print(" ");
					serial.print(crc, HEX, 8);
					serial.print(" ");
					serial.print(streamToUIn32(data + 3 + 4 + 4), HEX, 8);
					serial.println();
				}
	#endif
				if (crc == streamToUIn32(data + 3 + 4 + 4))
				{
					lastError = RUN_OR_EMULATE
							(iapProgram((byte *) address, ramBuffer, count));
				}
				else
					lastError = UDP_CRC_EROR;
			}
			else
				lastError = UPD_ADDRESS_NOT_ALLOWED_TO_FLASH;
        }
        else
        	lastError = UPD_DEVICE_LOCKED;
        ramLocation = 0;
        crc         = 0xFFFFFFFF;
        break;
    case UPD_UPDATE_BOOT_DESC:
        if (deviceLocked == DEVICE_UNLOCKED)
        {
			crc     = crc32(0xFFFFFFFF, ramBuffer, 256);
			address = FIRST_SECTOR - (1 + data[7]) * BOOT_BLOCK_SIZE; // start address of the descriptor block
	#ifdef SERIAL_DEBUG
			if (emulation & 0xF0)
			{
				serial.print("UPDB ");
				serial.print(data [7], HEX, 2);
				serial.print(" ");
				serial.print(BOOT_BLOCK_PAGE - data[7], DEC, 2);
				serial.print(" ");
				serial.print(address, HEX, 8);
				serial.print(" ");
				serial.print(crc, HEX, 8);
				serial.print(" ");
				serial.print(streamToUIn32(data + 3), HEX, 8);
				serial.println();
			}
	#endif
			if (crc == streamToUIn32(data + 3))
			{
				if (checkApplication ((AppDescriptionBlock *) ramBuffer))
				{
					lastError = RUN_OR_EMULATE
					    (iapErasePage(BOOT_BLOCK_PAGE - data[7]));
					if (lastError == IAP_SUCCESS)
					{
						lastError = RUN_OR_EMULATE(iapProgram((byte *) address, ramBuffer, 256));
					}
				}
				else
					lastError = UPD_APPLICATION_NOT_STARTABLE;
			}
			else
				lastError = UDP_CRC_EROR;
        }
        else
        	lastError = UPD_DEVICE_LOCKED;
        ramLocation = 0;
        crc         = 0xFFFFFFFF;
        break;
    case UPD_REQ_DATA:
        if (deviceLocked == DEVICE_UNLOCKED)
        {
			/*
			memcpy(bcu.sendTelegram + 9, (void *)address, count);
			bcu.sendTelegram[5] = 0x63 + count;
			bcu.sendTelegram[6] = 0x42;
			bcu.sendTelegram[7] = 0x40 | count;
			bcu.sendTelegram[8] = UPD_SEND_DATA;
			* sendTel = true;
			* */
			lastError = UDP_NOT_IMPLEMENTED; // set to any error
        }
        else
        	lastError = UPD_DEVICE_LOCKED;
        break;
    case UPD_GET_LAST_ERROR:
#ifdef SERIAL_DEBUG
        if (emulation & 0xF0)
        {
            serial.print("SNDE ");
            serial.print(lastError, HEX, 2);
            serial.println();
        }
#endif
        * sendTel = _prepareReturnTelegram(4, UPD_SEND_LAST_ERROR);
        memcpy(bcu->sendTelegram +10, (void *)&lastError, 4);
        lastError = IAP_SUCCESS;
        break;
#ifdef ENABLE_EMULATION
    case UPD_SET_EMULATION:
        emulation = data [3];
#ifdef SERIAL_DEBUG
        if (emulation & 0xF0)
        {
#ifndef DUMP_TELEGRAMS
            serial.begin(115200);
#endif
            serial.print("SETE ");
            serial.print(emulation, HEX, 2);
            serial.println();
        }
        else
        {
            //serial.end();
        }
#endif
        lastError = IAP_SUCCESS;
        break;
#endif
    default:
        lastError = UDP_UNKONW_COMMAND; // set to any error
    }
    if (lastError == IAP_SUCCESS)
        return T_ACK_PDU;
#ifdef SERIAL_DEBUG
    if (emulation & 0xF0)
    {
    	serial.print("  ER ");
        serial.print(lastError, HEX, 2);
        serial.println();
    }
#endif
    return T_NACK_PDU;
}
