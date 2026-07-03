// windows_prelude.hpp
// The single place the Windows/Bluetooth/HID/Sensor/COM headers are pulled in,
// in the order Winsock requires (WinSock2 before Windows.h), plus every import
// library the platform layer links against. Any .cpp in the platform layer
// includes this first, before its own header and the standard library.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Winsock2 headers must precede Windows.h.
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ws2bth.h>
#include <Windows.h>
#include <BluetoothAPIs.h>
#include <bluetoothleapis.h>
#include <bthledef.h>
#include <SetupAPI.h>
#include <newdev.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <SensorsApi.h>
#include <Sensors.h>
#include <PortableDeviceTypes.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <Uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "sensorsapi.lib")
#pragma comment(lib, "portabledeviceguids.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "BluetoothApis.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
