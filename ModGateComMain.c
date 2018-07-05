/*=======================================================================================
 *
 *	       KK    KK   UU    UU   NN    NN   BBBBBB    UU    UU    SSSSSS
 *	       KK   KK    UU    UU   NNN   NN   BB   BB   UU    UU   SS
 *	       KK  KK     UU    UU   NNNN  NN   BB   BB   UU    UU   SS
 *	+----- KKKKK      UU    UU   NN NN NN   BBBBB     UU    UU    SSSSS
 *	|      KK  KK     UU    UU   NN  NNNN   BB   BB   UU    UU        SS
 *	|      KK   KK    UU    UU   NN   NNN   BB   BB   UU    UU        SS
 *	|      KK    KKK   UUUUUU    NN    NN   BBBBBB     UUUUUU    SSSSSS     GmbH
 *	|
 *	|            [#]  I N D U S T R I A L   C O M M U N I C A T I O N
 *	|             |
 *	+-------------+
 *
 *---------------------------------------------------------------------------------------
 *
 * (C) KUNBUS GmbH, Heerweg 15C, 73770 Denkendorf, Germany
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License V2 as published by
 * the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  For licencing details see COPYING
 *
 *=======================================================================================
 */

#include <common_define.h>

#include <project.h>

#include "linux/types.h"
#include "revpi_common.h"
#include "revpi_core.h"

#include <bsp/bspError.h>
#include <bsp/mGateDecode/mGateDecode.h>
#include <kbUtilities.h>
#include <kbAlloc.h>

#include <bsp/spi/spi.h>

#include <ModGateComMain.h>
#include <ModGateComError.h>
#include <ModGateRS485.h>

#include <bsp/systick/systick.h>

static ETHERNET_INTERFACE *EthDrv_s;

void *LL_pvPacketHeapHandle_g;
static MODGATECOM_Packet asuPacketMem_s[7];	// 6 packets plus malloc-overhead

#define T1_ACK_TIMEOUT      20
#define T2_ACK_TIMEOUT      40
#define T2_ACK_RETRY         2

#define MG_AL_MAX_RETRY      5
#define MG_AL_TIMEOUT       80
#define MG_AL_ID_REQ_DELAY	 1

MODGATECOM_IDResp MODGATE_OwnID_g;	//!< ID-Data of this mGate
static INT8U i8uOwnFieldbusState_s;	//!< Fieldbus State of this mGate

//-----------------------------------------------------------------------------

#define ID_REQUEST_TIME      100	// time in ms for ID-Request
#define LED_GREEN_TIME      2000	// time in ms for green led at start in ID-Request status
#define PD_TIMEOUT_MAX         5

typedef enum {
	MODGATECOM_enPLR_INIT,
	MODGATECOM_enPLR_GREEN_PHASE,
	MODGATECOM_enPLR_RUN
} MODGATECOM_EPowerLedRun;

sALData AL_Data_s[MODGATECOM_MAX_MODULES];

static MODGATECOM_EPowerLedRun MODGATECOM_enLedRunState_s;
static kbUT_Timer MODGATECOM_tGreenTimer_s;

//**********************************************************************************************
// Link Layer
//**********************************************************************************************

INT32U MG_LL_init(LLHandle llHdl)
{
	// Ethernet destination
	llHdl->Header.i8uDestination[0] = 0xFF;
	llHdl->Header.i8uDestination[1] = 0xFF;
	llHdl->Header.i8uDestination[2] = 0xFF;
	llHdl->Header.i8uDestination[3] = 0xFF;
	llHdl->Header.i8uDestination[4] = 0xFF;
	llHdl->Header.i8uDestination[5] = 0xFF;

	// Ethernet source
	llHdl->Header.i8uSource[0] = 0xC8;
	llHdl->Header.i8uSource[1] = 0x3E;
	llHdl->Header.i8uSource[2] = 0xA7;
	llHdl->Header.i8uSource[3] = 0x00;
	llHdl->Header.i8uSource[4] = KUNBUS_FW_DESCR_TYP_PI_CORE;

	if (BSP_getMgateDecode() == MGATE_MODUL_LEFT)
		llHdl->Header.i8uSource[5] = 0x01;
	else
		llHdl->Header.i8uSource[5] = 0x02;

	llHdl->Header.i16uType = 0x9C41;	//Ethernet type
	llHdl->Header.i8uACK = 0;
	llHdl->Header.i8uCounter = 1;

	llHdl->state = 0;
	llHdl->send_tick = 0;
	llHdl->recv_tick = 0;
	llHdl->timed_out = bFALSE;
	llHdl->send_retry = 0;

	llHdl->pLastData = 0;

	if (LL_pvPacketHeapHandle_g == 0) {
		LL_pvPacketHeapHandle_g = kbUT_initHeap((INT8U *) asuPacketMem_s, sizeof(asuPacketMem_s));
	}

	if (!EthDrv_s->init()) {
		MODGATECOM_error(BSPE_KSZ8851_RESET, bTRUE, 0);
		return BSPE_KSZ8851_RESET;
	}

	return MODGATECOM_NO_ERROR;
}

