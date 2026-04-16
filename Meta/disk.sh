#!/bin/bash
set -euo pipefail

[ -z "${THEOS_DISK_SIZE:-}" ] && THEOS_DISK_SIZE=512M

[ -z "${THEOS_DISK_NAME:-}" ] && THEOS_DISK_NAME="disk.img"

[ -z "${THEOS_BASE_FOLDER:-}" ] && THEOS_BASE_FOLDER="../Base"
[ -z "${THEOS_USERLAND_APP:-}" ] && THEOS_USERLAND_APP="Userland/Apps/TheApp/TheApp"
[ -z "${THEOS_USERLAND_SHELL:-}" ] && THEOS_USERLAND_SHELL="Userland/Apps/TheShell/TheShell"
[ -z "${THEOS_USERLAND_TEST:-}" ] && THEOS_USERLAND_TEST="Userland/Apps/TheTest/TheTest"
[ -z "${THEOS_USERLAND_POWERMANAGER:-}" ] && THEOS_USERLAND_POWERMANAGER="Userland/Apps/ThePowerManager/ThePowerManager"
[ -z "${THEOS_USERLAND_SYSTEMMONITOR:-}" ] && THEOS_USERLAND_SYSTEMMONITOR="Userland/Apps/TheSystemMonitor/TheSystemMonitor"
[ -z "${THEOS_USERLAND_SYSTEMMONITORGUI:-}" ] && THEOS_USERLAND_SYSTEMMONITORGUI="Userland/Apps/TheSystemMonitorGUI/TheSystemMonitorGUI"
[ -z "${THEOS_USERLAND_WINDOWSERVER:-}" ] && THEOS_USERLAND_WINDOWSERVER="Userland/Apps/TheWindowServer/TheWindowServer"
[ -z "${THEOS_USERLAND_SHELLGUI:-}" ] && THEOS_USERLAND_SHELLGUI="Userland/Apps/TheShellGUI/TheShellGUI"
[ -z "${THEOS_DRIVERLAND_DHCPD:-}" ] && THEOS_DRIVERLAND_DHCPD="Driverland/Daemons/TheDHCPd/TheDHCPd"
[ -z "${THEOS_USERLAND_MICROPY:-}" ] && THEOS_USERLAND_MICROPY="Userland/Apps/TheMicroPython/TheMicroPython"
[ -z "${THEOS_USERLAND_MICROPY_SCRIPTS:-}" ] && THEOS_USERLAND_MICROPY_SCRIPTS="Userland/Apps/TheMicroPython/scripts"
[ -z "${THEOS_USERLAND_EMBEDDEDDOOM:-}" ] && THEOS_USERLAND_EMBEDDEDDOOM="Userland/Apps/TheEmbeddedDOOM/embeddedDOOM"
[ -z "${THEOS_USERLAND_LIBC_SO:-}" ] && THEOS_USERLAND_LIBC_SO="Userland/Libraries/LibC/libc.so"
[ -z "${THEOS_USERLAND_LIBTHETESTDYN_SO:-}" ] && THEOS_USERLAND_LIBTHETESTDYN_SO="Userland/Libraries/LibTheTestDyn/libthetestdyn.so"

if [ -n "${THEOS_USERLAND_DHCPD:-}" ] && [ -f "${THEOS_USERLAND_DHCPD}" ]; then
	THEOS_DRIVERLAND_DHCPD="${THEOS_USERLAND_DHCPD}"
fi


if [ ! -f "$THEOS_USERLAND_APP" ] && [ -f "Build/Userland/Apps/TheApp/TheApp" ]; then
	THEOS_USERLAND_APP="Build/Userland/Apps/TheApp/TheApp"
fi
if [ ! -f "$THEOS_USERLAND_SHELL" ] && [ -f "Build/Userland/Apps/TheShell/TheShell" ]; then
	THEOS_USERLAND_SHELL="Build/Userland/Apps/TheShell/TheShell"
