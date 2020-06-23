/*******************************************************************************

Project:
    ioc Logger for EPICS

File:
    iocLogClient.c

Description:
    server management.
    This file is separated from logAlarms.c which is implemented by Bernd Schoeneburg,
    and it is ported to osi.

Author:
    Bernd Schoeneburg & Gongfa Liu

Created:
    16 February 2009

Modification:
V0.1	2006-10-24	Sbg	Initial implementation
V0.2	2007-07-01	Sbg	Problem with bucketLib usage fixed, more msgTasks,
V0.3	2008-06-12	Sbg	Print VAL, DESC, VERSION, discard types, beacons:
				beaconSelected and beaconNotSelected
				use dbScanLock to access record in queue(All)Record(s)
V0.4	2008-12-12	Sbg	Value and description (TEXT) added
				logAlarmsShutdown function added
V0.4.1	2008-12-29	Sbg	Random function uses tickGet().
V0.4.2	2009-02-05	Sbg	; was missing after Version=..
V0.5	2009-02-16      Liu     separate from logAlarms.c and port to osi.
V0.5.1	2009-07-08      Liu     add snlLogMsg.

(c) 2006,2007,2008,2009 DESY

$Id: iocLogClient.c,v 1.3 2009/07/08 12:57:38 gliu Exp $ 
*******************************************************************************/
#define VERSION	"0.5"

/* C */
#include <stdio.h>
#include <stdlib.h>		/* getenv()... */
#include <string.h>		/* memset()...  */
#include <ctype.h>

/* EPICS */
#include <epicsEvent.h>		/* epicsEvent */
#include <epicsMutex.h>		/* epicsMutex */
#include <epicsThread.h>	/* epicsThread */
#include <epicsTime.h> 		/* epicsTimeStamp */
#include <osiSock.h>		/* aToIPAddr */
#include <errlog.h>	 	/* errlogPrintf() */
#include <iocsh.h>		/* iocshFuncDef... */	
#include <epicsExport.h>	/* epicsExportRegistrar() */

#include "iocLogClient.h"		

/* these name-value-pairs can appear in messages */
typedef enum {
    name_SERIAL,
    name_COMMAND,
    name_NAME,
    name_STATUS,	/* for old IC-server version (now: REPLY) */
    name_REPLY
} name_t;

/* these commands are accepted */
typedef enum {
    cmd_TAKEOVER,
    cmd_DISCONNECT,
    cmd_SENDALLALARMS,
    cmd_SENDALARM,
    cmd_SENDSTATUS
} cmd_t;

#ifdef DEBUG
int iocLogClientDebug = 6;
#endif

/* values for messageFlag */
#define SWITCH_OVER	1
#define SELECTED	0

#define DEFAULT_MSG_PORT	18324		/* port for messages */
#define DEFAULT_CMD_PORT	18325		/* port for commands */

#define IOC_LOG_SERVER_INET_LENGTH	80	/* max length of env var */

#define INIT_DELAY_SEC		15.0		/* wait at init for .. sec */
#define BEACON_PERIOD_SEC	3.0		/* check sending beacon every .. sec */
#define BEACON_IDLE_SEC		2.5		/* send beacon after .. sec idle */
/* define MSG_RETRY_SEC		0.1		** retry send msg after .. sec */
#define RETRY_MAX		8		/* max retries to send beacon
						to the selected server until switch */
#define BEACON_REPLY_TMO_SEC	0.6		/* tmo server response ... sec */
#define MESSAGE_REPLY_TMO_SEC	1.0		/* tmo server response ... sec */
#define MESSAGE_RETRY_WAIT_SEC	0.02		/* time after try to send msg */

#define LOCK_MSG_SOCK		epicsMutexLock (msgSockLock);
#define UNLOCK_MSG_SOCK		epicsMutexUnlock (msgSockLock);

#ifdef WIN32
#define strtok_r(a,b,c) strtok(a,b)
#endif

/* only static functions are declared here, the shared funtions are declared in iocLogClient.h */
static int iocLogBeaconTask ();			/* process */
static int iocLogCommandTask ();		/* server process */
static int replyReceiveTask ();			/* waiting for replies */
static int sendBeaconToServer (			/* beacon to server x (tmo) */
  int, unsigned long, taskPrivate_t *);
static int iName (const char *, name_t *);	/* recognize name */
static int iCmd (const char *, cmd_t *);	/* recognize command */
static void cleanup (int);			/* cleanup if return ERROR */
static void switchTo (int, char, taskPrivate_t *); /* select new server and send message */
static int findNewServer ();			/* find a new server which works */

static int		initialized=FALSE, allAcceptOption=FALSE;
static epicsMutexId	msgSockLock;
static epicsMutexId	getMessageIdLock;
static double		secMaxSilent;
static double		secBeaconReplyTmo;
static double		secMessageReplyTmo;
static int		numServersConfigured;
static int		serverSelectedLast, serverSwitchOver;
static struct sockaddr_in	msgReplyInAddr, msgServerAddr[SERVERS_MAX];
static struct sockaddr_in	cmdServerAddr; /* we are cmd_server */
static int		sockAddrSize=sizeof(struct sockaddr_in);
static unsigned short	msg_port, cmd_port;
static SOCKET		msg_soc, cmd_soc;
static unsigned long	beaconId=2000000000;

static epicsThreadId	btid, ctid, rtid;
static int		btidStop, ctidStop, rtidStop;

/* The following is shared variables */

taskPrivate_t	*taskPrivateTab[MSG_TASK_NUM]; 
double		secInitWait;
double		secBeaconPeriod;
double		secMsgRetryWait;
char		messageHeader[MESSAGE_HEADER_LENGTH];
int 		serverSelected;
serverStatus_t  serverStatus[SERVERS_MAX];


