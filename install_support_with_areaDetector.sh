#!/bin/sh
########################################################
#Installation of EPICS support modules                 #
#                                                      #
#Ali Can Canbay                         23.06.2020     #
########################################################
#SUPPORT=${EPICS_ROOT}/support                         #
#SNCSEQ=${SUPPORT}/seq                                 #
#IPAC=${SUPPORT}/ipac                        	       #
#ASYN=${SUPPORT}/asyn                        	       #
#AUTOSAVE=${SUPPORT}/autosave                	       #
#BUSY=${SUPPORT}/busy                        	       #
#SSCAN=${SUPPORT}/sscan                      	       #
#CALC=${SUPPORT}/calc                        	       #
#DEVIOCSTATS=${SUPPORT}/iocStats             	       #
#AREADETECTOR=${SUPPORT}/areaDetector        	       #
########################################################

cp -rf $MYDIR/support ${installation_dir}/epicsv3
cd ${installation_dir}/epicsv3/support
SUPDIR="$(dirname "$(realpath "$0")")"
echo
########################################################

printf "\033[1;31mInstalling seq\033[0m\n"
cd $SUPDIR/support/seq
make
echo
########################################################

printf "\033[1;31mInstalling ipac\033[0m\n"
cd $SUPDIR/support/ipac
make
echo
########################################################

printf "\033[1;31mInstalling asyn\033[0m\n"
cd $SUPDIR/support/asyn
make
echo
########################################################

printf "\033[1;31mInstalling autosave\033[0m\n"
cd $SUPDIR/support/autosave
make
echo
########################################################

printf "\033[1;31mInstalling busy\033[0m\n"
cd $SUPDIR/support/busy
make
echo
########################################################

printf "\033[1;31mInstalling sscan\033[0m\n"
cd $SUPDIR/support/sscan
make
echo
########################################################

printf "\033[1;31mInstalling calc\033[0m\n"
cd $SUPDIR/support/calc
make
echo

########################################################
printf "\033[1;31mInstalling iocStats\033[0m\n"
cd $SUPDIR/support/iocStats
make
echo

########################################################

printf "\033[1;31mInstalling areaDetector\033[0m\n"
cd $SUPDIR/support/areaDetector
make
echo
########################################################
