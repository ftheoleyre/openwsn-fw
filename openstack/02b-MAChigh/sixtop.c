#include "config.h"
#include "opendefs.h"
#include "sixtop.h"
#include "openserial.h"
#include "openqueue.h"
#include "neighbors.h"
#include "IEEE802154E.h"
#include "frag.h"
#include "iphc.h"
#include "packetfunctions.h"
#include "openrandom.h"
#include "scheduler.h"
#include "opentimers.h"
#include "debugpins.h"
#include "IEEE802154.h"
#include "IEEE802154_security.h"
#include "idmanager.h"
#include "schedule.h"
#include "msf.h"

//=========================== define ==========================================

// in seconds: sixtop maintaince is called every 30 seconds
#define MAINTENANCE_PERIOD        30
/**
 Drop the 6P request if number of 6P response with RC RESET in queue is larger
    than MAX6PRESPONSE. Value 0 means that alway drop 6P response when the node
    is in a 6P transcation.
*/
#define MAX6PRESPONSE             1

//=========================== variables =======================================

sixtop_vars_t sixtop_vars;

//=========================== prototypes ======================================

// send internal
owerror_t sixtop_send_internal(OpenQueueEntry_t *msg, bool payloadIEPresent);

// timer interrupt callbacks
void sixtop_maintenance_timer_cb(opentimers_id_t id);

void sixtop_timeout_timer_cb(opentimers_id_t id);

void sixtop_sendingEb_timer_cb(opentimers_id_t id);

//=== EB/KA task

void timer_sixtop_sendEb_fired(void);

void timer_sixtop_management_fired(void);

void sixtop_sendEB(void);

void sixtop_sendKA(void);

//=== six2six task

void timer_sixtop_six2six_timeout_fired(void);

void sixtop_six2six_sendDone(OpenQueueEntry_t *msg, owerror_t error);

bool sixtop_processIEs(
        OpenQueueEntry_t *pkt,
        uint16_t *lenIE
);

void sixtop_six2six_notifyReceive(
        uint8_t version,
        uint8_t type,
        uint8_t code,
        uint8_t sfId,
        uint8_t seqNum,
        uint8_t ptr,
        uint8_t length,
        OpenQueueEntry_t *pkt
);

//=== helper functions


bool sixtop_addCells(
        uint8_t slotframeID,
        cellInfo_ht *cellList,
        open_addr_t *previousHop,
        open_addr_t *neighbor2,
        uint8_t cellOptions
);

bool sixtop_removeCells(
        uint8_t slotframeID,
        cellInfo_ht *cellList,
        open_addr_t *previousHop,
        uint8_t cellOptions
);

bool sixtop_areAvailableCellsToBeScheduled(
        uint8_t frameID,
        uint8_t numOfCells,
        cellInfo_ht *cellList
);

bool sixtop_areAvailableCellsToBeRemoved(
        uint8_t frameID,
        uint8_t numOfCells,
        cellInfo_ht *cellList,
        open_addr_t *neighbor,
        uint8_t cellOptions
);

//=========================== public ==========================================

void sixtop_init(void) {

    sixtop_vars.periodMaintenance = 872 + (openrandom_get16b() & 0xff);
    sixtop_vars.busySendingKA = FALSE;
    sixtop_vars.busySendingEB = FALSE;
    sixtop_vars.dsn = 0;
    sixtop_vars.mgtTaskCounter = 0;
    sixtop_vars.kaPeriod = MAXKAPERIOD;
    sixtop_setState(SIX_STATE_IDLE);
    memset(&sixtop_vars.neighborOngoing3Steps, 0, sizeof(open_addr_t));

    sixtop_vars.ebSendingTimerId = opentimers_create(TIMER_GENERAL_PURPOSE, TASKPRIO_SIXTOP);
    opentimers_scheduleIn(
            sixtop_vars.ebSendingTimerId,
            SLOTFRAME_LENGTH * SLOTDURATION,
            TIME_MS,
            TIMER_PERIODIC,
            sixtop_sendingEb_timer_cb
    );

    sixtop_vars.maintenanceTimerId = opentimers_create(TIMER_GENERAL_PURPOSE, TASKPRIO_SIXTOP);
    opentimers_scheduleIn(
            sixtop_vars.maintenanceTimerId,
            sixtop_vars.periodMaintenance,
            TIME_MS,
            TIMER_PERIODIC,
            sixtop_maintenance_timer_cb
    );

    sixtop_vars.timeoutTimerId = opentimers_create(TIMER_GENERAL_PURPOSE, TASKPRIO_SIXTOP);
}

void  sixtop_setSFcallback(
    sixtop_sf_getsfid_cbt           cb0,
    sixtop_sf_getmetadata_cbt       cb1,
    sixtop_sf_translatemetadata_cbt cb2,
    sixtop_sf_handle_callback_cbt   cb3
){
   sixtop_vars.cb_sf_getsfid            = cb0;
   sixtop_vars.cb_sf_getMetadata        = cb1;
   sixtop_vars.cb_sf_translateMetadata  = cb2;
   sixtop_vars.cb_sf_handleRCError      = cb3;
}

//======= scheduling

owerror_t sixtop_request(
        uint8_t code,
        open_addr_t *neighbor,
        open_addr_t *neighbor2,
        uint8_t numCells,
        uint8_t cellOptions,
        cellInfo_ht *celllist_toBeAdded,
        cellInfo_ht *celllist_toBeDeleted,
        uint8_t sfid,
        uint16_t listingOffset,
        uint16_t listingMaxNumCells
) {
    OpenQueueEntry_t *pkt;
    uint8_t i;
    uint8_t len;
    uint16_t length_groupid_type;
    uint8_t sequenceNumber;
    owerror_t outcome;

    // filter parameters: handler, status and neighbor
    if (sixtop_vars.six2six_state != SIX_STATE_IDLE || neighbor == NULL) {
        // neighbor can't be none or previous transcation doesn't finish yet
        return E_FAIL;
    }
   
    openserial_printf("sixtop packet request created for %x:%x, seqnum %d, code %d, nbcells %d, anycast=%d",
                     neighbor->addr_64b[6],
                     neighbor->addr_64b[7],
                      neighbors_getSequenceNumber(neighbor),
                     code,
                     numCells,
                     neighbor2 != NULL
                     );

    if (openqueue_getNum6PReq(neighbor) > 0) {
        // remove previous request as it's not sent out
        openqueue_remove6PrequestToNeighbor(neighbor);
    }

    // get a free packet buffer
    pkt = openqueue_getFreePacketBuffer(COMPONENT_SIXTOP_RES);
    if (pkt == NULL) {
        LOG_ERROR(COMPONENT_SIXTOP_RES, ERR_NO_FREE_PACKET_BUFFER, (errorparameter_t) 0, (errorparameter_t) 0);
        return E_FAIL;
    }

    // take ownership
    pkt->creator = COMPONENT_SIXTOP_RES;
    pkt->owner = COMPONENT_SIXTOP_RES;

    // saves the id of the second receiver
    if (neighbor2 != NULL){
       memcpy(&(sixtop_vars.neigbor_secondReceiver), neighbor2, sizeof(open_addr_t));
       cellOptions = cellOptions;    // priority for neighbor = 0 (first one)
    }
    else
       bzero(&(sixtop_vars.neigbor_secondReceiver), sizeof(open_addr_t));
   
    memcpy(&(pkt->l2_nextORpreviousHop), neighbor, sizeof(open_addr_t));
    if (celllist_toBeDeleted != NULL) {
        memcpy(sixtop_vars.celllist_toDelete, celllist_toBeDeleted, CELLLIST_MAX_LEN * sizeof(cellInfo_ht));
    }
    sixtop_vars.cellOptions = cellOptions;

    len = 0;
    if (code == IANA_6TOP_CMD_ADD || code == IANA_6TOP_CMD_DELETE || code == IANA_6TOP_CMD_RELOCATE) {
        // append 6p celllists
        if (code == IANA_6TOP_CMD_ADD || code == IANA_6TOP_CMD_RELOCATE) {
           
           //3-steps handshake -> that's to the other side to propose cells to add
           if(celllist_toBeAdded == NULL)
              memcpy(&(sixtop_vars.neighborOngoing3Steps), neighbor, sizeof(open_addr_t));
           else{
              for (i = 0; i < numCells; i++) { //CELLLIST_MAX_LEN; i++) {
                   if (celllist_toBeAdded[i].isUsed) {
                       if (packetfunctions_reserveHeader(&pkt, 4) == E_FAIL){
                           return E_FAIL;
                       }
                       pkt->payload[0] = (uint8_t)(celllist_toBeAdded[i].slotoffset & 0x00FF);
                       pkt->payload[1] = (uint8_t)((celllist_toBeAdded[i].slotoffset & 0xFF00) >> 8);
                       pkt->payload[2] = (uint8_t)(celllist_toBeAdded[i].channeloffset & 0x00FF);
                       pkt->payload[3] = (uint8_t)((celllist_toBeAdded[i].channeloffset & 0xFF00) >> 8);
                       len += 4;
                   }
               }
              }
        }
        if (code == IANA_6TOP_CMD_DELETE || code == IANA_6TOP_CMD_RELOCATE) {
           for (i = 0; i < numCells; i++) { //CELLLIST_MAX_LEN; i++) {
                if (celllist_toBeDeleted[i].isUsed) {
                    if (packetfunctions_reserveHeader(&pkt, 4) == E_FAIL){
                        return E_FAIL;
                    }
                    pkt->payload[0] = (uint8_t)(celllist_toBeDeleted[i].slotoffset & 0x00FF);
                    pkt->payload[1] = (uint8_t)((celllist_toBeDeleted[i].slotoffset & 0xFF00) >> 8);
                    pkt->payload[2] = (uint8_t)(celllist_toBeDeleted[i].channeloffset & 0x00FF);
                    pkt->payload[3] = (uint8_t)((celllist_toBeDeleted[i].channeloffset & 0xFF00) >> 8);
                    len += 4;
                }
            }
        }
        // append 6p numberCells
        if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t))) {
            return E_FAIL;
        }
        *((uint8_t * )(pkt->payload)) = numCells;
        len += 1;
    }

    if (code == IANA_6TOP_CMD_LIST) {
        // append 6p max number of cells
        if (packetfunctions_reserveHeader(&pkt, sizeof(uint16_t)) == E_FAIL){
            return E_FAIL;
        }
        *((uint8_t * )(pkt->payload)) = (uint8_t)(listingMaxNumCells & 0x00FF);
        *((uint8_t * )(pkt->payload + 1)) = (uint8_t)(listingMaxNumCells & 0xFF00) >> 8;
        len += 2;
        // append 6p listing offset
        if (packetfunctions_reserveHeader(&pkt, sizeof(uint16_t)) == E_FAIL){
            return E_FAIL;
        }
        *((uint8_t * )(pkt->payload)) = (uint8_t)(listingOffset & 0x00FF);
        *((uint8_t * )(pkt->payload + 1)) = (uint8_t)(listingOffset & 0xFF00) >> 8;
        len += 2;
        // append 6p Reserved field
        if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL){
            return E_FAIL;
        }
        *((uint8_t * )(pkt->payload)) = 0;
        len += 1;
    }

    if (code != IANA_6TOP_CMD_CLEAR) {
        // append 6p celloptions
        if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL) {
            return E_FAIL;
        }
        *((uint8_t * )(pkt->payload)) = cellOptions;
        len += 1;
    } else {
        // record the neighbor in case no response  for clear
        memcpy(&sixtop_vars.neighborToClearCells, neighbor, sizeof(open_addr_t));
    }

    // append 6p metadata
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint16_t)) == E_FAIL){
        return E_FAIL;
    }
    pkt->payload[0] = (uint8_t)(sixtop_vars.cb_sf_getMetadata() & 0x00FF);
    pkt->payload[1] = (uint8_t)((sixtop_vars.cb_sf_getMetadata() & 0xFF00) >> 8);
    len += 2;

    // append 6p Seqnum and schedule Generation
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL) {
        return E_FAIL;
    }
    sequenceNumber = neighbors_getSequenceNumber(neighbor);
    *((uint8_t * )(pkt->payload)) = sequenceNumber;
    len += 1;
   
    // append 6p sfid
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL) {
        return E_FAIL;
    }
    *((uint8_t * )(pkt->payload)) = sfid;
    len += 1;

    // append 6p code
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL) {
        return E_FAIL;
    }
    *((uint8_t * )(pkt->payload)) = code;
    // record the code to determine the action after 6p senddone
    pkt->l2_sixtop_command = code;
    len += 1;

    // append 6p version, T(type) and  R(reserved)
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL) {
        return E_FAIL;
    }
    *((uint8_t * )(pkt->payload)) = IANA_6TOP_6P_VERSION | IANA_6TOP_TYPE_REQUEST;
    len += 1;

    // append 6p subtype id
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint8_t)) == E_FAIL) {
        return E_FAIL;
    }
    *((uint8_t * )(pkt->payload)) = IANA_6TOP_SUBIE_ID;
    len += 1;

    // append IETF IE header (length_groupid_type)
    if (packetfunctions_reserveHeader(&pkt, sizeof(uint16_t)) == E_FAIL) {
        return E_FAIL;
    }
    length_groupid_type = len;
    length_groupid_type |= (IANA_IETF_IE_GROUP_ID | IANA_IETF_IE_TYPE);
    pkt->payload[0] = length_groupid_type & 0xFF;
    pkt->payload[1] = (length_groupid_type >> 8) & 0xFF;

    // indicate IEs present
    pkt->l2_payloadIEpresent = TRUE;
    // record this packet as sixtop request message
    pkt->l2_sixtop_messageType = SIXTOP_CELL_REQUEST;

    // send packet
    outcome = sixtop_send(pkt);

    if (outcome == E_SUCCESS) {
        LOG_INFO(COMPONENT_SIXTOP, ERR_SIXTOP_REQUEST, (errorparameter_t) code, (errorparameter_t) 0);
             
        //update states
        switch (code) {
            case IANA_6TOP_CMD_ADD:
                sixtop_setState(SIX_STATE_WAIT_ADDREQUEST_SENDDONE);
                break;
            case IANA_6TOP_CMD_DELETE:
                sixtop_setState(SIX_STATE_WAIT_DELETEREQUEST_SENDDONE);
                break;
            case IANA_6TOP_CMD_RELOCATE:
                sixtop_setState(SIX_STATE_WAIT_RELOCATEREQUEST_SENDDONE);
                break;
            case IANA_6TOP_CMD_COUNT:
                sixtop_setState(SIX_STATE_WAIT_COUNTREQUEST_SENDDONE);
                break;
            case IANA_6TOP_CMD_LIST:
                sixtop_setState(SIX_STATE_WAIT_LISTREQUEST_SENDDONE);
                break;
            case IANA_6TOP_CMD_CLEAR:
                sixtop_setState(SIX_STATE_WAIT_CLEARREQUEST_SENDDONE);
                break;
        }
    } else {
        openqueue_freePacketBuffer(pkt);
    }
    openserial_printf("Novel State %d, outcome %d\n", sixtop_vars.six2six_state, outcome);
    return outcome;
}

