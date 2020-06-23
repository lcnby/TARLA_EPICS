/*******************************************************************************

Project:
    ioc Logger for EPICS

File:
    caPutLog.c 

Description:
    caPutLog2Array() is registered as asTrapWriteListener to trap caPut message
    and write it to the caPut message array. 
    caPutLog2Q() is invoked by the timer and it just sends the the element index
    of caPut message array to caPutLog message queue.
    When an index info is in caPutLog message queue, the related element of caPut
    message array is read out and cleaned. The readout message is formatted and
    sent to the server over the network.

    Make sure access security is enabled on the IOC by providing a suitable 
    configuration file and load it with a call to asSetFilename(<filename>) before 
    iocInit. Your configuration file should contain a TRAPWRITE rule. 
    The following snippet can be used to enable read/write access and write trapping
    for everyone (i.e. unrestricted access):
     ASG(DEFAULT) {
       RULE(1,READ)
       RULE(1,WRITE,TRAPWRITE)
     }

    Some codes are from the file caPutLogRng.c which implemented by Vladis Korobov.

Author:
    Gongfa Liu

Created:
    16 February 2009

Modification:
2009-02-16      Liu     Initial implementation

(c) 2009 DESY

$Id: caPutLog.c,v 1.4 2009/11/04 15:27:19 gliu Exp $
*******************************************************************************/

/* C */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* EPICS */
#include <epicsEvent.h>		/* epicsEvent */
#include <epicsMutex.h>		/* epicsMutex */
#include <epicsThread.h>	/* epicsThread */
#include <epicsMessageQueue.h>	/* epicsMessageQueue */
#include <epicsTime.h>		/* epicsTimeStamp */
#include <dbAccess.h>		/* DBADDR */
#include <errlog.h>		/* errlogPrintf() */
#include <asTrapWrite.h>	/* asTrapWriteRegisterListener() */
#include <dbDefs.h>		/* PVNAME_STRINGSZ */
#include <freeList.h>		/* freeListInitPvt()... */
#include <epicsTimer.h>		/* epicsTimerQueueAllocate()... */
#include <epicsExport.h>	/* epicsExportAddress() */
#include <asLib.h>		/* define asActive */

#include "iocLogClient.h"

#define MAX_USR_ID		10		/* Length of user ID */
#define MAX_HOST_ID		15		/* Length of host ID */
#define FREELIST_NMALLOC	20		/* Number of elements in free list */
						/* in base, the elements number of the free list */
						/* for trapped write messages is 20.             */
#define MAX_CAPUTLOG_AMSGS	50		/* max caPutlog message number in the array */
#define CAPUTLOG_TMO_SEC	2.0		/* time out for caPutLogMsg in the array */
#define MAX_CAPUTLOG_QMSGS	MAX_CAPUTLOG_AMSGS /* max caPutLog messages that can be queued */
#define MAX_CAPUTLOG_QMSG_LEN	8		/* max bytes in a caPutLog queue message */
#define MAX_CAPUTLOG_MSG_LEN	512		/* max bytes in a caPutLog formatted message */

typedef union {
    char		v_char;
    unsigned char	v_uchar;
    short		v_short;
    unsigned short	v_ushort;
    long		v_long;
    unsigned long	v_ulong;
    float		v_float;
    double		v_double;
    char		v_string[MAX_STRING_SIZE];
} VALUE;

struct tim_val {
   epicsTimeStamp	time;
   VALUE		value;
};

typedef struct asPutLog {
   char 	userid[MAX_USR_ID+1];		/* User ID */
   char 	hostid[MAX_HOST_ID+1];		/* Host ID */
   char 	pv_name[PVNAME_STRINGSZ+5];	/* rec_name.field_name */
   short 	type;				/* Field type */
   VALUE 	old_value;			/* Value  before Put */
   VALUE 	min_value;			/* max value within sequent Put */
   VALUE 	max_value;			/* min value within sequent Put */
   struct tim_val new_value;			/* Value & time after Put */
   short 	overwrite;			/* overwrite count  */
}LOGDATA;