// return TRUE, while we are waiting for an ACK
TBOOL MG_LL_pending(LLHandle llHdl)
{
	if (llHdl->send_tick != 0) {
		return bTRUE;
	} else {
		return bFALSE;
	}
}

// stop waiting for an ACK
void MG_LL_abort(LLHandle llHdl)
{
	if (llHdl->pLastData) {
		kbUT_free(llHdl->pLastData);
		llHdl->pLastData = 0;
	}
	llHdl->send_tick = 0;
}

// return TRUE once, if a timeout occurred
TBOOL MG_LL_timed_out(LLHandle llHdl)
{
	TBOOL r = llHdl->timed_out;
	llHdl->timed_out = bFALSE;
	return r;
}

INT32U MG_LL_send(LLHandle llHdl, MODGATECOM_Packet * pData_p)
{
	if (!EthDrv_s->link()) {
		kbUT_free(pData_p);
		MG_LL_abort(llHdl);
		return MODGATECOM_ERROR_NO_LINK;
	}
	//memcpy(pData_p, &llHdl->Header, sizeof(llHdl->Header));
	pData_p->strLinkLayer = llHdl->Header;
	if (EthDrv_s->packetSend((INT8U *) pData_p, MODGATE_TL_HEADER_LEN + pData_p->strTransportLayer.i16uDataLength)) {	// success
		llHdl->state++;	// wait for ack
		llHdl->recv_tick = 0;
		llHdl->send_retry = 0;
		llHdl->send_tick = kbUT_getCurrentMs();
		if (llHdl->send_tick == 0)
			llHdl->send_tick++;

		if (llHdl->pLastData) {
			kbUT_free(llHdl->pLastData);
		}
		llHdl->pLastData = pData_p;

		llHdl->Header.i8uCounter++;
		if (llHdl->Header.i8uCounter == 0)
			llHdl->Header.i8uCounter = 1;

		return MODGATECOM_NO_ERROR;
	} else {	// send failed
		MODGATECOM_error(MODGATECOM_ERROR_SEND_FAILED, bFALSE, 0);
		kbUT_free(pData_p);
		return MODGATECOM_ERROR_SEND_FAILED;
	}
}

void ksz8851_show(void);

MODGATECOM_Packet *MG_LL_recv(LLHandle llHdl, INT32U * pi32uStatus_p, INT16U * pi16uLen_p)
{
	INT16U i16uPackelLen_l = MODGATE_LL_MAX_LEN;
	MODGATECOM_Packet *pPacket_l;
	TBOOL ret;

	pPacket_l = (MODGATECOM_Packet *) kbUT_malloc(LL_pvPacketHeapHandle_g, 10, i16uPackelLen_l);
	if (pPacket_l == 0) {
		MODGATECOM_error(MODGATECOM_ERROR_OUT_OF_MEMORY_1, bTRUE, 0);
		*pi32uStatus_p = MODGATECOM_ERROR_OUT_OF_MEMORY_1;
		return 0;
	}

	(void)pi16uLen_p;	// ignore parameter

	if (!EthDrv_s->link()) {
		kbUT_free(pPacket_l);
		MG_LL_abort(llHdl);
		*pi32uStatus_p = MODGATECOM_ERROR_NO_LINK;
		return 0;
	}

	while (1) {
		i16uPackelLen_l = MODGATE_LL_MAX_LEN;
		ret = EthDrv_s->packetRead((INT8U *) pPacket_l, &i16uPackelLen_l);
		if (ret) {
			if (!IS_BROADCAST(pPacket_l->strLinkLayer.i8uDestination)
			    || pPacket_l->strLinkLayer.i16uType != 0x9C41) {
				// packet is from another protocol -> ignore it
				// try to read another packet
				pr_info_modgate("%d: MG_LL_recv: wrong ether type 0x%04x\n", spi_selected_chip(), pPacket_l->strLinkLayer.i16uType);
			} else if (i16uPackelLen_l <
				   MODGATE_TL_HEADER_LEN + pPacket_l->strTransportLayer.i16uDataLength) {
				// incomplete packet -> ignore
				// try to read another packet
				pr_info_modgate("%d: MG_LL_recv: packet too short  len=%d\n", spi_selected_chip(), i16uPackelLen_l);
			} else {
				// store number for next ACK
				llHdl->Header.i8uACK = pPacket_l->strLinkLayer.i8uCounter;

				// we have a valid packet -> return
				llHdl->recv_tick = kbUT_getCurrentMs();	// start timer for T1
				if (llHdl->recv_tick == 0)
					llHdl->recv_tick++;	// avoid 0 because it has a special meaning
				return pPacket_l;
			}
		} else {
			kbUT_free(pPacket_l);
			*pi32uStatus_p = MODGATECOM_ERROR_NO_PACKET;
			return 0;
		}
	}
}