/*******************************************************************************
Routine:
    iocLogClientInit

Purpose:
    Initialize iocLogClient pamrameters and call alarmLogInit, sysMsgLogInit
    and caPutLogInit().

Description:

Returns:
    OK, or ERROR

*/

int iocLogClientInit (void)
{
    struct sockaddr_in  *psin;
    int         node;
    char        string[32];
    char        serverList[IOC_LOG_SERVER_INET_LENGTH];
    char        *pstring, *pserverList, *ptok, *pheader;

    if (initialized) {
	errlogPrintf ("iocLogClientInit: already initialized\n");
	return OK;
    }

    secInitWait = INIT_DELAY_SEC;
    secMaxSilent = BEACON_IDLE_SEC;
    secBeaconPeriod = BEACON_PERIOD_SEC;
    secBeaconReplyTmo = BEACON_REPLY_TMO_SEC;
    secMessageReplyTmo = MESSAGE_REPLY_TMO_SEC;
    /* wait time between retrying to send messages (>= 0.02 seconds) */
    secMsgRetryWait = MESSAGE_RETRY_WAIT_SEC;
    if (secMsgRetryWait == 0.0) secMsgRetryWait = 0.02;

    pserverList = getenv ("EPICS_IOC_LOG_INET_LIST");
    if (!pserverList) {
	errlogPrintf ("iocLogClientInit: EPICS_IOC_LOG_INET_LIST not defined\n");
	return ERROR;
    }
    if (strlen (pserverList) >= IOC_LOG_SERVER_INET_LENGTH)
	errlogPrintf ("iocLogClientInit: Warning: EPICS_ALARM_SERVER_INET env too long\n");

    strncpy (serverList, pserverList, IOC_LOG_SERVER_INET_LENGTH-1);
    serverList[IOC_LOG_SERVER_INET_LENGTH-1] = '\0';

    pstring = getenv ("EPICS_IOC_LOG_MSG_PORT");
    if (!pstring)
	msg_port = DEFAULT_MSG_PORT;
    else {
	msg_port = (unsigned short) atoi (pstring);
	if (msg_port < 1024) {
	    msg_port = DEFAULT_MSG_PORT;
	    errlogPrintf ("Port number EPICS_IOC_LOG_MSG_PORT is wrong\n");
	}
    }

    pstring = getenv ("EPICS_IOC_LOG_CMD_PORT");
    if (!pstring)
	cmd_port = DEFAULT_CMD_PORT;
    else {
	cmd_port = (unsigned short) atoi (pstring);
	if (cmd_port < 1024) {
	    cmd_port = DEFAULT_CMD_PORT;
	    errlogPrintf ("Port number EPICS_IOC_LOG_MSG_PORT is wrong\n");
	}
    }

    /* if allAcceptOption is set, commands are accepted from all servers */
    pstring = getenv ("EPICS_IOC_LOG_CMD_ACCEPT_ALL");
    if (pstring) {	
	if (strcmp (pstring, "YES") == 0) allAcceptOption = TRUE;
    }

    psin = &msgServerAddr[0];
    node = 0;

    ptok = strtok_r (serverList, " ", &pstring);

    while (ptok && node < SERVERS_MAX && aToIPAddr (ptok, msg_port, psin) == 0) {
        node++;
        psin++;
	ptok = strtok_r (NULL, " ", &pstring);
    }
    numServersConfigured = node;

    if (numServersConfigured == 0) {
	errlogPrintf (
	  "iocLogClientInit: No server correctly defined in EPICS_IOC_LOG_INET_LIST\n");
    }

    msgSockLock = epicsMutexCreate ();
    if (!msgSockLock) {
	errlogPrintf ("iocLogClientInit: MsgSocket mutex could not be created\n");
        return ERROR;
    }

    getMessageIdLock = epicsMutexCreate ();
    if (!getMessageIdLock) {
	cleanup (1);
	errlogPrintf ("iocLogClientInit: getMessageId mutex could not be created\n");
        return ERROR;
    }

    serverSelected = NO_SERVER;
    serverSwitchOver = NO_SERVER;
    serverSelectedLast = NO_SERVER;

    btidStop = FALSE;
    /* spawn alarm beacon task */
    btid = epicsThreadCreate ("logBeacon", epicsThreadPriorityMedium+1,
				epicsThreadGetStackSize (epicsThreadStackSmall),
				(EPICSTHREADFUNC)iocLogBeaconTask, 0);

    if (!btid) {
	errlogPrintf ("iocLogClientInit: Beacon task could not be spawned\n");
	cleanup (2);
	return ERROR;
    }
    DEBUG1(1,"iocLogBeaconTask started. Task-ID = 0x%x\n", btid);

    msg_soc = epicsSocketCreate (AF_INET, SOCK_DGRAM, 0);
    if (msg_soc < 0) {
	errlogPrintf ("iocLogClientInit: Message socket create failed\n");
	cleanup (3);
	return ERROR;
    }
    memset ((char *) &msgReplyInAddr, 0, sockAddrSize);
    /* use different port to avoid conflict when client and server are in the same computer */
    msgReplyInAddr.sin_port = htons(msg_port + 10);
    msgReplyInAddr.sin_family = AF_INET;
    msgReplyInAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind (msg_soc, (struct sockaddr*)&msgReplyInAddr, sockAddrSize) < 0 ) {
	errlogPrintf ("iocLogClientInit: Message socket bind failed\n");
	cleanup (4);
	return ERROR;
    }

    cmd_soc = epicsSocketCreate (AF_INET, SOCK_DGRAM, 0);
    if (cmd_soc < 0) {
	errlogPrintf ("Command socket create failed\n");
	cleanup (4);
	return ERROR;
    }
    memset ((char *) &cmdServerAddr, 0, sockAddrSize);
    cmdServerAddr.sin_port = htons(cmd_port);
    cmdServerAddr.sin_family = AF_INET;
    cmdServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind (cmd_soc, (struct sockaddr*)&cmdServerAddr, sockAddrSize) < 0 ) {
	errlogPrintf ("Command socket bind failed\n");
	cleanup (5);
	return ERROR;
    }

    ctidStop = FALSE;
    /* spawn alarm log control (command receiver) server task */
    ctid = epicsThreadCreate ("logCommand", epicsThreadPriorityMedium+1,
				epicsThreadGetStackSize (epicsThreadStackSmall),
				(EPICSTHREADFUNC)iocLogCommandTask, 0);

    if (!ctid) {
	errlogPrintf ("iocLogClientInit: Control task could not be spawned\n");
	cleanup (5);
	return ERROR;
    }

    rtidStop = FALSE;
    /* spawn message reply receiver task */
    rtid = epicsThreadCreate ("logReplyRx", epicsThreadPriorityHigh-1,
				epicsThreadGetStackSize (epicsThreadStackSmall),
				(EPICSTHREADFUNC)replyReceiveTask, 0);

    if (!rtid) {
	errlogPrintf ("iocLogClientInit: Reply receiver task could not be spawned\n");
	cleanup (6);
	return ERROR;
    }

    pheader = messageHeader;

    pstring = getenv ("EPICS_IOC_NAME");
    if (pstring)		/* given IOC name */
	pheader = messageHeader +
	  sprintf (messageHeader, "HOST=%s;", pstring);
    pheader +=
      sprintf (pheader, "HOST-PHYS=%s;",
        gethostname (string, sizeof(string)) ? "TOO_LONG" : string);

    pstring = getenv ("EPICS_FACILITY");
    if (pstring)		/* name of application facility */
	pheader +=
	  sprintf (pheader, "FACILITY=%s;", pstring);

    alarmLogInit();
    sysMsgLogInit();
    caPutLogInit();
    snlMsgLogInit();

    initialized = TRUE;
    errlogPrintf("iocLogClientInit started\n");

    return OK;
}