/* only static functions are declared here, the shared funtions are declared in iocLogClient.h */
static void cleanup (int step);
static void caPutLog2Array (asTrapWriteMessage *pmessage, int afterPut); /* trap caPut msg to array */
static void caPutLog2Q (int *pidx);		/* send caPut msg to queue */
static int  caPutLogTask ();			/* send caPut msg to server */
static void val2a(VALUE *, short, char *);
static void val_min(short, VALUE *, VALUE *, VALUE *);
static void val_max(short, VALUE *, VALUE *, VALUE *);
static char *strcat_cnt(char *s1, const char *s2, int max);

int 	caPutLogDisable = 0;
epicsExportAddress(int,caPutLogDisable);

static int		initialized = FALSE;
static epicsThreadId	ptid;
static int		ptidStop;

static void 		*flPvt = NULL;		/* for freeList */		
static asTrapWriteId	asListenerId = NULL;	/* Trap Write Listener Id */
static int		caPutLogMsgTotalLost;	
static int		caPutLogMsg2ArrayLost;	
static int		caPutLogMsg2QLost;	
static int		caPutLogMsgInArrayNum; 	/* the message number in caPutLogMsgArray */
static LOGDATA		caPutLogMsgArray[MAX_CAPUTLOG_AMSGS]; 	/* caPutLogMsg array */
static epicsMutexId	caPutLogMsgArrayLock;			/* mutex semaphore for the array */
static epicsTimerQueueId caPutLogMsgTimerQueue; 		/* caPutLogMsg timer queue Id */
static epicsTimerId	caPutLogMsgTimer[MAX_CAPUTLOG_AMSGS]; 	/* caPutLogMsg timer Id array */
static int		caPutLogMsgTimerArg[MAX_CAPUTLOG_AMSGS]; /* caPutLogMsg timer Arg array */
static epicsMessageQueueId	caPutLogMsgQId = NULL;		/* caPutLogMsg queue Id */	


/*******************************************************************************

Routine:
    caPutLogInit

Purpose:
    Initialize caPutLog parameters and register asTrapWriteListener.

Description:

Returns:
    OK, or ERROR

*/

int caPutLogInit ()
{
    int		i;

    if (initialized) {
	errlogPrintf ("caPutLogInit: already initialized\n");
	return OK;
    }

    if(!pdbbase) {
	errlogPrintf ("caPutLogInit: No database has been loaded\n");
        return OK;
    }

    if (!asActive) {
	errlogPrintf ("caPutLogInit: asActive is inactive\n");
	return OK;
    }

    if (caPutLogDisable) {
	errlogPrintf ("caPutLogInit: caPutLog is disabled\n");
	return OK;
    }

    caPutLogMsgTotalLost = 0;
    caPutLogMsg2ArrayLost = 0;
    caPutLogMsg2QLost = 0;
    caPutLogMsgInArrayNum = 0;

    for (i=0; i<MAX_CAPUTLOG_AMSGS; i++) 
	caPutLogMsgArray[i].pv_name[0] = 0; /* clear pv_name */

    caPutLogMsgArrayLock = epicsMutexCreate ();
    if (caPutLogMsgArrayLock == NULL) {
	errlogPrintf ("caPutLogInit: create caPutLogMsgArrayLock failed\n");
	return ERROR;
    }

    caPutLogMsgTimerQueue = epicsTimerQueueAllocate(1, epicsThreadPriorityScanHigh);
    if (caPutLogMsgTimerQueue == NULL) {
	errlogPrintf ("caPutLogInit: create caPutLogMsgTimerQueue failed\n");
	cleanup (1);
	return ERROR;
    }

    for (i=0; i<MAX_CAPUTLOG_AMSGS; i++) {
	caPutLogMsgTimerArg[i] = i;	
	caPutLogMsgTimer[i] = epicsTimerQueueCreateTimer(caPutLogMsgTimerQueue,
				(epicsTimerCallback)caPutLog2Q, &caPutLogMsgTimerArg[i]);
	if (caPutLogMsgTimer[i] == NULL) {
	    errlogPrintf ("logAlarmsInit: create caPutLogMsgTimer[%d] failed\n", i);
	    cleanup (2);
	    while (i >= 0) epicsTimerQueueDestroyTimer (caPutLogMsgTimerQueue, caPutLogMsgTimer[--i]);
	    return ERROR;
	}
    }

    freeListInitPvt(&flPvt, sizeof(LOGDATA), FREELIST_NMALLOC);
    if (flPvt == NULL){
	errlogPrintf ("caPutLogInit: create flPvt failed\n");
	cleanup (3);
	return ERROR;
    }		

    caPutLogMsgQId = epicsMessageQueueCreate(MAX_CAPUTLOG_QMSGS, MAX_CAPUTLOG_QMSG_LEN);
    if (caPutLogMsgQId == NULL) {
	errlogPrintf("caPutLogInit: create caPutLogMsgQId failed!\n");
	cleanup (4);
	return ERROR;
    }

    asListenerId = asTrapWriteRegisterListener(caPutLog2Array);

    ptidStop = FALSE;
    ptid = epicsThreadCreate ("caPutLogTask", epicsThreadPriorityMedium,
				epicsThreadGetStackSize (epicsThreadStackSmall),
				(EPICSTHREADFUNC)caPutLogTask, 0);
    if (!ptid) {
	errlogPrintf ("caPutLogInit: caPutLogTask could not be spawned\n");
	cleanup (6);
	return ERROR;
    }

#ifdef DEBUG
    printf("caPutLogInit finished\n");
#endif
    initialized = TRUE;
    return OK;
}


