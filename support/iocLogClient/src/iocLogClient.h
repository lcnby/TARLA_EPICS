/*******************************************************************************

Project:
    ioc Logger for EPICS

File:
    iocLogClient.h

Description:

Author:
    Bernd Schoeneburg & Gongfa Liu

Created:
    16 February 2009

Modification:
2006-10-24	Sbg	Initial implementation
2009-02-16	Liu	separate from logAlarms.c which is implemented by Sbg
2009-07-08	Liu	add snl message log

(c) 2006,2007,2008,2009 DESY

$Id: iocLogClient.h,v 1.2 2009/07/08 12:57:52 gliu Exp $ 
*******************************************************************************/

#ifndef INCiocLogClienth
#define INCiocLogClienth 1
#include "shareLib.h"

#if 0 /* activate debugging here */
#define DEBUG
#endif

#ifdef DEBUG
epicsShareExtern int iocLogClientDebug;
#define DEBUG0(level,format) {if(iocLogClientDebug>=level) errlogPrintf(format);epicsThreadSleep(0.04);}
#define DEBUG1(level,format,p1) {if(iocLogClientDebug>=level) errlogPrintf(format,p1);epicsThreadSleep(0.04);}
#define DEBUG2(level,format,p1,p2) {if(iocLogClientDebug>=level) errlogPrintf(format,p1,p2);epicsThreadSleep(0.04);}
#define DEBUG3(level,format,p1,p2,p3) {if(iocLogClientDebug>=level) errlogPrintf(format,p1,p2,p3);epicsThreadSleep(0.04);}
#else
#define DEBUG0(level,format) {}
#define DEBUG1(level,format,p1) {}
#define DEBUG2(level,format,p1,p2) {}
#define DEBUG3(level,format,p1,p2,p3) {}
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef OK
#define OK 0
#endif
#ifndef ERROR
#define ERROR -1
#endif

#define NO_SERVER	-1
#define OTHER_SERVER	-2

#define MAX_MESSAGE_LENGTH	511		/* length of messages */
#define MESSAGE_HEADER_LENGTH	256		/* length of messages header */
#define RX_BUF_SIZE		127		/* server replies and commands */

#define SERVERS_MAX		6		/* max number of servers */

typedef struct {
  int 		up;
  double 	secMsg;		/* add beacon response time(secondes) here */
} serverStatus_t;		/* for reply distribution */

/* structure to distribute replies */
typedef struct {
    unsigned long	id;
    int			serverId;
    int			replyStatus;
    epicsEventId	replyEvent;
    epicsMutexId	privateLock;
} taskPrivate_t;

/* !!! the element number SHOULD be increased when new kind of msg client is added
 * taskPrivateTab[0]: iocLogBeaconTask
 * taskPrivateTab[1]: Al'Msg1
 * taskPrivateTab[2]: Al'Msg2
 * taskPrivateTab[3]: Al'Msg3
 * taskPrivateTab[4]: Al'Msg4
 * taskPrivateTab[5]: Al'Msg5
 * taskPrivateTab[6]: sysMsgLogTask
 * taskPrivateTab[7]: caPutLogTask
 * taskPrivateTab[8]: snlMsgLogTask
 */
#define MSG_TASK_NUM		9
epicsShareExtern taskPrivate_t  *taskPrivateTab[MSG_TASK_NUM];

epicsShareExtern double		secInitWait;
epicsShareExtern double		secBeaconPeriod;
epicsShareExtern double		secMsgRetryWait;
epicsShareExtern char           messageHeader[MESSAGE_HEADER_LENGTH];
epicsShareExtern int            serverSelected;
epicsShareExtern serverStatus_t serverStatus[SERVERS_MAX];

/* function declaration */
epicsShareExtern int alarmLogInit(void);
epicsShareExtern int alarmLogShutdown(void);
epicsShareExtern int alarmLogReport(int interest);
epicsShareExtern int  queueRecord (const char *);
epicsShareExtern void queueAllRecords ();

epicsShareExtern int sysMsgLogInit(void);
epicsShareExtern int sysMsgLogShutdown(void);
epicsShareExtern int sysMsgLogReport(int interest);

epicsShareExtern int caPutLogInit(void);
epicsShareExtern int caPutLogShutdown(void);
epicsShareExtern int caPutLogReport(int interest);

epicsShareExtern int snlMsgLogInit(void);
epicsShareExtern int snlMsgLogShutdown(void);
epicsShareExtern int snlMsgLogReport(int interest);
epicsShareExtern int snlMsgLog(const char *fmt, ...);

epicsShareExtern int iocLogClientInit(void);
epicsShareExtern int iocLogClientShutdown(void);
epicsShareExtern int iocLogClientReport(int interest);
epicsShareExtern int sendMessageToServer (const char *, 
  int, unsigned long, taskPrivate_t *);			/* msg to server x (tmo) */
epicsShareExtern char * stampToText (
  epicsTimeStamp *pStamp, char *textBuffer);		/* EPICS TS to Text */
epicsShareExtern unsigned long getMessageId ();		/* get net unique message id */
epicsShareExtern double logTimeGetCurrentDouble ();	/* get the current time in doubele (second) */

#endif /*INCiocLogClienth*/
