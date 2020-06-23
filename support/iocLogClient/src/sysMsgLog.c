/*******************************************************************************

Project:
    ioc Logger for EPICS

File:
    sysMsgLog.c

Description:
    sysMsgSend2Q() is registered as errlogListener to receive errlog messages
    and format it, then send the formatted messages to the sysMsg queue.
    When a message is in the sysMsg queue, it is read out and sent to the  
    server over the network.
    NOTE: errlog message (from base) is also called system message here.

Author:
    Gongfa Liu

Created:
    16 February 2009

Modification:
2009-02-16      Liu     Initial implementation

(c) 2009 DESY

$Id: sysMsgLog.c,v 1.2 2009/07/08 12:59:51 gliu Exp $
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
#include <errlog.h>		/* errlogAddListener() */
#include <epicsExport.h>	/* epicsExportAddress() */

#include "iocLogClient.h"

#define MAX_SYSMSGS		100		/* max system messages that can be queued */
#define MAX_SYSMSG_LEN		512		/* max bytes in an system message */

/* only static functions are declared here, the shared funtions are declared in iocLogClient.h */
static void sysMsgSend2Q (epicsMessageQueueId sysMsgQId, const char *message); /* send sys msg to q */
static int  sysMsgLogTask ();	/* send errlog msg from queue to server */

int	sysMsgLogDisable = 0;
epicsExportAddress(int,sysMsgLogDisable);

static int		initialized = FALSE;
static epicsThreadId	stid;
static int		stidStop;

static epicsMessageQueueId	sysMsgQId = NULL;		
static int		sysMsgLost;	
static int		sysMsgTotalLost;	

extern char		messageHeader[MESSAGE_HEADER_LENGTH];


/*******************************************************************************

Routine:
    sysMsgLogInit

Purpose:
    Initialize sysMsgLog parameters and register errlogListener.

Description:

Returns:
    OK, or ERROR

*/

int sysMsgLogInit ()
{
    if (initialized) {
	errlogPrintf ("sysMsgLogInit: already initialized\n");
	return OK;
    }

    if (sysMsgLogDisable) {
        errlogPrintf ("sysMsgLogInit: sysMsgLog is disabled\n");
        return OK;
    }

    sysMsgLost = 0;
    sysMsgTotalLost = 0;

    sysMsgQId = epicsMessageQueueCreate (MAX_SYSMSGS, MAX_SYSMSG_LEN);

    if (sysMsgQId == NULL) {
        errlogPrintf("sysMsgLogInit: create msgQCreate failed!\n");
        return ERROR;
    } 

    errlogAddListener ((errlogListener)sysMsgSend2Q, sysMsgQId);

    stidStop = FALSE;
    stid = epicsThreadCreate ("sysMsgLogTask", epicsThreadPriorityMedium,
				epicsThreadGetStackSize (epicsThreadStackSmall),
				(EPICSTHREADFUNC)sysMsgLogTask, 0);

    if (!stid) {
	errlogPrintf ("sysMsgLogInit: sysMsgtask could not be spawned\n");
	epicsMessageQueueDestroy (sysMsgQId);
	errlogRemoveListener((errlogListener)sysMsgSend2Q);
	return ERROR;
    }

#ifdef DEBUG
    printf("sysMsgLogInit finished\n");
#endif
    initialized = TRUE;
    return OK;
}

/*******************************************************************************

Routine:
    sysMsgLogShutdown

Purpose:
    shutdown sysMsgLog

Description:

*/

int sysMsgLogShutdown ()
{
    if (!initialized) {
        errlogPrintf ("sysMsgLogShutdown: sysMsgLog is not initialized");
        return ERROR;
    }

    stidStop = TRUE;
    while (stid) epicsThreadSleep(0.02);
    printf ("sysMsgLogShutdown: sysMsgLogTask deleted...\n");

    epicsMessageQueueDestroy (sysMsgQId);
    errlogRemoveListener((errlogListener)sysMsgSend2Q);
    printf ("sysMsgLogShutdown: Recources freed - sysMsgLog stopped.\n");

    initialized = FALSE;
    return OK;
}


/*******************************************************************************

Routine:
    sysMsgLogReport

Purpose:
    Report status of sysMsgLog

Description:

Returns:
    OK

*/

