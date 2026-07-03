// gui.cpp
// Diagnostics GUI: device list, details/log, live yaw/pitch/roll graph, axis
// controls, Recenter, and the one-click Repair Tracker button.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/gui.hpp"

#include "sony_head_tracker/app_config.hpp"
#include "sony_head_tracker/bluetooth.hpp"
#include "sony_head_tracker/diagnostics.hpp"
#include "sony_head_tracker/hid_backend.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/logger.hpp"
#include "sony_head_tracker/orientation.hpp"
#include "sony_head_tracker/output_udp.hpp"
#include "sony_head_tracker/sensor_api_backend.hpp"
#include "sony_head_tracker/types.hpp"
#include "sony_head_tracker/version.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sony {

int runGui(HINSTANCE instance, int showCommand);

namespace {
constexpr UINT kSampleMessage=WM_APP+1;
constexpr UINT kRawMessage=WM_APP+2;
constexpr int kRefresh=1001,kRecenter=1002,kDeviceList=1003,kRepair=1004,kShowAll=1005;
// Tools menu commands.
constexpr int kReconnect=1010,kResetDefaults=1011,kImportConfig=1012,kExportConfig=1013,kExportDiag=1014;
constexpr int kBackoffSeconds[]={1,2,5,10,30};

class Window {
public:
    HINSTANCE instance{}; HWND hwnd{}, list{}, details{}, raw{}, stats{}, motion{}, health{}, refresh{}, repair{}, recenter{}, invertX{}, invertY{}, invertZ{}, mapping{}, smoothing{}, smoothingLabel{}, showAll{};
    HFONT font{}, titleFont{}, sectionFont{}; HICON appIcon{};
    HBRUSH background{}, panel{}, headerBrush{};
    HidBackend hid; SensorBackend sensors; OrientationFilter filter; UdpOutput udp;
    std::vector<DeviceInfo> devices; std::vector<SensorInfo> sensorDevices;
    std::vector<std::size_t> listMap;   // list row -> devices index; devices.size()+i for Sensor API rows; SIZE_MAX for the info row
    std::array<std::vector<float>,3> history; bool connected{}; FilterConfig filterConfig{};
    AppConfig config_{};                                      // persisted settings (%LOCALAPPDATA%\SonyHeadTracker\config.json)
    std::uint16_t udpPort{4242};
    std::wstring headsetName;                                 // resolved Bluetooth name of the connected head tracker
    std::wstring connectedHidUsage, connectedDescriptor;     // captured on connect, for the diagnostics bundle
    std::wstring statusText{L"Searching for a head tracker…"};
    COLORREF statusColor{RGB(150,160,176)};
    // Live connection-health + auto-reconnect state.
    double lastPacketsPerSecond{}, lastLatencyMs{-1.0};
    bool angularVelocityAvailable{};
    int reconnectAttempts{};
    std::size_t backoffIndex{};
    std::chrono::steady_clock::time_point lastSampleTime{}, lastReconnectAttempt{};

    // Cohesive dark palette shared by the back-buffer paint and the child controls.
    static constexpr COLORREF kWindowBg{RGB(20,23,30)};
    static constexpr COLORREF kHeaderBg{RGB(27,31,42)};
    static constexpr COLORREF kPanelBg{RGB(14,17,23)};
    static constexpr COLORREF kText{RGB(226,231,240)};
    static constexpr COLORREF kMuted{RGB(150,160,176)};
    static constexpr COLORREF kGrid{RGB(46,53,66)};
    static constexpr COLORREF kBorder{RGB(56,64,80)};
    static constexpr COLORREF kAccent{RGB(96,165,255)};
    static constexpr COLORREF kOk{RGB(94,214,140)};
    static constexpr COLORREF kWarn{RGB(255,196,86)};

    ~Window(){hid.disconnect();sensors.disconnect();for(auto f:{font,titleFont,sectionFont})if(f)DeleteObject(f);for(auto b:{background,panel,headerBrush})if(b)DeleteObject(b);if(appIcon)DestroyIcon(appIcon);}

    // Fixed regions, computed from the client size so paint(), layout(), and
    // invalidation always agree exactly.
    RECT clientRect() const { RECT r{}; GetClientRect(hwnd,&r); return r; }
    RECT headerRect() const { const auto r=clientRect(); return RECT{0,0,r.right,64}; }
    RECT outputRect() const { const auto r=clientRect(); return RECT{16,r.bottom-322,r.right-16,r.bottom-230}; }
    RECT graphRect()  const { const auto r=clientRect(); return RECT{16,r.bottom-124,r.right-16,r.bottom-14}; }
    int  listWidth()  const { const auto r=clientRect(); return std::max(300,static_cast<int>(r.right)*17/50); }