/*******************************************************************************

Routine:
    iocLogClientShutdown

Purpose:
    shutdown iocLogClient

Description:

*/

int iocLogClientShutdown (void)
{
    if (!initialized) {
	errlogPrintf ("iocLogClientShutdown: iocLogClient is not initialized");
	return ERROR;
    }

    caPutLogShutdown();
    sysMsgLogShutdown();
    alarmLogShutdown();
    snlMsgLogShutdown();
    
    /* send disconnect message ?? */

    cleanup (7);
    printf ("iocLogClientShutdown: tasks stopped and recources freed - iocLogClient stopped.\n");

    initialized = FALSE;
    return OK;
}


/*******************************************************************************
Routine:
    iocLogClientReport

Purpose:
    Report status of iocLogclient

Description:

Returns:
    OK

*/

int iocLogClientReport (int interest)
{
    char        inetAddr[32];
    int		serverId;

    printf ("iocLogClientReport: version: %s\n", VERSION);
    if (!initialized) {
	printf ("iocLogClientReport: iocLogClient is not initialized! Use iocLogClientInit.\n");
	return OK;
    }

    printf ("iocLogClientReport! use level parameter of [0..4]\n\n");

    printf ("iocLogClient: %d servers are configured\n", numServersConfigured);
    if (serverSelected != NO_SERVER)
	printf ("iocLogClient: The server number %d is selected\n", serverSelected);
    else
	printf ("iocLogClient: No server is selected\n");

    if (interest > 0 && numServersConfigured > 0) {
	printf ("iocLogClient: Configured Servers\n");
	for (serverId=0; serverId < numServersConfigured; serverId++) {
	    ipAddrToA (&msgServerAddr[serverId], inetAddr, 32);
	    printf ("        Server %2d: %-20s ", serverId, inetAddr);

	    if (serverStatus[serverId].up)
		printf ("++ online");
	    else
		printf ("-- offline");
	    if (serverId == serverSelected) printf (" + selected");
	    printf ("\n");
	}
	printf ("\niocLogClient: Message port: %d  Command port: %d\n", msg_port, cmd_port);
    }

    if (interest > 3) {
	printf ("\niocLogClient: Timing: \n");
	printf ("        Initial wait time: %f seconds\n", secInitWait);
	printf ("        Max time between beacons: %f seconds\n", secMaxSilent);
	printf ("        Timeout waiting for message replies: %f seconds\n", secMessageReplyTmo);
	printf ("        Timeout waiting for beacon replies: %f seconds\n", secBeaconReplyTmo);
    }
    printf ("\n");

    alarmLogReport (interest);
    sysMsgLogReport (interest);
    caPutLogReport (interest);
    snlMsgLogReport (interest);

    return OK;
}


/*******************************************************************************

Routine:
    iocLogBeaconTask

Purpose:
    Task sends beacons to the alarm servers and manage server states.

Description:
    Check if sending beacons is necesary. If the server is offline, beacons
    are sent every time. If the server is online beacons are send only
    if for longer than "secMaxSilent" seconds no message is sent to a
    server.

*/

