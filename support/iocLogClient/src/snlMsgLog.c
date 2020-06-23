/*******************************************************************************

Project:
    ioc Logger for EPICS

File:
    snlMsgLog.c

Description:
    snlMsgLog() is a shared function which can be called in snl program. It 
    formats the meassage and send the formatted messages to the snlMsg queue.
    When a message is in the snlMsg queue, it is read out and sent to the  
    server over the network.

Author:
    Gongfa Liu

Created:
    07 July 2009

Modification:
2009-07-08      Liu     Initial implementation

(c) 2009 DESY

$Id: snlMsgLog.c,v 1.3 2009/10/05 12:59:49 schoeneb Exp $
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
#include <errlog.h>		/* errlogPrintf() */
#include <epicsStdio.h>		/* epicsVsnprintf() */
#include <epicsExport.h>	/* epicsExportAddress() */

#include "iocLogClient.h"

#define MAX_SNLMSGS		100		/* max system messages that can be queued */
#define MAX_SNLMSG_LEN		512		/* max bytes in an snl log message */
#define MAX_SNLFMTMSG_LEN  	256		/* max bytes in an snl format message */

/* only static functions are declared here, the shared funtions are declared in iocLogClient.h */
static int  snlMsgLogTask ();	/* send errlog msg from queue to server */

int	snlMsgLogDisable = 0;
epicsExportAddress(int,snlMsgLogDisable);

static int		initialized = FALSE;
static epicsThreadId	stid;
static int		stidStop;
static epicsMutexId	tsModifyLock;

static epicsTimeStamp	last_ts = { 0 };

static epicsMessageQueueId	snlMsgQId = NULL;		
static int		snlMsgLost;	
static int		snlMsgTotalLost;	

extern char		messageHeader[MESSAGE_HEADER_LENGTH];


/*******************************************************************************

Routine:
    snlMsgLogInit

Purpose:
    Initialize snlMsgLog parameters and register errlogListener.

Description:

Returns:
    OK, or ERROR

*/

int snlMsgLogInit ()
{
    if (initialized) {
	errlogPrintf ("snlMsgLogInit: already initialized\n");
	return OK;
    }

    if (snlMsgLogDisable) {
        errlogPrintf ("snlMsgLogInit: snlMsgLog is disabled\n");
        return OK;
    }

    snlMsgLost = 0;
    snlMsgTotalLost = 0;

    snlMsgQId = epicsMessageQueueCreate (MAX_SNLMSGS, MAX_SNLMSG_LEN);

    if (snlMsgQId == NULL) {
        errlogPrintf("snlMsgLogInit: create msgQCreate failed!\n");
        return ERROR;
    } 

    stidStop = FALSE;
    stid = epicsThreadCreate ("snlMsgLogTask", epicsThreadPriorityMedium,
				epicsThreadGetStackSize (epicsThreadStackSmall),
				(EPICSTHREADFUNC)snlMsgLogTask, 0);

    if (!stid) {
	errlogPrintf ("snlMsgLogInit: snlMsgtask could not be spawned\n");
	epicsMessageQueueDestroy (snlMsgQId);
	return ERROR;
    }

    tsModifyLock = epicsMutexMustCreate ();

#ifdef DEBUG
    printf("snlMsgLogInit finished\n");
#endif
    initialized = TRUE;
    return OK;
}

/*******************************************************************************

Routine:
    snlMsgLogShutdown

Purpose:
    shutdown snlMsgLog

Description:

*/

int snlMsgLogShutdown ()
{
    if (!initialized) {
        errlogPrintf ("snlMsgLogShutdown: snlMsgLog is not initialized");
        return ERROR;
    }

    epicsMutexDestroy (tsModifyLock);

    stidStop = TRUE;
    while (stid) epicsThreadSleep(0.02);
    printf ("snlMsgLogShutdown: snlMsgLogTask deleted...\n");

    epicsMessageQueueDestroy (snlMsgQId);
    printf ("snlMsgLogShutdown: Recources freed - snlMsgLog stopped.\n");

    initialized = FALSE;
    return OK;
}


/*******************************************************************************

Routine:
    snlMsgLogReport

Purpose:
    Report status of snlMsgLog

Description:

Returns:
    OK

*/

int snlMsgLogReport (int interest)
{
    if (!initialized) {
	printf ("snlMsgLogReport: snlMsgLog is not initialized\n");
	return OK;
    }

    printf ("snlMsgLogReport! use level parameter of [0..4]......\n");

    printf ("snlMsgLog: %d messages are waiting in the snlMsgQ\n", epicsMessageQueuePending (snlMsgQId));
    printf ("snlMsgLog: %d messages are lost\n", snlMsgLost);
    printf ("snlMsgLog: totally %d messages are lost\n", snlMsgTotalLost);

    if (interest > 0) {
	printf ("snlMsgLog: max message number of snlMsgQ is %d\n", MAX_SNLMSGS);
    }

    if (interest > 1) {
	printf ("snlMsgLog: snlMsgLogDisable is %s\n", snlMsgLogDisable ? "TRUE" : "FALSE");
    }

    printf ("\n");
    return OK;
}