/*******************************************************************************

Routine:
    caPutLogShutdown

Purpose:
    shutdown caPutLog

Description:

*/

int caPutLogShutdown ()
{
    if (!initialized) {
	errlogPrintf ("caPutLogShutdown: caPutLog is not initialized\n");
	return ERROR;
    }

    ptidStop = TRUE;
    while (ptid) epicsThreadSleep(0.02); 
    printf ("caPutLogShutdown: caPutLogTask stopped...\n");

    cleanup (6);
    printf ("caPutLogShutdown: Recources freed - caPutLog stopped.\n");

    initialized = FALSE;
    return OK;
}


/*******************************************************************************

Routine:
    caPutLogReport

Purpose:
    Report status of caPutLog

Description:

Returns:
    OK

*/

int caPutLogReport (int interest)
{
    if (!initialized) {
	printf ("caPutLogReport: caPutLog is not initialized\n");
	return OK;
    }

    printf ("caPutLogReport! use level parameter of [0..4]......\n");

    printf ("caPutLog: %d messages are waiting in caPutLogMsgArray\n", caPutLogMsgInArrayNum);
    printf ("caPutLog: %d messages are waiting in the caPutLogMsgQ\n", epicsMessageQueuePending (caPutLogMsgQId));
    printf ("caPutLog: %d messages are lost when writing to array\n", caPutLogMsg2ArrayLost);
    printf ("caPutLog: %d messages are lost when sending to queue\n", caPutLogMsg2QLost);
    printf ("caPutLog: totally %d messages are lost\n", caPutLogMsgTotalLost);

    if (interest > 0) {
	printf ("\ncaPutLog: element number of caPutLog array is %d\n", MAX_CAPUTLOG_AMSGS);
	printf ("caPutLog: max message number of caPutLog queue is %d\n", MAX_CAPUTLOG_QMSGS);
	printf ("caPutLog: timeout for caPutLog messages in array is %f seconds\n", CAPUTLOG_TMO_SEC);
    }

    if (interest > 1) {
	printf ("\ncaPutLog: asActive is %s\n", asActive ? "TRUE" : "FALSE");
	printf ("caPutLog: caPutLogDisable is %s\n", caPutLogDisable ? "TRUE" : "FALSE");
    }

    printf ("\n");
    return OK;
}