//======= from upper layer

owerror_t sixtop_send(OpenQueueEntry_t *msg) {

    // set metadata
    msg->owner = COMPONENT_SIXTOP;
    msg->l2_frameType = IEEE154_TYPE_DATA;

    // set l2-security attributes
    msg->l2_securityLevel = IEEE802154_security_getSecurityLevel(msg);
    msg->l2_keyIdMode = IEEE802154_SECURITY_KEYIDMODE;
    msg->l2_keyIndex = IEEE802154_security_getDataKeyIndex();
   
    if (msg->l2_payloadIEpresent == FALSE) {
        return sixtop_send_internal(msg, FALSE);
    } else {
        return sixtop_send_internal(msg, TRUE);
    }

}

//======= from lower layer

void task_sixtopNotifSendDone(void) {
    OpenQueueEntry_t *msg;

    // get recently-sent packet from openqueue
    msg = openqueue_sixtopGetSentPacket();
    if (msg == NULL) {
        LOG_CRITICAL(COMPONENT_SIXTOP, ERR_NO_SENT_PACKET, (errorparameter_t) 0, (errorparameter_t) 0);
        return;
    }

    // take ownership
    msg->owner = COMPONENT_SIXTOP;

    // update neighbor statistics
    if (msg->l2_sendDoneError == E_SUCCESS) {
        neighbors_indicateTx(
                &(msg->l2_nextORpreviousHop),
                msg->l2_numTxAttempts,
                msg->l2_sendOnTxCell,
                TRUE,
                &msg->l2_asn
        );
    } else {
        neighbors_indicateTx(
                &(msg->l2_nextORpreviousHop),
                msg->l2_numTxAttempts,
                msg->l2_sendOnTxCell,
                FALSE,
                &msg->l2_asn
        );
    }

    // send the packet to where it belongs
    switch (msg->creator) {
        case COMPONENT_SIXTOP:
            if (msg->l2_frameType == IEEE154_TYPE_BEACON) {
                // this is a EB and not busy sending EB anymore
                sixtop_vars.busySendingEB = FALSE;
            } else {
                // this is a KA and not busy sending KA anymore
                sixtop_vars.busySendingKA = FALSE;
            }
            // discard packets
            openqueue_freePacketBuffer(msg);
            break;
        case COMPONENT_SIXTOP_RES:
            sixtop_six2six_sendDone(msg, msg->l2_sendDoneError);
            break;
        default:
            // send the rest up the stack
#if OPENWSN_6LO_FRAGMENTATION_C
            frag_sendDone(msg, msg->l2_sendDoneError);
#else
            iphc_sendDone(msg, msg->l2_sendDoneError);
#endif
            break;
    }
}

void task_sixtopNotifReceive(void) {
    OpenQueueEntry_t *msg;
    uint16_t lenIE;
   
    // get received packet from openqueue
    msg = openqueue_sixtopGetReceivedPacket();
    if (msg == NULL) {
        LOG_CRITICAL(COMPONENT_SIXTOP, ERR_NO_RECEIVED_PACKET, (errorparameter_t) 0, (errorparameter_t) 0);
        return;
    }

    // take ownership
    msg->owner = COMPONENT_SIXTOP;

    // update neighbor statistics
    neighbors_indicateRx(
            &(msg->l2_nextORpreviousHop),
            msg->l1_rssi,
            &msg->l2_asn,
            msg->l2_joinPriorityPresent,
            msg->l2_joinPriority,
            msg->l2_securityLevel == IEEE154_ASH_SLF_TYPE_NOSEC ? TRUE : FALSE
    );

    // process the header IEs
    lenIE = 0;
    if (
            msg->l2_frameType == IEEE154_TYPE_DATA &&
            msg->l2_payloadIEpresent == TRUE &&
            sixtop_processIEs(msg, &lenIE) == FALSE
            ) {
        // free the packet's RAM memory
        openqueue_freePacketBuffer(msg);
        //log error
        return;
    }

    // toss the header IEs
    packetfunctions_tossHeader(&msg, lenIE);

    // reset it to avoid race conditions with this var.
    msg->l2_joinPriorityPresent = FALSE;

    // send the packet up the stack, if it qualifies
    switch (msg->l2_frameType) {
        case IEEE154_TYPE_BEACON:
        case IEEE154_TYPE_DATA:
        case IEEE154_TYPE_CMD:
            if (msg->length > 0) {
                if (msg->l2_frameType == IEEE154_TYPE_BEACON) {
                    // I have one byte frequence field, no useful for upper layer
                    // free up the RAM
                    openqueue_freePacketBuffer(msg);
                    break;
                }
                // send to upper layer
#if OPENWSN_6LO_FRAGMENTATION_C
                frag_receive(msg);
#else
                iphc_receive(msg);
#endif
            } else {
                // free up the RAM
                openqueue_freePacketBuffer(msg);
            }
            break;
        case IEEE154_TYPE_ACK:
        default:
            // free the packet's RAM memory
            openqueue_freePacketBuffer(msg);
            // log the error
            LOG_ERROR(COMPONENT_SIXTOP, ERR_MSG_UNKNOWN_TYPE, (errorparameter_t) msg->l2_frameType,
                      (errorparameter_t) 0);
            break;
    }
}

//======= debugging

/**
\brief Trigger this module to print status information, over serial.

debugPrint_* functions are used by the openserial module to continuously print
status information about several modules in the OpenWSN stack.

\returns TRUE if this function printed something, FALSE otherwise.
*/
bool debugPrint_myDAGrank(void) {
    uint16_t output;

    output = 0;
    output = icmpv6rpl_getMyDAGrank();
    openserial_printStatus(STATUS_DAGRANK, (uint8_t * ) & output, sizeof(uint16_t));
    return TRUE;
}

/**
\brief Trigger this module to print status information, over serial.

debugPrint_* functions are used by the openserial module to continuously print
status information about several modules in the OpenWSN stack.

\returns TRUE if this function printed something, FALSE otherwise.
*/
bool debugPrint_kaPeriod(void) {
    uint16_t output;

    output = sixtop_vars.kaPeriod;
    openserial_printStatus(STATUS_KAPERIOD, (uint8_t * ) & output, sizeof(output));
    return TRUE;
}

//=========================== private =========================================



/**
\brief Transfer packet to MAC.

This function adds a IEEE802.15.4 header to the packet and leaves it the
OpenQueue buffer. The very last thing it does is assigning this packet to the
virtual component COMPONENT_SIXTOP_TO_IEEE802154E. Whenever it gets a change,
IEEE802154E will handle the packet.

\param[in] msg The packet to the transmitted
\param[in] payloadIEPresent Indicates wheter an Information Element is present in the
   packet.

\returns E_SUCCESS iff successful.
*/
owerror_t sixtop_send_internal(
        OpenQueueEntry_t *msg,
        bool payloadIEPresent) {

    // assign a number of retries
    if (packetfunctions_isBroadcastMulticast(&(msg->l2_nextORpreviousHop)) == TRUE) {
        msg->l2_retriesLeft = 1;
    } else {
        msg->l2_retriesLeft = TXRETRIES + 1;
    }
    // record this packet's dsn (for matching the ACK)
    msg->l2_dsn = sixtop_vars.dsn++;
    // this is a new packet which I never attempted to send
    msg->l2_numTxAttempts = 0;
    // transmit with the default TX power
    msg->l1_txPower = TX_POWER;
    // add a IEEE802.15.4 header
    if (ieee802154_prependHeader(
            msg,
            msg->l2_frameType,
            payloadIEPresent,
            msg->l2_dsn,
            &(msg->l2_nextORpreviousHop)
    ) == E_FAIL) {
        return E_FAIL;
    }
    // change owner to IEEE802154E fetches it from queue
    msg->owner = COMPONENT_SIXTOP_TO_IEEE802154E;

    if (
            packetfunctions_isBroadcastMulticast(&(msg->l2_nextORpreviousHop)) == FALSE &&
            schedule_hasNegotiatedCellToNeighbor(&(msg->l2_nextORpreviousHop), CELLTYPE_TX) == FALSE &&
            schedule_hasAutoTxCellToNeighbor(&(msg->l2_nextORpreviousHop)) == FALSE
            ) {
        // the frame source address is not broadcast/multicast
        // no negotiated tx cell to that neighbor
        // no auto tx cell to that neighbor

        open_addr_t null_neighbor;
        memset(&null_neighbor, 0, sizeof(null_neighbor));
       
        schedule_addActiveSlot(
                msf_hashFunction_getSlotoffset(&(msg->l2_nextORpreviousHop)),    // slot offset
                CELLTYPE_TX,                                                     // type of slot
                TRUE,                                                            // shared?
                FALSE,                                                           // anycast?
                TRUE,                                                            // auto cell?
                msf_hashFunction_getChanneloffset(&(msg->l2_nextORpreviousHop)), // channel offset
                0,                                                               // default priority
                &(msg->l2_nextORpreviousHop),                                    // neighbor
                &null_neighbor                                                   // neighbor2
        );
    }
    return E_SUCCESS;
}

