#!/bin/sh
########################################################
#Installation of EPICS base 3.15 and Support Modules   #
#                                                      #
#Ali Can Canbay                         23.06.2020     #
########################################################
#EPICS_ROOT=${installation_dir}/epicsv3                #
#EPICS_BASE=${EPICS_ROOT}/base                         #
#SUPPORT=${EPICS_ROOT}/support                         #
########################################################

echo "________________________________________________________"
echo "| Default Installation Directory : /opt		        |"
echo "| If you want to change default installation directory, |"
echo "| Please remove '/opt' and write the new directory.     |"
echo "| ENTER for skip                                        |"
echo "|_______________________________________________________|"
echo 
read -p "Installation Directory: " -i /opt -e installation_dir
echo
########################################################

echo "______________________________________"
echo "| Please choose your GNU/Linux Distro |"
echo "|_____________________________________|"
echo "| 1: Centos/Fedora                    |"
echo "| 2: Ubuntu                           |"
echo "|_____________________________________|"
echo
read distro
echo

if  [ $distro -ne 1 ] && [ $distro -ne 2 ]; then
	echo "Wrong choice!"
	exit 1
fi
########################################################

echo "_________________________"
echo "| Installation Type:     |"
echo "|                        |"
echo "| 1: All Support Modules |"
echo "| 2: For Area Detector   |"
echo "|________________________|"
echo
read instalaltion_type
echo

if  [ $distro -ne 1 ] && [ $distro -ne 2 ]; then
	echo "Wrong choice!"
	exit 1
fi
########################################################

printf "\033[1;31mStarting installation\033[0m\n"
echo
########################################################

if [ $distro -eq 1 ]; then
	echo "Centos/Fedora"
	echo "Installing necessary packages"
	if [ $instalaltion_type -eq 1 ]; then
		sudo yum install -y groupinstall -y "Development Tools"
		sudo yum install -y epel-release
		sudo yum install -y readline-devel re2c
		bash install_epics-base.sh
		bash install_support.sh
	elif [ $instalaltion_type -eq 2 ]; then
		sudo yum install -y groupinstall -y "Development Tools"
		sudo yum install -y epel-release
		sudo yum install -y readline-devel re2c libtiff zlib libjpeg-turbo-devel libxml2-devel glib2-devel libXext-devel libusbx-devel glibmm24-devel gstreamer-plugins-base-devel gstreamer1-plugins-base-devel python-gobject gobject-introspection gtk-doc gtk3-devel libnotify-devel GraphicsMagick
		bash install_epics-base.sh
		bash install_support_with_areaDetector.sh
elif [ $distro -eq 2 ]; then
	echo "UBUNTU"
	echo "Installing necessary packages"
	if [ $instalaltion_type -eq 1 ]; then
		sudo apt-get install build-essential
		sudo apt-get install -y libreadline-dev re2c
		bash install_epics-base.sh
		bash install_support.sh
	elif [ $instalaltion_type -eq 2 ]; then
		sudo apt-get install build-essential
		sudo apt-get install -y libreadline-dev re2c libtiff-dev zlib libjpeg-turbo libxml2-dev libglib2.0-dev libxext-dev libusb-dev 
libgstreamer-plugins-base1.0-dev libgstreamer1 python-gobject gobject-introspection gtk-doc-tools libgtk-3-dev libnotify-dev GraphicsMagick
		bash install_epics-base.sh
		bash install_support_with_areaDetector.sh
	fi
fi
mkdir ${installation_dir}/epicsv3

########################################################
cd $MYDIR
echo
printf "\033[1;31mInstallation Complete!\033[0m\n"
echo
if [[ instalaltion_type -eq 1 ]]
then	
	echo "MAIN DIRECTORIES:
EPICS_ROOT="${installation_dir}"/epicsv3
EPICS_BASE="${installation_dir}"/epicsv3/base
SUPPORT="${installation_dir}"/epicsv3/support

SUPPORT MODULES:
SNCSEQ="${installation_dir}"/epicsv3/support/seq
IPAC="${installation_dir}"/epicsv3/support/ipac
ASYN="${installation_dir}"/epicsv3/support/asyn
AUTOSAVE="${installation_dir}"/epicsv3/support/autosave
BUSY="${installation_dir}"/epicsv3/support/busy
SSCAN="${installation_dir}"/epicsv3/support/sscan
CALC="${installation_dir}"/epicsv3/support/calc
DEVIOCSTATS="${installation_dir}"/epicsv3/support/iocStats
CAPUTLOG="${installation_dir}"/epicsv3/support/caPutLog
IOCLOGCLIENT="${installation_dir}"/epicsv3/support/iocLogClient
MODBUS="${installation_dir}"/epicsv3/support/modbus
MOTOR="${installation_dir}"/epicsv3/support/motor
S7PLC="${installation_dir}"/epicsv3/support/s7plc
STREAM="${installation_dir}"/epicsv3/support/streamDevice
"
elif [[ instalaltion_type -eq 2 ]]
then
	echo "MAIN DIRECTORIES:
EPICS_ROOT="${installation_dir}"/epicsv3
EPICS_BASE="${installation_dir}"/epicsv3/base
SUPPORT="${installation_dir}"/epicsv3/support

SUPPORT MODULES:
SNCSEQ="${installation_dir}"/epicsv3/seq
IPAC="${installation_dir}"/epicsv3/ipac
ASYN="${installation_dir}"/epicsv3/asyn
AUTOSAVE="${installation_dir}"/epicsv3/autosave
BUSY="${installation_dir}"/epicsv3/busy
SSCAN="${installation_dir}"/epicsv3/sscan
CALC="${installation_dir}"/epicsv3/calc
DEVIOCSTATS="${installation_dir}"/epicsv3/iocStats
AREADETECTOR="${installation_dir}"/epicsv3/areaDetector

AREADETECTOR MODULES
ADSUPPORT="${installation_dir}"/epicsv3/support/areaDetector/ADSupport
ADCORE="${installation_dir}"/epicsv3/support/areaDetector/ADCore
ARAVISGIGE="${installation_dir}"/epicsv3/support/aravisgigE
"
fi
echo