static int iocLogBeaconTask (void)
{
    double		secStart, secDiff;
    int			serverId, retries;
    taskPrivate_t       private;

    private.privateLock = epicsMutexCreate (); 	/* create mutex for private manipulation */
    private.replyEvent = epicsEventCreate (epicsEventEmpty); /* create semaphore for reply */
    private.serverId = NO_SERVER;
    taskPrivateTab[0] = &private;

    while (btidStop == FALSE) {
	secStart = logTimeGetCurrentDouble();
	/* first check all servers and notify, which are running */
	for (serverId=0; serverId < numServersConfigured; serverId++) {
	    if (serverStatus[serverId].up) {
		if (logTimeGetCurrentDouble() - serverStatus[serverId].secMsg > secMaxSilent) {
		    beaconId++;
		    for (retries=0; retries < RETRY_MAX; retries++) {
			if (sendBeaconToServer (serverId, beaconId, &private) == OK) {
			    serverStatus[serverId].secMsg = logTimeGetCurrentDouble();
			    break;
			}
		    }
		    if (retries == RETRY_MAX)
			serverStatus[serverId].up = FALSE;
		}
	    }
	    else {
		if (sendBeaconToServer (serverId, ++beaconId, &private) == OK) {
		    serverStatus[serverId].secMsg = logTimeGetCurrentDouble();
		    serverStatus[serverId].up = TRUE; 
		}
	    }
	}
	/* then select a new one if necessary */
	if (serverSwitchOver >= 0) {
	    switchTo (serverSwitchOver, SWITCH_OVER, &private);
	    serverSwitchOver = NO_SERVER;
	}
	else if (serverSwitchOver == OTHER_SERVER) {
	    int newServer;
	    newServer = findNewServer ();
	    if (newServer != NO_SERVER)
		switchTo (newServer, SWITCH_OVER, &private);
	    serverSwitchOver = NO_SERVER;
	}
	else if (serverSelected == NO_SERVER || !serverStatus[serverSelected].up) {
	    /* no server is active or our selected one went down */
	    int newServer;
	    serverSelected = NO_SERVER; /* stop messages to dead server */
	    newServer = findNewServer ();
	    if (newServer != NO_SERVER)
		switchTo (newServer, SELECTED, &private);
	}
	secDiff = logTimeGetCurrentDouble() - secStart;
	if (secBeaconPeriod > secDiff)
	    epicsThreadSleep (secBeaconPeriod - secDiff);
	else
	    epicsThreadSleep (0.1);
    }

    btid = NULL;
    epicsEventDestroy (private.replyEvent);
    epicsMutexDestroy (private.privateLock);
    return OK;
}
    

/*******************************************************************************

Routine:
    findNewServer

Purpose:
    Find a new IC-Server when old one fails or sent "disconnect"

Description:
    Select the next possible server which is online.
*/

int findNewServer (void)
{
    int index;
    int available=0, list[SERVERS_MAX];

    /* find out which alternative servers are online */
    for (index=0; index<numServersConfigured; index++) {
	if (serverStatus[index].up && index != serverSelectedLast)
	    list[available++] = index;
    }
    if (available) {
	return (list[rand() % available]);
    }

    if (serverStatus[serverSelectedLast].up)
	return serverSelectedLast;

    return NO_SERVER;
}


	    /* if sending message to new server is not		*/
	    /* sucessfull, selected server will not change	*/
/*******************************************************************************

Routine:
    switchTo

Purpose:
    Switch the server selection to a new one

Description:
    Send a system message to the new server. If this is successfull,
    the new server becomes the "selected" server and is used by the
    message tasks.
*/

void switchTo (int newServer, char messageFlag, taskPrivate_t *pprivate)
{
    int retry;
    unsigned long myMsgId;
    static char	messageText[MAX_MESSAGE_LENGTH+1];
    static char	*pmessage;
    static int messageInitialized=FALSE;

    if (!messageInitialized) {
	strcpy (messageText, messageHeader);
	pmessage = messageText + strlen(messageText);
	messageInitialized = TRUE;
    }

    /* send first message to new server */
    myMsgId = getMessageId();
    switch (messageFlag) {
	case SWITCH_OVER:
	    sprintf (pmessage, "ID=%lu;TYPE=sysMsg;TEXT=switchOver;", myMsgId);
	    break;
	case SELECTED:
	    sprintf (pmessage, "ID=%lu;TYPE=sysMsg;TEXT=selected;", myMsgId);
	    break;
	default:
	    errlogPrintf ("unknown messageFlag in switchTo\n");
	    sprintf (pmessage, "ID=%lu;TYPE=sysMsg;TEXT=Error - unclear switch;", myMsgId);
	    break;
    }
    /* try to send a "welcome" message to the new server */
    /* if sending is successfull, server is selected     */
    for (retry=0; retry<5; retry++) {
	if (sendMessageToServer (messageText, newServer, myMsgId, pprivate) == OK) {
	    serverStatus[newServer].secMsg = logTimeGetCurrentDouble();
	    serverSelected = newServer;	/* accept server */
	    serverSelectedLast = serverSelected;
	    break; /* message sent! */
        }
	/* error in transmission, retry after short delay */
	epicsThreadSleep (secMsgRetryWait);
    }
}


/*******************************************************************************

Routine:
    replyReceiveTask

Purpose:
    Task receiving replies for messages and beacons from the alarm servers

Description:
    When a reply arrives on the message port, it looks into a distribution
    table and copies the result to a buffer. Finally it releases a sema-
    phor to set the waiting message- or beacon task into ready state.

*/

