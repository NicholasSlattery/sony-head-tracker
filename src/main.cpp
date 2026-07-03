// main.cpp
// CLI entry point (wmain) + one-click Repair Tracker orchestration. Dispatches
// to the GUI, probe/dump/bridge, and the Bluetooth recovery commands.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/bluetooth.hpp"
#include "sony_head_tracker/device.hpp"
#include "sony_head_tracker/diagnostics.hpp"
#include "sony_head_tracker/gui.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/logger.hpp"
#include "sony_head_tracker/orientation.hpp"
#include "sony_head_tracker/output_udp.hpp"
#include "sony_head_tracker/sensor_api_backend.hpp"
#include "sony_head_tracker/types.hpp"
#include "sony_head_tracker/version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <format>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::atomic_bool stopRequested{};
BOOL WINAPI consoleHandler(DWORD type){if(type==CTRL_C_EVENT||type==CTRL_CLOSE_EVENT){stopRequested=true;return TRUE;}return FALSE;}
void console(){
    if(!AttachConsole(ATTACH_PARENT_PROCESS)&&GetLastError()==ERROR_INVALID_HANDLE)AllocConsole();
    FILE* ignored{};
    const auto out=GetStdHandle(STD_OUTPUT_HANDLE),err=GetStdHandle(STD_ERROR_HANDLE);
    if(!out||out==INVALID_HANDLE_VALUE||GetFileType(out)==FILE_TYPE_CHAR)freopen_s(&ignored,"CONOUT$","w",stdout);
    if(!err||err==INVALID_HANDLE_VALUE||GetFileType(err)==FILE_TYPE_CHAR)freopen_s(&ignored,"CONOUT$","w",stderr);
    // UTF-8 output mode: device names can contain non-ANSI characters (AirPods
    // default to a curly apostrophe, e.g. "Nicholas's AirPods Pro"), and the
    // default narrow translation poisons wcout at the first one, silently
    // truncating everything after it.
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout),_O_U8TEXT);
    _setmode(_fileno(stderr),_O_U8TEXT);
    SetConsoleCtrlHandler(consoleHandler,TRUE);
}
void printUsage(std::wostream& out){
    out<<L"Sony Head Tracker for Windows "<<sony::kVersion<<L"\n"
       <<L"Streams head tracking from compatible Sony headphones that expose the Android\n"
       <<L"Head Tracker HID sensor, over UDP (OpenTrack + JSON). The connected headset is\n"
       <<L"auto-detected and named in the output. Unofficial; not affiliated with Sony.\n"
       <<L"AirPods are recognised but cannot work: Apple uses a proprietary protocol\n"
       <<L"that Windows does not expose to applications (see README > Compatibility).\n\n"
       <<L"Usage:\n"
       <<L"  sony-head-tracker.exe                       diagnostics GUI (default)\n"
       <<L"  sony-head-tracker.exe bridge [--port 4242] [--seconds N]\n"
       <<L"                             [--axis-map YXZ] [--invert XZ] [--smoothing 0.18]\n"
       <<L"  sony-head-tracker.exe probe [--include-disabled]\n"
       <<L"  sony-head-tracker.exe dump [--seconds N]\n"
       <<L"  sony-head-tracker.exe diagnostics             (print a redacted support bundle)\n"
       <<L"  sony-head-tracker.exe repair\n"
       <<L"  sony-head-tracker.exe bluetooth-probe [--all-le] [--name FILTER]\n"
       <<L"  sony-head-tracker.exe bluetooth-rebind [--name FILTER]\n"
       <<L"                             (--name defaults to auto-detecting the headset)\n"
       <<L"  sony-head-tracker.exe bluetooth-generic-hid   (run from an elevated prompt)\n"
       <<L"  sony-head-tracker.exe help | version\n\n"
       <<L"bridge sends six little-endian doubles (x, y, z, yaw, pitch, roll) to UDP\n"
       <<L"127.0.0.1:<port> and a JSON datagram to <port>+1. Loopback only; unauthenticated.\n";
}
void printDevice(const sony::DeviceInfo& d){std::wcout<<std::format(L"HID {}\n  {} {}\n  usage 0x{:04X}:0x{:04X}, VID/PID {:04X}:{:04X}, reports input={} feature={}\n",d.instanceId,d.manufacturer,d.product,d.usagePage,d.usage,d.vendorId,d.productId,d.inputReportBytes,d.featureReportBytes);if(!d.bluetoothName.empty())std::wcout<<L"  Bluetooth headset: "<<d.bluetoothName<<L'\n';std::wcout<<std::format(L"  description: {}\n  verified Android tracker: {}\n",std::wstring(d.sensorDescription.begin(),d.sensorDescription.end()),d.androidHeadTracker?L"yes":L"no");for(const auto& f:d.fields)std::wcout<<std::format(L"    {} id={} {:04X}:{:04X} count={} bits={} logical={}..{} physical={}..{} exp={}\n",f.feature?L"feature":L"input",f.reportId,f.usagePage,f.usage,f.reportCount,f.bitSize,f.logicalMin,f.logicalMax,f.physicalMin,f.physicalMax,f.unitExponent);}