/*******************************************************************************

Routine:
    cleanup

Purpose:
    Release resources if startup fails or shutdown

Description:

*/

static void cleanup (int step)
{
    int i;

    switch (step) {
	case 6: asTrapWriteUnregisterListener(asListenerId);
	case 5: epicsMessageQueueDestroy (caPutLogMsgQId);
	case 4: freeListCleanup (flPvt);
	case 3: 
		for (i=0; i<MAX_CAPUTLOG_AMSGS; i++) 
		    epicsTimerQueueDestroyTimer (caPutLogMsgTimerQueue, caPutLogMsgTimer[i]);
	case 2: epicsTimerQueueRelease (caPutLogMsgTimerQueue);
	case 1: epicsMutexDestroy (caPutLogMsgArrayLock);
    }
}


/*******************************************************************************

Routine:
    caPutLog2Array

Purpose:
    send caPut msg to array

Description:
  
    This function is registered as asTrapWriteListener to trap caPut message 
    and write it to the caPut message array.
    The behaviour of writing the caPut message to the array is described below:
      (1) A error message is printed if the array is full.
      (2) The existing caPut message is overwritten if it has the same pv_name.
      (3) A timer is issued when a new caPut message is written to the array.

*/

static void caPutLog2Array (asTrapWriteMessage *pmessage, int afterPut)
{

    DBADDR *paddr = (DBADDR *)pmessage->serverSpecific;
    struct dbCommon *precord = (struct dbCommon *)paddr->precord;
    dbFldDes *pdbFldDes = (dbFldDes *)(paddr->pfldDes);
    LOGDATA *plocal = NULL;
    long  options, num_elm = 1;
    long status;
    int i;
    epicsTimeStamp currTime;

    if (afterPut == FALSE) {	/* Call before caPut, allocate the Log structure */
#ifdef DEBUG
        errlogPrintf("caPutLog2Array: enter, before caPut\n"); 
#endif
	if (caPutLogDisable) {	/* caPutLogDisable is TRUE, just set pmessage->userPvt to NULL */
	    pmessage->userPvt = NULL;   
	    return;
	}
	plocal = freeListMalloc(flPvt);
	if (plocal == NULL) {
	   errlogPrintf("caPutLog2Array: memory allocation failed\n");
	   pmessage->userPvt = NULL;
	   return;
	}
	pmessage->userPvt = (void *)plocal;	/* Save the pointer on allocated structure */
	plocal->userid[0] = 0;
	strcat_cnt(plocal->userid,pmessage->userid,MAX_USR_ID);	/* Set user ID */
    	plocal->hostid[0] = 0;
	strcat_cnt(plocal->hostid,pmessage->hostid,MAX_HOST_ID);/* and host ID into LogData */

	/* Set rec_name.field_name & field type */
	plocal->pv_name[0]=0;
	strcat(plocal->pv_name,precord->name);
	strcat(plocal->pv_name,".");
	strcat(plocal->pv_name,pdbFldDes->name);

	plocal->type = paddr->field_type;
	plocal->overwrite = 0;

	options = 0;
	num_elm = 1;
	if (plocal->type > DBF_ENUM)		/* Get other field as a string */
	   status = dbGetField(paddr, DBR_STRING, &plocal->old_value, &options,
	 		    &num_elm, 0);
	else			/* Up to ENUM type get as a value */
	   status = dbGetField(paddr, plocal->type, &plocal->old_value, &options,
	 		    &num_elm, 0);

	if (status) {
	   errlogPrintf("caPutLog2Array: dbGetField error=%ld\n", status);
	   plocal->type = DBR_STRING;
	   strcpy(plocal->old_value.v_string,"Not_Accesable");	   
	}
	return;
    }

    { 						/* Call after caPut */
#ifdef DEBUG
        errlogPrintf("caPutLog2Array: enter, after caPut\n"); 
#endif
	plocal = (LOGDATA *) pmessage->userPvt;	/* Restore the pointer */

        if (!plocal) return;

	if (caPutLogDisable) {	 		/* caPutLogDisable is TRUE, just release resource */
            freeListFree (flPvt, plocal);	/* Release caPutLog Data memory  */
	    return;
	}

	options = DBR_TIME;

	num_elm = 1;
	if (plocal->type > DBF_ENUM)		/* Get other field as a string */
	   status = dbGetField(paddr, DBR_STRING, &plocal->new_value, &options,
	 		    &num_elm, 0);
	else					/* Up to ENUM type get as a value */
	   status = dbGetField(paddr, plocal->type, &plocal->new_value, &options,
			    &num_elm, 0);
	if (status) {
	   errlogPrintf("caPutLog2Array: dbGetField error=%ld\n", status);
	   plocal->type = DBR_STRING;
	   strcpy(plocal->new_value.value.v_string,"Not_Accesable");	   
	}

	/* Set the initial min & max values ONLY for numeric value */
	if ((plocal->type > DBF_STRING) && (plocal->type <= DBF_ENUM)) {
	    memcpy(&plocal->max_value, &plocal->new_value.value, sizeof(VALUE));
	    memcpy(&plocal->min_value, &plocal->new_value.value, sizeof(VALUE));
	}

	epicsTimeGetCurrent(&currTime);	/* get current time stamp */
	/* replace, if necessary, the time stamp */
	if (plocal->new_value.time.secPastEpoch < currTime.secPastEpoch) {
		plocal->new_value.time.secPastEpoch = currTime.secPastEpoch;
		plocal->new_value.time.nsec = currTime.nsec;
	}

	if (caPutLogMsgInArrayNum >= MAX_CAPUTLOG_AMSGS) {
	    errlogPrintf("caPutLog2Array: Array is full! The current caPutLogMsg is lost!\n");
	    caPutLogMsgTotalLost ++;
	    caPutLogMsg2ArrayLost ++;
	} else {	
	    for (i = 0; i < MAX_CAPUTLOG_AMSGS; i++) 
	 	if (!strcmp (caPutLogMsgArray[i].pv_name, plocal->pv_name)) break;

	    if (i < MAX_CAPUTLOG_AMSGS ) {
	        /* The existing caPut message is overwritten if it has the same pv_name */
		epicsMutexLock (caPutLogMsgArrayLock);
		if ((plocal->type > DBF_STRING) && (plocal->type <= DBF_ENUM)) {
		    val_max(plocal->type, &plocal->max_value, &caPutLogMsgArray[i].max_value, &plocal->max_value);
		    val_min(plocal->type, &plocal->min_value, &caPutLogMsgArray[i].min_value, &plocal->min_value);
		}
		plocal->overwrite = caPutLogMsgArray[i].overwrite + 1;
	 	memcpy((char *)&caPutLogMsgArray[i], (char *)plocal, sizeof(LOGDATA));
		epicsMutexUnlock (caPutLogMsgArrayLock);
#ifdef DEBUG
            	errlogPrintf("caPutLog2Array: overwrite=%d\n", plocal->overwrite); 
#endif
	    } else { /* no caPut message with the same pv_name in the array */
	   	for (i = 0; i < MAX_CAPUTLOG_AMSGS; i++) 
		    if (caPutLogMsgArray[i].pv_name[0] == 0 ) break;
		
		if (i < MAX_CAPUTLOG_AMSGS ) {
		    epicsMutexLock (caPutLogMsgArrayLock);
	   	    memcpy((char *)&caPutLogMsgArray[i], (char *)plocal, sizeof(LOGDATA));
	   	    caPutLogMsgInArrayNum ++; 
		    epicsMutexUnlock (caPutLogMsgArrayLock);
		    /* A timer is issued when a new caPut message is written to the array */
	   	    epicsTimerStartDelay(caPutLogMsgTimer[i], CAPUTLOG_TMO_SEC);  
#ifdef DEBUG
            	    errlogPrintf("caPutLog2Array: epicsTimerStartDelay, i=%d\n", i); 
#endif
		} else {
		   errlogPrintf("caPutLog2Array: caPutLogMsgInArrayNum < MAX_CAPUTLOG_AMSGS, but no space in the array, sth WRONG!\n");
		   caPutLogMsgTotalLost ++;
		   caPutLogMsg2ArrayLost ++;
		}
	    }
	}

        freeListFree(flPvt, plocal);		/* Release caPutLog Data memory */

#ifdef DEBUG
        errlogPrintf("caPutLog2Array: leave, after caPut\n"); 
#endif
        return;
    }
}