fi
if [ ! -f "$THEOS_USERLAND_TEST" ] && [ -f "Build/Userland/Apps/TheTest/TheTest" ]; then
	THEOS_USERLAND_TEST="Build/Userland/Apps/TheTest/TheTest"
fi
if [ ! -f "$THEOS_USERLAND_POWERMANAGER" ] && [ -f "Build/Userland/Apps/ThePowerManager/ThePowerManager" ]; then
	THEOS_USERLAND_POWERMANAGER="Build/Userland/Apps/ThePowerManager/ThePowerManager"
fi
if [ ! -f "$THEOS_USERLAND_SYSTEMMONITOR" ] && [ -f "Build/Userland/Apps/TheSystemMonitor/TheSystemMonitor" ]; then
	THEOS_USERLAND_SYSTEMMONITOR="Build/Userland/Apps/TheSystemMonitor/TheSystemMonitor"
fi
if [ ! -f "$THEOS_USERLAND_SYSTEMMONITORGUI" ] && [ -f "Build/Userland/Apps/TheSystemMonitorGUI/TheSystemMonitorGUI" ]; then
	THEOS_USERLAND_SYSTEMMONITORGUI="Build/Userland/Apps/TheSystemMonitorGUI/TheSystemMonitorGUI"
fi
if [ ! -f "$THEOS_USERLAND_WINDOWSERVER" ] && [ -f "Build/Userland/Apps/TheWindowServer/TheWindowServer" ]; then
	THEOS_USERLAND_WINDOWSERVER="Build/Userland/Apps/TheWindowServer/TheWindowServer"
fi
if [ ! -f "$THEOS_USERLAND_SHELLGUI" ] && [ -f "Build/Userland/Apps/TheShellGUI/TheShellGUI" ]; then
	THEOS_USERLAND_SHELLGUI="Build/Userland/Apps/TheShellGUI/TheShellGUI"
fi
if [ ! -f "$THEOS_DRIVERLAND_DHCPD" ] && [ -f "Build/Driverland/Daemons/TheDHCPd/TheDHCPd" ]; then
	THEOS_DRIVERLAND_DHCPD="Build/Driverland/Daemons/TheDHCPd/TheDHCPd"
fi
if [ ! -f "$THEOS_DRIVERLAND_DHCPD" ] && [ -f "Build/Userland/Apps/TheDHCPd/TheDHCPd" ]; then
	THEOS_DRIVERLAND_DHCPD="Build/Userland/Apps/TheDHCPd/TheDHCPd"
fi
if [ ! -f "$THEOS_USERLAND_MICROPY" ] && [ -f "Build/Userland/Apps/TheMicroPython/TheMicroPython" ]; then
	THEOS_USERLAND_MICROPY="Build/Userland/Apps/TheMicroPython/TheMicroPython"
fi
if [ ! -d "$THEOS_USERLAND_MICROPY_SCRIPTS" ] && [ -d "../Userland/Apps/TheMicroPython/scripts" ]; then
	THEOS_USERLAND_MICROPY_SCRIPTS="../Userland/Apps/TheMicroPython/scripts"
fi
if [ ! -d "$THEOS_USERLAND_MICROPY_SCRIPTS" ] && [ -d "Build/Userland/Apps/TheMicroPython/scripts" ]; then
	THEOS_USERLAND_MICROPY_SCRIPTS="Build/Userland/Apps/TheMicroPython/scripts"
fi
if [ ! -f "$THEOS_USERLAND_EMBEDDEDDOOM" ] && [ -f "Build/Userland/Apps/TheEmbeddedDOOM/embeddedDOOM" ]; then
	THEOS_USERLAND_EMBEDDEDDOOM="Build/Userland/Apps/TheEmbeddedDOOM/embeddedDOOM"
fi
if [ ! -f "$THEOS_USERLAND_LIBC_SO" ] && [ -f "Build/Userland/Libraries/LibC/libc.so" ]; then
	THEOS_USERLAND_LIBC_SO="Build/Userland/Libraries/LibC/libc.so"