// Names the first paired AirPods (any generation), or empty when none are paired.
// AirPods never implement the Android Head Tracker protocol, so when they are the
// only headphones present the diagnostics say why nothing was found.
std::wstring pairedAirPodsName(){
    for(const auto& name:sony::pairedBluetoothDeviceNames()){
        std::wstring low(name);std::ranges::transform(low,low.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
        if(low.find(L"airpods")!=std::wstring::npos)return name;
    }
    return {};
}

bool elevated(){
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    TOKEN_ELEVATION value{};DWORD bytes{};const bool result=GetTokenInformation(token,TokenElevation,&value,sizeof(value),&bytes)&&value.TokenIsElevated;CloseHandle(token);return result;
}
BOOL CALLBACK closeBridgeWindow(HWND hwnd,LPARAM){wchar_t name[64]{};GetClassNameW(hwnd,name,64);if(std::wstring_view(name)==L"SonyHeadTrackerWindow")PostMessageW(hwnd,WM_CLOSE,0,0);return TRUE;}
bool trackerAccessible(){sony::HidBackend hid;const auto devices=hid.enumerate();return std::ranges::any_of(devices,[](const auto& d){return d.androidHeadTracker;});}
int elevatedRepair(){
    std::wcout<<L"Head tracker one-click repair\n=============================\n";
    if(trackerAccessible()){std::wcout<<L"The Android Head Tracker is already accessible. No driver change was needed.\n";return 0;}
    std::wcout<<L"Recreating the headset's Bluetooth HID child (auto-detecting the headset)...\n";
    const auto rebind=sony::rebindBluetoothHid(L"",std::wcout);if(rebind!=0)return rebind;
    int binding=2;for(int attempt=0;attempt<4&&binding==2;++attempt){if(attempt)std::this_thread::sleep_for(std::chrono::seconds(2));binding=sony::useGenericHidDriver(std::wcout);}
    if(binding!=0&&binding!=1)return binding;
    for(int attempt=0;attempt<10;++attempt){std::this_thread::sleep_for(std::chrono::seconds(1));if(trackerAccessible()){std::wcout<<L"Repair complete: #AndroidHeadTracker# is accessible.\n";return 0;}}
    std::wcerr<<L"Repair completed its device steps, but the tracker did not become accessible. Power-cycle the headphones and press Repair Tracker once more.\n";return 4;
}
int runRepair(bool launchGui){
    EnumWindows(closeBridgeWindow,0);std::this_thread::sleep_for(std::chrono::milliseconds(500));
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);int result{};
    if(elevated())result=elevatedRepair();
    else{SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"runas";launch.lpFile=executable;launch.lpParameters=L"repair --elevated --no-launch";launch.nShow=SW_HIDE;if(!ShellExecuteExW(&launch)){result=GetLastError()==ERROR_CANCELLED?5:4;}else{WaitForSingleObject(launch.hProcess,INFINITE);DWORD code{};GetExitCodeProcess(launch.hProcess,&code);CloseHandle(launch.hProcess);result=static_cast<int>(code);}}
    if(launchGui)ShellExecuteW(nullptr,L"open",executable,L"gui",nullptr,SW_SHOWNORMAL);return result;
}
}

