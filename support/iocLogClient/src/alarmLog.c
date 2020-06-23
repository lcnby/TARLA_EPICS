/*******************************************************************************

Project:
    ioc Logger for EPICS

File:
    alarmLog.c

Description:
    logEvent is set as recGblAlarmHook and places the alarm event into the alarm ring buffer.
    When a message is in the ring buffer, alarmMessageTask sends it to the alarm server over
    the network. 
    This file is separated from logAlarms.c which is implemented by Bernd Schoeneburg,
    and it is ported to osi.

Author:
    Bernd Schoeneburg & Gongfa Liu

Created:
    16 February 2009

Modification:
2006-10-24	Sbg	Initial implementation
2009-02-23      Liu     separate from logAlarms.c and port to osi..

(c) 2006,2007,2008,2009 DESY

$Id: alarmLog.c,v 1.3 2009/07/20 15:52:17 schoeneb Exp $
*******************************************************************************/

/* C */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* EPICS */
#include <alarmString.h>	/* alarm strings */
#include <epicsEvent.h>		/* epicsEvent */
#include <epicsMutex.h>		/* epicsMutex */
#include <epicsThread.h>	/* epicsThread */
#include <epicsTime.h>		/* epicsTimeStamp */
#include <dbAccess.h>		/* DBADDR */
#include <recGbl.h>		/* Hook */
#include <dbStaticLib.h>	/* DBENTRY */
#include <bucketLib.h>		/* EPICS hash facility */
#include <recSup.h>		/* rset */
#include <errlog.h>		/* errlogPrintf() */
#include <epicsExport.h>	/* epicsExportAddress() */

#include "iocLogClient.h"

/* Ring buffer size relative to number of records in database */
#define RING_NORMAL_MULTI	0.8		/* ring has 80% of rec */
#define RING_OVER_DIVI		10		/* over has rec + rec/10 */

#define VAL_LENGTH		8		/* bytes to save of records VAL field */
#define DESC_LENGTH		29		/* max length of DESC field */

#define ALARM_MSG_TASK_NUM	5		/* alarm task number */

#define EXCLUDED_RECORD_TYPES   "arch "
#define BITWISE_RECORD_TYPES    "mbbiDirect mbboDirect"

#define RINGPOS(p)		((((p) + ringSize) - pRingRead) % ringSize)
#define LOCK_BUCK		epicsMutexLock (bucketLock);
#define UNLOCK_BUCK		epicsMutexUnlock (bucketLock);
#define LOCK_RING		epicsMutexLock (ringLock);
#define UNLOCK_RING		epicsMutexUnlock (ringLock);
#define LOCK_RING_W		epicsMutexLock (ringWriteLock);
#define UNLOCK_RING_W		epicsMutexUnlock (ringWriteLock);

typedef enum {
    mt_Event,
    mt_Status
} msgType_t;

typedef struct {
    dbCommon		*pdbc;
    char		value[VAL_LENGTH];
    unsigned short	ostat;
    unsigned short	osevr;
    unsigned short	nstat;
    unsigned short	nsevr;
    epicsTimeStamp		stamp;
    msgType_t		msgType;
} msg_t;

typedef struct {
    msg_t		msg;
    enum {
	hs_Non,
	hs_Passive,
	hs_Active
    }			hashStatus;
    unsigned short	msevr;
    unsigned short	overwritten;
} msgBuffer_t;

/* only static functions are declared here, the shared funtions are declared in iocLogClient.h */
static int  logEvent (dbCommon *, unsigned short, unsigned short); /* hook */
static int  alarmMessageTask (int);		/* process */
static int  ringInsert (msg_t *);		/* msg into ring */
static int  ringRead (msgBuffer_t *);		/* msg from ring */
static int  msgToText (char *, short, msgBuffer_t *, unsigned long); /* generate text message */
static void strChrRep (char *, char, char);	/* replace a character in a string */
static void copyValue (dbCommon *, msg_t *);	/* copy VAL field of record into msg */
static void cleanup (int);                      /* cleanup if return ERROR */

extern char		messageHeader[MESSAGE_HEADER_LENGTH];

int	alarmLogDisable = 0;
epicsExportAddress(int,alarmLogDisable);