/**
\brief sixtop sendingEb timer callback function.

\note This timer callback function is executed in task mode by opentimer
    already. No need to push a task again.
*/
void sixtop_sendingEb_timer_cb(opentimers_id_t id) {
    timer_sixtop_sendEb_fired();
}

/**
\brief sixtop maintenance timer callback function.

\note This timer callback function is executed in task mode by opentimer
    already. No need to push a task again.
*/
void sixtop_maintenance_timer_cb(opentimers_id_t id) {
    timer_sixtop_management_fired();
}

/**
\brief sixtop timeout timer callback function.

\note This timer callback function is executed in task mode by opentimer
    already. No need to push a task again.
*/
void sixtop_timeout_timer_cb(opentimers_id_t id) {
    timer_sixtop_six2six_timeout_fired();
}

//======= EB/KA task

void timer_sixtop_sendEb_fired(void) {
    if (openrandom_get16b() < (0xffff / EB_PORTION)) {
        sixtop_sendEB();
    }
}

/**
\brief Timer handlers which triggers MAC management task.

This function is called in task context by the scheduler after the RES timer
has fired. This timer is set to fire every second, on average.

The body of this function executes one of the MAC management task.
*/
void timer_sixtop_management_fired(void) {

    sixtop_vars.mgtTaskCounter = (sixtop_vars.mgtTaskCounter + 1) % MAINTENANCE_PERIOD;

    switch (sixtop_vars.mgtTaskCounter) {
        case 0:
            // called every MAINTENANCE_PERIOD seconds
            neighbors_removeOld();
            break;
        default:
            // called every second, except once every MAINTENANCE_PERIOD seconds
            sixtop_sendKA();
            break;
    }
}

/**
\brief Send an EB.

This is one of the MAC management tasks. This function inlines in the
timers_res_fired() function, but is declared as a separate function for better
readability of the code.
*/
port_INLINE void sixtop_sendEB(void) {
    OpenQueueEntry_t *eb;
    uint8_t i;
    uint8_t eb_len;
    uint16_t temp16b;
    open_addr_t addressToWrite;

    memset(&addressToWrite, 0, sizeof(open_addr_t));

    if (
            (ieee154e_isSynch() == FALSE) ||
            (IEEE802154_security_isConfigured() == FALSE) ||
            (icmpv6rpl_getMyDAGrank() == DEFAULTDAGRANK) ||
            icmpv6rpl_daoSent() == FALSE) {
        // I'm not sync'ed, or did not join, or did not acquire a DAGrank or did not send out a DAO
        // before starting to advertize the network, we need to make sure that we are reachable downwards,
        // thus, the condition if DAO was sent

        // delete packets genereted by this module (EB and KA) from openqueue
        openqueue_removeAllCreatedBy(COMPONENT_SIXTOP);

        // I'm not busy sending an EB or KA
        sixtop_vars.busySendingEB = FALSE;
        sixtop_vars.busySendingKA = FALSE;

        // stop here
        return;
    }

    if (sixtop_vars.busySendingEB == TRUE) {
        // don't continue if I'm still sending a previous EB
        return;
    }

    // if I get here, I will schedule an EB, get a free packet buffer
    eb = openqueue_getFreePacketBuffer(COMPONENT_SIXTOP);
    if (eb == NULL) {
        LOG_ERROR(COMPONENT_SIXTOP, ERR_NO_FREE_PACKET_BUFFER, (errorparameter_t) 0, (errorparameter_t) 0);
        return;
    }

    // declare ownership over that packet
    eb->creator = COMPONENT_SIXTOP;
    eb->owner = COMPONENT_SIXTOP;

    // in case we none default number of shared cells defined in minimal configuration
    if (ebIEsBytestream[EB_SLOTFRAME_NUMLINK_OFFSET] > 1) {
        for (i = ebIEsBytestream[EB_SLOTFRAME_NUMLINK_OFFSET] - 1; i > 0; i--) {
            packetfunctions_reserveHeader(&eb, 5);
            eb->payload[0] = i;    // slot offset
            eb->payload[1] = 0x00;
            eb->payload[2] = 0x00; // channel offset
            eb->payload[3] = 0x00;
            eb->payload[4] = 0x0F; // link options
        }
    }

    // reserve space for EB IEs
    packetfunctions_reserveHeader(&eb, EB_IE_LEN);
    for (i = 0; i < EB_IE_LEN; i++) {
        eb->payload[i] = ebIEsBytestream[i];
    }

    if (ebIEsBytestream[EB_SLOTFRAME_NUMLINK_OFFSET] > 1) {
        // reconstruct the MLME IE header since length changed
        eb_len = EB_IE_LEN - 2 + 5 * (ebIEsBytestream[EB_SLOTFRAME_NUMLINK_OFFSET] - 1);
        temp16b = eb_len | IEEE802154E_PAYLOAD_DESC_GROUP_ID_MLME | IEEE802154E_PAYLOAD_DESC_TYPE_MLME;
        eb->payload[0] = (uint8_t)(temp16b & 0x00ff);
        eb->payload[1] = (uint8_t)((temp16b & 0xff00) >> 8);
    }

    eb->payload[EB_SLOTFRAME_LEN_OFFSET] = (uint8_t)(0x00FF & (schedule_getFrameLength()));
    eb->payload[EB_SLOTFRAME_LEN_OFFSET + 1] = (uint8_t)(0x00FF & (schedule_getFrameLength() >> 8));

    // Keep a pointer to where the ASN will be
    // Note: the actual value of the current ASN and JP will be written by the
    //    IEEE802.15.4e when transmitting
    eb->l2_ASNpayload = &eb->payload[EB_ASN0_OFFSET];

    // some l2 information about this packet
    eb->l2_frameType = IEEE154_TYPE_BEACON;
    eb->l2_nextORpreviousHop.type = ADDR_16B;
    eb->l2_nextORpreviousHop.addr_16b[0] = 0xff;
    eb->l2_nextORpreviousHop.addr_16b[1] = 0xff;

    //I has an IE in my payload
    eb->l2_payloadIEpresent = TRUE;

    // set l2-security attributes
    eb->l2_securityLevel = IEEE802154_SECURITY_LEVEL_BEACON;
    eb->l2_keyIdMode = IEEE802154_SECURITY_KEYIDMODE;
    eb->l2_keyIndex = IEEE802154_security_getBeaconKeyIndex();

    // put in queue for MAC to handle
    sixtop_send_internal(eb, eb->l2_payloadIEpresent);

    // I'm now busy sending an EB
    sixtop_vars.busySendingEB = TRUE;
}

/**
\brief Send an keep-alive message, if necessary.

This is one of the MAC management tasks. This function inlines in the
timers_res_fired() function, but is declared as a separate function for better
readability of the code.
*/
port_INLINE void sixtop_sendKA(void) {
    OpenQueueEntry_t *kaPkt;
    open_addr_t *kaNeighAddr;

    if (ieee154e_isSynch() == FALSE) {
        // I'm not sync'ed

        // delete packets genereted by this module (EB and KA) from openqueue
        openqueue_removeAllCreatedBy(COMPONENT_SIXTOP);

        // I'm not busy sending an EB or KA
        sixtop_vars.busySendingEB = FALSE;
        sixtop_vars.busySendingKA = FALSE;

        // stop here
        return;
    }

    if (sixtop_vars.busySendingKA == TRUE) {
        // don't proceed if I'm still sending a KA
        return;
    }

    kaNeighAddr = neighbors_getKANeighbor(sixtop_vars.kaPeriod);
    if (kaNeighAddr == NULL) {
        // don't proceed if I have no neighbor I need to send a KA to
        return;
    }

    if (schedule_hasNegotiatedCellToNeighbor(kaNeighAddr, CELLTYPE_TX) == FALSE) {
        // delete packets genereted by this module (EB and KA) from openqueue
        openqueue_removeAllCreatedBy(COMPONENT_SIXTOP);

        // I'm not busy sending an EB or KA
        sixtop_vars.busySendingEB = FALSE;
        sixtop_vars.busySendingKA = FALSE;

        return;
    }

    // if I get here, I will send a KA

    // get a free packet buffer
    kaPkt = openqueue_getFreePacketBuffer(COMPONENT_SIXTOP);
    if (kaPkt == NULL) {
        LOG_ERROR(COMPONENT_SIXTOP, ERR_NO_FREE_PACKET_BUFFER, (errorparameter_t) 1, (errorparameter_t) 0);
        return;
    }

    // declare ownership over that packet
    kaPkt->creator = COMPONENT_SIXTOP;
    kaPkt->owner = COMPONENT_SIXTOP;

    // some l2 information about this packet
    kaPkt->l2_frameType = IEEE154_TYPE_DATA;
    memcpy(&(kaPkt->l2_nextORpreviousHop), kaNeighAddr, sizeof(open_addr_t));

    // set l2-security attributes
    kaPkt->l2_securityLevel = IEEE802154_SECURITY_LEVEL; // do not exchange KAs with
    kaPkt->l2_keyIdMode = IEEE802154_SECURITY_KEYIDMODE;
    kaPkt->l2_keyIndex = IEEE802154_security_getDataKeyIndex();

    // put in queue for MAC to handle
    sixtop_send_internal(kaPkt, FALSE);

    // I'm now busy sending a KA
    sixtop_vars.busySendingKA = TRUE;

#ifdef OPENSIM
    debugpins_ka_set();
    debugpins_ka_clr();
#endif
}

//======= six2six task

void timer_sixtop_six2six_timeout_fired(void) {

    if (sixtop_vars.six2six_state == SIX_STATE_WAIT_CLEARRESPONSE) {
       openserial_printf("no response for the 6p clear, just clear locally\n");
       
        // no response for the 6p clear, just clear locally
        schedule_removeAllNegotiatedCellsToNeighbor(sixtop_vars.cb_sf_getMetadata(), &sixtop_vars.neighborToClearCells);
        neighbors_resetSequenceNumber(&sixtop_vars.neighborToClearCells);
        memset(&sixtop_vars.neighborToClearCells, 0, sizeof(open_addr_t));
    }
    // timeout timer fired, reset the state of sixtop to idle
    memset(&sixtop_vars.neighborOngoing3Steps, 0, sizeof(open_addr_t));
    sixtop_setState(SIX_STATE_IDLE);
    opentimers_cancel(sixtop_vars.timeoutTimerId);
}