/*******************************************************************************

Routine:
    caPutLog2Q

Purpose:
    send caPut msg to queue

Description:
  
    This function is invoked by the timer, it should be as simple as possible.
    It just sends the the element index to the queue. 

*/

static void caPutLog2Q (int *pidx)
{
    int 	*pIndex;
    char 	caPutLogQMsgBuf[MAX_CAPUTLOG_QMSG_LEN];

#ifdef DEBUG
        errlogPrintf("caPutLog2Q: enter,  *pidx=%d\n", *pidx); 
#endif

    pIndex = (int *)caPutLogQMsgBuf;
    *pIndex = *pidx; 

    if (!caPutLogMsgQId) {
        return;
    }
    
    if (epicsMessageQueueTrySend (caPutLogMsgQId, caPutLogQMsgBuf, MAX_CAPUTLOG_QMSG_LEN) == ERROR) {
#ifdef DEBUG
        errlogPrintf("caPutLog2Q: msgQSend ERROR\n"); 
#endif
        caPutLogMsgTotalLost++;
        caPutLogMsg2QLost++;
    }
}


/*******************************************************************************

Routine:
    caPutLogTask

Purpose:
    send caPut messages to the server

Description:
    When an index info is in the caPutLogMsg queue, the related element of caPut
    message array is read out and cleaned. The readout message is formatted and
    sent to the server over the network. 

*/