static int		initialized = FALSE;
static int		ringSize, ringHighWater, alarmMsgLost, alarmMsgLostTotal;
static msgBuffer_t	*pRingBottom, *pRingTop;
static msgBuffer_t	*pRingRead, *pRingWrite;
static epicsEventId	wakeupEvent;
static epicsMutexId	ringLock;
static epicsMutexId	ringWriteLock;
static epicsMutexId	bucketLock;
static BUCKET		*pbuck;
static epicsThreadId	tid[ALARM_MSG_TASK_NUM];
static int		tidStop[ALARM_MSG_TASK_NUM];


/*******************************************************************************
Routine:
    alarmLogInit

Purpose:
    Initialize parameters and set AlarmHook pointer

Description:

Returns:
    OK, or ERROR

*/

int alarmLogInit ()
{
    DBENTRY     dbentry;
    DBENTRY     *pdbentry=&dbentry;
    long        status;
    int		numRecords=0;
    int		normalRangeSize, overRangeSize;
    char	string[32];
    int		i;

    if (initialized) {
	errlogPrintf ("alarmLogInit: already initialized\n");
	return OK;
    }

    if(!pdbbase) {
	errlogPrintf ("alarmLogInit: No database has been loaded\n");
        return OK;
    }

    if (alarmLogDisable) {
	errlogPrintf ("alarmLogInit: alarmLog is disabled\n");
	return OK;
    }

    wakeupEvent = epicsEventCreate (epicsEventEmpty);
    if (!wakeupEvent) {
	errlogPrintf ("alarmLogInit: Reader wakeup semaphore could not be created\n");
        return ERROR;
    }

    bucketLock = epicsMutexCreate();
    if (!bucketLock) {
	errlogPrintf ("alarmLogInit: Hash facility mutex could not be created\n");
	cleanup (1);
        return ERROR;
    }

    ringLock = epicsMutexCreate();
    if (!ringLock) {
	errlogPrintf ("alarmLogInit: Ring r/w mutex could not be created\n");
	cleanup (2);
        return ERROR;
    }

    ringWriteLock = epicsMutexCreate();
    if (!ringWriteLock) {
	errlogPrintf ("alarmLogInit: Ring (write) mutex could not be created\n");
	cleanup (3);
        return ERROR;
    }

    dbInitEntry(pdbbase,pdbentry);

    status = dbFirstRecordType(pdbentry);
    while(!status) {
	int	numRecordsType;

	numRecordsType = dbGetNRecords(pdbentry);
	DEBUG2(4,"There are %d records of type %s\n",
	  numRecordsType, dbGetRecordTypeName(pdbentry))
	numRecords += numRecordsType;
	status = dbNextRecordType(pdbentry);
    }
    dbFinishEntry(pdbentry);

    normalRangeSize = (int)(numRecords * RING_NORMAL_MULTI) + 1;
    overRangeSize = numRecords + numRecords / RING_OVER_DIVI + 1;

    ringSize = normalRangeSize + overRangeSize;

    pRingBottom = (msgBuffer_t *)calloc (ringSize, sizeof(msgBuffer_t));
    if (!pRingBottom) {
	errlogPrintf ("alarmLogInit: Ring buffer could not be created\n");
	cleanup (4);
	return ERROR;
    }
    pRingTop = pRingBottom + ringSize;
    DEBUG2(2,"pRingBottom:%lu  pRingTop:%lu\n",
      (unsigned long)pRingBottom, (unsigned long)pRingTop)

    ringHighWater = normalRangeSize;
    DEBUG2(2,"Ring buffer size:%d  highwater:%d\n",ringSize,ringHighWater)
    pRingRead = pRingBottom;
    pRingWrite = pRingBottom;
    alarmMsgLost = 0;
    alarmMsgLostTotal = 0;

    pbuck = bucketCreate (numRecords);
    if (!pbuck) {
	errlogPrintf ("alarmLogInit: Hash table could not be initalized\n");
	cleanup (5);
	return ERROR;
    }

    queueAllRecords();

    /* spawn alarm message tasks */
    for (i=0; i<ALARM_MSG_TASK_NUM; i++) {
	tidStop[i] = FALSE;
	sprintf (string, "Al'Msg%d", i+1);
	tid[i] = epicsThreadCreate (string, epicsThreadPriorityMedium, 
					epicsThreadGetStackSize (epicsThreadStackSmall),
					(EPICSTHREADFUNC)alarmMessageTask,i+1);

	if (!tid[i]) {
	    errlogPrintf ("alarmLogInit: Message task %d could not be spawned\n", i);
	    cleanup (6);
	    while (i > 0) tidStop[--i] = TRUE;
	    return ERROR;
	}
    }

    recGblAlarmHook = (RECGBL_ALARM_HOOK_ROUTINE)logEvent;
    DEBUG1(3,"recGblAlarmHook = 0x%lx\n",(unsigned long)recGblAlarmHook)

#ifdef DEBUG
    printf("alarmLogInit: alarmLog started\n");
#endif
    initialized = TRUE;
    return OK;
}