void sixtop_six2six_sendDone(OpenQueueEntry_t *msg, owerror_t error) {
    msg->owner = COMPONENT_SIXTOP_RES;
     
    openserial_printf("packet sent to %x:%x, type %d\n",
                      msg->l2_nextORpreviousHop.addr_64b[6],
                      msg->l2_nextORpreviousHop.addr_64b[7],
                      msg->l2_sixtop_messageType);

    // if this is a request send done
    if (msg->l2_sixtop_messageType == SIXTOP_CELL_REQUEST) {
      
        if (error == E_FAIL) {
            // max retries, without ack
            switch (sixtop_vars.six2six_state) {

                case SIX_STATE_WAIT_CLEARREQUEST_SENDDONE:
                   sixtop_setState(SIX_STATE_WAIT_CLEARRESPONSE);
                    timer_sixtop_six2six_timeout_fired();
                    break;
                default:
                    // reset handler and state if the request is failed to send out
                    memset(&sixtop_vars.neighborOngoing3Steps, 0, sizeof(open_addr_t));
                    sixtop_setState(SIX_STATE_IDLE);
                    break;
            }
        } else {
           
            // the packet has been sent out successfully
            switch (sixtop_vars.six2six_state) {
                case SIX_STATE_WAIT_ADDREQUEST_SENDDONE:
                   //first ADD_REQ for a 3-steps handshake
                   if (packetfunctions_sameAddress(&(sixtop_vars.neighborOngoing3Steps),&(msg->l2_nextORpreviousHop)))
                      sixtop_setState(SIX_STATE_WAIT_ADDREQUEST);
                   else
                     sixtop_setState(SIX_STATE_WAIT_ADDRESPONSE);
                   break;
                case SIX_STATE_WAIT_DELETEREQUEST_SENDDONE:
                    sixtop_setState(SIX_STATE_WAIT_DELETERESPONSE);
                    break;
                case SIX_STATE_WAIT_RELOCATEREQUEST_SENDDONE:
                    sixtop_setState(SIX_STATE_WAIT_RELOCATERESPONSE);
                    break;
                case SIX_STATE_WAIT_LISTREQUEST_SENDDONE:
                    sixtop_setState(SIX_STATE_WAIT_LISTRESPONSE);
                    break;
                case SIX_STATE_WAIT_COUNTREQUEST_SENDDONE:
                    sixtop_setState(SIX_STATE_WAIT_COUNTRESPONSE);
                    break;
                case SIX_STATE_WAIT_CLEARREQUEST_SENDDONE:
                    sixtop_setState(SIX_STATE_WAIT_CLEARRESPONSE);
                    break;
                default:
                    openserial_printf("Tx of a request while we are in state %d -> should never happen\n", sixtop_vars.six2six_state);
                    sixtop_setState(SIX_STATE_IDLE);
                    // should never happen
                    break;
            }
            // start timeout timer if I am waiting for a response
            opentimers_scheduleIn(
                    sixtop_vars.timeoutTimerId,
                    SIX2SIX_TIMEOUT_MS,
                    TIME_MS,
                    TIMER_ONESHOT,
                    sixtop_timeout_timer_cb
            );
        }
    }
   
    // if this is a response send done
    if (msg->l2_sixtop_messageType == SIXTOP_CELL_RESPONSE) {
        if (error == E_SUCCESS) {           
            neighbors_updateSequenceNumber(&(msg->l2_nextORpreviousHop));
            
            openserial_printf("REP SENDONE, code %d, command %d\n", msg->l2_sixtop_returnCode, msg->l2_sixtop_command);
           
            // in case a response is sent out, check the return code
            if (msg->l2_sixtop_returnCode == IANA_6TOP_RC_SUCCESS) {
               if (msg->l2_sixtop_command == IANA_6TOP_CMD_ADD) {
                   sixtop_addCells(
                            msg->l2_sixtop_frameID,
                            msg->l2_sixtop_celllist_add,
                            &(msg->l2_nextORpreviousHop),
                            &(sixtop_vars.neigbor_secondReceiver),
                            msg->l2_sixtop_cellOptions
                   );
                  
                   // we are already idle if we received an unicast sixtop request
                   if (sixtop_vars.six2six_state != SIX_STATE_IDLE)
                      sixtop_setState(SIX_STATE_IDLE);
                  
               }
                if (msg->l2_sixtop_command == IANA_6TOP_CMD_DELETE) {
                    sixtop_removeCells(
                            msg->l2_sixtop_frameID,
                            msg->l2_sixtop_celllist_delete,
                            &(msg->l2_nextORpreviousHop),
                            msg->l2_sixtop_cellOptions
                    );
                }

                if (msg->l2_sixtop_command == IANA_6TOP_CMD_RELOCATE) {
                    sixtop_removeCells(
                            msg->l2_sixtop_frameID,
                            msg->l2_sixtop_celllist_delete,
                            &(msg->l2_nextORpreviousHop),
                            msg->l2_sixtop_cellOptions
                    );
                    sixtop_addCells(
                            msg->l2_sixtop_frameID,
                            msg->l2_sixtop_celllist_add,
                            &(msg->l2_nextORpreviousHop),
                            &(sixtop_vars.neigbor_secondReceiver),
                            msg->l2_sixtop_cellOptions
                    );
                }

                if (msg->l2_sixtop_command == IANA_6TOP_CMD_CLEAR) {
                   openserial_printf("CLEAR COMMAND  send done\n");
                   
                    schedule_removeAllNegotiatedCellsToNeighbor(
                            msg->l2_sixtop_frameID,
                            &(msg->l2_nextORpreviousHop)
                    );
                    neighbors_resetSequenceNumber(&(msg->l2_nextORpreviousHop));
                }
            } else {
                // the return code doesn't end up with SUCCESS
                // The return code will be processed on request side.
            }
        } else {
            // doesn't receive the ACK of response packet from request side after maximum retries.

            // if the response is for CLEAR command, remove all the cells and reset seqnum regardless NO ack received.
            if (msg->l2_sixtop_command == IANA_6TOP_CMD_CLEAR) {
               openserial_printf("clear command send done, without ack received\n");
               
                schedule_removeAllNegotiatedCellsToNeighbor(msg->l2_sixtop_frameID, &(msg->l2_nextORpreviousHop));
                neighbors_resetSequenceNumber(&(msg->l2_nextORpreviousHop));
            }
        }
    }
    // free the buffer
    openqueue_freePacketBuffer(msg);
   
}

port_INLINE bool sixtop_processIEs(OpenQueueEntry_t* pkt, uint16_t* lenIE) {
    uint8_t ptr;
    uint8_t temp_8b;
    uint8_t subtypeid,code,sfid,version,type,seqNum;
    uint16_t temp_16b,len,headerlen;

    ptr = 0;
    headerlen = 0;

    // candidate IE header  if type ==0 header IE if type==1 payload IE
    temp_8b = *((uint8_t*)(pkt->payload)+ptr);
    ptr++;
    temp_16b = temp_8b + ((*((uint8_t*)(pkt->payload)+ptr))<<8);
    ptr++;
    *lenIE += 2;
    // check ietf ie group id, type
    if ((temp_16b & IEEE802154E_DESC_LEN_PAYLOAD_ID_TYPE_MASK) != (IANA_IETF_IE_GROUP_ID | IANA_IETF_IE_TYPE)){
        // wrong IE ID or type, record and drop the packet
        LOG_ERROR(COMPONENT_SIXTOP, ERR_UNSUPPORTED_FORMAT, (errorparameter_t)0, (errorparameter_t)0);
        return FALSE;
    }
    len = temp_16b & IEEE802154E_DESC_LEN_PAYLOAD_IE_MASK;
    *lenIE += len;

    // check 6p subtype Id
    subtypeid = *((uint8_t*)(pkt->payload)+ptr);
    ptr += 1;
    if (subtypeid != IANA_6TOP_SUBIE_ID){
        // wrong subtypeID, record and drop the packet
        LOG_ERROR(COMPONENT_SIXTOP, ERR_UNSUPPORTED_FORMAT, (errorparameter_t) 1, (errorparameter_t) 0);
        return FALSE;
    }
    headerlen += 1;

    // check 6p version
    temp_8b = *((uint8_t*)(pkt->payload)+ptr);
    ptr += 1;
    // 6p doesn't define type 3
    if (temp_8b >> IANA_6TOP_TYPE_SHIFT == 3){
        // wrong type, record and drop the packet
        LOG_ERROR(COMPONENT_SIXTOP, ERR_UNSUPPORTED_FORMAT, (errorparameter_t) 2, (errorparameter_t) 0);
        return FALSE;
    }
    version = temp_8b & IANA_6TOP_VESION_MASK;
    type = temp_8b >> IANA_6TOP_TYPE_SHIFT;
    headerlen += 1;

    // get 6p code
    code = *((uint8_t*)(pkt->payload)+ptr);
    ptr += 1;
    headerlen += 1;
   
    // get 6p sfid
    sfid = *((uint8_t*)(pkt->payload)+ptr);
    ptr += 1;
    headerlen += 1;
    // get 6p seqNum and GEN
    seqNum = *((uint8_t*)(pkt->payload)+ptr) & 0xff;
    ptr += 1;
    headerlen += 1;

    // give six2six to process
    sixtop_six2six_notifyReceive(version, type, code, sfid, seqNum, ptr, len-headerlen, pkt);
    *lenIE = len+2;
    return TRUE;
}