static int caPutLogTask () 
{
    taskPrivate_t       private;

    /* create mutex for private manipulation */
    private.privateLock = epicsMutexCreate ();

    /* create semaphore for reply */
    private.replyEvent = epicsEventCreate (epicsEventEmpty);

    private.serverId = NO_SERVER;  /* -1 */
    taskPrivateTab[7] = &private;
    
    while (ptidStop == FALSE) {
        char 		caPutLogQMsgBuf[MAX_CAPUTLOG_QMSG_LEN];
	int		*pIndex, idx;
        char 		caPutLogMsgBuf[MAX_CAPUTLOG_MSG_LEN];
	int 		nchar;
	char 		timeStr[32];
	LOGDATA 	local;
	char 		nValStr[MAX_STRING_SIZE], oValStr[MAX_STRING_SIZE];
	char 		minValStr[16], maxValStr[16];
        unsigned long   myMsgId;

        if (epicsMessageQueueTryReceive (caPutLogMsgQId, caPutLogQMsgBuf, MAX_CAPUTLOG_QMSG_LEN) != ERROR) {
	    pIndex = (int *)caPutLogQMsgBuf;
	    idx  = *pIndex;
	    epicsMutexLock (caPutLogMsgArrayLock);
	    memcpy((char *)&local, (char *)&caPutLogMsgArray[idx], sizeof(LOGDATA));
	    caPutLogMsgArray[idx].pv_name[0] = 0;
	    caPutLogMsgInArrayNum --; 
	    epicsMutexUnlock (caPutLogMsgArrayLock);

	    stampToText (&(local.new_value.time), timeStr);
	    nchar  = sprintf (caPutLogMsgBuf, "EVENTTIME=%s;", timeStr); 
	    nchar += sprintf (caPutLogMsgBuf+nchar, "APPLICATION-ID=caPutLog;"); 
	    nchar += sprintf (caPutLogMsgBuf+nchar, "TYPE=putLog;"); 

	    nchar += sprintf (caPutLogMsgBuf+nchar, "HOST=%s;", local.hostid); 
	    nchar += sprintf (caPutLogMsgBuf+nchar, "USER=%s;", local.userid); 
	    nchar += sprintf (caPutLogMsgBuf+nchar, "NAME=%s;", local.pv_name); 

	    if ((local.type > DBF_STRING) && (local.type <= DBF_ENUM)) {
		val2a(&local.new_value.value, local.type, nValStr); 
		val2a(&local.old_value, local.type, oValStr); 
 		nchar += sprintf (caPutLogMsgBuf+nchar, "VALUE=%s;", nValStr); 
 		nchar += sprintf (caPutLogMsgBuf+nchar, "VALUE-OLD=%s;", oValStr); 
	    } else {
 		nchar += sprintf (caPutLogMsgBuf+nchar, "VALUE=%s;", local.new_value.value.v_string); 
 		nchar += sprintf (caPutLogMsgBuf+nchar, "VALUE-OLD=%s;", local.old_value.v_string);
	    }

	    if (local.overwrite) {
		if ((local.type > DBF_STRING) && (local.type <= DBF_ENUM)) {
		    val2a(&local.min_value, local.type, minValStr); 
		    val2a(&local.max_value, local.type, maxValStr); 
 		    nchar += sprintf (caPutLogMsgBuf+nchar, "VALUE-MIN=%s;", minValStr); 
 		    nchar += sprintf (caPutLogMsgBuf+nchar, "VALUE-MAX=%s;", maxValStr); 
		}
 		nchar += sprintf (caPutLogMsgBuf+nchar, "OVERWRITES=%d;", local.overwrite); 
	    }

	    if (caPutLogMsg2ArrayLost || caPutLogMsg2QLost) {
		nchar += sprintf (caPutLogMsgBuf+nchar, "LOST=%d;" 
				  , (caPutLogMsg2ArrayLost + caPutLogMsg2QLost));
		caPutLogMsg2ArrayLost = 0;
		caPutLogMsg2QLost = 0;
	    }

	    /* generate text message from message structure */
	    myMsgId = getMessageId();
 	    nchar += sprintf (caPutLogMsgBuf+nchar, "ID=%ld;", myMsgId); 

#ifdef DEBUG
            printf("caPutLogTask: caPutLogMsgBuf(len=%d):%s\n", nchar, caPutLogMsgBuf); 
#endif

	    while (1) {		/* send message again until success */
		volatile int serverSendTo;

		serverSendTo = serverSelected;	/* local copy */

		if (serverSendTo >= 0) {
		    if (sendMessageToServer ( caPutLogMsgBuf, serverSendTo,
						 myMsgId, &private) == OK) {
			serverStatus[serverSendTo].secMsg = logTimeGetCurrentDouble();
			break; /* message sent! */
		    }
		    /* error in transmission, retry after short delay */
		    epicsThreadSleep (secMsgRetryWait);
		}
		else {
		    /* no server is selected, wait until next beacon period */
		    epicsThreadSleep (secBeaconPeriod);
		}
	    }/* message sent */
	} else { /* no  messages in caPutLogMsgQ */
	    epicsThreadSleep (0.02);
	}
    } /* forever */

    ptid = NULL;
    epicsEventDestroy (private.replyEvent);
    epicsMutexDestroy (private.privateLock);
    return OK; 
}


