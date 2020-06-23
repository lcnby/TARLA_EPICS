TOP=..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#==================================================
# build a support library

LIBRARY_IOC += iocLogClient

#USR_CFALGS += -g

# install dbd into <top>/dbd
DBD += iocLogClient.dbd

# install header file to <top>/include
INC += snlMsgLog.h

# specify all source files to be compiled and added to the library
iocLogClient_SRCS += iocLogClient.c
iocLogClient_SRCS += alarmLog.c
iocLogClient_SRCS += sysMsgLog.c
iocLogClient_SRCS += caPutLog.c
iocLogClient_SRCS += snlMsgLog.c

iocLogClient_LIBS += $(EPICS_BASE_IOC_LIBS)

#unnecessay when base is compiled with option SHARED_LIBRARIES=NO and STATIC_BUILD=YES
#iocLogClient_SYS_LIBS_WIN32 = ws2_32 advapi32 user32

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