void sixtop_six2six_notifyReceive(
        uint8_t version,
        uint8_t type,
        uint8_t code,
        uint8_t sfId,
        uint8_t seqNum,
        uint8_t ptr,
        uint8_t length,
        OpenQueueEntry_t *pkt
) {
    uint8_t returnCode = -1;
    uint16_t metadata = -1;
    uint8_t cellOptions = -1;
    uint8_t cellOptions_transformed;
    uint16_t offset;
    uint16_t length_groupid_type;
    uint16_t startingOffset;
    uint8_t maxNumCells;
    uint16_t i;
    uint16_t slotoffset;
    uint16_t channeloffset;
    uint8_t priority;
    uint16_t numCells;
    uint16_t temp16;
    OpenQueueEntry_t * response_pkt;
    uint8_t pktLen = length;
    uint8_t response_pktLen = 0;
    cellInfo_ht celllist_list[CELLLIST_MAX_LEN];
    uint8_t response_type = IANA_6TOP_TYPE_RESPONSE;  //by default, we respond with a response

    openserial_printf("sixtop packet received from %x:%x, type %d, we are in state %d\n",
                      pkt->l2_nextORpreviousHop.addr_64b[6],
                      pkt->l2_nextORpreviousHop.addr_64b[7],
                      type,
                      sixtop_vars.six2six_state);

   
    //specific case: if I receive a reply in anycast from the second receiver -> I must send a reply to the first receiver
    if (type == SIXTOP_CELL_REQUEST){
        // if this is a 6p request message

        // drop the packet if there are too many 6P response in the queue
        if (openqueue_getNum6PResp() >= MAX6PRESPONSE) {
            return;
        }

        // get a free packet buffer
        response_pkt = openqueue_getFreePacketBuffer(COMPONENT_SIXTOP_RES);
        if (response_pkt == NULL) {
            LOG_ERROR(COMPONENT_SIXTOP_RES, ERR_NO_FREE_PACKET_BUFFER, (errorparameter_t) 0, (errorparameter_t) 0);
            return;
        }

        // take ownership
        response_pkt->creator = COMPONENT_SIXTOP_RES;
        response_pkt->owner = COMPONENT_SIXTOP_RES;

        memcpy(&(response_pkt->l2_nextORpreviousHop), &(pkt->l2_nextORpreviousHop), sizeof(open_addr_t));

        // the follow while loop only execute once
        do {
            // version check
            if (version != IANA_6TOP_6P_VERSION) {
                returnCode = IANA_6TOP_RC_VER_ERR;
                break;
            }
            // sfid check
            if (sfId != sixtop_vars.cb_sf_getsfid()) {
                returnCode = IANA_6TOP_RC_SFID_ERR;
                break;
            }
           
            // sequenceNumber check
            if (seqNum != neighbors_getSequenceNumber(&(pkt->l2_nextORpreviousHop)) && code != IANA_6TOP_CMD_CLEAR) {
               
                openserial_printf("Mismatch for the seqnum received from %x:%x (%d , expected %d)\n",
                                  pkt->l2_nextORpreviousHop.addr_64b[6],
                                  pkt->l2_nextORpreviousHop.addr_64b[7],
                                  seqNum,
                                  neighbors_getSequenceNumber(&(pkt->l2_nextORpreviousHop)));


               
                returnCode = IANA_6TOP_RC_SEQNUM_ERR;
                break;
            }
            // previous 6p transaction check
            if ((sixtop_vars.six2six_state != SIX_STATE_IDLE) &&
                !(
                  sixtop_vars.six2six_state == SIX_STATE_WAIT_ADDREQUEST
                   &&
                   packetfunctions_sameAddress(&(sixtop_vars.neighborOngoing3Steps), &(pkt->l2_nextORpreviousHop))
                 )
                ) {
               
               openserial_printf("Incorrect State (%d) when the request is received from %x:%x / %x:%x\n",
                                 sixtop_vars.six2six_state,
                                 pkt->l2_nextORpreviousHop.addr_64b[6],
                                 pkt->l2_nextORpreviousHop.addr_64b[7],
                                 sixtop_vars.neighborOngoing3Steps.addr_64b[6],
                                 sixtop_vars.neighborOngoing3Steps.addr_64b[7]);
               
                returnCode = IANA_6TOP_RC_RESET;
                break;
            }
            // metadata meaning check
            if (sixtop_vars.cb_sf_translateMetadata() != METADATA_TYPE_FRAMEID) {
                LOG_ERROR(COMPONENT_SIXTOP, ERR_UNSUPPORTED_METADATA, sixtop_vars.cb_sf_translateMetadata(), 0);
                returnCode = IANA_6TOP_RC_ERROR;
                break;
            }

            // commands check
           
            // get metadata, metadata indicates frame id
            metadata = *((uint8_t * )(pkt->payload) + ptr);
            metadata |= *((uint8_t * )(pkt->payload) + ptr + 1) << 8;
            ptr += 2;
            pktLen -= 2;

            // clear command
            if (code == IANA_6TOP_CMD_CLEAR) {
                // the cells will be removed when the repsonse sendone successfully
                // don't clear cells here
                returnCode = IANA_6TOP_RC_SUCCESS;
                break;
            }

            cellOptions = *((uint8_t * )(pkt->payload) + ptr);
            ptr += 1;
            pktLen -= 1;

            // list command
            if (code == IANA_6TOP_CMD_LIST) {
                ptr += 1; // skip the one byte reserved field
                offset = *((uint8_t * )(pkt->payload) + ptr);
                offset |= *((uint8_t * )(pkt->payload) + ptr + 1) << 8;
                ptr += 2;
                maxNumCells = *((uint8_t * )(pkt->payload) + ptr);
                maxNumCells |= *((uint8_t * )(pkt->payload) + ptr + 1) << 8;
                ptr += 2;

                returnCode = IANA_6TOP_RC_SUCCESS;
                startingOffset = offset;
                if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
                    cellOptions_transformed = cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX);
                } else {
                    cellOptions_transformed = cellOptions;
                }
                for (i = 0; i < maxNumCells; i++) {
                    if (
                            schedule_getOneCellAfterOffset(
                                    metadata,
                                    startingOffset,
                                    &(pkt->l2_nextORpreviousHop),
                                    cellOptions_transformed,
                                    &slotoffset,
                                    &channeloffset,
                                    &priority)
                            ) {
                        // found one cell after slot offset+i
                        packetfunctions_reserveHeader(&response_pkt, 4);
                        response_pkt->payload[0] = slotoffset & 0x00FF;
                        response_pkt->payload[1] = (slotoffset & 0xFF00) >> 8;
                        response_pkt->payload[2] = channeloffset & 0x00FF;
                        response_pkt->payload[3] = (channeloffset & 0xFF00) >> 8;
                        response_pktLen += 4;
                        startingOffset = slotoffset + 1;
                    } else {
                        // no more cell after offset
                        returnCode = IANA_6TOP_RC_EOL;
                        break;
                    }
                }
                if (
                        schedule_getOneCellAfterOffset(
                                metadata,
                                startingOffset,
                                &(pkt->l2_nextORpreviousHop),
                                cellOptions_transformed,
                                &slotoffset,
                                &channeloffset,
                                &priority) == FALSE
                        ) {
                    returnCode = IANA_6TOP_RC_EOL;
                }

                break;
            }

            // count command
            if (code == IANA_6TOP_CMD_COUNT) {
                numCells = 0;
                startingOffset = 0;
                if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
                    cellOptions_transformed = cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX);
                } else {
                    cellOptions_transformed = cellOptions;
                }
                for (i = 0; i < schedule_getFrameLength(); i++) {
                    if (
                            schedule_getOneCellAfterOffset(
                                    metadata,
                                    startingOffset,
                                    &(pkt->l2_nextORpreviousHop),
                                    cellOptions_transformed,
                                    &slotoffset,
                                    &channeloffset,
                                    &priority)
                            ) {
                        // found one cell after slot i
                        numCells++;
                        startingOffset = slotoffset + 1;
                    }
                }
                returnCode = IANA_6TOP_RC_SUCCESS;
                packetfunctions_reserveHeader(&response_pkt, sizeof(uint16_t));
                response_pkt->payload[0] = numCells & 0x00FF;
                response_pkt->payload[1] = (numCells & 0xFF00) >> 8;
                response_pktLen += 2;
                break;
            }

            numCells = *((uint8_t * )(pkt->payload) + ptr);
            ptr += 1;
            pktLen -= 1;
           
            // add command
            if (code == IANA_6TOP_CMD_ADD) {
                if (schedule_getNumberOfFreeEntries() < numCells) {
                    returnCode = IANA_6TOP_RC_BUSY;
                    break;
                }
               
               // retrieve cell list (or i=0 if the list is empty)
               i = 0;
               memset(response_pkt->l2_sixtop_celllist_add, 0, sizeof(response_pkt->l2_sixtop_celllist_add));
               while (pktLen > 0) {
                   response_pkt->l2_sixtop_celllist_add[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                   response_pkt->l2_sixtop_celllist_add[i].slotoffset |=
                           (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                   response_pkt->l2_sixtop_celllist_add[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                   response_pkt->l2_sixtop_celllist_add[i].channeloffset |=
                           (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                   response_pkt->l2_sixtop_celllist_add[i].isUsed = TRUE;
                   ptr += 4;
                   pktLen -= 4;
                   i++;
               }
         
               // 3-steps handshake (1st packet) -> respond with a list of cells
               if (i == 0){
                  openserial_printf("Request received without list (1st packet of a 3-steps handshake)\n");
                  
                  //asks MSF the list of cells
                  cellInfo_ht celllist_add[CELLLIST_MAX_LEN];
                  if (msf_candidateAddCellList(celllist_add, NUMCELLS_MSF) == FALSE) {
                     msf_candidateAddCellList(celllist_add, NUMCELLS_MSF);

                     // failed to get cell list to add
                     returnCode = IANA_6TOP_RC_CELLLIST_ERR;
                     break;
                  }
                  //insert the cells in the request
                  uint8_t num = 0;                  
                  for(i=0; i<CELLLIST_MAX_LEN; i++){
                     if(celllist_add[i].isUsed){
                        packetfunctions_reserveHeader(&response_pkt, 4);
                        response_pkt->payload[0] = celllist_add[i].slotoffset & 0x00FF;
                        response_pkt->payload[1] = (celllist_add[i].slotoffset & 0xFF00) >> 8;
                        response_pkt->payload[2] = celllist_add[i].channeloffset & 0x00FF;
                        response_pkt->payload[3] = (celllist_add[i].channeloffset & 0xFF00) >> 8;
                        response_pktLen += 4;
                        num++;
                     }
                  }
                  
                  // append 6p numberCells
                  packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                  *((uint8_t * )(response_pkt->payload)) = numCells;
                  response_pktLen += 1;
                  
                  // append 6p celloptions
                  if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
                      cellOptions_transformed = cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX);
                  } else {
                      cellOptions_transformed = cellOptions;
                  }
              
                  sixtop_vars.cellOptions = cellOptions_transformed;
                  packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                  *((uint8_t * )(response_pkt->payload)) = cellOptions_transformed;
                  response_pktLen += 1;
                  
                  // append 6p metadata
                  packetfunctions_reserveHeader(&response_pkt, sizeof(uint16_t));
                  response_pkt->payload[0] = (uint8_t)(sixtop_vars.cb_sf_getMetadata() & 0x00FF);
                  response_pkt->payload[1] = (uint8_t)((sixtop_vars.cb_sf_getMetadata() & 0xFF00) >> 8);
                  response_pktLen += 2;
                  

                  //everything was correct
                  returnCode = IANA_6TOP_CMD_ADD; //IANA_6TOP_RC_SUCCESS;
                  response_type = IANA_6TOP_TYPE_REQUEST;  //3 steps: request / request / reply

                  //change the sixtop state to wait for the reply (3rd step of the handshake)
                  // as if we were the initator of this 6P_ADD_CMD
                  sixtop_setState(SIX_STATE_WAIT_ADDREQUEST_SENDDONE);
                          
                  break;
                }
                //anycast negociation: I received the request with the list (2nd action), and I must send the req to the second receiver (3rd action)
                else if (
                         (sixtop_vars.six2six_state == SIX_STATE_WAIT_ADDREQUEST)
                         &&
                         (packetfunction_isNullAddress(&(sixtop_vars.neigbor_secondReceiver)) == FALSE)
                         ){
                   
                   openserial_printf("anycast negociation -> request with the list received from %d:%d, has now to negociate with %x:%x (ongoing %x:%x), seqnum %d\n",
                                     response_pkt->l2_nextORpreviousHop.addr_64b[6],
                                     response_pkt->l2_nextORpreviousHop.addr_64b[7],
                                     sixtop_vars.neigbor_secondReceiver.addr_64b[6],
                                     sixtop_vars.neigbor_secondReceiver.addr_64b[7],
                                     sixtop_vars.neighborOngoing3Steps.addr_64b[6],
                                     sixtop_vars.neighborOngoing3Steps.addr_64b[7],
                                     neighbors_getSequenceNumber(&(sixtop_vars.neigbor_secondReceiver)));
               
                  // Anycast negociation -> I must send a request (with the list) to the second receiver
                  memcpy(&(response_pkt->l2_nextORpreviousHop), &(sixtop_vars.neigbor_secondReceiver), sizeof(open_addr_t));
                   
                  //has to use the seqnum corresponding to the second receiver
                  seqNum = neighbors_getSequenceNumber(&(sixtop_vars.neigbor_secondReceiver));
                   
                  //insert the cells in the request
                  uint8_t num = 0;
                  for(i=0; i<CELLLIST_MAX_LEN; i++){
                     if(response_pkt->l2_sixtop_celllist_add[i].isUsed){
                        packetfunctions_reserveHeader(&response_pkt, 4);
                        response_pkt->payload[0] = response_pkt->l2_sixtop_celllist_add[i].slotoffset & 0x00FF;
                        response_pkt->payload[1] = (response_pkt->l2_sixtop_celllist_add[i].slotoffset & 0xFF00) >> 8;
                        response_pkt->payload[2] = response_pkt->l2_sixtop_celllist_add[i].channeloffset & 0x00FF;
                        response_pkt->payload[3] = (response_pkt->l2_sixtop_celllist_add[i].channeloffset & 0xFF00) >> 8;
                        response_pktLen += 4;
                        num++;
                     }
                  }
                  // append 6p numberCells
                  packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                  *((uint8_t * )(response_pkt->payload)) = numCells;
                  response_pktLen += 1;
                  
                  // append 6p celloptions
                  if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
                      cellOptions_transformed = (cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX)) | CELLOPTIONS_PRIORITY;
                  } else {
                      cellOptions_transformed = cellOptions | CELLOPTIONS_PRIORITY;
                  }
                  // priority = 1 for this second receiver (ack backoff = 1)
                  sixtop_vars.cellOptions = cellOptions_transformed;
                  packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                  *((uint8_t * )(response_pkt->payload)) = cellOptions_transformed;
                  response_pktLen += 1;
                              
                  // append 6p metadata
                  packetfunctions_reserveHeader(&response_pkt, sizeof(uint16_t));
                  response_pkt->payload[0] = (uint8_t)(sixtop_vars.cb_sf_getMetadata() & 0x00FF);
                  response_pkt->payload[1] = (uint8_t)((sixtop_vars.cb_sf_getMetadata() & 0xFF00) >> 8);
                  response_pktLen += 2;
                  

                  //everything was correct
                  returnCode = IANA_6TOP_CMD_ADD; //IANA_6TOP_RC_SUCCESS;
                  response_type = IANA_6TOP_TYPE_REQUEST;  //3 steps: request / request / reply

                  //change the sixtop state to wait for the reply (4th step of the anycast handshake)
                  sixtop_setState(SIX_STATE_WAIT_ADDREQUEST_SENDDONE);
                  break;
               }
               //Finalization: I must send the reply to the sender
               else{
                   openserial_printf("Request with list -> rep to send\n");
                  
                   if (sixtop_areAvailableCellsToBeScheduled(metadata, numCells, response_pkt->l2_sixtop_celllist_add)) {
                       for (i = 0; i < numCells; i++) {
                           if (response_pkt->l2_sixtop_celllist_add[i].isUsed) {
                               packetfunctions_reserveHeader(&response_pkt, 4);
                               response_pkt->payload[0] = (uint8_t)(
                                       response_pkt->l2_sixtop_celllist_add[i].slotoffset & 0x00FF);
                               response_pkt->payload[1] = (uint8_t)(
                                       (response_pkt->l2_sixtop_celllist_add[i].slotoffset & 0xFF00) >> 8);
                               response_pkt->payload[2] = (uint8_t)(
                                       response_pkt->l2_sixtop_celllist_add[i].channeloffset & 0x00FF);
                               response_pkt->payload[3] = (uint8_t)(
                                       (response_pkt->l2_sixtop_celllist_add[i].channeloffset & 0xFF00) >> 8);
                               response_pktLen += 4;
                           }
                       }
                   }
                  returnCode = IANA_6TOP_RC_SUCCESS;
                  break;
                }
            }

            // delete command
            if (code == IANA_6TOP_CMD_DELETE) {
                i = 0;
                memset(response_pkt->l2_sixtop_celllist_delete, 0, sizeof(response_pkt->l2_sixtop_celllist_delete));
                while (pktLen > 0) {
                    response_pkt->l2_sixtop_celllist_delete[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                    response_pkt->l2_sixtop_celllist_delete[i].slotoffset |=
                            (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                    response_pkt->l2_sixtop_celllist_delete[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                    response_pkt->l2_sixtop_celllist_delete[i].channeloffset |=
                            (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                    response_pkt->l2_sixtop_celllist_delete[i].isUsed = TRUE;
                    ptr += 4;
                    pktLen -= 4;
                    i++;
                }
                if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
                    cellOptions_transformed = cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX);
                } else {
                    cellOptions_transformed = cellOptions;
                }
                if (sixtop_areAvailableCellsToBeRemoved(metadata, numCells, response_pkt->l2_sixtop_celllist_delete,
                                                        &(pkt->l2_nextORpreviousHop), cellOptions_transformed)) {
                    returnCode = IANA_6TOP_RC_SUCCESS;
                    for (i = 0; i < CELLLIST_MAX_LEN; i++) {
                        if (response_pkt->l2_sixtop_celllist_delete[i].isUsed) {
                            packetfunctions_reserveHeader(&response_pkt, 4);
                            response_pkt->payload[0] = (uint8_t)(
                                    response_pkt->l2_sixtop_celllist_delete[i].slotoffset & 0x00FF);
                            response_pkt->payload[1] = (uint8_t)(
                                    (response_pkt->l2_sixtop_celllist_delete[i].slotoffset & 0xFF00) >> 8);
                            response_pkt->payload[2] = (uint8_t)(
                                    response_pkt->l2_sixtop_celllist_delete[i].channeloffset & 0x00FF);
                            response_pkt->payload[3] = (uint8_t)(
                                    (response_pkt->l2_sixtop_celllist_delete[i].channeloffset & 0xFF00) >> 8);
                            response_pktLen += 4;
                        }
                    }
                } else {
                    returnCode = IANA_6TOP_RC_CELLLIST_ERR;
                }
                break;
            }

            // relocate command
            if (code == IANA_6TOP_CMD_RELOCATE) {
                // retrieve cell list to be relocated
                i = 0;
                memset(response_pkt->l2_sixtop_celllist_delete, 0, sizeof(response_pkt->l2_sixtop_celllist_delete));
                temp16 = numCells;
                while (temp16 > 0) {
                    response_pkt->l2_sixtop_celllist_delete[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                    response_pkt->l2_sixtop_celllist_delete[i].slotoffset |=
                            (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                    response_pkt->l2_sixtop_celllist_delete[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                    response_pkt->l2_sixtop_celllist_delete[i].channeloffset |=
                            (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                    response_pkt->l2_sixtop_celllist_delete[i].isUsed = TRUE;
                    ptr += 4;
                    pktLen -= 4;
                    temp16--;
                    i++;
                }
                if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
                    cellOptions_transformed = cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX);
                } else {
                    cellOptions_transformed = cellOptions;
                }
                if (sixtop_areAvailableCellsToBeRemoved(metadata, numCells, response_pkt->l2_sixtop_celllist_delete,
                                                        &(pkt->l2_nextORpreviousHop), cellOptions_transformed) ==
                    FALSE) {
                    returnCode = IANA_6TOP_RC_CELLLIST_ERR;
                    break;
                }
                // retrieve cell list to be relocated
                i = 0;
                memset(response_pkt->l2_sixtop_celllist_add, 0, sizeof(response_pkt->l2_sixtop_celllist_add));
                while (pktLen > 0) {
                    response_pkt->l2_sixtop_celllist_add[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                    response_pkt->l2_sixtop_celllist_add[i].slotoffset |=
                            (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                    response_pkt->l2_sixtop_celllist_add[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                    response_pkt->l2_sixtop_celllist_add[i].channeloffset |=
                            (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                    response_pkt->l2_sixtop_celllist_add[i].isUsed = TRUE;
                    ptr += 4;
                    pktLen -= 4;
                    i++;
                }
                if (sixtop_areAvailableCellsToBeScheduled(metadata, numCells, response_pkt->l2_sixtop_celllist_add)) {
                    for (i = 0; i < CELLLIST_MAX_LEN; i++) {
                        if (response_pkt->l2_sixtop_celllist_add[i].isUsed) {
                            packetfunctions_reserveHeader(&response_pkt, 4);
                            response_pkt->payload[0] = (uint8_t)(
                                    response_pkt->l2_sixtop_celllist_add[i].slotoffset & 0x00FF);
                            response_pkt->payload[1] = (uint8_t)(
                                    (response_pkt->l2_sixtop_celllist_add[i].slotoffset & 0xFF00) >> 8);
                            response_pkt->payload[2] = (uint8_t)(
                                    response_pkt->l2_sixtop_celllist_add[i].channeloffset & 0x00FF);
                            response_pkt->payload[3] = (uint8_t)(
                                    (response_pkt->l2_sixtop_celllist_add[i].channeloffset & 0xFF00) >> 8);
                            response_pktLen += 4;
                           
                           
                        }
                    }
                }
                returnCode = IANA_6TOP_RC_SUCCESS;
                break;
            }
        } while (0);

       
       
        // record code, returnCode, frameID and cellOptions. They will be used when 6p repsonse senddone
        response_pkt->l2_sixtop_command = code;
        response_pkt->l2_sixtop_returnCode = returnCode;
        response_pkt->l2_sixtop_frameID = metadata;
        // revert tx and rx link option bits
        if ((cellOptions & (CELLOPTIONS_TX | CELLOPTIONS_RX)) != (CELLOPTIONS_TX | CELLOPTIONS_RX)) {
            response_pkt->l2_sixtop_cellOptions = cellOptions ^ (CELLOPTIONS_TX | CELLOPTIONS_RX);
        } else {
            response_pkt->l2_sixtop_cellOptions = cellOptions;
        }
   
        openserial_printf("sixtop packet response generated, seqnum %d", seqNum);
                                               
        // append 6p Seqnum
        packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
        *((uint8_t * )(response_pkt->payload)) = seqNum;
        response_pktLen += 1;

        // append 6p sfid
        packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
        *((uint8_t * )(response_pkt->payload)) = sixtop_vars.cb_sf_getsfid();
        response_pktLen += 1;

        // append 6p code
        packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
        *((uint8_t * )(response_pkt->payload)) = returnCode;
        response_pktLen += 1;

        // append 6p version, T(type) and  R(reserved)
        packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
        *((uint8_t * )(response_pkt->payload)) = IANA_6TOP_6P_VERSION | response_type;
        //*((uint8_t * )(response_pkt->payload)) = IANA_6TOP_6P_VERSION | IANA_6TOP_TYPE_RESPONSE;
        response_pktLen += 1;
       
        // append 6p subtype id
        packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
        *((uint8_t * )(response_pkt->payload)) = IANA_6TOP_SUBIE_ID;
        response_pktLen += 1;

        // append IETF IE header (length_groupid_type)
        packetfunctions_reserveHeader(&response_pkt, sizeof(uint16_t));
        length_groupid_type = response_pktLen;
        length_groupid_type |= (IANA_IETF_IE_GROUP_ID | IANA_IETF_IE_TYPE);
        response_pkt->payload[0] = length_groupid_type & 0xFF;
        response_pkt->payload[1] = (length_groupid_type >> 8) & 0xFF;

        // indicate IEs present
        response_pkt->l2_payloadIEpresent = TRUE;
        // record this packet as sixtop request message
        if (response_type == IANA_6TOP_TYPE_REQUEST)
           response_pkt->l2_sixtop_messageType = SIXTOP_CELL_REQUEST;
        else
           response_pkt->l2_sixtop_messageType = SIXTOP_CELL_RESPONSE; //SIXTOP_CELL_RESPONSE;

       
        sixtop_send(response_pkt);
    }

    if (type == SIXTOP_CELL_RESPONSE) {
        // this is a 6p response message

        // if the code is SUCCESS
        if (code == IANA_6TOP_RC_SUCCESS || code == IANA_6TOP_RC_EOL) {
            switch (sixtop_vars.six2six_state) {
                  
                // a reply is coming
                case SIX_STATE_WAIT_ADDRESPONSE:
                    // updates the seqnum with the tx in any case (seqnums are updated per link)
                    neighbors_updateSequenceNumber(&(pkt->l2_nextORpreviousHop));

                    // extract the list of cells from the response
                    i = 0;
                    memset(pkt->l2_sixtop_celllist_add, 0, sizeof(pkt->l2_sixtop_celllist_add));
                    while (pktLen > 0) {
                        pkt->l2_sixtop_celllist_add[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                        pkt->l2_sixtop_celllist_add[i].slotoffset |= (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                        pkt->l2_sixtop_celllist_add[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                        pkt->l2_sixtop_celllist_add[i].channeloffset |= (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                        pkt->l2_sixtop_celllist_add[i].isUsed = TRUE;
                        ptr += 4;
                        pktLen -= 4;
                        i++;
                    }
                    numCells = i;
            
                    openserial_printf("rep received from %x:%x, ongoing %x:%x\n",
                                      pkt->l2_nextORpreviousHop.addr_64b[6],
                                      pkt->l2_nextORpreviousHop.addr_64b[7],
                                      sixtop_vars.neighborOngoing3Steps.addr_64b[6],
                                      sixtop_vars.neighborOngoing3Steps.addr_64b[7]
                                       );
                  
                    //we need to send another reply in anycast before inserting the cells in the schedule
                    if (
                        (sixtop_vars.neighborOngoing3Steps.type != 0)
                        &&
                        !packetfunctions_sameAddress(&(sixtop_vars.neighborOngoing3Steps), &(pkt->l2_nextORpreviousHop))
                        ){
                       //sends now a "normal reply" to the first receiver with the complete list id
                       openserial_printf("now generates the reply to %x:%x, with %d cell(s)\n",
                                          sixtop_vars.neighborOngoing3Steps.addr_64b[6],
                                          sixtop_vars.neighborOngoing3Steps.addr_64b[7],
                                          numCells
                                          );
                       
                       // get a free packet buffer
                       response_pkt = openqueue_getFreePacketBuffer(COMPONENT_SIXTOP_RES);
                       if (response_pkt == NULL) {
                          LOG_ERROR(COMPONENT_SIXTOP_RES, ERR_NO_FREE_PACKET_BUFFER, (errorparameter_t) 0, (errorparameter_t) 0);
                          return;
                       }
                       
                       // take ownership
                       response_pkt->creator = COMPONENT_SIXTOP_RES;
                       response_pkt->owner = COMPONENT_SIXTOP_RES;
                       
                       memcpy(&(response_pkt->l2_nextORpreviousHop), &(sixtop_vars.neighborOngoing3Steps), sizeof(open_addr_t));
                       memset(response_pkt->l2_sixtop_celllist_add, 0, sizeof(pkt->l2_sixtop_celllist_add));
                       if (sixtop_areAvailableCellsToBeScheduled(0, numCells, pkt->l2_sixtop_celllist_add)) {
                          for (i = 0; i < numCells; i++) {
                             if (pkt->l2_sixtop_celllist_add[i].isUsed) {
                                packetfunctions_reserveHeader(&response_pkt, 4);
                                response_pkt->payload[0] = (uint8_t)(pkt->l2_sixtop_celllist_add[i].slotoffset & 0x00FF);
                                response_pkt->payload[1] = (uint8_t)((pkt->l2_sixtop_celllist_add[i].slotoffset & 0xFF00) >> 8);
                                response_pkt->payload[2] = (uint8_t)(pkt->l2_sixtop_celllist_add[i].channeloffset & 0x00FF);
                                response_pkt->payload[3] = (uint8_t)((pkt->l2_sixtop_celllist_add[i].channeloffset & 0xFF00) >> 8);
                                response_pktLen += 4;
                                
                                //copy the list to add them in my schedule if the response is correctly txed
                                response_pkt->l2_sixtop_celllist_add[i].slotoffset = pkt->l2_sixtop_celllist_add[i].slotoffset;
                                response_pkt->l2_sixtop_celllist_add[i].channeloffset = pkt->l2_sixtop_celllist_add[i].channeloffset;
                                response_pkt->l2_sixtop_celllist_add[i].isUsed = TRUE;
                                
                                if (response_pkt->l2_sixtop_celllist_add[i].isUsed)
                                   openserial_printf("%d / %d / %d\n",
                                                  response_pkt->l2_sixtop_celllist_add[i].slotoffset,
                                                  response_pkt->l2_sixtop_celllist_add[i].channeloffset,
                                                  response_pkt->l2_sixtop_celllist_add[i].isUsed
                                                  );
                             }
                          }
                       }
                       else
                          openserial_printf("Problem: the cells are not available in the schedule\n");
                          
                       // record code, returnCode, frameID and cellOptions. They will be used when 6p response senddone
                       response_pkt->l2_sixtop_command = IANA_6TOP_CMD_ADD;
                       response_pkt->l2_sixtop_returnCode = IANA_6TOP_RC_SUCCESS;
                       response_pkt->l2_sixtop_cellOptions = sixtop_vars.cellOptions;
                       
                       openserial_printf("rep packet generated seqnum %d\n",neighbors_getSequenceNumber(&(sixtop_vars.neighborOngoing3Steps)));
                       
                       // append 6p Seqnum
                       packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                       *((uint8_t * )(response_pkt->payload)) = neighbors_getSequenceNumber(&(sixtop_vars.neighborOngoing3Steps));
                       response_pktLen += 1;
                       
                       // append 6p sfid
                       packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                       *((uint8_t * )(response_pkt->payload)) = sixtop_vars.cb_sf_getsfid();
                       response_pktLen += 1;
                       
                       // append 6p code
                       packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                       *((uint8_t * )(response_pkt->payload)) = response_pkt->l2_sixtop_returnCode;
                       response_pktLen += 1;
                       
                       // append 6p version, T(type) and  R(reserved)
                       packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                       *((uint8_t * )(response_pkt->payload)) = IANA_6TOP_6P_VERSION | IANA_6TOP_TYPE_RESPONSE;
                       response_pktLen += 1;
                       
                       // append 6p subtype id
                       packetfunctions_reserveHeader(&response_pkt, sizeof(uint8_t));
                       *((uint8_t * )(response_pkt->payload)) = IANA_6TOP_SUBIE_ID;
                       response_pktLen += 1;
                       
                       // append IETF IE header (length_groupid_type)
                       packetfunctions_reserveHeader(&response_pkt, sizeof(uint16_t));
                       length_groupid_type = response_pktLen;
                       length_groupid_type |= (IANA_IETF_IE_GROUP_ID | IANA_IETF_IE_TYPE);
                       response_pkt->payload[0] = length_groupid_type & 0xFF;
                       response_pkt->payload[1] = (length_groupid_type >> 8) & 0xFF;
                       
                       // indicate IEs present
                       response_pkt->l2_payloadIEpresent = TRUE;
                       
                       // record this packet as sixtop request message
                       response_pkt->l2_sixtop_messageType = SIXTOP_CELL_RESPONSE;
                       
                       sixtop_send(response_pkt);
                       //stops here the function -> we will finalize the handshake when the packet will be sent
                       return;                       
                    }
                    else{
                        sixtop_addCells(
                            sixtop_vars.cb_sf_getMetadata(),      // frame id
                            pkt->l2_sixtop_celllist_add,          // celllist to be added
                            &(pkt->l2_nextORpreviousHop),         // neighbor that cells to be added to
                            &(sixtop_vars.neigbor_secondReceiver),// second receiver if needed
                            sixtop_vars.cellOptions               // cell options
                        );
                    }
                  
                    
                  break;
                case SIX_STATE_WAIT_DELETERESPONSE:
                    i = 0;
                    memset(pkt->l2_sixtop_celllist_delete, 0, sizeof(pkt->l2_sixtop_celllist_delete));
                    while (pktLen > 0) {
                        pkt->l2_sixtop_celllist_delete[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                        pkt->l2_sixtop_celllist_delete[i].slotoffset |= (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                        pkt->l2_sixtop_celllist_delete[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                        pkt->l2_sixtop_celllist_delete[i].channeloffset |=
                                (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                        pkt->l2_sixtop_celllist_delete[i].isUsed = TRUE;
                        ptr += 4;
                        pktLen -= 4;
                        i++;
                    }
                    sixtop_removeCells(
                            sixtop_vars.cb_sf_getMetadata(),
                            pkt->l2_sixtop_celllist_delete,
                            &(pkt->l2_nextORpreviousHop),
                            sixtop_vars.cellOptions
                    );
                    neighbors_updateSequenceNumber(&(pkt->l2_nextORpreviousHop));
                    break;
                case SIX_STATE_WAIT_RELOCATERESPONSE:
                    i = 0;
                    memset(pkt->l2_sixtop_celllist_add, 0, sizeof(pkt->l2_sixtop_celllist_add));
                    while (pktLen > 0) {
                        pkt->l2_sixtop_celllist_add[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                        pkt->l2_sixtop_celllist_add[i].slotoffset |= (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                        pkt->l2_sixtop_celllist_add[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                        pkt->l2_sixtop_celllist_add[i].channeloffset |= (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                        pkt->l2_sixtop_celllist_add[i].isUsed = TRUE;
                        ptr += 4;
                        pktLen -= 4;
                        i++;
                    }
                    sixtop_removeCells(
                            sixtop_vars.cb_sf_getMetadata(),
                            sixtop_vars.celllist_toDelete,
                            &(pkt->l2_nextORpreviousHop),
                            sixtop_vars.cellOptions
                    );
                    sixtop_addCells(
                            sixtop_vars.cb_sf_getMetadata(),     // frame id
                            pkt->l2_sixtop_celllist_add,  // celllist to be added
                            &(pkt->l2_nextORpreviousHop), // neighbor that cells to be added to
                            &(sixtop_vars.neigbor_secondReceiver), //second receiver if present
                            sixtop_vars.cellOptions       // cell options
                    );
                    neighbors_updateSequenceNumber(&(pkt->l2_nextORpreviousHop));
                    break;
                case SIX_STATE_WAIT_COUNTRESPONSE:
                    numCells = *((uint8_t * )(pkt->payload) + ptr);
                    numCells |= (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                    ptr += 2;
                    LOG_INFO(COMPONENT_SIXTOP, ERR_SIXTOP_COUNT,
                             (errorparameter_t) numCells,
                             (errorparameter_t) sixtop_vars.six2six_state);
                    neighbors_updateSequenceNumber(&(pkt->l2_nextORpreviousHop));
                    break;
                case SIX_STATE_WAIT_LISTRESPONSE:
                    i = 0;
                    memset(celllist_list, 0, CELLLIST_MAX_LEN * sizeof(cellInfo_ht));
                    while (pktLen > 0) {
                        celllist_list[i].slotoffset = *((uint8_t * )(pkt->payload) + ptr);
                        celllist_list[i].slotoffset |= (*((uint8_t * )(pkt->payload) + ptr + 1)) << 8;
                        celllist_list[i].channeloffset = *((uint8_t * )(pkt->payload) + ptr + 2);
                        celllist_list[i].channeloffset |= (*((uint8_t * )(pkt->payload) + ptr + 3)) << 8;
                        celllist_list[i].isUsed = TRUE;
                        ptr += 4;
                        pktLen -= 4;
                        i++;
                    }
                    // print out first two cells in the list
                    LOG_INFO(COMPONENT_SIXTOP, ERR_SIXTOP_LIST,
                             (errorparameter_t) celllist_list[0].slotoffset,
                             (errorparameter_t) celllist_list[1].slotoffset);
                    neighbors_updateSequenceNumber(&(pkt->l2_nextORpreviousHop));
                    break;
                case SIX_STATE_WAIT_CLEARRESPONSE:
                  openserial_printf("clear response wait\n");
                  
                    schedule_removeAllNegotiatedCellsToNeighbor(
                            sixtop_vars.cb_sf_getMetadata(),
                            &(pkt->l2_nextORpreviousHop)
                    );
                    neighbors_resetSequenceNumber(&(pkt->l2_nextORpreviousHop));
                    break;
                default:
                  openserial_printf("sixtop response received after the timeout from %x:%x, we discard it\n",
                                    pkt->l2_nextORpreviousHop.addr_64b[6],
                                    pkt->l2_nextORpreviousHop.addr_64b[7]);
 
                   //TODO: be careful, we may have an inconsistent schedule if we update the seqnum without handling the content of the sixtop packet
                   //neighbors_updateSequenceNumber(&(pkt->l2_nextORpreviousHop));

                    // The sixtop response arrived after 6P TIMEOUT, or it's a duplicated response. Remove 6P request if I have.
                    openqueue_remove6PrequestToNeighbor(&(pkt->l2_nextORpreviousHop));
                    break;
            }
        } else {
            sixtop_vars.cb_sf_handleRCError(code, &(pkt->l2_nextORpreviousHop));
        }

        if (code == IANA_6TOP_RC_SUCCESS) {
           LOG_SUCCESS(COMPONENT_SIXTOP, ERR_SIXTOP_RETURNCODE,
                       (errorparameter_t)code,
                       (errorparameter_t)
                       sixtop_vars.six2six_state);
        } else if (code == IANA_6TOP_RC_EOL || code == IANA_6TOP_RC_BUSY || code == IANA_6TOP_RC_LOCKED) {
           LOG_SUCCESS(COMPONENT_SIXTOP, ERR_SIXTOP_RETURNCODE,
                    (errorparameter_t) code,
                    (errorparameter_t) sixtop_vars.six2six_state);
        } else if (code == IANA_6TOP_RC_RESET){
           LOG_SUCCESS(COMPONENT_SIXTOP, ERR_SIXTOP_RETURNCODE,
                   (errorparameter_t) code,
                   (errorparameter_t) sixtop_vars.six2six_state);
           
        } else {
            LOG_ERROR(COMPONENT_SIXTOP, ERR_SIXTOP_RETURNCODE,
                    (errorparameter_t) code,
                    (errorparameter_t) sixtop_vars.six2six_state);
        }

        memset(&sixtop_vars.neighborToClearCells, 0, sizeof(open_addr_t));
        memset(&sixtop_vars.neighborOngoing3Steps, 0, sizeof(open_addr_t));
        sixtop_setState(SIX_STATE_IDLE);
        opentimers_cancel(sixtop_vars.timeoutTimerId);
    }
}


//======= helper functions


/**
\brief Changes the state in the FSM.

This function changes the state in the Finite State Machine.
A single place to change it for debuging.
 
\param[in] state  The novel sixtop state

*/
void sixtop_setState(uint8_t state){
   LOG_SUCCESS(COMPONENT_SIXTOP, ERR_SIXTOP_CHANGESTATE, sixtop_vars.six2six_state, state);
   
   sixtop_vars.six2six_state = state;
}



bool sixtop_addCells(
        uint8_t slotframeID,
        cellInfo_ht *cellList,
        open_addr_t *previousHop,
        open_addr_t *neighbor2,
        uint8_t cellOptions
) {
    uint8_t i;
    bool isShared, isAnycast;
    open_addr_t temp_neighbor;
    cellType_t type;
    bool hasCellsAdded;
    uint8_t priority;
   
    // translate cellOptions to cell type
    if (cellOptions == CELLOPTIONS_TX) {
        type = CELLTYPE_TX;
        isShared = FALSE;
        isAnycast = FALSE;
        priority = 0;
    }
    else if (cellOptions == CELLOPTIONS_RX) {
        type = CELLTYPE_RX;
        isShared = FALSE;
        isAnycast = FALSE;
        priority = 0;
    }
    else if (cellOptions == (CELLOPTIONS_TX | CELLOPTIONS_ANYCAST)){
        type = CELLTYPE_TX;
        isShared = FALSE;
        isAnycast = TRUE;
        priority = 0;
        openserial_printf("CELLTYPE_TX_ANYCAST PRIO=%d\n", priority);
    }
    else if (cellOptions == (CELLOPTIONS_TX | CELLOPTIONS_ANYCAST | CELLOPTIONS_PRIORITY)){
        type = CELLTYPE_TX;
        isShared = FALSE;
        isAnycast = TRUE;
        priority = 1;
        openserial_printf("CELLTYPE_TX_ANYCAST PRIO=%d\n", priority);
    }
    else if (cellOptions == (CELLOPTIONS_RX | CELLOPTIONS_ANYCAST)){
        type = CELLTYPE_RX;
        isShared = FALSE;
        isAnycast = TRUE;
        priority = 0;
        openserial_printf("CELLTYPE_RX_ANYCAST PRIO=%d\n", priority);
    }
    else if (cellOptions == (CELLOPTIONS_RX | CELLOPTIONS_ANYCAST | CELLOPTIONS_PRIORITY)){
        type = CELLTYPE_RX;
        isShared = FALSE;
        isAnycast = TRUE;
        priority = 1;
        openserial_printf("CELLTYPE_RX_ANYCAST PRIO=%d\n", priority);
    }
    else if (cellOptions == (CELLOPTIONS_TX | CELLOPTIONS_RX | CELLOPTIONS_SHARED)) {
        type = CELLTYPE_TXRX;
        isShared = TRUE;
        isAnycast = FALSE;
        priority = 0;
    }
    else{
       LOG_ERROR(COMPONENT_SIXTOP, ERR_BAD_CELLOPTIONS, cellOptions, 2);
       return(FALSE);
    }

   memcpy(&temp_neighbor, previousHop, sizeof(open_addr_t));

   openserial_printf("SCHEDULE, insertion of cells (timeslot / channel offset), type=%d, isShared=%d, isAnycast=%d:", type, isShared, isAnycast);
    hasCellsAdded = FALSE;
    // add cells to schedule
    for (i = 0; i < CELLLIST_MAX_LEN; i++) {
        openserial_printf("%d: %d / %d", cellList[i].isUsed, cellList[i].slotoffset, cellList[i].channeloffset);
        if (cellList[i].isUsed) {
            hasCellsAdded = TRUE;
            schedule_addActiveSlot(cellList[i].slotoffset, type, isShared, isAnycast, FALSE, cellList[i].channeloffset, priority, &temp_neighbor, neighbor2);
        }
    }
    return hasCellsAdded;
}

bool sixtop_removeCells(
        uint8_t slotframeID,
        cellInfo_ht *cellList,
        open_addr_t *previousHop,
        uint8_t cellOptions
) {
    uint8_t i;
    bool isShared;
    open_addr_t temp_neighbor;
    cellType_t type;
    bool hasCellsRemoved;

    // translate cellOptions to cell type
    if (cellOptions == CELLOPTIONS_TX) {
        type = CELLTYPE_TX;
        isShared = FALSE;
    }
    if (cellOptions == CELLOPTIONS_RX) {
        type = CELLTYPE_RX;
        isShared = FALSE;
    }
    if (cellOptions == (CELLOPTIONS_TX | CELLOPTIONS_RX | CELLOPTIONS_SHARED)) {
        type = CELLTYPE_TXRX;
        isShared = TRUE;
    }

    memcpy(&temp_neighbor, previousHop, sizeof(open_addr_t));

    hasCellsRemoved = FALSE;
    // delete cells from schedule
    for (i = 0; i < CELLLIST_MAX_LEN; i++) {
        if (cellList[i].isUsed) {
            hasCellsRemoved = TRUE;
            schedule_removeActiveSlot(
                    cellList[i].slotoffset,
                    type,
                    isShared,
                    &temp_neighbor
            );
        }
    }

    return hasCellsRemoved;
}

bool sixtop_areAvailableCellsToBeScheduled(
        uint8_t frameID,
        uint8_t numOfCells,
        cellInfo_ht *cellList
) {
    uint8_t i;
    uint8_t numbOfavailableCells;
    bool available;

    i = 0;
    numbOfavailableCells = 0;
    available = FALSE;

    if (numOfCells == 0 || numOfCells > CELLLIST_MAX_LEN) {
        // log wrong parameter error TODO

        available = FALSE;
    } else {
        do {
            if (schedule_isSlotOffsetAvailable(cellList[i].slotoffset) == TRUE) {
                numbOfavailableCells++;
            } else {
                // mark the cell
                cellList[i].isUsed = FALSE;
            }
            i++;
        } while (i < CELLLIST_MAX_LEN && numbOfavailableCells != numOfCells);

        if (numbOfavailableCells > 0) {
            // there are more than one cell can be added.
            // the rest cells in the list will not be used
            while (i < CELLLIST_MAX_LEN) {
                cellList[i].isUsed = FALSE;
                i++;
            }
            available = TRUE;
        } else {
            // No cell in the list is able to be added
            available = FALSE;
        }
    }
    return available;
}

bool sixtop_areAvailableCellsToBeRemoved(
        uint8_t frameID,
        uint8_t numOfCells,
        cellInfo_ht *cellList,
        open_addr_t *neighbor,
        uint8_t cellOptions
) {
    uint8_t i;
    uint8_t numOfavailableCells;
    bool available;
    slotinfo_element_t info;
    cellType_t type;
    open_addr_t anycastAddr;

    i = 0;
    numOfavailableCells = 0;
    available = TRUE;

    // translate cellOptions to cell type
    if (cellOptions == CELLOPTIONS_TX) {
        type = CELLTYPE_TX;
    }
    if (cellOptions == CELLOPTIONS_RX) {
        type = CELLTYPE_RX;
    }
    if (cellOptions == (CELLOPTIONS_TX | CELLOPTIONS_RX | CELLOPTIONS_SHARED)) {
        type = CELLTYPE_TXRX;
        memset(&anycastAddr, 0, sizeof(open_addr_t));
        anycastAddr.type = ADDR_ANYCAST;
    }

    if (numOfCells == 0 || numOfCells > CELLLIST_MAX_LEN) {
        // log wrong parameter error TODO
        available = FALSE;
    } else {
        do {
            if (cellList[i].isUsed) {
                memset(&info, 0, sizeof(slotinfo_element_t));
                if (type == CELLTYPE_TXRX) {
                    schedule_getSlotInfo(cellList[i].slotoffset, &info);
                } else {
                    schedule_getSlotInfo(cellList[i].slotoffset, &info);
                }
                if (info.link_type != type) {
                    available = FALSE;
                    break;
                } else {
                    numOfavailableCells++;
                }
            }
            i++;
        } while (i < CELLLIST_MAX_LEN && numOfavailableCells < numOfCells);

        if (numOfavailableCells == numOfCells && available == TRUE) {
            //the rest link will not be scheduled, mark them as off type
            while (i < CELLLIST_MAX_LEN) {
                cellList[i].isUsed = FALSE;
                i++;
            }
        } else {
            // local schedule can't satisfy the bandwidth of cell request
            available = FALSE;
        }
    }
    return available;
}