/*******************************************************************************

Routine:
    alarmLogShutdown

Purpose:
    shutdown alarmLog 

Description:

*/

int alarmLogShutdown ()
{
    int i;

    if (!initialized) {
	errlogPrintf ("alarmLogShutdown: alarmLog is not initialized\n");
	return ERROR;
    }

    recGblAlarmHook = NULL;

    for (i=0; i<ALARM_MSG_TASK_NUM; i++) { 
	tidStop[i] = TRUE;
	while (tid[i]) epicsThreadSleep(0.02);
    }
    printf ("alarmLogShutdown: alarmLog tasks stopped...\n");

    cleanup (6);
    printf ("alarmLogShutdown: Recources freed - alarmLog stopped.\n");

    initialized = FALSE;
    return OK;
}


/*******************************************************************************

Routine:
    alarmLogReport

Purpose:
    Report status of alarmLog

Description:

Returns:
    OK

*/

int alarmLogReport (int interest) 
{
    if (!initialized) {
	printf ("alarmLogReport: alarmLog is not initialized!\n");
	return OK;
    }

    printf ("alarmLogReport! use level parameter of [0..4].....\n");

    printf ("alarmLog: Ring buffer usage: %ld messages are waiting in the ring buffer\n",
      (long)RINGPOS(pRingWrite));
    printf ("alarmLog: %d messages are lost currently\n", alarmMsgLost);
    printf ("alarmLog: %d messages have been lost since startup\n", alarmMsgLostTotal);

    if (interest > 1) {
	printf ("alarmLog: Ring buffer size is %d messages\n", ringSize);
	printf ("alarmLog: Ring base area is %d messages, extention is %d\n",
	  ringHighWater, ringSize - ringHighWater);
	printf ("alarmLog: alarmLogDisable is %s\n", alarmLogDisable ? "TRUE" : "FALSE");
    }

    if (interest > 3) {
	printf ("alarmLog: Ring address is 0x%lx\n", (unsigned long)pRingBottom);
	printf ("alarmLog: Read pointer: 0x%lx  Write pointer: 0x%lx\n",
	  (unsigned long)pRingRead, (unsigned long)pRingWrite);
    }

    if (interest > 2) {
	printf ("alarmLog: The hash facility to find messages in the ring buffer:\n");
	if (pbuck)
	    bucketShow (pbuck);
	else
	    printf ("alarmLog: Hash table not created\n");
    }
    printf ("\n");
    return OK;
}


/*******************************************************************************

Routine:
    cleanup

Purpose:
    Release resources if startup fails

Description:
*/

static void cleanup (int step)
{
    switch (step) {
	case 6: bucketFree (pbuck);
	case 5: free ((void *)pRingBottom);
	case 4: epicsMutexDestroy (ringWriteLock);
	case 3: epicsMutexDestroy (ringLock);
	case 2: epicsMutexDestroy (bucketLock);
	case 1: epicsEventDestroy (wakeupEvent);
    }
}


/*******************************************************************************

Routine:
    queueRecord

Purpose:
    Put status message for one record into the ring buffer

Description:
    Use dbFindRecord to find the record and place the alarm status
    into the ring buffer.

*/

