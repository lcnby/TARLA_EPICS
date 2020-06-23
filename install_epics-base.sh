#!/bin/sh
########################################################
#Installation of EPICS base 3.15                       #
#                                                      #
#Ali Can Canbay                         23.06.2020     #
########################################################
#EPICS_ROOT=${installation_dir}/epicsv3                #
#EPICS_BASE=${EPICS_ROOT}/base/                        #
########################################################

MYDIR="$(dirname "$(realpath "$0")")"
########################################################

printf "\033[1;31mInstalling EPICS Base 3.15\033[0m\n"
cp -rf $MYDIR/base ${installation_dir}/epicsv3
cd ${installation_dir}/epicsv3/base
make -j3
echo
########################################################

printf "\033[1;31mSetting up the EPICS Base 3.15 enviroment variables\033[0m\n"
#
echo '#EPICS BASE
export EPICS_ROOT='${installation_dir}'/epicsv3
export EPICS_BASE=${EPICS_ROOT}/base
export EPICS_HOST_ARCH=`${EPICS_BASE}/startup/EpicsHostArch`
export EPICS_BASE_BIN=${EPICS_BASE}/bin/${EPICS_HOST_ARCH}
export EPICS_BASE_LIB=${EPICS_BASE}/lib/${EPICS_HOST_ARCH}
if [ "" = "${LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH=${EPICS_BASE_LIB}
else
    export LD_LIBRARY_PATH=${EPICS_BASE_LIB}:${LD_LIBRARY_PATH}
fi
export PATH=${PATH}:${EPICS_BASE_BIN}
##' >> ~/.bashrc
. ~/.bashrc
echo
########################################################