/*******************************************************************************

Routine:
    val_min

Purpose:
    get the min between tow variables based on the type 

*/

static void val_min(short type, VALUE *pa, VALUE *pb, VALUE *pres)
{
	switch (type) {
	   case DBF_SHORT: 
	        pres->v_short = (pb->v_short < pa->v_short)? pb->v_short: pa->v_short;
		return;

	   case DBF_USHORT: 
	   case DBF_ENUM: 
	        pres->v_ushort = (pb->v_ushort < pa->v_ushort)? pb->v_ushort: pa->v_ushort;
		return;

	   case DBF_LONG: 
	        pres->v_long = (pb->v_long < pa->v_long)? pb->v_long: pa->v_long;
		return;

	   case DBF_ULONG: 
	        pres->v_ulong = (pb->v_ulong < pa->v_ulong)? pb->v_ulong: pa->v_ulong;
		return;

	   case DBF_FLOAT: 
	        pres->v_float = (pb->v_float < pa->v_float)? pb->v_float: pa->v_float;
		return;

	   case DBF_DOUBLE: 
	        pres->v_double = (pb->v_double < pa->v_double)? pb->v_double: pa->v_double;
		return;

	   case DBF_CHAR: 
	        pres->v_char = (pb->v_char < pa->v_char)? pb->v_char: pa->v_char;
		return;

	   case DBF_UCHAR: 
	        pres->v_uchar = (pb->v_uchar < pa->v_uchar)? pb->v_uchar: pa->v_uchar;
		return;
	}
}	