int queueRecord (const char *precName)
{
    DBENTRY	dbentry;
    DBENTRY	*pdbentry=&dbentry;
    dbCommon	*pdbc;
    msg_t	msg;
    int		locked=FALSE;

    if (alarmLogDisable) return OK;

    dbInitEntry (pdbbase, pdbentry);
    if (dbFindRecord (pdbentry, precName) != 0) {
	dbFinishEntry (pdbentry);
	return ERROR;
    }
    pdbc = dbentry.precnode->precord;
    dbFinishEntry (pdbentry);

    DEBUG2(4,"queueRecord: Name: %s ptr: 0x%x\n",precName,(unsigned)pdbc)

    msg.pdbc = pdbc;

    if (pdbc->lset) {
	dbScanLock (pdbc);
	locked = TRUE;
    }
    copyValue (pdbc, &msg);
    msg.osevr = pdbc->sevr;
    msg.ostat = pdbc->stat;
    msg.nsevr = pdbc->sevr;
    msg.nstat = pdbc->stat;

    if (locked) dbScanUnlock (pdbc);

    tsLocalTime (&msg.stamp);
    msg.msgType = mt_Status;

    ringInsert (&msg);

    return OK;
}


/*******************************************************************************
Routine:
    queueAllRecords

Purpose:
    Put status messages of all records into the ring buffer

Description:
    Go through the database and create status messages for all records of all
    record types. Status messages are normally queued in the ring buffer. They
    do not overwrite event messages.

*/

void queueAllRecords ()
{
    DBENTRY     dbentry;
    DBENTRY     *pdbentry=&dbentry;
    dbCommon	*pdbc;
    long        status;

    if (alarmLogDisable) return;

    dbInitEntry(pdbbase,pdbentry);
    status = dbFirstRecordType(pdbentry);
    while(!status) {
	if (!strstr (EXCLUDED_RECORD_TYPES, pdbentry->precordType->name)) {
	    status = dbFirstRecord(pdbentry);
	    while(!status) {
		msg_t msg;
		int locked;

		pdbc = pdbentry->precnode->precord;
		DEBUG2(4,"queueAllRecords:Name: %s ptr: 0x%x\n",
		  dbGetRecordName(pdbentry),(unsigned)pdbc)

		msg.pdbc = pdbc;

		if (pdbc->lset) {
		    dbScanLock (pdbc);
		    locked = TRUE;
		}
		else
		    locked = FALSE;

		copyValue (pdbc, &msg);
		msg.osevr = pdbc->sevr;
		msg.ostat = pdbc->stat;
		msg.nsevr = pdbc->sevr;
		msg.nstat = pdbc->stat;

		if (locked) dbScanUnlock (pdbc);

		tsLocalTime (&msg.stamp);
		msg.msgType = mt_Status;

		ringInsert (&msg);

		status = dbNextRecord(pdbentry);
	    }
	}
	status = dbNextRecordType(pdbentry);
    }
    dbFinishEntry(pdbentry);
}


/*******************************************************************************

Routine:
    logEvent

Purpose:
    Log a new alarm event

Description:
    This function is called by the record support. It places the
    alarm event into the alarm ring buffer. If this fails, the function
    returns ERROR and the message is lost (this does not happen).

Returns:
    int   

Example:
    logEvent (precord, sevr, stat);

*/

static int logEvent (
    dbCommon *pdbc,
    unsigned short osevr,
    unsigned short ostat )
{
    msg_t msg;
    int status;

    if (alarmLogDisable) return OK;
 
    if (strstr (EXCLUDED_RECORD_TYPES, pdbc->rdes->name))
	return OK;	/* do not report about some records */

    msg.pdbc = pdbc;

    copyValue (pdbc, &msg);

    msg.osevr = osevr;
    msg.ostat = ostat;
    msg.nsevr = pdbc->sevr;
    msg.nstat = pdbc->stat;
    msg.stamp = pdbc->time;
    msg.msgType = mt_Event;

    status = ringInsert (&msg);
    DEBUG1(3,"logEvent: ringInsert returned %d\n",status)

    return status;
}