static int replyReceiveTask ()
{
    char		rx_buf[RX_BUF_SIZE+1];
    int			mlen, flen;
    char		*pstring = NULL, *ptok, *pvalue;
    int			idExist;
    int			replyStatus;
    unsigned long 	replyId;
    taskPrivate_t	*pPvt;
    struct sockaddr_in	reply_addr;
    int			ti;

    DEBUG1(1,"replyReceiveTask entry; msg_sock=%d\n",msg_soc);
    while (rtidStop == FALSE) {
	flen = sockAddrSize;
	mlen = recvfrom (msg_soc, rx_buf, RX_BUF_SIZE, 0,
	  (struct sockaddr *)&reply_addr, &flen);
	if (mlen < 0) {
	    DEBUG0(2,"recvfrom failed - cheksum error?\n");
	    continue;
	}
	DEBUG1(3,"received %d chars\n",mlen);
	rx_buf[mlen] = '\0';

	/* we got something from somewhere on our port */

	/* now analyze the reply from the server */
	idExist = FALSE;
	replyStatus = ERROR;
	replyId = 0;

	ptok = strtok_r (rx_buf, "=", &pstring);
	while (ptok) {
	    char *pnext;
	    name_t i_name;

	    pvalue = strtok_r (NULL, ";", &pstring);

	    if (iName (ptok, &i_name) == OK) {
		switch (i_name) {
		    case name_SERIAL:
			replyId = strtoul (pvalue, &pnext, 10);
			idExist = (pvalue != pnext);
			break;
		    case name_STATUS:	/* for old IC-server version (now: REPLY) */
		    case name_REPLY:
			if (strcmp (pvalue, "Ok") == 0)
			    replyStatus = OK;
			break;
		/*  case ....break; */
		    default:
			DEBUG2(1,"Unexpected item in reply message %s=%s\n",
			 ptok, pvalue)
		}
	    }
	    else
		DEBUG2(1,"Undefined item in reply message %s=%s\n",
		 ptok, pvalue)

	    ptok = strtok_r (NULL, "=", &pstring);	/* next name-value-pair */
	}

	/* test serial number */
	if (!idExist) {
	    DEBUG0(1,"Serial number of server reply missing\n")
	    replyId = 0;
	    continue;
	}

	DEBUG2(3,"received reply: serial=%ld status=%d\n",replyId,replyStatus);

	for (ti=0; ti < MSG_TASK_NUM; ti++) {
	    pPvt = taskPrivateTab[ti];

	    if (!pPvt) continue;

	    epicsMutexLock (pPvt->privateLock);

	    /* test if correct server answered and ID is what we wait for */
	    if ( (pPvt->serverId >= 0) && 
		 (reply_addr.sin_addr.s_addr == msgServerAddr[pPvt->serverId].sin_addr.s_addr) &&
	     	 (replyId == pPvt->id) ) {

		DEBUG3(5,"return status %d to task %d id=%ld\n",
		 replyStatus,ti,pPvt->id);
		pPvt->replyStatus = replyStatus;
		epicsEventSignal (pPvt->replyEvent);	/* trigger msg or beacon task */
		epicsMutexUnlock (pPvt->privateLock);	/* release private access */
		break;	/* dont try other, waiting task found */
	    }
	    epicsMutexUnlock (pPvt->privateLock);		/* release private access */
	}
    } /* forever */

    rtid = NULL;
    return OK; 
}

/*******************************************************************************

Routine:
    iocLogCommandTask

Purpose:
    Task waits for commands from servers.

Description:
    If a command arrives on the command port, it is executed. The command
    "takeOver" will switch the active server to that one the command has sent.
    Any messages from nodes which are not configured are ignored. When 
    allAcceptOption is FALSE, Commands from servers whichare not selected
    are refused except the "takeOver" command.
*/