//**********************************************************************************************
// Transport/Application Layer
//**********************************************************************************************

INT32U MODGATECOM_init(INT8U * pi8uInData_p, INT16U i16uInDataLen_p,
		       INT8U * pi8uOutData_p, INT16U i16uOutDataLen_p, ETHERNET_INTERFACE * EthDrv_p)
{
	INT32U i32uRet_l = MODGATECOM_NO_ERROR;
	int inst;

	EthDrv_s = EthDrv_p;

	// ID response
	MODGATE_OwnID_g.i32uSerialnumber = RevPiDevice_getDev(0)->sId.i32uSerialnumber;
	MODGATE_OwnID_g.i16uModulType = RevPiDevice_getDev(0)->sId.i16uModulType;
	MODGATE_OwnID_g.i16uHW_Revision = RevPiDevice_getDev(0)->sId.i16uHW_Revision;
	MODGATE_OwnID_g.i16uSW_Major = RevPiDevice_getDev(0)->sId.i16uSW_Major;
	MODGATE_OwnID_g.i16uSW_Minor = RevPiDevice_getDev(0)->sId.i16uSW_Minor;
	MODGATE_OwnID_g.i32uSVN_Revision = RevPiDevice_getDev(0)->sId.i32uSVN_Revision;

	MODGATE_OwnID_g.i16uFBS_InputLength = i16uInDataLen_p;
	MODGATE_OwnID_g.i16uFBS_OutputLength = i16uOutDataLen_p;
	MODGATE_OwnID_g.i16uFeatureDescriptor = MODGATE_feature_IODataExchange;

	spi_select_chip(0);
	EthDrv_s->hardwareReset();

	for (inst = 0; inst < MODGATECOM_MAX_MODULES; inst++) {
		if (piCore_g.abMGateActive[inst]) {
			if (piDev_g.machine_type == REVPI_CONNECT) {
				// on revpi connect always chip 0 is used
				spi_select_chip(0);
			} else {
				spi_select_chip(inst);
			}

			// initialize fieldbus states
			AL_Data_s[inst].i8uState = MODGATE_ST_HW_CHECK;	//!< modular Gateway state machine state
			i8uOwnFieldbusState_s = FBSTATE_OFFLINE;
			AL_Data_s[inst].i8uOtherFieldbusState = FBSTATE_OFFLINE;
			MODGATECOM_enLedRunState_s = MODGATECOM_enPLR_INIT;

			AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_INIT;
			AL_Data_s[inst].enLedStateOld = MODGATECOM_enPLS_INIT;

			i32uRet_l = MG_LL_init(&AL_Data_s[inst].llParas);
			if (i32uRet_l != MODGATECOM_NO_ERROR) {
				goto laError;
			}

			AL_Data_s[inst].pi8uInData = pi8uInData_p + (inst * i16uInDataLen_p);
			AL_Data_s[inst].i16uInDataLen = i16uInDataLen_p;
			AL_Data_s[inst].i16uInDataLenActive = 0;	// unknown yet
			AL_Data_s[inst].pi8uOutData = pi8uOutData_p + (inst * i16uOutDataLen_p);
			AL_Data_s[inst].i16uOutDataLen = i16uOutDataLen_p;
			AL_Data_s[inst].i16uOutDataLenActive = 0;	// unknown yet

			kbUT_TimerInit(&AL_Data_s[inst].AL_Timeout);

			//init buffer
			memset(AL_Data_s[inst].pi8uInData, 0, i16uInDataLen_p);
			memset(AL_Data_s[inst].pi8uOutData, 0, i16uOutDataLen_p);
		}
	}

	return MODGATECOM_NO_ERROR;

laError:
	MODGATECOM_enLedRunState_s = MODGATECOM_enPLS_ERROR;
	MODGATECOM_managePowerLedRun();
	return i32uRet_l;
}