/*******************************************************************************

Routine:
    alarmMessageTask

Purpose:
    Task sending messages to the alarm servers

Description:
    When a message is in the ring buffer, it sends it to the alarm server
    over the network.
    When the IOC has just started, the task waits some time to allow the
    records to get out of udf-alarm. Status message in the ring buffer will
    be overwritten by the udf-to-normal-events during this time.

*/

static int alarmMessageTask (int taskIndex)
{
    msgBuffer_t		msgBuffer;
    char		messageText[MAX_MESSAGE_LENGTH+1];
    char		*pmessage;
    short		messageSize;
    taskPrivate_t	private;

    strcpy (messageText, messageHeader);
    pmessage = messageText + strlen (messageText);
    messageSize = MAX_MESSAGE_LENGTH - strlen (messageHeader);

    /* create mutex for private manipulation */
    private.privateLock = epicsMutexCreate ();

    /* create semaphore for reply */
    private.replyEvent = epicsEventCreate (epicsEventEmpty);

    private.serverId = NO_SERVER;  /* -1 */
    taskPrivateTab[taskIndex] = &private;
    
    epicsThreadSleep (secInitWait);

    while (tidStop[taskIndex-1] == FALSE) {
      unsigned long   myMsgId;
      int nmsg;

      if (epicsEventTryWait (wakeupEvent) == epicsEventWaitOK) {
	nmsg = ringRead (&msgBuffer);
	if (nmsg > 0) {	/* msg to send */

	    if (nmsg > 1)		/* more messages to send? */
		epicsEventSignal (wakeupEvent);	/* wake up other msgTasks */

	    /* generate text message from message structure */
	    myMsgId = getMessageId();
	    DEBUG2(5,"msgTask %d sending Message %ld\n",taskIndex,myMsgId);

	    if (msgToText (pmessage, messageSize, &msgBuffer, myMsgId) == ERROR) {
		if (messageSize >= 40)
		    sprintf (pmessage, "TYPE=sysMsg;TEXT=Error - Message too long;");
		else {
		    errlogPrintf (
		      "Message task %d terminated. Message buffer too small\n",
		      taskIndex);
		    return ERROR;
		}
	    }

#ifdef DEBUG
	    if(iocLogClientDebug>=6) errlogPrintf ("Task %d: %s\n", taskIndex, messageText);
#endif

	    while (1) {		/* send message again until success */
		volatile int serverSendTo;

		serverSendTo = serverSelected;	/* local copy */

		if (serverSendTo >= 0) {
		    if (sendMessageToServer (
		      messageText, serverSendTo, myMsgId, &private) == OK) {
			serverStatus[serverSendTo].secMsg = logTimeGetCurrentDouble();
			break; /* message sent! */
		    }
		    /* error in transmission, retry after short delay */
#ifdef DEBUG
		    printf ("retry: %s\n",messageText);
#endif
		    epicsThreadSleep (secMsgRetryWait);
		}
		else {
		    /* no server is selected, wait until next beacon period */
		    epicsThreadSleep (secBeaconPeriod);
		}
	    } /* message sent */
	} /* while messages in ringbuffer */
      } else { /* no signal */
	epicsThreadSleep (0.02);
      }
    } /* forever */

    tid[taskIndex-1] = NULL;
    epicsEventDestroy (private.replyEvent);
    epicsMutexDestroy (private.privateLock);
    return OK; 
}

/*******************************************************************************

Routine:
    msgToText

Purpose:
    Take a message from the ring buffer and create the message text.

Description:

Retuns:
    number of characters in text or ERROR (no space etc.)

*/