/*******************************************************************************

Routine:
    val_max

Purpose:
    get the max between tow variables based on the type 

*/

static void val_max(short type, VALUE *pa, VALUE *pb, VALUE *pres)
{
	switch (type) {
	   case DBF_SHORT: 
	        pres->v_short = (pb->v_short > pa->v_short)? pb->v_short: pa->v_short;
		return;

	   case DBF_USHORT: 
	   case DBF_ENUM: 
	        pres->v_ushort = (pb->v_ushort > pa->v_ushort)? pb->v_ushort: pa->v_ushort;
		return;

	   case DBF_LONG: 
	        pres->v_long = (pb->v_long > pa->v_long)? pb->v_long: pa->v_long;
		return;

	   case DBF_ULONG: 
	        pres->v_ulong = (pb->v_ulong > pa->v_ulong)? pb->v_ulong: pa->v_ulong;
		return;

	   case DBF_FLOAT: 
	        pres->v_float = (pb->v_float > pa->v_float)? pb->v_float: pa->v_float;
		return;

	   case DBF_DOUBLE: 
	        pres->v_double = (pb->v_double > pa->v_double)? pb->v_double: pa->v_double;
		return;

	   case DBF_CHAR: 
	        pres->v_char = (pb->v_char > pa->v_char)? pb->v_char: pa->v_char;
		return;

	   case DBF_UCHAR: 
	        pres->v_uchar = (pb->v_uchar > pa->v_uchar)? pb->v_uchar: pa->v_uchar;
		return;
	}
}	


/*******************************************************************************

Routine:
    val2a

Purpose:
    convert val to string based on the type 

*/

static void val2a(VALUE *pval, short type, char *pstr)
{
	int tmp;
 
	switch (type) {
	   case DBF_CHAR:
	   case DBF_UCHAR:
	   	tmp = (int) pval->v_uchar;
		sprintf(pstr,"%d", tmp);	/* replace %c with %d to avoid invisible char */
		return;

	   case DBF_SHORT:
		sprintf(pstr,"%hd", pval->v_short);
		return;

	   case DBF_USHORT:
	   case DBF_ENUM:
		sprintf(pstr,"%hu", pval->v_ushort);
		return;

	   case DBF_LONG:
		sprintf(pstr,"%ld", pval->v_long);
		return;

	   case DBF_ULONG:
		sprintf(pstr,"%lu", pval->v_ulong);
		return;

	   case DBF_FLOAT:
		sprintf(pstr,"%g", pval->v_float);
		return;

	   case DBF_DOUBLE:
		sprintf(pstr,"%g", pval->v_double);
		return;
	}
}


/*******************************************************************************

Routine:
    strcat_cnt

Purpose:
    concatenate string not overflowing a string buffer 

*/

static char *strcat_cnt(char *s1, const char *s2, int max)
{
	int len = strlen(s1);          /* Get the original string length */
	int cnt;

	if (len < max) {
		cnt = max - len - 1;        /* Count the rest of the buffer */
		strncat(s1,s2,cnt);         /* Concatenate not more than cnt chars */
		return (s1);
	} else {
		return(NULL);
	}
}

