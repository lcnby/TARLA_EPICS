# Makefile
TOP = ../..
include $(TOP)/configure/CONFIG

# The following are used for debugging messages.
USR_CXXFLAGS += -DDEBUG

DBD += devAcsMotor.dbd

LIBRARY_IOC = Acs

SRCS += AcsRegister.cc

# Advanced Control Systems driver support.
SRCS += devMCB4B.c drvMCB4B.c
SRCS += MCB4BDriver.cpp

Acs_LIBS += motor asyn
Acs_LIBS += $(EPICS_BASE_IOC_LIBS)

include $(TOP)/configure/RULES