static int msgToText (char *ptext, short size, msgBuffer_t *pmsgBuf, unsigned long Id)
{
    msg_t		*pmsg = &pmsgBuf->msg;
    int			nchar;
    char		textBuffer[32];
    dbRecordType	*prdes = pmsg->pdbc->rdes;
    char		sval[MAX_STRING_SIZE+1];
    char		*pval;
    RECSUPFUN		get_enum_str;

    if (15 > size) return ERROR;
    nchar = sprintf(ptext, "ID=%lu;", Id);

    if (nchar + 30 > size) return ERROR;
    nchar += sprintf(ptext+nchar, "APPLICATION-ID=alarmLog;");

    if (nchar + 25 > size) return ERROR;
    stampToText (&pmsg->stamp, textBuffer);
    switch (pmsg->msgType) {
	case mt_Event:
	    nchar += sprintf(ptext+nchar, "TYPE=event;");
	    nchar += sprintf(ptext+nchar, "EVENTTIME=%s;", textBuffer);
	    break;
	case mt_Status:
	    nchar += sprintf(ptext+nchar, "TYPE=status;");
	    nchar += sprintf(ptext+nchar, "EVENTTIME=%s;", textBuffer);
	    break;
	default:
	    nchar += sprintf(ptext+nchar, "TYPE=unknown;");
	    nchar += sprintf(ptext+nchar, "EVENTTIME=%s;", textBuffer);
    }

    if (nchar + 35 > size) return ERROR;
    nchar += sprintf(ptext+nchar, "NAME=%.29s;",pmsg->pdbc->name);

    if (nchar + 35 > size) return ERROR;
    strncpy (sval, pmsg->pdbc->desc, DESC_LENGTH);
    sval[DESC_LENGTH] = '\0';
    strChrRep (sval,  ';', ',');
    nchar += sprintf(ptext+nchar, "TEXT=%s;", sval);

    if (prdes && prdes->pvalFldDes && (
        (prdes->pvalFldDes->size <= VAL_LENGTH) ||
        (prdes->pvalFldDes->field_type == DBF_STRING)) ) {

	*sval = '\0';
	pval = pmsg->value;
	switch (prdes->pvalFldDes->field_type) {
	    case DBF_FLOAT:
		sprintf (sval, "%g", *((float *)pval));
		break;
            case DBF_DOUBLE:
		sprintf (sval, "%g", *((double *)pval));
		break;
	    case DBF_STRING:
		strncpy (sval, pval, VAL_LENGTH);
		sval[VAL_LENGTH] = '\0';
		strChrRep (sval, ';', ',');
		break;
	    case DBF_CHAR:
		sprintf (sval, "%d", *pval);
		break;
	    case DBF_UCHAR:
		sprintf (sval, "%u", *((unsigned char *)pval));
		break;
	    case DBF_SHORT:
		sprintf (sval, "%d", *((short *)pval));
		break;
	    case DBF_USHORT:
		if (strstr (BITWISE_RECORD_TYPES, prdes->name)) {
		    register int i;
		    register unsigned short val_short = *((unsigned short *)pval);
		    char *p = sval;

		    for (i=0; i<16; i++, p++) {
			*p = (val_short & 0x8000) ? '1' : '0';
			val_short <<= 1;
			if (i==3 || i==7 || i==11)
			    *(++p) = ' ';		/* extra space after 4 digits */
		    }
		    *p = '\0';
		}
		else
		    sprintf (sval, "%u", *((unsigned short *)pval));
		break;
	    case DBF_LONG:
		sprintf (sval, "%ld", *((long *)pval));
		break;
	    case DBF_ULONG:
		sprintf (sval, "%lu", *((unsigned long *)pval));
		break;
	    case DBF_ENUM:
		get_enum_str = pmsg->pdbc->rset->get_enum_str;
		if (get_enum_str) {
		    DBADDR addr;
		    addr.precord = pmsg->pdbc;
		    addr.pfldDes = (void *)prdes->pvalFldDes;
		    addr.pfield = (void *)pval;
		    (*get_enum_str)(&addr, sval);
		    strChrRep (sval, ';', ',');
		}
		if (!*sval)
		    sprintf (sval, "%u", *((unsigned short *)pval));
		break;
	    default:
		break;
	} /* switch */
	if (*sval && (nchar + strlen(sval) + 6 <= size))
	    nchar += sprintf(ptext+nchar, "VALUE=%s;", sval);
    }
    if (pmsg->msgType == mt_Event) {
	if (nchar + 44 > size) return ERROR;
	nchar += sprintf(ptext+nchar, "SEVERITY-OLD=%.8s;STATUS-OLD=%.8s;",
	  alarmSeverityString[pmsg->osevr],
	  alarmStatusString[pmsg->ostat]);
    }
    if (nchar + 34 > size) return ERROR;
    nchar += sprintf(ptext+nchar, "SEVERITY=%.8s;STATUS=%.8s;",
      alarmSeverityString[pmsg->nsevr],
      alarmStatusString[pmsg->nstat]);
    if (pmsgBuf->overwritten) {
	if (nchar + 41 > size) return ERROR;
	nchar += sprintf(ptext+nchar, "OVERWRITES=%d;SEVERITY-MAX=%.8s;",
	  pmsgBuf->overwritten,
	  alarmSeverityString[pmsgBuf->msevr]);
    }
    if (alarmMsgLost) {
	if (nchar + 17 > size) return ERROR;
	LOCK_RING_W
	nchar += sprintf(ptext+nchar, "LOST=%d;", alarmMsgLost);
	alarmMsgLost = 0;
	UNLOCK_RING_W
    }

    return nchar;
}