static int iocLogCommandTask (void)
{
    int			serverIdRecvd, ix;
    int			mlen, flen;
    struct sockaddr_in	cmdRecvdAddr;
    char		cmd_buf[RX_BUF_SIZE+1];
    char		*pstring, *ptok;
    char		*pname, *pvalue=NULL, *pstatusString=NULL;
    char		replyString[64];
    int			idExist, cmdExist;
    unsigned long	id=0;
    cmd_t		cmdId;
    int			cmdStatus=ERROR;


    while (ctidStop == FALSE) {
	
	flen = sockAddrSize;
	mlen = recvfrom (cmd_soc, cmd_buf, RX_BUF_SIZE, 0,
	  (struct sockaddr *)&cmdRecvdAddr, &flen);
	if (mlen < 0) {
	    errlogPrintf ("recvfrom failed - cheksum error?\n");
	    continue;
	}
	cmd_buf[mlen] = '\0';

	DEBUG1(4,"Command received: %s\n", cmd_buf);

	/* we got something from somewhere on our port */

	/* now analyze the string from the server */

	idExist = FALSE;
	cmdExist = FALSE;
	pname = NULL;
	ptok = strtok_r (cmd_buf, "=", &pstring);
	while (ptok) {
	    name_t i_name;

	    pvalue = strtok_r (NULL, ";", &pstring);
	    if (iName (ptok, &i_name) == OK) {
		char *pnext;

		switch (i_name) {
		    case name_SERIAL:
			id = strtoul (pvalue, &pnext, 10);
			idExist = (pvalue != pnext);
			break;
		    case name_COMMAND:
			cmdExist = TRUE;
			cmdStatus = iCmd (pvalue, &cmdId);
			break;
		    case name_NAME:
			pname = pvalue;
			if (!*pname) pname = NULL;
			break;
		/*  case ....break; */
		    default:
			DEBUG2(1,"Unexpected item for ctlTask %s=%s\n",
			  ptok, pvalue)
		}
	    }
	    else
		DEBUG2(1,"Undefined item in control message %s=%s\n",
		  ptok, pvalue)

	    ptok = strtok_r (NULL, "=", &pstring);	/* next name-value-pair */
	}

	/* did this command message arrive from our selected server? */
	serverIdRecvd = NO_SERVER;
	if ((ix = serverSelected) != NO_SERVER &&
	  cmdRecvdAddr.sin_addr.s_addr == msgServerAddr[ix].sin_addr.s_addr)
	    serverIdRecvd = serverSelected;
	else {
	    int serverId;

	    for (serverId=0; serverId < numServersConfigured; serverId++) {
		if (cmdRecvdAddr.sin_addr.s_addr == msgServerAddr[serverId].sin_addr.s_addr) {
		    serverIdRecvd = serverId;
		    break;
		}
	    }
	    if (serverIdRecvd < 0) {
#ifdef DEBUG
		if (iocLogClientDebug) {
		    char	inetAddr[32];

		    ipAddrToA (&cmdRecvdAddr, inetAddr, 32);
		    DEBUG1(1,"Message from unknown sender %s\n", inetAddr)
		}
#endif
		continue;	/* ignore this message */
	    }
	}


	if (cmdExist) {
	  if (cmdStatus == OK) {
	    switch (cmdId) {
		case cmd_TAKEOVER:
		    if (serverIdRecvd != serverSelected) {
			if (serverStatus[serverIdRecvd].up) {
			    serverSwitchOver = serverIdRecvd;
			    pstatusString = "done;";
			}
			else {
			    pstatusString = "error;";
			}
		    }
		    else
			pstatusString = "ok;";
		    break;
		case cmd_DISCONNECT:
		    if (serverIdRecvd == serverSelected) {
			serverSwitchOver = OTHER_SERVER;
			pstatusString = "done;";
		    }
		    else
			pstatusString = "ok;";
		    break;
		case cmd_SENDALLALARMS:
		    if (allAcceptOption || serverIdRecvd == serverSelected) {
			queueAllRecords();
			pstatusString = "done;";
		    }
		    else
			pstatusString = "refused;";
		    break;
		case cmd_SENDALARM:
		    if (allAcceptOption || serverIdRecvd == serverSelected) {
			if (pname && queueRecord (pname) == OK)
			    pstatusString = "done;";
			else
			    pstatusString = "error;";
		    }
		    else
			pstatusString = "refused;";
		    break;
		case cmd_SENDSTATUS:
		    pstatusString = (serverIdRecvd == serverSelected) ?
		      "selected;" : "notSelected;";
		    break;
		default:
		    DEBUG1(1,"Unexpected Command: %s\n", pvalue)
	    }
	  }
	  else {
	      DEBUG1(1,"Not a Command: %s\n", pvalue)
	      pstatusString = "cmdUnknown;";
	  }
	}
	else
	    pstatusString = "cmdMissing;";

	replyString[0] = '\0';

	if (idExist)
	    sprintf (replyString, "ID=%ld;VERSION=%s;", id, VERSION);
	strcat (replyString, "REPLY=");
	strcat (replyString, pstatusString);

	/* send a reply to the server we got the message from */
	sendto (cmd_soc, replyString, strlen(replyString)+1, 0,
	  (struct sockaddr*)&cmdRecvdAddr, sockAddrSize);
    }  /* forever */

    ctid = NULL;
    return OK;
}


/*******************************************************************************

Routine:
    iName

Purpose:
    Check if a name in a command string is identical to a token name.

Description:
    Return an index of the discovered token.

Retuns:
    OK (token found) or ERROR (not found)
*/

static int iName (const char *text, name_t *index)
{
    int i;

    static struct {name_t index; const char *text;} nameTab[] = {
	{ name_SERIAL, "ID" },
	{ name_COMMAND, "COMMAND" },
	{ name_NAME, "NAME" },
	{ name_STATUS, "STATUS" },	/* for old IC-server version (now: REPLY) */
	{ name_REPLY, "REPLY" }
    };

    for (i=0; i<5; i++) {
	if (strcmp (nameTab[i].text, text) == 0) {
	    *index = nameTab[i].index;
	    return OK;
	}
    }
    return ERROR;
}


/*******************************************************************************

Routine:
    iCmd

Purpose:
    Check if a name in a valid command.

Description:
    Return an index of the discovered command.

Retuns:
    OK (valid command found) or ERROR (not found)
*/

static int iCmd (const char *text, cmd_t *index)
{
    int i;

    static struct {cmd_t index; const char *text;} cmdTab[] = {
	{ cmd_TAKEOVER, "takeOver" },
	{ cmd_DISCONNECT, "disconnect" },
	{ cmd_SENDALLALARMS, "sendAllAlarms" },
	{ cmd_SENDALARM, "sendAlarm" },
	{ cmd_SENDSTATUS, "sendStatus" }
    };

    for (i=0; i<5; i++) {
	if (strcmp (cmdTab[i].text, text) == 0) {
	    *index = cmdTab[i].index;
	    return OK;
	}
    }
    return ERROR;
}


/*******************************************************************************

Routine:
    sendMessageToServer

Purpose:
    Send a message (not beacon) to the IC-Server over UDP/IP

Description:
    Send message, register ID and wait for reply.

Retuns:
    OK or ERROR (not sent or no reply)
*/

int sendMessageToServer (const char *ptext, int serverId,
	unsigned long messageId, taskPrivate_t *pPvt)
{

    epicsMutexLock (pPvt->privateLock);

    pPvt->id = messageId;
    pPvt->serverId = serverId;
    epicsEventTryWait (pPvt->replyEvent);

    epicsMutexUnlock (pPvt->privateLock);

    /* send to network */
    LOCK_MSG_SOCK

    if (sendto (msg_soc, (char*)ptext, strlen(ptext)+1, 0,
      (struct sockaddr*)&msgServerAddr[serverId], sockAddrSize) < 0) {
	UNLOCK_MSG_SOCK
	DEBUG2(1,"Send message %ld to server %d: sendto failed\n",
	 messageId, serverId);
	pPvt->serverId = NO_SERVER;
	return ERROR;
    }
    UNLOCK_MSG_SOCK

    DEBUG1(5,"msg %ld sent, now wait...\n",pPvt->id);

    if (epicsEventWaitWithTimeout (pPvt->replyEvent, secMessageReplyTmo) == ERROR) {
	DEBUG1(2,"Message to server %d timed out\n", serverId);
	pPvt->serverId = NO_SERVER;
	return ERROR;
    }

    pPvt->serverId = NO_SERVER;
    return pPvt->replyStatus;
}