    void setStatus(std::wstring text,COLORREF color){
        statusText=std::move(text);statusColor=color;
        const auto h=headerRect();InvalidateRect(hwnd,&h,FALSE);
    }
    void enumerate(){
        hid.disconnect();sensors.disconnect();connected=false;devices=hid.enumerate();sensorDevices=sensors.enumerate();
        // Verified trackers first, unverified custom-sensor candidates second,
        // every other HID collection (mice, touchpads, ...) last.
        std::stable_sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){
            const auto rank=[](const DeviceInfo& d){return d.androidHeadTracker?0:(d.usagePage==kSensorPage&&d.usage==kOtherCustom)?1:2;};
            return rank(a)<rank(b);});
        rebuildList();connectFirst();showDetails(0);
    }
    // Fills the device list. By default only head-tracker candidates are shown;
    // the "Show all devices" checkbox reveals every HID collection for debugging.
    void rebuildList(){
        SendMessageW(list,LB_RESETCONTENT,0,0);listMap.clear();
        const bool everything=SendMessageW(showAll,BM_GETCHECK,0,0)==BST_CHECKED;
        const auto add=[&](const std::wstring& label,std::size_t mapped){SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));listMap.push_back(mapped);};
        for(std::size_t i=0;i<devices.size();++i){
            const auto& d=devices[i];
            const bool candidate=d.usagePage==kSensorPage&&d.usage==kOtherCustom;
            if(!everything&&!candidate)continue;
            const auto& shown=!d.bluetoothName.empty()?d.bluetoothName:(!d.product.empty()?d.product:d.instanceId);
            if(d.androidHeadTracker)add(std::format(L"✔  {}   — Android head tracker",shown),i);
            else if(candidate)add(std::format(L"?  {}   — custom sensor, no Android marker",shown),i);
            else add(std::format(L"HID {:04X}:{:04X}  {}",d.vendorId,d.productId,shown),i);
        }
        for(std::size_t i=0;i<sensorDevices.size();++i){
            const auto& s=sensorDevices[i];
            if(!everything&&!s.androidHeadTracker)continue;
            add(std::format(L"{}  {}   — Windows Sensor API",s.androidHeadTracker?L"✔":L"·",s.friendlyName),devices.size()+i);
        }
        if(listMap.empty())add(L"No head tracker found — press Repair Tracker (admin approval required)",SIZE_MAX);
        SendMessageW(list,LB_SETCURSEL,0,0);
    }
    void showDetails(int selection){
        std::wostringstream o;
        const auto row=static_cast<std::size_t>(selection);
        if(selection<0||row>=listMap.size()||listMap[row]==SIZE_MAX){
            o<<L"No Android Head Tracker HID collection is currently visible.\r\n\r\n"
             <<L"  1.  Make sure the headphones are paired, powered on, and connected.\r\n"
             <<L"  2.  Press Repair Tracker in the toolbar. It asks for one administrator\r\n"
             <<L"      approval and only touches the headset's own HID service — it never\r\n"
             <<L"      installs a custom driver or touches other devices.\r\n"
             <<L"  3.  Tick 'Show all devices' to inspect every HID collection Windows sees.\r\n\r\n"
             <<L"AirPods (and other Apple headphones) cannot work: Apple uses a proprietary\r\n"
             <<L"protocol that Windows does not expose to applications — see the README.";
        }else if(listMap[row]<devices.size()){
            const auto& d=devices[listMap[row]];
            o<<L"Path: "<<d.path<<L"\r\nInstance: "<<d.instanceId<<L"\r\nManufacturer: "<<d.manufacturer<<L"\r\nProduct: "<<d.product<<L"\r\nBluetooth headset: "<<(d.bluetoothName.empty()?L"(unresolved)":d.bluetoothName.c_str());
            o<<std::format(L"\r\nUsage: 0x{:04X}:0x{:04X}   VID/PID: {:04X}:{:04X}\r\nInput bytes: {}   Feature bytes: {}\r\nAndroid description: {}\r\nVerified: {}\r\n\r\nDescriptor fields:\r\n",d.usagePage,d.usage,d.vendorId,d.productId,d.inputReportBytes,d.featureReportBytes,std::wstring(d.sensorDescription.begin(),d.sensorDescription.end()),d.androidHeadTracker?L"yes":L"no");
            for(const auto& f:d.fields)o<<std::format(L"{} id={} usage={:04X}:{:04X} count={} bits={} logical={}..{} physical={}..{} exp={} unit=0x{:X} data={}\r\n",f.feature?L"FEATURE":L"INPUT",f.reportId,f.usagePage,f.usage,f.reportCount,f.bitSize,f.logicalMin,f.logicalMax,f.physicalMin,f.physicalMax,f.unitExponent,f.unit,f.dataIndex);
            for(const auto& v:d.featureValues)o<<L"Feature: "<<std::wstring(v.begin(),v.end())<<L"\r\n";
        }else{
            const auto& s=sensorDevices[listMap[row]-devices.size()];
            o<<L"Windows Sensor API fallback\r\nName: "<<s.friendlyName<<L"\r\nDescription: "<<s.description<<L"\r\nID: "<<s.id<<L"\r\nType: "<<s.type;
        }
        o<<L"\r\n\r\nDiscovery and permission log:\r\n";for(const auto& line:Logger::instance().history())o<<line<<L"\r\n";
        SetWindowTextW(details,o.str().c_str());
    }
    void connectFirst(){
        const auto it=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.androidHeadTracker;});
        if(it!=devices.end()){
            headsetName=!it->bluetoothName.empty()?it->bluetoothName:(!it->product.empty()?it->product:L"head tracker");
            connectedHidUsage=std::format(L"0x{:04X}:0x{:04X}",it->usagePage,it->usage);connectedDescriptor=std::wstring(it->sensorDescription.begin(),it->sensorDescription.end());
            udp.setDeviceLabel(headsetName);
            connected=hid.connect(*it,[w=hwnd](const auto& bytes){PostMessageW(w,kRawMessage,0,reinterpret_cast<LPARAM>(new std::vector<std::uint8_t>(bytes)));},[w=hwnd](MotionSample s){PostMessageW(w,kSampleMessage,0,reinterpret_cast<LPARAM>(new MotionSample(std::move(s))));});
            if(connected){
                SetWindowTextW(hwnd,std::format(L"Sony Head Tracker {} - {}",kVersion,headsetName).c_str());
                setStatus(std::format(L"Tracking {}",headsetName),kOk);
                SetWindowTextW(stats,std::format(L"Connected to {}. Waiting for the first sample…",headsetName).c_str());
            }else setStatus(std::format(L"Found {} but could not open it — see the log in the details pane",headsetName),kWarn);
            return;}
        const auto fallback=std::find_if(sensorDevices.begin(),sensorDevices.end(),[](const auto& s){return s.androidHeadTracker;});
        if(fallback!=sensorDevices.end()){
            headsetName=fallback->friendlyName;connectedHidUsage.clear();connectedDescriptor=fallback->description;udp.setDeviceLabel(headsetName);
            SetWindowTextW(raw,L"Raw packet: unavailable through Windows Sensor API");
            connected=sensors.connect(*fallback,[w=hwnd](MotionSample s){PostMessageW(w,kSampleMessage,0,reinterpret_cast<LPARAM>(new MotionSample(std::move(s))));});
            if(connected)setStatus(std::format(L"Tracking {} via the Windows Sensor API",headsetName),kOk);
            return;}
        headsetName.clear();connectedHidUsage.clear();connectedDescriptor.clear();udp.setDeviceLabel({});
        SetWindowTextW(hwnd,std::format(L"Sony Head Tracker {}",kVersion).c_str());
        bool airpods=false;for(const auto& name:pairedBluetoothDeviceNames()){std::wstring low(name);std::ranges::transform(low,low.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});if(low.find(L"airpods")!=std::wstring::npos){airpods=true;break;}}
        setStatus(L"No head tracker found — if the headset is paired and on, press Repair Tracker (admin approval required)",kWarn);
        SetWindowTextW(stats,airpods
            ?L"AirPods detected: Apple's protocol is not readable on Windows — only Android-Head-Tracker headsets can work. See README › Compatibility."
            :L"Waiting for an Android Head Tracker HID profile. Power-cycle the headphones if they are already paired.");
    }
    void onSample(std::unique_ptr<MotionSample> s){
        auto filtered=filter.process(std::move(*s));udp.send(filtered);for(int i=0;i<3;++i){history[i].push_back(static_cast<float>(i==0?filtered.euler.yaw:i==1?filtered.euler.pitch:filtered.euler.roll));if(history[i].size()>360)history[i].erase(history[i].begin());}
        lastSampleTime=std::chrono::steady_clock::now();lastPacketsPerSecond=filtered.packetsPerSecond;lastLatencyMs=filtered.receiveLatencyMs;angularVelocityAvailable=filtered.angularVelocity.has_value();
        auto text=std::format(L"Yaw {:7.2f}°   Pitch {:7.2f}°   Roll {:7.2f}°     {:6.1f} packets/s     latency: {}",filtered.euler.yaw,filtered.euler.pitch,filtered.euler.roll,filtered.packetsPerSecond,filtered.receiveLatencyMs<0?L"N/A (no device timestamp)":std::format(L"{:.2f} ms",filtered.receiveLatencyMs));SetWindowTextW(stats,text.c_str());
        const auto gyro=filtered.angularVelocity?std::format(L"{:6.2f}, {:6.2f}, {:6.2f} rad/s",filtered.angularVelocity->x,filtered.angularVelocity->y,filtered.angularVelocity->z):std::wstring(L"unavailable");
        const auto accel=filtered.acceleration?std::format(L"{:6.2f}, {:6.2f}, {:6.2f} m/s²",filtered.acceleration->x,filtered.acceleration->y,filtered.acceleration->z):std::wstring(L"not reported by this device");
        SetWindowTextW(motion,std::format(L"Gyroscope  {}        Accelerometer  {}",gyro,accel).c_str());
        updateHealth();
        const auto rect=graphRect();InvalidateRect(hwnd,&rect,FALSE); // repaint only the live graph -- no whole-window flicker
    }
    // Compact live connection-health readout: rate, packet age, backend, gyro,
    // UDP packets/destination, reconnect attempts, and a custom-mapping flag.
    void updateHealth(){
        const bool live=hid.connected()||sensors.connected();
        const wchar_t* backend=hid.connected()?L"Raw HID":sensors.connected()?L"Sensor API":L"none";
        std::wstring age=L"—";
        if(lastSampleTime.time_since_epoch().count()){
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-lastSampleTime).count();
            age=ms<10000?std::format(L"{} ms",ms):std::format(L"{:.0f} s",ms/1000.0);
        }
        std::wstring line=std::format(L"Health:  {:.1f} samples/s   ·   last packet {}   ·   backend {}   ·   gyro {}   ·   UDP {} pkts → :{}   ·   reconnects {}",
            live?lastPacketsPerSecond:0.0,age,backend,angularVelocityAvailable?L"yes":L"no",udp.packetsSent(),udp.port(),reconnectAttempts);
        if(lastLatencyMs>=0.0)line+=std::format(L"   ·   sensor age {:.1f} ms",lastLatencyMs);
        if(!isDefaultAxisMapping(filterConfig.axes))line+=L"   ·   ⚙ custom axis mapping";
        SetWindowTextW(health,line.c_str());
    }
    // Auto-reconnect with increasing back-off (1, 2, 5, 10, 30 s). Manual Refresh /
    // "Reconnect now" reset the back-off and retry immediately.
    void attemptReconnect(){
        lastReconnectAttempt=std::chrono::steady_clock::now();++reconnectAttempts;enumerate();
        if(hid.connected()||sensors.connected())backoffIndex=0;
        else if(backoffIndex+1<std::size(kBackoffSeconds))++backoffIndex;
    }
    void reconnectNow(){backoffIndex=0;attemptReconnect();}
    void tickReconnect(){
        if(hid.connected()||sensors.connected()){backoffIndex=0;return;}
        const auto waited=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-lastReconnectAttempt).count();
        if(waited>=kBackoffSeconds[std::min(backoffIndex,std::size(kBackoffSeconds)-1)])attemptReconnect();
    }
    void applyControls(){
        static constexpr std::array<std::array<unsigned,3>,6> maps{{{{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}};
        const auto choice=std::clamp<LRESULT>(SendMessageW(mapping,CB_GETCURSEL,0,0),0,5);filterConfig.axes.source=maps[choice];
        filterConfig.axes.sign={SendMessageW(invertX,BM_GETCHECK,0,0)==BST_CHECKED?-1.0:1.0,SendMessageW(invertY,BM_GETCHECK,0,0)==BST_CHECKED?-1.0:1.0,SendMessageW(invertZ,BM_GETCHECK,0,0)==BST_CHECKED?-1.0:1.0};
        filterConfig.smoothing=std::clamp(SendMessageW(smoothing,TBM_GETPOS,0,0)/100.0,0.01,1.0);filter.setConfig(filterConfig);
        config_.axes=filterConfig.axes;config_.smoothing=filterConfig.smoothing;config_.showAllDevices=SendMessageW(showAll,BM_GETCHECK,0,0)==BST_CHECKED;
        updateHealth();
    }
    // Index of the axis-map combo entry matching a source order, or 2 (YXZ) if none.
    static int mappingIndex(const std::array<unsigned,3>& source){
        static constexpr std::array<std::array<unsigned,3>,6> maps{{{{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}};
        for(int i=0;i<6;++i)if(maps[i]==source)return i;
        return 2;
    }
    void applyConfigToControls(){
        SendMessageW(mapping,CB_SETCURSEL,mappingIndex(config_.axes.source),0);
        SendMessageW(invertX,BM_SETCHECK,config_.axes.sign[0]<0?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(invertY,BM_SETCHECK,config_.axes.sign[1]<0?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(invertZ,BM_SETCHECK,config_.axes.sign[2]<0?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(smoothing,TBM_SETPOS,TRUE,std::clamp<LONG>(static_cast<LONG>(std::lround(config_.smoothing*100.0)),1,100));
        SendMessageW(showAll,BM_SETCHECK,config_.showAllDevices?BST_CHECKED:BST_UNCHECKED,0);
        filterConfig.axes=config_.axes;filterConfig.smoothing=config_.smoothing;filter.setConfig(filterConfig);
    }
    void captureWindowPlacement(){
        WINDOWPLACEMENT wp{sizeof(wp)};if(!GetWindowPlacement(hwnd,&wp))return;const auto& n=wp.rcNormalPosition;
        config_.window={n.left,n.top,n.right-n.left,n.bottom-n.top};
    }
    void persistConfig(){config_.udpPort=udpPort;captureWindowPlacement();saveAppConfig(config_);}
    // Builds a redacted support bundle from the live GUI state.
    std::wstring diagnosticsBundle(){
        DiagnosticsInput in;
        in.appVersion=std::wstring(kVersion);in.windowsBuild=windowsBuildString();
        in.backend=hid.connected()?L"Raw HID":sensors.connected()?L"Windows Sensor API":L"none";
        in.headsetModel=headsetName;in.hidUsage=connectedHidUsage;in.descriptor=connectedDescriptor;
        in.packetsPerSecond=(hid.connected()||sensors.connected())?lastPacketsPerSecond:-1.0;
        if(lastSampleTime.time_since_epoch().count())in.sampleAgeMs=static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-lastSampleTime).count());
        in.angularVelocity=angularVelocityAvailable;in.destinationPort=std::format(L"{} / {}",udp.port(),udp.port()+1);
        in.udpPacketsSent=udp.packetsSent();in.reconnectionAttempts=reconnectAttempts;
        const wchar_t axes[]=L"XYZ";std::wstring map,inv;for(int i=0;i<3;++i)map+=axes[filterConfig.axes.source[i]%3];for(int i=0;i<3;++i)if(filterConfig.axes.sign[i]<0)inv+=axes[i];
        in.settings=std::format(L"  Axis map: {}\n  Inverted: {}\n  Smoothing: {:.2f}\n  UDP port: {}\n",map,inv.empty()?L"(none)":inv,filterConfig.smoothing,udpPort);
        auto log=Logger::instance().history();for(const auto& l:log)if(l.find(L" ERROR ")!=std::wstring::npos)in.lastError=l;
        const std::size_t keep=std::min<std::size_t>(log.size(),40);in.logLines.assign(log.end()-static_cast<std::ptrdiff_t>(keep),log.end());
        return redactDiagnostics(formatDiagnostics(in),currentRedactionTokens());
    }
    // Common Save/Open dialog helper. save=true -> GetSaveFileName.
    std::wstring chooseFile(bool save,const wchar_t* fileFilter,const wchar_t* defExt,const wchar_t* defName){
        wchar_t path[MAX_PATH]{};if(defName)wcsncpy_s(path,defName,_TRUNCATE);
        OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=hwnd;ofn.lpstrFilter=fileFilter;ofn.lpstrFile=path;ofn.nMaxFile=MAX_PATH;ofn.lpstrDefExt=defExt;
        ofn.Flags=save?(OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR):(OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR);
        const bool ok=save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn);
        return ok?std::wstring(path):std::wstring();
    }
    void exportDiagnostics(){
        const auto path=chooseFile(true,L"Text files\0*.txt\0All files\0*.*\0",L"txt",L"sony-head-tracker-diagnostics.txt");
        if(path.empty())return;
        const auto bundle=diagnosticsBundle();
        std::ofstream f(path,std::ios::binary|std::ios::trunc);
        if(f){const auto utf8=WideCharToMultiByte(CP_UTF8,0,bundle.data(),static_cast<int>(bundle.size()),nullptr,0,nullptr,nullptr);std::string out(utf8>0?static_cast<std::size_t>(utf8):0,'\0');if(utf8>0)WideCharToMultiByte(CP_UTF8,0,bundle.data(),static_cast<int>(bundle.size()),out.data(),utf8,nullptr,nullptr);f.write(out.data(),static_cast<std::streamsize>(out.size()));}
        MessageBoxW(hwnd,f?std::format(L"Redacted diagnostics saved to:\n{}",path).c_str():L"Could not write the diagnostics file.",L"Sony Head Tracker",MB_OK|(f?MB_ICONINFORMATION:MB_ICONERROR));
    }
    void exportConfig(){
        const auto path=chooseFile(true,L"JSON files\0*.json\0All files\0*.*\0",L"json",L"sony-head-tracker-config.json");
        if(path.empty())return;persistConfig();
        MessageBoxW(hwnd,exportAppConfig(config_,path)?L"Configuration exported.":L"Could not write the configuration file.",L"Sony Head Tracker",MB_OK|MB_ICONINFORMATION);
    }
    void importConfig(){
        const auto path=chooseFile(false,L"JSON files\0*.json\0All files\0*.*\0",L"json",nullptr);
        if(path.empty())return;
        if(importAppConfig(config_,path)){udpPort=config_.udpPort;applyConfigToControls();udp.open("127.0.0.1",udpPort);persistConfig();rebuildList();showDetails(0);updateHealth();MessageBoxW(hwnd,L"Configuration imported.",L"Sony Head Tracker",MB_OK|MB_ICONINFORMATION);}
        else MessageBoxW(hwnd,L"Could not read that configuration file.",L"Sony Head Tracker",MB_OK|MB_ICONERROR);
    }
    void resetDefaults(){config_=AppConfig{};udpPort=config_.udpPort;applyConfigToControls();udp.open("127.0.0.1",udpPort);persistConfig();rebuildList();showDetails(0);updateHealth();}
    void layout(){const auto r=clientRect();const int w=r.right,h=r.bottom;
        // Toolbar row under the header.
        MoveWindow(refresh,16,76,96,30,TRUE);MoveWindow(repair,118,76,158,30,TRUE);MoveWindow(recenter,282,76,96,30,TRUE);
        MoveWindow(mapping,412,80,74,220,TRUE);MoveWindow(invertX,494,80,88,24,TRUE);MoveWindow(invertY,586,80,88,24,TRUE);MoveWindow(invertZ,678,80,88,24,TRUE);MoveWindow(smoothingLabel,772,76,74,30,TRUE);MoveWindow(smoothing,848,76,150,30,TRUE);
        // Device list + details columns, above the painted output panel.
        const int lw=listWidth();const int panelTop=136;const int panelBottom=h-334;
        MoveWindow(showAll,16+lw-152,112,152,20,TRUE);
        MoveWindow(list,16,panelTop,lw,panelBottom-panelTop,TRUE);
        MoveWindow(details,16+lw+12,panelTop,w-44-lw,panelBottom-panelTop,TRUE);
        // Telemetry lines between the output panel and the graph.
        MoveWindow(raw,16,h-222,w-32,22,TRUE);MoveWindow(stats,16,h-196,w-32,22,TRUE);MoveWindow(motion,16,h-170,w-32,22,TRUE);MoveWindow(health,16,h-148,w-32,22,TRUE);}
    void paintSectionLabel(HDC dc,int x,int y,const wchar_t* text){
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,sectionFont);SetTextColor(dc,kMuted);
        TextOutW(dc,x,y,text,static_cast<int>(wcslen(text)));
    }
    // Header band: app icon, title, version, and the live status line.
    void paintHeader(HDC dc){
        const auto hr=headerRect();FillRect(dc,&hr,headerBrush);
        RECT line{hr.left,hr.bottom-1,hr.right,hr.bottom};HBRUSH lb=CreateSolidBrush(kBorder);FillRect(dc,&line,lb);DeleteObject(lb);
        if(appIcon)DrawIconEx(dc,16,16,appIcon,32,32,0,nullptr,DI_NORMAL);
        SetBkMode(dc,TRANSPARENT);
        SelectObject(dc,titleFont);SetTextColor(dc,kText);
        constexpr std::wstring_view title=L"Sony Head Tracker";
        TextOutW(dc,60,6,title.data(),static_cast<int>(title.size()));
        SIZE ts{};GetTextExtentPoint32W(dc,title.data(),static_cast<int>(title.size()),&ts);
        SelectObject(dc,font);SetTextColor(dc,kMuted);
        // Version, then an "Unofficial" tag so users know this is not a Sony product.
        const auto version=std::wstring(kVersion)+L"  ·  Unofficial, not affiliated with Sony";
        TextOutW(dc,60+ts.cx+12,12,version.c_str(),static_cast<int>(version.size()));
        // Status dot + text.
        HBRUSH dot=CreateSolidBrush(statusColor);auto oldBrush=SelectObject(dc,dot);
        HPEN nopen=CreatePen(PS_NULL,0,0);auto oldPen=SelectObject(dc,nopen);
        Ellipse(dc,60,42,70,52);
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(dot);DeleteObject(nopen);
        SetTextColor(dc,kText);
        TextOutW(dc,78,38,statusText.c_str(),static_cast<int>(statusText.size()));
    }
    // Output panel: exactly where the data goes, in plain language.
    void paintOutput(HDC dc){
        const auto r=outputRect();
        FillRect(dc,&r,panel);HBRUSH bb=CreateSolidBrush(kBorder);FrameRect(dc,&r,bb);DeleteObject(bb);
        paintSectionLabel(dc,r.left+12,r.top+8,L"OUTPUT — WHERE THE DATA GOES   (UDP to 127.0.0.1, loopback only: nothing leaves this PC)");
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);
        const auto row=[&](int y,const wchar_t* what,const std::wstring& endpoint,const wchar_t* note){
            SetTextColor(dc,kText);TextOutW(dc,r.left+12,y,what,static_cast<int>(wcslen(what)));
            SetTextColor(dc,kAccent);TextOutW(dc,r.left+150,y,endpoint.c_str(),static_cast<int>(endpoint.size()));
            SetTextColor(dc,kMuted);TextOutW(dc,r.left+320,y,note,static_cast<int>(wcslen(note)));
        };
        row(r.top+30,L"OpenTrack",std::format(L"UDP 127.0.0.1:{}",udpPort),L"six doubles (x y z yaw pitch roll) — set OpenTrack's 'UDP over network' input to this port");
        row(r.top+56,L"JSON telemetry",std::format(L"UDP 127.0.0.1:{}",udpPort+1),L"one JSON object per sample, for your own apps — format in docs/PROTOCOL.md");
    }
    // Renders the live graph into an arbitrary DC (used by the back buffer).
    void drawGraph(HDC dc, const RECT& graph){
        FillRect(dc,&graph,panel);
        HBRUSH bb=CreateSolidBrush(kBorder);FrameRect(dc,&graph,bb);DeleteObject(bb);
        const int gw=graph.right-graph.left, gh=graph.bottom-graph.top;
        const int mid=graph.top+gh/2, plot=gh-26; // vertical pixels for ±180°
        SetBkMode(dc,TRANSPARENT);
        // Horizontal grid + degree labels at +180 / +90 / 0 / -90 / -180.
        HPEN grid=CreatePen(PS_SOLID,1,kGrid);auto oldPen=SelectObject(dc,grid);
        SelectObject(dc,font);SetTextColor(dc,kMuted);
        for(int deg=-180;deg<=180;deg+=90){
            const int y=mid-static_cast<int>(deg*(plot/360.0));
            MoveToEx(dc,graph.left+1,y,nullptr);LineTo(dc,graph.right-1,y);
            const auto label=std::format(L"{:>4}°",deg);TextOutW(dc,graph.left+4,y-16,label.c_str(),static_cast<int>(label.size()));
        }
        SelectObject(dc,oldPen);DeleteObject(grid);
        paintSectionLabel(dc,graph.left+64,graph.top+6,L"LIVE ORIENTATION — degrees, Ctrl+Alt+C recenters");
        // Traces.
        const std::array<COLORREF,3> colors{RGB(96,165,255),RGB(94,214,140),RGB(255,120,150)};
        for(int a=0;a<3;++a){if(history[a].size()<2)continue;HPEN pen=CreatePen(PS_SOLID,2,colors[a]);auto old=SelectObject(dc,pen);
            for(std::size_t i=0;i<history[a].size();++i){const auto x=graph.left+static_cast<int>(i*(gw-1)/359.0);const auto y=mid-static_cast<int>(std::clamp(history[a][i],-180.0f,180.0f)*(plot/360.0f));if(i)LineTo(dc,x,y);else MoveToEx(dc,x,y,nullptr);}
            SelectObject(dc,old);DeleteObject(pen);}
        // Legend with live values (top-right swatches).
        SelectObject(dc,font);
        const std::array<const wchar_t*,3> names{L"Yaw",L"Pitch",L"Roll"};int lx=graph.right-330;
        for(int a=0;a<3;++a){
            RECT sw{lx,graph.top+8,lx+12,graph.top+20};HBRUSH b=CreateSolidBrush(colors[a]);FillRect(dc,&sw,b);DeleteObject(b);
            const auto value=history[a].empty()?std::wstring(L"—"):std::format(L"{:+.0f}°",history[a].back());
            const auto text=std::format(L"{} {}",names[a],value);
            SetTextColor(dc,kText);TextOutW(dc,lx+16,graph.top+5,text.c_str(),static_cast<int>(text.size()));lx+=110;
        }
    }
    void paint(){
        PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);
        const auto r=clientRect();
        // Double-buffer: build the frame off-screen, then blit once. This is what
        // eliminates the flicker the live graph used to cause at ~25 fps.
        HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom);auto oldBmp=SelectObject(mem,bmp);
        FillRect(mem,&r,background);
        paintHeader(mem);
        paintSectionLabel(mem,16,114,L"DEVICES");
        paintSectionLabel(mem,16+listWidth()+12,114,L"DETAILS & ACTIVITY LOG");
        paintOutput(mem);
        drawGraph(mem,graphRect());
        BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,oldBmp);DeleteObject(bmp);DeleteDC(mem);
        EndPaint(hwnd,&ps);
    }
};

LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_NCCREATE){self=reinterpret_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_CREATE:{BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        self->background=CreateSolidBrush(Window::kWindowBg);self->panel=CreateSolidBrush(Window::kPanelBg);self->headerBrush=CreateSolidBrush(Window::kHeaderBg);
        self->font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        self->titleFont=CreateFontW(-26,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        self->sectionFont=CreateFontW(-13,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        self->appIcon=static_cast<HICON>(LoadImageW(self->instance,MAKEINTRESOURCEW(1),IMAGE_ICON,32,32,LR_DEFAULTCOLOR));
        self->refresh=CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefresh)),self->instance,nullptr);self->repair=CreateWindowW(L"BUTTON",L"Repair Tracker",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRepair)),self->instance,nullptr);self->recenter=CreateWindowW(L"BUTTON",L"Recenter",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRecenter)),self->instance,nullptr);self->list=CreateWindowW(L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY|WS_VSCROLL,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceList)),self->instance,nullptr);self->details=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->raw=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Raw packet: waiting",WS_CHILD|WS_VISIBLE|ES_READONLY|ES_AUTOHSCROLL,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->stats=CreateWindowW(L"STATIC",L"Discovering devices…",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->motion=CreateWindowW(L"STATIC",L"Gyroscope  …        Accelerometer  …",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->health=CreateWindowW(L"STATIC",L"Health:  waiting for the first sample…",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,self->instance,nullptr);
        self->mapping=CreateWindowW(WC_COMBOBOXW,L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,0,0,0,0,hwnd,nullptr,self->instance,nullptr);for(const auto* text:{L"XYZ",L"XZY",L"YXZ",L"YZX",L"ZXY",L"ZYX"})SendMessageW(self->mapping,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));SendMessageW(self->mapping,CB_SETCURSEL,2,0);
        self->invertX=CreateWindowW(L"BUTTON",L"Invert X",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->invertY=CreateWindowW(L"BUTTON",L"Invert Y",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,nullptr,self->instance,nullptr);self->invertZ=CreateWindowW(L"BUTTON",L"Invert Z",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,nullptr,self->instance,nullptr);SendMessageW(self->invertX,BM_SETCHECK,BST_CHECKED,0);SendMessageW(self->invertZ,BM_SETCHECK,BST_CHECKED,0);self->smoothing=CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS,0,0,0,0,hwnd,nullptr,self->instance,nullptr);SendMessageW(self->smoothing,TBM_SETRANGE,TRUE,MAKELONG(1,100));SendMessageW(self->smoothing,TBM_SETPOS,TRUE,18);
        self->smoothingLabel=CreateWindowW(L"STATIC",L"Smoothing",WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,0,0,0,0,hwnd,nullptr,self->instance,nullptr);
        self->showAll=CreateWindowW(L"BUTTON",L"Show all devices",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowAll)),self->instance,nullptr);
        // The UAC shield tells users up front that Repair Tracker will ask for
        // administrator approval (the app itself always runs unelevated).
        SendMessageW(self->repair,BCM_SETSHIELD,0,TRUE);
        // Dark theming: DarkMode_Explorer covers buttons and scrollbars,
        // DarkMode_CFD the combo; checkboxes and the trackbar fall back to
        // classic rendering so the WM_CTLCOLOR palette below applies to them.
        for(auto h:{self->refresh,self->repair,self->recenter,self->list,self->details})SetWindowTheme(h,L"DarkMode_Explorer",nullptr);
        SetWindowTheme(self->mapping,L"DarkMode_CFD",nullptr);
        for(auto h:{self->invertX,self->invertY,self->invertZ,self->showAll,self->smoothing})SetWindowTheme(h,L" ",L" ");
        for(auto h:{self->refresh,self->repair,self->recenter,self->list,self->details,self->raw,self->stats,self->motion,self->health,self->mapping,self->invertX,self->invertY,self->invertZ,self->smoothing,self->smoothingLabel,self->showAll})SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(self->font),TRUE);
        // Tools menu for the less-frequent actions (keeps the toolbar uncluttered).
        {HMENU bar=CreateMenu();HMENU tools=CreatePopupMenu();
         AppendMenuW(tools,MF_STRING,kReconnect,L"Reconnect now");
         AppendMenuW(tools,MF_STRING,kResetDefaults,L"Reset settings to defaults");
         AppendMenuW(tools,MF_SEPARATOR,0,nullptr);
         AppendMenuW(tools,MF_STRING,kImportConfig,L"Import configuration…");
         AppendMenuW(tools,MF_STRING,kExportConfig,L"Export configuration…");
         AppendMenuW(tools,MF_SEPARATOR,0,nullptr);
         AppendMenuW(tools,MF_STRING,kExportDiag,L"Export diagnostics…");
         AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(tools),L"Tools");SetMenu(hwnd,bar);}
        // Load persisted settings and apply them to the controls + filter + port.
        self->config_=loadAppConfig();self->applyConfigToControls();
        self->udpPort=self->config_.udpPort;self->udp.open("127.0.0.1",self->udpPort);
        if(self->config_.window.valid())SetWindowPos(hwnd,nullptr,self->config_.window.x,self->config_.window.y,self->config_.window.width,self->config_.window.height,SWP_NOZORDER|SWP_NOACTIVATE);
        RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT,'C');SetTimer(hwnd,1,1000,nullptr);self->enumerate();return 0;}
    case WM_SIZE:self->layout();return 0;case WM_PAINT:self->paint();return 0;
    case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={1020,760};return 0;}
    case WM_ERASEBKGND:return 1; // the double-buffered WM_PAINT owns the surface; skip the erase that caused flicker
    case WM_CTLCOLORSTATIC:{auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,Window::kWindowBg);SetTextColor(dc,Window::kText);return reinterpret_cast<LRESULT>(self->background);}
    case WM_CTLCOLORBTN:{auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,Window::kWindowBg);SetTextColor(dc,Window::kText);return reinterpret_cast<LRESULT>(self->background);}
    case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{auto dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,Window::kPanelBg);SetTextColor(dc,Window::kText);return reinterpret_cast<LRESULT>(self->panel);}
    case WM_COMMAND:{const auto id=LOWORD(wp);
        if(id==kRefresh){self->backoffIndex=0;self->lastReconnectAttempt={};self->enumerate();return 0;}
        if(id==kReconnect){self->reconnectNow();return 0;}
        if(id==kResetDefaults){self->resetDefaults();return 0;}
        if(id==kImportConfig){self->importConfig();return 0;}
        if(id==kExportConfig){self->exportConfig();return 0;}
        if(id==kExportDiag){self->exportDiagnostics();return 0;}
        if(id==kRepair){wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",executable,L"repair",nullptr,SW_SHOWNORMAL));if(result>32){SetWindowTextW(self->stats,L"Repair started. Approve the Windows prompt; this window will reopen automatically.");PostMessageW(hwnd,WM_CLOSE,0,0);}else MessageBoxW(hwnd,L"Could not start the repair command.",L"Sony Head Tracker",MB_OK|MB_ICONERROR);return 0;}
        if(id==kRecenter){self->filter.recenter();return 0;}
        if(id==kShowAll){self->config_.showAllDevices=SendMessageW(self->showAll,BM_GETCHECK,0,0)==BST_CHECKED;self->persistConfig();self->rebuildList();self->showDetails(0);return 0;}
        if(id==kDeviceList&&HIWORD(wp)==LBN_SELCHANGE){self->showDetails(static_cast<int>(SendMessageW(self->list,LB_GETCURSEL,0,0)));return 0;}
        if(reinterpret_cast<HWND>(lp)==self->mapping||reinterpret_cast<HWND>(lp)==self->invertX||reinterpret_cast<HWND>(lp)==self->invertY||reinterpret_cast<HWND>(lp)==self->invertZ){self->applyControls();self->persistConfig();return 0;}
        break;}
    case WM_HSCROLL:if(reinterpret_cast<HWND>(lp)==self->smoothing){self->applyControls();return 0;}break;
    case WM_HOTKEY:self->filter.recenter();return 0;
    case WM_TIMER:self->tickReconnect();self->updateHealth();return 0;
    case kRawMessage:{std::unique_ptr<std::vector<std::uint8_t>> p(reinterpret_cast<std::vector<std::uint8_t>*>(lp));auto text=L"Raw packet: "+hexDump(*p);SetWindowTextW(self->raw,text.c_str());return 0;}
    case kSampleMessage:self->onSample(std::unique_ptr<MotionSample>(reinterpret_cast<MotionSample*>(lp)));return 0;
    case WM_DESTROY:self->persistConfig();UnregisterHotKey(hwnd,1);KillTimer(hwnd,1);PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}

int runGui(HINSTANCE instance,int showCommand){
#ifdef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);Window state;state.instance=instance;WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=proc;wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"SonyHeadTrackerWindow";
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));if(!wc.hIcon)wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,16,16,LR_DEFAULTCOLOR));
    RegisterClassExW(&wc);const auto title=std::format(L"Sony Head Tracker {}",kVersion);auto hwnd=CreateWindowExW(0,wc.lpszClassName,title.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1160,860,nullptr,nullptr,instance,&state);if(!hwnd)return 1;ShowWindow(hwnd,showCommand);UpdateWindow(hwnd);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return static_cast<int>(msg.wParam);}
} // namespace sony