int wmain(int argc,wchar_t** argv){const std::wstring command=argc>1?argv[1]:L"gui";if(command==L"gui"){FreeConsole();return sony::runGui(GetModuleHandleW(nullptr),SW_SHOWDEFAULT);}if(command==L"repair"){bool launch=true;for(int i=2;i<argc;++i)if(std::wstring_view(argv[i])==L"--no-launch")launch=false;return runRepair(launch);}console();
    if(command==L"version"||command==L"--version"||command==L"-v"){std::wcout<<L"sony-head-tracker "<<sony::kVersion<<L'\n';return 0;}
    if(command==L"help"||command==L"--help"||command==L"-h"||command==L"/?"){printUsage(std::wcout);return 0;}
    sony::Logger::instance().setSink([](sony::LogLevel,const std::wstring& line){std::wcerr<<line<<L'\n';});
    if(command==L"bluetooth-probe"){sony::BluetoothProbeOptions options;std::wstring name;for(int i=2;i<argc;++i){const std::wstring_view option=argv[i];if(option==L"--all-le")options.probeAllLeDevices=true;else if(option==L"--name"&&i+1<argc)name=argv[++i];}options.nameFilter=name;return sony::runBluetoothProbe(options,std::wcout);}
    if(command==L"bluetooth-rebind"){std::wstring name;for(int i=2;i+1<argc;++i)if(std::wstring_view(argv[i])==L"--name")name=argv[++i];return sony::rebindBluetoothHid(name,std::wcout);}
    if(command==L"bluetooth-generic-hid")return sony::useGenericHidDriver(std::wcout);
    if(command==L"diagnostics")return sony::runDiagnostics(std::wcout);
    sony::HidBackend hid;sony::SensorBackend sensor;
    bool includeDisabled=false;if(command==L"probe")for(int i=2;i<argc;++i)if(std::wstring_view(argv[i])==L"--include-disabled")includeDisabled=true;
    auto devices=hid.enumerate(!includeDisabled);auto sensors=sensor.enumerate();
    if(command==L"probe"){for(const auto& d:devices)printDevice(d);for(const auto& s:sensors)std::wcout<<L"Sensor API "<<s.friendlyName<<L" | "<<s.description<<L" | "<<s.id<<L'\n';const auto found=std::any_of(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;})||std::any_of(sensors.begin(),sensors.end(),[](const auto& s){return s.androidHeadTracker;});
        for(const auto& d:devices)if(d.androidHeadTracker)std::wcout<<std::format(L"\nVerified Android head tracker on '{}'.\n",d.bluetoothName.empty()?(d.product.empty()?d.instanceId:d.product):d.bluetoothName);
        if(!found){
            std::wcout<<L"\nNo Android Head Tracker HID sensor was found.\n";
            if(const auto airpods=pairedAirPodsName();!airpods.empty())std::wcout<<L"Note: '"<<airpods<<L"' is paired, but AirPods use Apple's proprietary accessory\nprotocol (L2CAP PSM 0x1001), which Windows does not expose to applications.\nHead tracking cannot be read from AirPods on Windows without a third-party\nkernel driver. See README > Compatibility.\n";
        }
        return found?0:2;}
    auto selected=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;});
    if(command==L"dump"){if(selected==devices.end()){std::wcerr<<L"No verified raw HID head tracker is accessible; Sensor API cannot expose raw packets.\n";return 2;}unsigned seconds{};for(int i=2;i+1<argc;++i)if(std::wstring_view(argv[i])==L"--seconds")seconds=std::wcstoul(argv[++i],nullptr,10);if(!hid.connect(*selected,[](const auto& b){std::wcout<<sony::hexDump(b)<<L'\n';},[](auto){}))return 3;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);while(!stopRequested&&(!seconds||std::chrono::steady_clock::now()<deadline))std::this_thread::sleep_for(std::chrono::milliseconds(100));hid.disconnect();return 0;}
    if(command==L"bridge"){
        std::uint16_t port=4242;unsigned seconds{};sony::FilterConfig config;
        for(int i=2;i<argc;++i){
            const std::wstring_view option=argv[i];
            if(option==L"--port"&&i+1<argc){const auto value=std::wcstoul(argv[++i],nullptr,10);if(value<1||value>65534){std::wcerr<<L"--port must be between 1 and 65534 (the JSON stream uses port+1)\n";return 1;}port=static_cast<std::uint16_t>(value);}
            else if(option==L"--seconds"&&i+1<argc)seconds=std::wcstoul(argv[++i],nullptr,10);
            else if(option==L"--smoothing"&&i+1<argc)config.smoothing=std::clamp(std::wcstod(argv[++i],nullptr),0.01,1.0);
            else if(option==L"--invert"&&i+1<argc){const std::wstring axes=argv[++i];config.axes.sign={1.0,1.0,1.0};for(const auto axis:axes){if(axis==L'x'||axis==L'X')config.axes.sign[0]=-1;if(axis==L'y'||axis==L'Y')config.axes.sign[1]=-1;if(axis==L'z'||axis==L'Z')config.axes.sign[2]=-1;}}
            else if(option==L"--axis-map"&&i+1<argc){const std::wstring map=argv[++i];if(map.size()==3){for(unsigned output=0;output<3;++output){const auto c=static_cast<wchar_t>(towlower(map[output]));config.axes.source[output]=c==L'x'?0:c==L'y'?1:2;}}}
        }
        sony::UdpOutput udp;if(!udp.open("127.0.0.1",port)){std::wcerr<<L"Could not open UDP output\n";return 4;}
        std::wcout<<std::format(L"Streaming head-tracking data:\n  OpenTrack doubles -> UDP 127.0.0.1:{}\n  JSON telemetry    -> UDP 127.0.0.1:{}\n(loopback only; unauthenticated -- do not forward to an untrusted network)\n",port,port+1);
        sony::OrientationFilter filter(config);auto selectedSensor=std::find_if(sensors.begin(),sensors.end(),[](const auto& s){return s.androidHeadTracker;});
        auto output=[&](sony::MotionSample s){auto out=filter.process(std::move(s));udp.send(out);std::wcout<<std::format(L"\rYPR {:7.2f} {:7.2f} {:7.2f}  {:5.1f} pps   ",out.euler.yaw,out.euler.pitch,out.euler.roll,out.packetsPerSecond)<<std::flush;};
        auto connect=[&]{
            if(selected!=devices.end()){
                const auto& name=!selected->bluetoothName.empty()?selected->bluetoothName:selected->product;
                udp.setDeviceLabel(name);
                if(!name.empty())std::wcout<<L"Tracking headset: "<<name<<L'\n';
                return hid.connect(*selected,{},output);
            }
            if(selectedSensor!=sensors.end()){udp.setDeviceLabel(selectedSensor->friendlyName);std::wcout<<L"Tracking headset (Sensor API): "<<selectedSensor->friendlyName<<L'\n';return sensor.connect(*selectedSensor,output);}
            return false;};
        if(!connect()){
            std::wcerr<<L"No Android Head Tracker was found on any connected headset.\n";
            if(const auto airpods=pairedAirPodsName();!airpods.empty())std::wcerr<<L"Note: '"<<airpods<<L"' is paired, but AirPods use Apple's proprietary protocol and\ncannot provide head tracking on Windows. See README > Compatibility.\n";
            return 3;
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);while(!stopRequested&&(!seconds||std::chrono::steady_clock::now()<deadline)){if(!hid.connected()&&!sensor.connected()){std::wcerr<<L"\nDisconnected; probing for reconnection…\n";std::this_thread::sleep_for(std::chrono::seconds(2));devices=hid.enumerate();sensors=sensor.enumerate();selected=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;});selectedSensor=std::find_if(sensors.begin(),sensors.end(),[](const auto& s){return s.androidHeadTracker;});connect();}std::this_thread::sleep_for(std::chrono::milliseconds(100));}
        hid.disconnect();sensor.disconnect();return 0;
    }
    std::wcerr<<L"Unknown command '"<<command<<L"'.\n\n";printUsage(std::wcerr);return 1;
}