/*******************************************************************************

Routine:
    sendBeaconToServer

Purpose:
    Send a beacon to the IC-Server over UDP/IP

Description:
    Send beacon, register ID and wait for reply.
    The beacon text depends on the selection state

Retuns:
    OK or ERROR (not sent or no reply)

*/

static int sendBeaconToServer (int serverId, unsigned long beaconId, taskPrivate_t *pPvt)
{
    char	message[40];

    if (serverId == serverSelected)
	sprintf(message, "ID=%ld;TYPE=beaconSelected;", beaconId);
    else
	sprintf(message, "ID=%ld;TYPE=beaconNotSelected;", beaconId);

    epicsMutexLock (pPvt->privateLock);
    pPvt->id = beaconId;
    pPvt->serverId = serverId;
    epicsEventTryWait (pPvt->replyEvent);
    epicsMutexUnlock (pPvt->privateLock);

    /* send to network */
    LOCK_MSG_SOCK
    if (sendto (msg_soc, (char*)message, strlen(message)+1, 0,
      (struct sockaddr*)&msgServerAddr[serverId], sockAddrSize) < 0) {
	UNLOCK_MSG_SOCK
	DEBUG1(1,"Send beacon to server %d: sendto failed\n", serverId);
	pPvt->serverId = NO_SERVER;
	return ERROR;
    }
    UNLOCK_MSG_SOCK

    if (epicsEventWaitWithTimeout (pPvt->replyEvent, secBeaconReplyTmo) == epicsEventWaitTimeout) {
	DEBUG1(2,"Beacon to server %d timed out\n", serverId);
	pPvt->serverId = NO_SERVER;
	return ERROR;
    }

    pPvt->serverId = NO_SERVER;
    return pPvt->replyStatus;
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
    char trigString[32] = "ID=2147483647"; /* 2147483647 = 0x7fffffff */

    switch (step) {
    case 7: rtidStop = TRUE; while (rtid) epicsThreadSleep(0.02);
    case 6: ctidStop = TRUE;
	    sendto (cmd_soc, trigString, strlen(trigString)+1, 0,
	  		(struct sockaddr*)&cmdServerAddr, sockAddrSize);
	    while (ctid) epicsThreadSleep(0.02);
    case 5: epicsSocketDestroy (cmd_soc);
    case 4: epicsSocketDestroy (msg_soc);
    case 3: btidStop = TRUE; while (btid) epicsThreadSleep(0.02); 
    case 2: epicsMutexDestroy (getMessageIdLock);
    case 1: epicsMutexDestroy (msgSockLock);
    }
}


/*******************************************************************************

Routine:
    stampToText

Purpose:
    convert a time stamp to text JMS conform

Description:
    An EPICS standard time stamp is converted to text. The text
    contains the time stamp's representation in the local time zone,
    taking daylight savings time into account.

    The required size of the caller's text buffer depends on the type
    of conversion being requested.  The conversion types, buffer sizes,
    and conversion format is:

    yyyy-mm-dd hh:mm:ss.mmm         mmm=milliseconds

Retuns:
    pointer to buffer

*/

char * stampToText (epicsTimeStamp *pStamp, char *textBuffer)
{
    if (textBuffer != NULL) {
	epicsTimeToStrftime(textBuffer, 30, "%Y-%m-%d %H:%M:%S.%03f", pStamp);
	return textBuffer;
    }
    else
	return NULL;
}


/*******************************************************************************

Routine:
    getMessageId

Purpose:
    get a new message ID (atomic)

Description:
    This is the only function that has access to the global messageId.
    Task switching during read-modify-write operation is prevented
    by interrupt disabling.

Retuns:
    New message ID (ulong)

*/

unsigned long getMessageId ()
{
    static unsigned long	messageId = 0L;
    volatile unsigned long	value;

    epicsMutexMustLock (getMessageIdLock);
    value = ++messageId;
    epicsMutexUnlock (getMessageIdLock);

    return value;
}


/*******************************************************************************

Routine:
    logTimeGetCurrentDouble

Purpose:
    Get the current time in doubel, unit is second. It's for replacing tickGet(). 

Description:
    The result is 15 effective number, e.g. 567349816.374799 seconds, the resolution
    is enough.

Retuns:
    the current time in doubel (seconds)

*/

double logTimeGetCurrentDouble()
{
    epicsTimeStamp stamp;
    double time;

    time = 0.0;
    if ( epicsTimeGetCurrent( &stamp ) == epicsTimeERROR )
        return time;

    time = ( double ) stamp.secPastEpoch +  ( ( double ) stamp.nsec / 1e9 );
    return time;
}


/*******************************************************************************

  iocsh stuff

*/

static const iocshFuncDef iocLogClientInitFuncDef = {"iocLogClientInit", 0, NULL};
static void iocLogClientInitCallFunc (const iocshArgBuf *args)
{
    iocLogClientInit();
}

static const iocshFuncDef iocLogClientShutdownFuncDef = {"iocLogClientShutdown", 0, NULL};
static void iocLogClientShutdownCallFunc (const iocshArgBuf *args)
{
    iocLogClientShutdown();
}