/*******************************************************************************

Routine:
    ringInsert

Purpose:
    Log a new alarm event

Description:
    This function is called by logEvent and logStatus. It tries to
    place the alarm event into the alarm ring buffer. This should
    work always if the ring buffer has at least as many places above
    its highwater mark as records in the database.
    If nevertheless insertion fails, a counter is incremented counting
    how many entries are lost. When the ring buffer is filled over
    its highwater mark, the message can overwrite an existing message
    of the same record in the 'upper' part of the ring. This happens
    if the records message is already in this part. Status messages
    in the ring can be overwritten wherever they are. Otherwise the
    new message is normally written to the next free position.
    To find a record in the ring a hash table is used.
    Locking: Writing to a new ring position can be done simultanously
    while reading but not while a second writer is active. If over-
    writing is nessecary it must be locked against reading as well.
    Third lock mechanism is needed for the bucket (hash) facility.

Returns:
    OK or ERROR if the ring buffer overflows.   

Example:
    ringInsert (&myNewMsg);

*/

static int ringInsert (msg_t *pmsg)
{
    msgBuffer_t *prng;

    LOCK_RING_W
    LOCK_RING
    LOCK_BUCK
    prng = (msgBuffer_t *)bucketLookupItemPointerId
      (pbuck, (void * const *)&pmsg->pdbc);
    UNLOCK_BUCK

    if (prng && prng->hashStatus == hs_Active) {/* hash entry exist	*/
	prng->msg = *pmsg;			/* -> overwrite!	*/
	if (pmsg->nsevr > prng->msevr)
	    prng->msevr = pmsg->nsevr;

        prng->overwritten++;

	/* if event and low ring position do not overwrite in future */
	if (prng->msg.msgType == mt_Event && RINGPOS(prng) <= ringHighWater)
	    prng->hashStatus = hs_Passive;

	UNLOCK_RING
    }
    else {					/* hash entry doesn't exist */
	msgBuffer_t *pnext;			/* ... or not active	*/

	UNLOCK_RING		/* we are not accessing the read position */
	pnext = pRingWrite + 1;			/* prepare next write pos */
	if (pnext >= pRingTop)
	    pnext = pRingBottom;
	if (pnext == pRingRead) {		/* do not write to read pos */
	    alarmMsgLost++;				/* msg not placed into ring! */
	    alarmMsgLostTotal++;
	    UNLOCK_RING_W
	    return ERROR;
	}
	pRingWrite->msg = *pmsg;		/* write message into ring */
	pRingWrite->msevr = pmsg->nsevr;
	pRingWrite->overwritten = 0;

	if (RINGPOS(pRingWrite) > ringHighWater ||
	  pmsg->msgType == mt_Status) {		/* free for overwrite */

	    pRingWrite->hashStatus = hs_Active;
	    if (prng) {
		/* REPLACE HASH TABLE ITEM (bad performance here) */
		/* better: use bucketLookupItemAddressPointerId */
		/* (has to be implemented, easy) */
		/* *pprng = (void *)pRingWrite; */

		LOCK_BUCK
		if (bucketRemoveItemPointerId (pbuck,
		 (void * const *)&prng->msg.pdbc) != 0)
		    errlogPrintf ("ringInsert: Remove hash for %d failed\n",
		     prng->msg.pdbc);

		if (bucketAddItemPointerId (pbuck,
		 (void * const *)&prng->msg.pdbc, (void *)pRingWrite) != 0)
		    errlogPrintf ("ringInsert: Add hash for %d failed\n",
		     prng->msg.pdbc);

		UNLOCK_BUCK
		prng->hashStatus = hs_Non;
	    }
	    else {
		LOCK_BUCK
		if (bucketAddItemPointerId ( pbuck,
		 (void * const *)&pRingWrite->msg.pdbc, (void *)pRingWrite) != 0)
		    errlogPrintf ("ringInsert: Add hash for %d failed\n",
		     pRingWrite->msg.pdbc);
		UNLOCK_BUCK
	    }
	}
	else {
	    pRingWrite->hashStatus = hs_Non;
	    if (prng)
		prng->hashStatus = hs_Passive;
	}

	pRingWrite = pnext;
	epicsEventSignal (wakeupEvent);
    }
    UNLOCK_RING_W
    return OK;
}