void MODGATECOM_run(void)
{
	static TBOOL bSend = bTRUE;
	INT32U i32uStatus_l, i32uRet;
	INT16U i16uLen_l;
	MODGATECOM_Packet *pPacket_l = 0;
	INT32U oldState, newState;
	int inst;
	static int msgCnt = 1000;
	static int retry = 0;

	oldState = 0;
	for (inst = 0; inst < MODGATECOM_MAX_MODULES; inst++) {
		if (piCore_g.abMGateActive[inst]) {
			TBOOL bDone = bFALSE;

			oldState <<= 8;
			if (AL_Data_s[inst].i8uState == MODGATE_ST_ID_RESP) {
				oldState += MODGATE_ST_ID_REQ;
			} else {
				oldState += AL_Data_s[inst].i8uState;
			}

			if (piDev_g.machine_type == REVPI_CONNECT) {
				// on revpi connect always chip 0 is used
				spi_select_chip(0);
			} else {
				spi_select_chip(inst);
			}
			i32uStatus_l = MODGATECOM_NO_ERROR;

			while (!bDone) {
				// try to receive a packet first
				pPacket_l = MG_LL_recv(&AL_Data_s[inst].llParas, &i32uStatus_l, &i16uLen_l);

				if (pPacket_l && piCore_g.eBridgeState != piBridgeRun) {
					// while the modules are not enumerated, all packets are ignored
					if (msgCnt > 0) {
						pr_info_modgate("%d: MG_LL_recv: drop\n", spi_selected_chip());
						msgCnt--;
					}
					kbUT_free(pPacket_l);
					pPacket_l = 0;
				}
				if (   i32uStatus_l != 0
				    && i32uStatus_l != MODGATECOM_ERROR_NO_PACKET
				    && i32uStatus_l != MODGATECOM_ERROR_NO_LINK
				    && msgCnt > 0) {
					pr_info_modgate("%d: MG_LL_recv: status 0x%08x\n", spi_selected_chip(), i32uStatus_l);
					msgCnt--;
				}

				if (pPacket_l) {
					if (msgCnt > 0) { // pPacket_l->strTransportLayer.i16uCmd != 2) {
						pr_info_modgate("%d: MG_LL_recv: packet 0x%x\n", spi_selected_chip(), pPacket_l->strTransportLayer.i16uCmd);
						msgCnt--;
					}

					switch (pPacket_l->strTransportLayer.i16uCmd) {
					case MODGATE_AL_CMD_ID_Req:
						i32uRet = MODGATECOM_send_ID_Req(&AL_Data_s[inst]);
						pr_info_modgate("%d: send id req 0x%x\n", spi_selected_chip(), i32uRet);
						if (i32uRet != MODGATECOM_NO_ERROR) {
						}
						break;

					case MODGATE_AL_CMD_ID_Resp:
						if (AL_Data_s[inst].i8uState >= MODGATE_ST_ID_REQ) {
							// if we are waiting for the response, process it and go to run state
							if (MODGATECOM_recv_Id_Resp(&AL_Data_s[inst], pPacket_l)) {
								AL_Data_s[inst].i8uState = MODGATE_ST_RUN_NO_DATA;
								AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_DATA_MISSING;

								// send response in all states
								i32uRet = MODGATECOM_send_ID_Resp(&AL_Data_s[inst]);
								pr_info_modgate("%d: send id resp 0x%x\n", spi_selected_chip(), i32uRet);
								if (i32uRet != MODGATECOM_NO_ERROR) {
								}
							} else {
								pr_info_modgate("%d: error in id resp\n", spi_selected_chip());
							}
							pPacket_l = 0;
						} else {
							pr_info_modgate("%d: id resp in wrong state 0x%x\n", spi_selected_chip(), AL_Data_s[inst].i8uState);
						}
						//else {
						//    // otherwise ignore message
						//    wrong_state++;
						//}
						break;

					case MODGATE_AL_CMD_cyclicPD:
						if (AL_Data_s[inst].i8uState == MODGATE_ST_RUN_NO_DATA ||
						    AL_Data_s[inst].i8uState == MODGATE_ST_RUN) {
							MODGATECOM_recv_cyclicPD(&AL_Data_s[inst], pPacket_l);
							pPacket_l = 0;
							AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_RUN;
							AL_Data_s[inst].i8uState = MODGATE_ST_RUN;
							bSend = bTRUE;
							retry = 0;

#if 0				//MD Testcode:
							// this code was implemented for the EtherNet/IP Module because the LED 1 is not used there
							// the LED 1 will become red, when the time between two packets with cyclic process data
							// is much higher than the average of the last 10 packets
							// the LED is turned on for 2 seconds
							// if more packets a missing the power LED will start blinking
							static INT32U tick, cnt, diff, diffAvg;
							static INT32U ltick[20], ldiff[20], lidx, lidx2;

							ltick[lidx] = kbUT_getCurrentMs();
							ldiff[lidx] = ltick[lidx] - ltick[lidx2];
							lidx2 = lidx;
							lidx = (lidx + 1) % 20;

							if (cnt == 0) {
								BSP_count_up_timer_init(1);
								BSP_count_up_timer_start();
								cnt++;
							} else {
								diff = BSP_count_up_timer_get();
								BSP_count_up_timer_start();
								if (cnt > 10) {
									diffAvg = ((diffAvg * 9) / 10) + diff;
									if (diff * 6 > diffAvg) {
										LED_setLed(1, LED_ST_RED_ON);
										cnt = 1;
										diffAvg = 0;
										tick = kbUT_getCurrentMs();
									} else {
										if ((kbUT_getCurrentMs() - tick) > 2000) {
											LED_setLed(1, LED_ST_RED_OFF);
										}
									}
								} else {
									diffAvg += diff;
									cnt++;
								}
							}
#endif
						} else {
							pr_info_modgate("%d: data in wrong state 0x%x\n", spi_selected_chip(), AL_Data_s[inst].i8uState);
						}
						break;

					case MODGATE_AL_CMD_ST_Req:	// not used
					case MODGATE_AL_CMD_ST_Resp:	// not used
					case MODGATE_AL_CMD_updatePD:	// not used
					default:
						// ignore packet
						pr_err("%d: MG_LL_recv: return unknown packet 0x%x\n", spi_selected_chip(),
						       pPacket_l->strTransportLayer.i16uCmd);
						break;
					}
					if (pPacket_l != 0) {
						// ignore packet
						kbUT_free(pPacket_l);
						pPacket_l = 0;
					}
				} else {
					if (i32uStatus_l == MODGATECOM_ERROR_NO_LINK) {
						AL_Data_s[inst].i8uState = MODGATE_ST_LINK_CHECK;
						//pr_info_modgate("%d: no link\n", spi_selected_chip());
					}
					// queue is empty, leave the loop
					//break;
					bDone = bTRUE;
				}
			}

			// check state
			if (piCore_g.eBridgeState == piBridgeRun)
			{
				switch (AL_Data_s[inst].i8uState) {
				case MODGATE_ST_HW_CHECK:
					{
						AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_LINK_MISSING;
						// nothing to do
						AL_Data_s[inst].i8uState = MODGATE_ST_LINK_CHECK;
					}
					break;

				case MODGATE_ST_LINK_CHECK:
					{
						// set input data to 0
						memset(AL_Data_s[inst].pi8uInData, 0,
						       AL_Data_s[inst].i16uInDataLenActive);

						if (EthDrv_s->link()) {
							AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_DATA_MISSING;
							AL_Data_s[inst].i8uState = MODGATE_ST_ID_REQ;
							msgCnt = 100;
						} else {
							AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_LINK_MISSING;
						}
					}
					break;

				case MODGATE_ST_ID_REQ:
					{
					}
					break;

				case MODGATE_ST_ID_RESP:
					{
					}
					break;

				case MODGATE_ST_RUN_NO_DATA:
					// set input data to 0
					memset(AL_Data_s[inst].pi8uInData, 0, AL_Data_s[inst].i16uInDataLenActive);
				case MODGATE_ST_RUN:
					if (msgCnt > 0) {
						pr_info_modgate("%d: send data 0x%x\n", spi_selected_chip(), (int)bSend);
						msgCnt--;
					}
					if (bSend) {
						INT32U ret = MODGATECOM_send_cyclicPD(&AL_Data_s[inst]);
						if (ret != MODGATECOM_NO_ERROR) {
							pr_info_modgate("%d: send data error 0x%x\n", spi_selected_chip(), ret);
						} else {
							kbUT_TimerStart(&AL_Data_s[inst].AL_Timeout, MG_AL_TIMEOUT);
							bSend = bFALSE;
						}
					} else {
						if (kbUT_TimerExpired(&AL_Data_s[inst].AL_Timeout)) {
							if (retry < 5) {
								pr_info_modgate("%d: no data timeout\n", spi_selected_chip());
								bSend = bTRUE;
								retry++;
							} else {
								pr_info_modgate("%d: no data timeout\n", spi_selected_chip());
								AL_Data_s[inst].i8uState = MODGATE_ST_ID_REQ;
								AL_Data_s[inst].enLedStateAct = MODGATECOM_enPLS_DATA_MISSING;
								msgCnt = 100;
							}
						}
					}
					break;

				default:
					break;
				}
			}
		}
	}

	newState = 0;
	if (AL_Data_s[0].i8uState == MODGATE_ST_ID_RESP) {
		newState += MODGATE_ST_ID_REQ;
	} else {
		newState += AL_Data_s[0].i8uState;
	}

#ifdef PRINT_MODGATE_COM_STATE
#if MODGATECOM_MAX_MODULES == 1
	if (oldState != newState) {
		switch (AL_Data_s[0].i8uState) {
		case MODGATE_ST_HW_CHECK:
			pr_info("Module %d: HW check\n", 0);
			break;
		case MODGATE_ST_LINK_CHECK:
			pr_info("Module %d: Link check\n", 0);
			break;
		case MODGATE_ST_ID_REQ:	// id request und response werden für die Ausgabe nicht unterschieden
		case MODGATE_ST_ID_RESP:
			pr_info("Module %d: id request\n", 0);
			break;
		case MODGATE_ST_RUN_NO_DATA:
			pr_info("Module %d: run no data\n", 0);
			break;
		case MODGATE_ST_RUN:
			pr_info("Module %d: run\n", 0);
			break;
		}
	}
#else
	newState <<= 8;
	if (AL_Data_s[1].i8uState == MODGATE_ST_ID_RESP) {
		newState += MODGATE_ST_ID_REQ;
	} else {
		newState += AL_Data_s[1].i8uState;
	}

	if (oldState != newState) {
		char string[80];
		int len = 0;
		INT8U set, clr;
		set = 0;
		clr = 0;

		string[0] = 0;
		// left module has index 1
		switch (AL_Data_s[1].i8uState) {
		case MODGATE_ST_HW_CHECK:
			len = sprintf(string, "Module L: HW check     ");
			clr |= PICONTROL_STATUS_LEFT_GATEWAY;
			break;
		case MODGATE_ST_LINK_CHECK:
			len = sprintf(string, "Module L: Link check   ");
			clr |= PICONTROL_STATUS_LEFT_GATEWAY;
			break;
		case MODGATE_ST_ID_REQ:	// id request und response werden für die Ausgabe nicht unterschieden
		case MODGATE_ST_ID_RESP:
			len = sprintf(string, "Module L: id request   ");
			clr |= PICONTROL_STATUS_LEFT_GATEWAY;
			break;
		case MODGATE_ST_RUN_NO_DATA:
			len = sprintf(string, "Module L: run no data  ");
			clr |= PICONTROL_STATUS_LEFT_GATEWAY;
			break;
		case MODGATE_ST_RUN:
			len = sprintf(string, "Module L: run          ");
			set |= PICONTROL_STATUS_LEFT_GATEWAY;
			break;
		}

		// right module has index 0
		switch (AL_Data_s[0].i8uState) {
		case MODGATE_ST_HW_CHECK:
			sprintf(&string[len], "Module R: HW check\n");
			clr |= PICONTROL_STATUS_RIGHT_GATEWAY;
			break;
		case MODGATE_ST_LINK_CHECK:
			sprintf(&string[len], "Module R: Link check\n");
			clr |= PICONTROL_STATUS_RIGHT_GATEWAY;
			break;
		case MODGATE_ST_ID_REQ:	// id request und response werden für die Ausgabe nicht unterschieden
		case MODGATE_ST_ID_RESP:
			sprintf(&string[len], "Module R: id request\n");
			clr |= PICONTROL_STATUS_RIGHT_GATEWAY;
			break;
		case MODGATE_ST_RUN_NO_DATA:
			sprintf(&string[len], "Module R: run no data\n");
			clr |= PICONTROL_STATUS_RIGHT_GATEWAY;
			break;
		case MODGATE_ST_RUN:
			sprintf(&string[len], "Module R: run\n");
			set |= PICONTROL_STATUS_RIGHT_GATEWAY;
			break;
		}
		pr_info("%d %s", kbUT_getCurrentMs(), string);

		RevPiDevice_setStatus(clr, set);
	}
#endif
#endif

	MODGATECOM_managePowerLedRun();
}