fi
if [ ! -f "$THEOS_USERLAND_LIBTHETESTDYN_SO" ] && [ -f "Build/Userland/Libraries/LibTheTestDyn/libthetestdyn.so" ]; then
	THEOS_USERLAND_LIBTHETESTDYN_SO="Build/Userland/Libraries/LibTheTestDyn/libthetestdyn.so"
fi

STAGE_DIR="$(mktemp -d)"
cleanup() {
	rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

echo "[disk] prepare staged root tree in '$STAGE_DIR'"
if [ -d "$THEOS_BASE_FOLDER" ]; then
	cp -a "$THEOS_BASE_FOLDER"/. "$STAGE_DIR"/
fi
mkdir -p "$STAGE_DIR/bin"
mkdir -p "$STAGE_DIR/drv"
mkdir -p "$STAGE_DIR/lib"
mkdir -p "$STAGE_DIR/system/fonts"
mkdir -p "$STAGE_DIR/system/python"

if [ -f "$THEOS_USERLAND_APP" ]; then
	echo "[disk] install TheApp -> /bin/TheApp from '$THEOS_USERLAND_APP'"
	cp "$THEOS_USERLAND_APP" "$STAGE_DIR/bin/TheApp"
else
	echo "[disk] warning: TheApp binary not found at '$THEOS_USERLAND_APP'"
fi

if [ -f "$THEOS_USERLAND_SHELL" ]; then
	echo "[disk] install TheShell -> /bin/TheShell from '$THEOS_USERLAND_SHELL'"
	cp "$THEOS_USERLAND_SHELL" "$STAGE_DIR/bin/TheShell"
else
	echo "[disk] warning: TheShell binary not found at '$THEOS_USERLAND_SHELL'"
fi

if [ -f "$THEOS_USERLAND_TEST" ]; then
	echo "[disk] install TheTest -> /bin/TheTest from '$THEOS_USERLAND_TEST'"
	cp "$THEOS_USERLAND_TEST" "$STAGE_DIR/bin/TheTest"
else
	echo "[disk] warning: TheTest binary not found at '$THEOS_USERLAND_TEST'"
fi

if [ -f "$THEOS_USERLAND_POWERMANAGER" ]; then
	echo "[disk] install ThePowerManager -> /bin/ThePowerManager from '$THEOS_USERLAND_POWERMANAGER'"
	cp "$THEOS_USERLAND_POWERMANAGER" "$STAGE_DIR/bin/ThePowerManager"
else
	echo "[disk] warning: ThePowerManager binary not found at '$THEOS_USERLAND_POWERMANAGER'"
fi

if [ -f "$THEOS_USERLAND_SYSTEMMONITOR" ]; then
	echo "[disk] install TheSystemMonitor -> /bin/TheSystemMonitor from '$THEOS_USERLAND_SYSTEMMONITOR'"
	cp "$THEOS_USERLAND_SYSTEMMONITOR" "$STAGE_DIR/bin/TheSystemMonitor"
else
	echo "[disk] warning: TheSystemMonitor binary not found at '$THEOS_USERLAND_SYSTEMMONITOR'"
fi

if [ -f "$THEOS_USERLAND_SYSTEMMONITORGUI" ]; then
	echo "[disk] install TheSystemMonitorGUI -> /bin/TheSystemMonitorGUI from '$THEOS_USERLAND_SYSTEMMONITORGUI'"
	cp "$THEOS_USERLAND_SYSTEMMONITORGUI" "$STAGE_DIR/bin/TheSystemMonitorGUI"
else
	echo "[disk] warning: TheSystemMonitorGUI binary not found at '$THEOS_USERLAND_SYSTEMMONITORGUI'"
fi

if [ -f "$THEOS_USERLAND_WINDOWSERVER" ]; then
	echo "[disk] install TheWindowServer -> /bin/TheWindowServer from '$THEOS_USERLAND_WINDOWSERVER'"
	cp "$THEOS_USERLAND_WINDOWSERVER" "$STAGE_DIR/bin/TheWindowServer"
else
	echo "[disk] warning: TheWindowServer binary not found at '$THEOS_USERLAND_WINDOWSERVER'"
fi

if [ -f "$THEOS_USERLAND_SHELLGUI" ]; then
	echo "[disk] install TheShellGUI -> /bin/TheShellGUI from '$THEOS_USERLAND_SHELLGUI'"
	cp "$THEOS_USERLAND_SHELLGUI" "$STAGE_DIR/bin/TheShellGUI"
else
	echo "[disk] warning: TheShellGUI binary not found at '$THEOS_USERLAND_SHELLGUI'"
fi

if [ -f "$THEOS_DRIVERLAND_DHCPD" ]; then
	echo "[disk] install TheDHCPd -> /drv/TheDHCPd from '$THEOS_DRIVERLAND_DHCPD'"
	cp "$THEOS_DRIVERLAND_DHCPD" "$STAGE_DIR/drv/TheDHCPd"
else
	echo "[disk] warning: TheDHCPd binary not found at '$THEOS_DRIVERLAND_DHCPD'"
fi

if [ -f "$THEOS_USERLAND_MICROPY" ]; then
	echo "[disk] install TheMicroPython -> /bin/TheMicroPython and /bin/MicroPython from '$THEOS_USERLAND_MICROPY'"
	cp "$THEOS_USERLAND_MICROPY" "$STAGE_DIR/bin/TheMicroPython"
else
	echo "[disk] warning: TheMicroPython binary not found at '$THEOS_USERLAND_MICROPY'"
fi

if [ -d "$THEOS_USERLAND_MICROPY_SCRIPTS" ]; then
	echo "[disk] install MicroPython scripts -> /system/python from '$THEOS_USERLAND_MICROPY_SCRIPTS'"
	cp -a "$THEOS_USERLAND_MICROPY_SCRIPTS"/. "$STAGE_DIR/system/python/"
else
	echo "[disk] warning: MicroPython scripts dir not found at '$THEOS_USERLAND_MICROPY_SCRIPTS'"
fi

if [ -f "$THEOS_USERLAND_EMBEDDEDDOOM" ]; then
	echo "[disk] install embeddedDOOM -> /bin/embeddedDOOM from '$THEOS_USERLAND_EMBEDDEDDOOM'"
	cp "$THEOS_USERLAND_EMBEDDEDDOOM" "$STAGE_DIR/bin/embeddedDOOM"
else
	echo "[disk] warning: embeddedDOOM binary not found at '$THEOS_USERLAND_EMBEDDEDDOOM'"
fi

if [ -f "$THEOS_USERLAND_LIBC_SO" ]; then
	echo "[disk] install libc.so -> /lib/libc.so from '$THEOS_USERLAND_LIBC_SO'"
	cp "$THEOS_USERLAND_LIBC_SO" "$STAGE_DIR/lib/libc.so"
else
	echo "[disk] warning: libc.so not found at '$THEOS_USERLAND_LIBC_SO'"
fi

if [ -f "$THEOS_USERLAND_LIBTHETESTDYN_SO" ]; then
	echo "[disk] install libthetestdyn.so -> /lib/libthetestdyn.so from '$THEOS_USERLAND_LIBTHETESTDYN_SO'"
	cp "$THEOS_USERLAND_LIBTHETESTDYN_SO" "$STAGE_DIR/lib/libthetestdyn.so"
else
	echo "[disk] warning: libthetestdyn.so not found at '$THEOS_USERLAND_LIBTHETESTDYN_SO'"
fi

echo "[disk] create image '$THEOS_DISK_NAME' size=$THEOS_DISK_SIZE"
qemu-img create -f raw "$THEOS_DISK_NAME" "$THEOS_DISK_SIZE"

# Keep ext4 features limited to what the in-kernel driver currently supports.
echo "[disk] format ext4 (compat profile) and populate tree"
mkfs.ext4 -F -d "$STAGE_DIR" -O ^has_journal,^64bit,^metadata_csum "$THEOS_DISK_NAME"
tune2fs -c0 -i0 "$THEOS_DISK_NAME"

echo "[disk] done"

exit 0