/*******************************************************************************

Routine:
    ringRead

Purpose:
    Get a message out of the ring buffer

Description:
    Check read and write pointer of the ring buffer
    Copy message to msg-buffer. Delete hash entry of the
    ring buffer entry if neccessary.

Retuns:
    0 - there was nothing in the ring buffer
    1 - there where 1 message in the ring buffer
    2 - there are more messages in the ring buffer
*/

static int ringRead (msgBuffer_t *pbuffer)
{
    msgBuffer_t *pnext;
    msgBuffer_t *prng;
    int 	status;

    if (pRingRead == pRingWrite)		/* ring empty? */
	return 0;				/* ring is empty */

    LOCK_RING
    pnext = pRingRead + 1;			/* next read position */
    if (pnext >= pRingTop) pnext = pRingBottom;

    *pbuffer = *pRingRead;			/* copy msg to buffer */

    if (pRingRead->hashStatus != hs_Non) {	/* need to delete from hash */
	LOCK_BUCK
	if (bucketRemoveItemPointerId (pbuck,
	 (void * const *)&pRingRead->msg.pdbc) != 0)
	    errlogPrintf ("ringRead: Remove hash for %d failed\n",
                     pRingRead->msg.pdbc);

	UNLOCK_BUCK
    }
    pRingRead = pnext;				/* now increment read pointer */

    /* protect event msg in ring from overwriting if they are close to read
       pointer. It doesn't matter if we modify unused memory in the ring  */
    if (RINGPOS(pRingWrite) > ringHighWater) {
	prng = pRingRead + ringHighWater;	/* calculate position to test */
	if (prng >= pRingTop)
	    prng -= ringSize;

	if (prng->hashStatus == hs_Active && prng->msg.msgType == mt_Event)
	    prng->hashStatus = hs_Passive;	/* set element protected */
    }
    status = (pnext != pRingWrite) ? 2 : 1;	/* are there more messages ? */

    UNLOCK_RING
    return status;
}


/*******************************************************************************

Routine:
    copyValue

Purpose:
    copy VAL field of record into msg

Description:
    Taking a pointer to the record (common part) this function finds
    the value field of the record and copies the contents to the
    message buffer, pointed to by pmsg.

*/

static void copyValue (dbCommon *pdbc, msg_t *pmsg)
{
    dbFldDes		*pvalFldDes;
    struct dbRecordType	*rdes;
    void		*base;
    int			size;

    if ((rdes = pdbc->rdes) && (pvalFldDes = rdes->pvalFldDes)) {
	base = (void *)((char *)pdbc + pvalFldDes->offset);
	size = pvalFldDes->size; if (size > VAL_LENGTH) size = VAL_LENGTH;
	memcpy ((void *)pmsg->value, base, size);
    }
    else
	for (size=0; size<VAL_LENGTH; size++)
	    pmsg->value[size] = '\0';
}


/*******************************************************************************

Routine:
    strChrRep

Purpose:
    replace characters in a string

Description:
    Replaces all characters "cSearch" with "cReplace" in a string.

*/

static void strChrRep (char *pstr, char cSearch, char cReplace)
{
    while (*pstr) {
	if (*pstr == cSearch)
	    *pstr = cReplace;
	pstr++;
    }
}