/*******************************************************************************

Routine:
    snlMsgLog

Purpose:
    send snl msg to queue

Description:
  
    This function formats the snl message and send the formatted message to snlMsgQ. 

*/
int snlMsgLog (const char *fmt, ...)
{
    char 	snlMsgBuf[MAX_SNLMSG_LEN];
    int 	nchar;
    epicsTimeStamp ts;
    char 	timeStr[32];

    va_list	args;
    int		tnchar;
    static const char tmsg[] = "<<TRUNCATED>>\n";

    if (snlMsgLogDisable || !snlMsgQId ) {  /* for standalone application */
	va_start (args, fmt);
	vfprintf(stdout,fmt,args);
	va_end (args);
	fflush(stdout);
	puts("");  /* new line */
        return -1;
    }
    
    epicsTimeGetCurrent(&ts);

    epicsMutexMustLock (tsModifyLock);

    if (!epicsTimeGreaterThan (&ts, &last_ts)) {
        epicsTimeAddSeconds (&last_ts, 0.001);
        ts = last_ts;
    }
    else
        last_ts = ts;

    epicsMutexUnlock (tsModifyLock);

    stampToText (&ts, timeStr);
    
    /* MAX_SNLMSG_LEN = 512,  MESSAGE_HEADER_LENGTH = 256 */
    nchar  = sprintf (snlMsgBuf, "%s", messageHeader); 
    nchar += sprintf (snlMsgBuf+nchar, "APPLICATION-ID=snlMsgLog;"); 
    nchar += sprintf (snlMsgBuf+nchar, "TYPE=snlLog;"); 
    nchar += sprintf (snlMsgBuf+nchar, "EVENTTIME=%s;", timeStr);
    nchar += sprintf (snlMsgBuf+nchar, "NAME=%s;", epicsThreadGetNameSelf());

    if (snlMsgLost) {
        nchar += sprintf (snlMsgBuf+nchar, "snlMsgLost=%d;", snlMsgLost);
        snlMsgLost = 0;
    }
    
    /* MAX_SNLMSG_LEN = 512, MAX_SNLFMTMSG_LEN = 256 */

    nchar += sprintf (snlMsgBuf+nchar, "TEXT=");
    if (nchar > 256 - 1)
        nchar += sprintf (snlMsgBuf+nchar, "header is too long;");  
    else
    {
        va_start (args, fmt);
        tnchar = epicsVsnprintf (snlMsgBuf+nchar, MAX_SNLFMTMSG_LEN, fmt?fmt:"", args);
        if (tnchar >= MAX_SNLFMTMSG_LEN)
	{
	   if (MAX_SNLFMTMSG_LEN > sizeof(tmsg))
		strcpy (snlMsgBuf+nchar+MAX_SNLFMTMSG_LEN-sizeof(tmsg), tmsg);
	   tnchar = MAX_SNLFMTMSG_LEN - 1;
	}
	nchar += tnchar;
        nchar += sprintf (snlMsgBuf+nchar, ";");
        va_end (args);
    }

    if (epicsMessageQueueTrySend (snlMsgQId, snlMsgBuf, MAX_SNLMSG_LEN) == ERROR) {
        snlMsgLost++;
        snlMsgTotalLost++;
    }
    
    return 0;
}


/*******************************************************************************

Routine:
    snlMsgLogTask

Purpose:
    send messages to the server

Description:
    When a message is in the snlMsg queue, it is sent to the server over the network.

*/

static int snlMsgLogTask () 
{
    taskPrivate_t       private;

    /* create mutex for private manipulation */
    private.privateLock = epicsMutexCreate ();

    /* create semaphore for reply */
    private.replyEvent = epicsEventCreate (epicsEventEmpty);

    private.serverId = NO_SERVER;  /* -1 */
    taskPrivateTab[8] = &private;
    
    while (stidStop == FALSE) {
        char 		snlMsgBuf[MAX_SNLMSG_LEN+32];
        unsigned long   myMsgId;

        if (epicsMessageQueueTryReceive (snlMsgQId, snlMsgBuf, MAX_SNLMSG_LEN) != ERROR) {

	    /* generate text message from message structure */
	    myMsgId = getMessageId();

            sprintf(snlMsgBuf+strlen(snlMsgBuf), "ID=%ld;", myMsgId);

#ifdef DEBUG
            errlogPrintf("snlMsgLogTask: snlMsgBuf:%s\n", snlMsgBuf); 
#endif

	    while (1) {		/* send message again until success */
		volatile int serverSendTo;

		serverSendTo = serverSelected;	/* local copy */

		if (serverSendTo >= 0) {
		    if (sendMessageToServer (
		      snlMsgBuf, serverSendTo, myMsgId, &private) == OK) {
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
	} else {  /* no messages in snlMsgQ */
	    epicsThreadSleep (0.02);
	}
    } /* forever */

    epicsEventDestroy (private.replyEvent);
    epicsMutexDestroy (private.privateLock);
    stid = NULL;
    return OK; 
}