//*************************************************************************************************
//| Function: MODGATECOM_managePowerLedRun
//|
//! \brief
//!
//! \detailed
//!
//!
//!
//! \ingroup
//-------------------------------------------------------------------------------------------------
void MODGATECOM_managePowerLedRun(void)
{
	switch (MODGATECOM_enLedRunState_s) {
	case MODGATECOM_enPLR_INIT:
		{
			kbUT_TimerStart(&MODGATECOM_tGreenTimer_s, LED_GREEN_TIME);
			MODGATECOM_enLedRunState_s = MODGATECOM_enPLR_GREEN_PHASE;
		}
		break;
	case MODGATECOM_enPLR_GREEN_PHASE:
		{
			if (kbUT_TimerExpired(&MODGATECOM_tGreenTimer_s)) {
				MODGATECOM_enLedRunState_s = MODGATECOM_enPLR_RUN;
				AL_Data_s[0].enLedStateOld = MODGATECOM_enPLS_INIT;	// give new state a chance to react on the state of the communication
				AL_Data_s[1].enLedStateOld = MODGATECOM_enPLS_INIT;	// give new state a chance to react on the state of the communication
			}
		}
		break;
	case MODGATECOM_enPLR_RUN:
		{
			MODGATECOM_EPowerLedState ledStateAct;

			if (AL_Data_s[0].enLedStateAct <= MODGATECOM_enPLS_LINK_MISSING) {
				// if the state is less or equal to link check the module is missing and the led shows the state of the other module only.
				ledStateAct = AL_Data_s[1].enLedStateAct;
			} else if (AL_Data_s[1].enLedStateAct <= MODGATECOM_enPLS_LINK_MISSING) {
				// if the state is less or equal to link check the module is missing and the led shows the state of the other module only.
				ledStateAct = AL_Data_s[0].enLedStateAct;
			} else {
				// if both modules are present, we show the lower state
				if (AL_Data_s[0].enLedStateAct <= AL_Data_s[1].enLedStateAct) {
					ledStateAct = AL_Data_s[0].enLedStateAct;
				} else {
					ledStateAct = AL_Data_s[1].enLedStateAct;
				}
			}

			if (ledStateAct != AL_Data_s[0].enLedStateOld) {
				switch (ledStateAct) {
				case MODGATECOM_enPLS_RUN:
					{
					}
					break;
				case MODGATECOM_enPLS_LINK_MISSING:
				case MODGATECOM_enPLS_DATA_MISSING:
				default:
					{
					}
					break;
				}
				AL_Data_s[0].enLedStateOld = ledStateAct;
			}

			if (ledStateAct != AL_Data_s[0].enLedStateOld) {
				switch (ledStateAct) {
				case MODGATECOM_enPLS_RUN:
					{
					}
					break;
				case MODGATECOM_enPLS_LINK_MISSING:
				case MODGATECOM_enPLS_DATA_MISSING:
				default:
					{
					}
					break;
				}
				AL_Data_s[0].enLedStateOld = ledStateAct;
			}
		}
		break;
	default:
		{

		}
		break;
	}
}

    //*************************************************************************************************
    //| Function: MODGATECOM_Send_ID_Req
    //|
    //! \brief
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT32U MODGATECOM_send_ID_Req(ALHandle alHdl)
{
	MODGATECOM_Packet *pPacket_l = (MODGATECOM_Packet *) kbUT_malloc(LL_pvPacketHeapHandle_g, 2,
									 MODGATE_TL_HEADER_LEN);
	if (pPacket_l == 0) {
		MODGATECOM_error(MODGATECOM_ERROR_OUT_OF_MEMORY_3, bTRUE, 0);
		return MODGATECOM_ERROR_OUT_OF_MEMORY_3;
	}

	pPacket_l->strTransportLayer.i16uCmd = MODGATE_AL_CMD_ID_Req;
	pPacket_l->strTransportLayer.i16uDataLength = 0;
	pPacket_l->strTransportLayer.i32uError = MODGATECOM_NO_ERROR;
	pPacket_l->strTransportLayer.i8uVersion = 0xaa;
	pPacket_l->strTransportLayer.i8uReserved = 0x51;

	return MG_LL_send(&alHdl->llParas, pPacket_l);
}

    //*************************************************************************************************
    //| Function: MODGATECOM_Send_ID_Resp
    //|
    //! \brief
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT32U MODGATECOM_send_ID_Resp(ALHandle alHdl)
{
	MODGATECOM_Packet *pPacket_l = (MODGATECOM_Packet *) kbUT_malloc(LL_pvPacketHeapHandle_g, 3,
									 MODGATE_TL_HEADER_LEN +
									 sizeof(MODGATECOM_IDResp));
	if (pPacket_l == 0) {
		MODGATECOM_error(MODGATECOM_ERROR_OUT_OF_MEMORY_4, bTRUE, 0);
		return MODGATECOM_ERROR_OUT_OF_MEMORY_4;
	}

	pPacket_l->strTransportLayer.i16uCmd = MODGATE_AL_CMD_ID_Resp;
	pPacket_l->strTransportLayer.i16uDataLength = sizeof(MODGATECOM_IDResp);
	pPacket_l->strTransportLayer.i32uError = MODGATECOM_NO_ERROR;
	pPacket_l->strTransportLayer.i8uVersion = 0xaa;
	pPacket_l->strTransportLayer.i8uReserved = 0x52;
	memcpy(pPacket_l->i8uData, &MODGATE_OwnID_g, sizeof(MODGATECOM_IDResp));

	return MG_LL_send(&alHdl->llParas, pPacket_l);
}

    //*************************************************************************************************
    //| Function: MG_recv_Id_Resp
    //|
    //! \brief
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
TBOOL MODGATECOM_recv_Id_Resp(ALHandle alHdl, MODGATECOM_Packet * pPacket_p)
{
	memcpy(&alHdl->OtherID, pPacket_p->i8uData, sizeof(MODGATECOM_IDResp));

	// Test feature descriptor
	if (!(alHdl->OtherID.i16uFeatureDescriptor & MODGATE_feature_IODataExchange)) {
		kbUT_free(pPacket_p);
		return bFALSE;
	}
	// Output Datenlänge an Input Datenlänge des anderen mGates anpassen
	if (alHdl->OtherID.i16uFBS_InputLength < alHdl->i16uOutDataLen) {
		alHdl->i16uOutDataLenActive = alHdl->OtherID.i16uFBS_InputLength;
	} else {
		alHdl->i16uOutDataLenActive = alHdl->i16uOutDataLen;
	}

	// Input Datenlänge an Output Datenlänge des anderen mGates anpassen
	if (alHdl->OtherID.i16uFBS_OutputLength < alHdl->i16uInDataLen) {
		alHdl->i16uInDataLenActive = alHdl->OtherID.i16uFBS_OutputLength;
	} else {
		alHdl->i16uInDataLenActive = alHdl->i16uInDataLen;
	}

	kbUT_free(pPacket_p);
	return bTRUE;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_Send_cyclicPD
    //|
    //! \brief
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT32U MODGATECOM_send_cyclicPD(ALHandle alHdl)
{
	MODGATECOM_CyclicPD *strCyclicPd;
	MODGATECOM_Packet *pPacket_l = (MODGATECOM_Packet *) kbUT_malloc(LL_pvPacketHeapHandle_g, 4,
									 MODGATE_TL_HEADER_LEN +
									 sizeof(MODGATECOM_CyclicPD) +
									 alHdl->i16uOutDataLenActive);
	if (pPacket_l == 0) {
		MODGATECOM_error(MODGATECOM_ERROR_OUT_OF_MEMORY_5, bTRUE, 0);
		return MODGATECOM_ERROR_OUT_OF_MEMORY_5;
	}
	strCyclicPd = (MODGATECOM_CyclicPD *) pPacket_l->i8uData;

	pPacket_l->strTransportLayer.i16uCmd = MODGATE_AL_CMD_cyclicPD;
	pPacket_l->strTransportLayer.i16uDataLength = sizeof(MODGATECOM_CyclicPD) + alHdl->i16uOutDataLenActive;
	pPacket_l->strTransportLayer.i32uError = MODGATECOM_NO_ERROR;
	pPacket_l->strTransportLayer.i8uVersion = 0xaa;
	pPacket_l->strTransportLayer.i8uReserved = 0x53;

	// Eigener Feldbusstatus
	strCyclicPd->i8uFieldbusStatus = i8uOwnFieldbusState_s;
	// Maximale Datenlänge des anderen mGate verwenden
	strCyclicPd->i16uDataLen = alHdl->i16uOutDataLenActive;
	// TODO: Offset immer 0
	strCyclicPd->i16uOffset = 0;
	// Aktuelle Prozessdaten kopieren
	memcpy(strCyclicPd->i8uData, alHdl->pi8uOutData, alHdl->i16uOutDataLenActive);

	return MG_LL_send(&alHdl->llParas, pPacket_l);
}

    //*************************************************************************************************
    //| Function: MG_recv_cyclicPD
    //|
    //! \brief
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
TBOOL MODGATECOM_recv_cyclicPD(ALHandle alHdl, MODGATECOM_Packet * pPacket_p)
{
	MODGATECOM_CyclicPD *strCyclicPd = (MODGATECOM_CyclicPD *) pPacket_p->i8uData;

	if (alHdl->i16uInDataLenActive < strCyclicPd->i16uDataLen) {
		MODGATECOM_error(MODGATECOM_ERROR_INVALID_DATA_SIZE, bFALSE, 0);
	} else if (strCyclicPd->i16uOffset + strCyclicPd->i16uDataLen > alHdl->i16uInDataLenActive) {
		MODGATECOM_error(MODGATECOM_ERROR_INVALID_DATA_OFFSET, bFALSE, 0);
	} else {
		memcpy(alHdl->pi8uInData + strCyclicPd->i16uOffset, strCyclicPd->i8uData, strCyclicPd->i16uDataLen);	//FBS_Input to PLC
		alHdl->i8uOtherFieldbusState = strCyclicPd->i8uFieldbusStatus;
	}

	kbUT_free(pPacket_p);
	return bTRUE;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_SetOwnFieldbusState
    //|
    //! \brief  Set the own fieldbus state. It will be sent to the partner in the next cyclicPD packet
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
void MODGATECOM_SetOwnFieldbusState(INT8U i8uOwnFieldbusState_p)
{
	i8uOwnFieldbusState_s = i8uOwnFieldbusState_p;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_GetOwnFieldbusState
    //|
    //! \brief  Return the own fieldbus state.
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT8U MODGATECOM_GetOwnFieldbusState(void)
{
	return i8uOwnFieldbusState_s;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_GetOtherFieldbusState
    //|
    //! \brief  Return the fieldbus state of the partner.
    //! It is updated with each cyclicPD packet from partner.
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT8U MODGATECOM_GetOtherFieldbusState(INT8U i8uInst_p)
{
	return AL_Data_s[i8uInst_p].i8uOtherFieldbusState;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_GetState
    //|
    //! \brief  Return the state of the mGate state machine
    //!
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
MODGATE_AL_Status MODGATECOM_GetState(INT8U i8uInst_p)
{
	return (MODGATE_AL_Status) AL_Data_s[i8uInst_p].i8uState;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_GetLedState
    //|
    //! \brief  Return the state of the mGate LED state machine
    //!
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
MODGATECOM_EPowerLedState MODGATECOM_GetLedState(void)
{
	return MODGATECOM_enPLS_INIT;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_GetInputDataLengthActive
    //|
    //! \brief  Return the active length of the (own) input data (minimum negotiated with peer)
    //!
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT16U MODGATECOM_GetInputDataLengthActive(INT8U i8uInst_p)	// in modular Gateways, always 0 must be passed
{
	return AL_Data_s[i8uInst_p].i16uInDataLenActive;
}

    //*************************************************************************************************
    //| Function: MODGATECOM_GetOutputDataLengthActive
    //|
    //! \brief  Return the active length of the (own) output data (minimum negotiated with peer)
    //!
    //!
    //! \detailed
    //!
    //!
    //!
    //! \ingroup
    //-------------------------------------------------------------------------------------------------
INT16U MODGATECOM_GetOutputDataLengthActive(INT8U i8uInst_p)	// in modular Gateways, always 0 must be passed
{
	return AL_Data_s[i8uInst_p].i16uOutDataLenActive;
}