int sysMsgLogReport (int interest)
{
    if (!initialized) {
	printf ("sysMsgLogReport: sysMsgLog is not initialized\n");
	return OK;
    }

    printf ("sysMsgLogReport! use level parameter of [0..4]......\n");

    printf ("sysMsgLog: %d messages are waiting in the sysMsgQ\n", epicsMessageQueuePending (sysMsgQId));
    printf ("sysMsgLog: %d messages are lost\n", sysMsgLost);
    printf ("sysMsgLog: totally %d messages are lost\n", sysMsgTotalLost);

    if (interest > 0) {
	printf ("sysMsgLog: max message number of sysMsgQ is %d\n", MAX_SYSMSGS);
    }

    if (interest > 1) {
	printf ("sysMsgLog: sysMsgLogDisable is %s\n", sysMsgLogDisable ? "TRUE" : "FALSE");
    }

    printf ("\n");
    return OK;
}


/*******************************************************************************

Routine:
    sysMsgSend2Q

Purpose:
    send errlog/sys msg to queue

Description:
  
    This function is registered as errlogListener to receive errlog message 
    and format it, then send the formatted message to sysMsgQ.

*/

static void sysMsgSend2Q (epicsMessageQueueId sysMsgQId, const char *message)
{
    char sysMsgBuf[MAX_SYSMSG_LEN];
    int nchar;
    epicsTimeStamp ts;
    char timeStr[32];

    if (sysMsgLogDisable || !sysMsgQId || !message) {
        return;
    }
    
    epicsTimeGetCurrent(&ts);
    stampToText (&ts, timeStr);
    
    nchar  = sprintf (sysMsgBuf, "%s", messageHeader); 
    nchar += sprintf (sysMsgBuf+nchar, "APPLICATION-ID=sysMsgLog;"); 
    nchar += sprintf (sysMsgBuf+nchar, "TYPE=sysLog;"); 
    nchar += sprintf (sysMsgBuf+nchar, "EVENTTIME=%s;", timeStr);

    if (sysMsgLost) {
        nchar += sprintf (sysMsgBuf+nchar, "sysMsgLost=%d;", sysMsgLost);
        sysMsgLost = 0;
    }
    
    /* MAX_SYSMSG_LEN = 512, max length of errlog message is 256 */
    if (nchar > 256 - 6) 
        nchar = sprintf (sysMsgBuf+nchar, "TEXT=header is too long;");
    else
        nchar = sprintf (sysMsgBuf+nchar, "TEXT=%s;", message);

    if (epicsMessageQueueTrySend (sysMsgQId, sysMsgBuf, MAX_SYSMSG_LEN) == ERROR) {
        sysMsgLost++;
        sysMsgTotalLost++;
    }
}


/*******************************************************************************

Routine:
    sysMsgLogTask

Purpose:
    send messages to the server

Description:
    When a message is in the sysMsg queue, it is sent to the server over the network.

*/

static int sysMsgLogTask () 
{
    taskPrivate_t       private;

    /* create mutex for private manipulation */
    private.privateLock = epicsMutexCreate ();

    /* create semaphore for reply */
    private.replyEvent = epicsEventCreate (epicsEventEmpty);

    private.serverId = NO_SERVER;  /* -1 */
    taskPrivateTab[6] = &private;
    
    while (stidStop == FALSE) {
        char 		sysMsgBuf[MAX_SYSMSG_LEN+32];
        unsigned long   myMsgId;

        if (epicsMessageQueueTryReceive (sysMsgQId, sysMsgBuf, MAX_SYSMSG_LEN) != ERROR) {

	    /* generate text message from message structure */
	    myMsgId = getMessageId();

            sprintf(sysMsgBuf+strlen(sysMsgBuf), "ID=%ld;", myMsgId);

#ifdef DEBUG
	    /* don't use errlogPrintf() to avoid closed loop */
            printf("sysMsgLogTask: sysMsgBuf:%s\n", sysMsgBuf); 
#endif

	    while (1) {		/* send message again until success */
		volatile int serverSendTo;

		serverSendTo = serverSelected;	/* local copy */

		if (serverSendTo >= 0) {
		    if (sendMessageToServer (
		      sysMsgBuf, serverSendTo, myMsgId, &private) == OK) {
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
	} else {  /* no messages in sysMsgQ */
	    epicsThreadSleep (0.02);
	}
    } /* forever */

    epicsEventDestroy (private.replyEvent);
    epicsMutexDestroy (private.privateLock);
    stid = NULL;
    return OK; 
}