static const iocshArg iocLogClientReportArg0 = {"level",iocshArgInt};
static const iocshArg * const iocLogClientReportArgs[1] = {&iocLogClientReportArg0};
static const iocshFuncDef iocLogClientReportFuncDef = {"iocLogClientReport",1,iocLogClientReportArgs};
static void iocLogClientReportCallFunc(const iocshArgBuf *args)
{
    iocLogClientReport (args[0].ival);
}

static const iocshFuncDef alarmLogInitFuncDef = {"alarmLogInit", 0, NULL};
static void alarmLogInitCallFunc (const iocshArgBuf *args)
{
    alarmLogInit();
}

static const iocshFuncDef alarmLogShutdownFuncDef = {"alarmLogShutdown", 0, NULL};
static void alarmLogShutdownCallFunc (const iocshArgBuf *args)
{
    alarmLogShutdown();
}

static const iocshArg alarmLogReportArg0 = {"level",iocshArgInt};
static const iocshArg * const alarmLogReportArgs[1] = {&alarmLogReportArg0};
static const iocshFuncDef alarmLogReportFuncDef = {"alarmLogReport",1,alarmLogReportArgs};
static void alarmLogReportCallFunc(const iocshArgBuf *args)
{
    alarmLogReport (args[0].ival);
}

static const iocshFuncDef sysMsgLogInitFuncDef = {"sysMsgLogInit", 0, NULL};
static void sysMsgLogInitCallFunc (const iocshArgBuf *args)
{
    sysMsgLogInit();
}

static const iocshFuncDef sysMsgLogShutdownFuncDef = {"sysMsgLogShutdown", 0, NULL};
static void sysMsgLogShutdownCallFunc (const iocshArgBuf *args)
{
    sysMsgLogShutdown();
}

static const iocshArg sysMsgLogReportArg0 = {"level",iocshArgInt};
static const iocshArg * const sysMsgLogReportArgs[1] = {&sysMsgLogReportArg0};
static const iocshFuncDef sysMsgLogReportFuncDef = {"sysMsgLogReport",1,sysMsgLogReportArgs};
static void sysMsgLogReportCallFunc(const iocshArgBuf *args)
{
    sysMsgLogReport (args[0].ival);
}

static const iocshFuncDef caPutLogInitFuncDef = {"caPutLogInit", 0, NULL};
static void caPutLogInitCallFunc (const iocshArgBuf *args)
{
    caPutLogInit();
}

static const iocshFuncDef caPutLogShutdownFuncDef = {"caPutLogShutdown", 0, NULL};
static void caPutLogShutdownCallFunc (const iocshArgBuf *args)
{
    caPutLogShutdown();
}

static const iocshArg caPutLogReportArg0 = {"level",iocshArgInt};
static const iocshArg * const caPutLogReportArgs[1] = {&caPutLogReportArg0};
static const iocshFuncDef caPutLogReportFuncDef = {"caPutLogReport",1,caPutLogReportArgs};
static void caPutLogReportCallFunc(const iocshArgBuf *args)
{
    caPutLogReport (args[0].ival);
}

static const iocshFuncDef snlMsgLogInitFuncDef = {"snlMsgLogInit", 0, NULL};
static void snlMsgLogInitCallFunc (const iocshArgBuf *args)
{
    snlMsgLogInit();
}

static const iocshFuncDef snlMsgLogShutdownFuncDef = {"snlMsgLogShutdown", 0, NULL};
static void snlMsgLogShutdownCallFunc (const iocshArgBuf *args)
{
    snlMsgLogShutdown();
}

static const iocshArg snlMsgLogReportArg0 = {"level",iocshArgInt};
static const iocshArg * const snlMsgLogReportArgs[1] = {&snlMsgLogReportArg0};
static const iocshFuncDef snlMsgLogReportFuncDef = {"snlMsgLogReport",1,snlMsgLogReportArgs};
static void snlMsgLogReportCallFunc(const iocshArgBuf *args)
{
    snlMsgLogReport (args[0].ival);
}

static void iocLogClientInitRegister(void)
{
    iocshRegister(&iocLogClientInitFuncDef, iocLogClientInitCallFunc);
    iocshRegister(&iocLogClientShutdownFuncDef, iocLogClientShutdownCallFunc);
    iocshRegister(&iocLogClientReportFuncDef, iocLogClientReportCallFunc);
    iocshRegister(&alarmLogInitFuncDef, alarmLogInitCallFunc);
    iocshRegister(&alarmLogShutdownFuncDef, alarmLogShutdownCallFunc);
    iocshRegister(&alarmLogReportFuncDef, alarmLogReportCallFunc);
    iocshRegister(&sysMsgLogInitFuncDef, sysMsgLogInitCallFunc);
    iocshRegister(&sysMsgLogShutdownFuncDef, sysMsgLogShutdownCallFunc);
    iocshRegister(&sysMsgLogReportFuncDef, sysMsgLogReportCallFunc);
    iocshRegister(&caPutLogInitFuncDef, caPutLogInitCallFunc);
    iocshRegister(&caPutLogShutdownFuncDef, caPutLogShutdownCallFunc);
    iocshRegister(&caPutLogReportFuncDef, caPutLogReportCallFunc);
    iocshRegister(&snlMsgLogInitFuncDef, snlMsgLogInitCallFunc);
    iocshRegister(&snlMsgLogShutdownFuncDef, snlMsgLogShutdownCallFunc);
    iocshRegister(&snlMsgLogReportFuncDef, snlMsgLogReportCallFunc);
}

epicsExportRegistrar(iocLogClientInitRegister);

