// hid_backend.cpp
// Raw HID backend. Windows/HID coupled; produces normalized MotionSamples.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/hid_backend.hpp"

#include "sony_head_tracker/bluetooth.hpp"
#include "sony_head_tracker/hid_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sony {

namespace {

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, INVALID_HANDLE_VALUE); } return *this; }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
};

struct Preparsed {
    PHIDP_PREPARSED_DATA value{};
    ~Preparsed() { if (value) HidD_FreePreparsedData(value); }
    Preparsed() = default;
    Preparsed(Preparsed&& o) noexcept : value(std::exchange(o.value, nullptr)) {}
    Preparsed& operator=(Preparsed&& o) noexcept { if (this != &o) { if(value) HidD_FreePreparsedData(value); value=std::exchange(o.value,nullptr); } return *this; }
    Preparsed(const Preparsed&) = delete; Preparsed& operator=(const Preparsed&) = delete;
};

std::wstring hidString(HANDLE h, BOOLEAN (__stdcall *fn)(HANDLE, PVOID, ULONG)) {
    std::array<wchar_t, 256> b{}; return fn(h, b.data(), static_cast<ULONG>(b.size()*sizeof(wchar_t))) ? b.data() : L"";
}

DescriptorField makeField(const HIDP_VALUE_CAPS& c, bool feature) {
    DescriptorField f;
    f.usagePage=c.UsagePage; f.usage=c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
    f.reportId=c.ReportID; f.reportCount=c.ReportCount; f.bitSize=c.BitSize;
    f.logicalMin=c.LogicalMin; f.logicalMax=c.LogicalMax; f.physicalMin=c.PhysicalMin; f.physicalMax=c.PhysicalMax;
    f.unitExponent=decodeHidUnitExponent(c.UnitsExp); f.unit=c.Units;
    f.dataIndex=c.IsRange ? c.Range.DataIndexMin : c.NotRange.DataIndex; f.feature=feature;
    return f;
}

std::vector<HIDP_VALUE_CAPS> getValueCaps(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA ppd, USHORT count) {
    std::vector<HIDP_VALUE_CAPS> result(count);
    if (!count) return result;
    auto n=count; if (HidP_GetValueCaps(type, result.data(), &n, ppd) != HIDP_STATUS_SUCCESS) return {};
    result.resize(n); return result;
}

std::vector<HIDP_BUTTON_CAPS> getButtonCaps(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA ppd, USHORT count) {
    std::vector<HIDP_BUTTON_CAPS> result(count);
    if (!count) return result;
    auto n=count; if (HidP_GetButtonCaps(type, result.data(), &n, ppd) != HIDP_STATUS_SUCCESS) return {};
    result.resize(n); return result;
}

std::string extractDescription(HANDLE handle, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps,
                               const std::vector<HIDP_VALUE_CAPS>& featureCaps, std::vector<std::string>& diagnostics) {
    for (const auto& c : featureCaps) {
        const auto usage = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        if (c.UsagePage != kSensorPage || usage != kSensorDescription) continue;
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=c.ReportID;
        if (!HidD_GetFeature(handle, report.data(), static_cast<ULONG>(report.size()))) {
            diagnostics.push_back(std::format("feature report {} read failed: {}", c.ReportID, GetLastError())); continue;
        }
        std::ostringstream raw; raw<<"feature report "<<static_cast<unsigned>(c.ReportID)<<":"<<std::hex<<std::setfill('0');
        for(const auto b:report)raw<<' '<<std::setw(2)<<static_cast<unsigned>(b);diagnostics.push_back(raw.str());
        const auto byteCount = static_cast<USHORT>((static_cast<unsigned long long>(c.ReportCount)*c.BitSize+7)/8);
        std::vector<std::uint8_t> value(byteCount);
        auto status = HidP_GetUsageValueArray(HidP_Feature, c.UsagePage, c.LinkCollection, usage,
            reinterpret_cast<PCHAR>(value.data()), byteCount, ppd, reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size()));
        if (status == HIDP_STATUS_SUCCESS) {
            std::string s(value.begin(), value.end());
            while (!s.empty() && s.back()=='\0') s.pop_back();
            return s;
        }
        // Constant sensor-description fields are not exposed by some Windows HID parser versions.
        const auto it = std::search(report.begin(), report.end(), kMarker.begin(), kMarker.end());
        if (it != report.end()) {
            std::string s(it, report.end()); while (!s.empty() && (s.back()=='\0' || static_cast<unsigned char>(s.back())==0xff)) s.pop_back(); return s;
        }
    }
    // Some Sensor HID class stacks omit constant fields from value capabilities. Probe only report IDs
    // discovered from the descriptor's remaining feature capabilities, never guessed numeric IDs.
    std::set<std::uint8_t> reportIds;
    for (const auto& c : featureCaps) reportIds.insert(c.ReportID);
    for (const auto reportId : reportIds) {
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=reportId;
        if (!HidD_GetFeature(handle,report.data(),static_cast<ULONG>(report.size()))) continue;
        std::ostringstream dump; dump<<"feature report "<<static_cast<unsigned>(reportId)<<":";
        dump<<std::hex<<std::setfill('0'); for(const auto b:report) dump<<' '<<std::setw(2)<<static_cast<unsigned>(b);
        diagnostics.push_back(dump.str());
        const auto it=std::search(report.begin(),report.end(),kMarker.begin(),kMarker.end());
        if(it!=report.end()){std::string s(it,report.end());while(!s.empty()&&(s.back()=='\0'||static_cast<unsigned char>(s.back())==0xff))s.pop_back();return s;}
    }
    return {};
}

bool updateArrayFeature(HANDLE handle, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps,
                        const std::vector<HIDP_BUTTON_CAPS>& buttons, USAGE desired, std::wstring_view label, bool warnIfMissing=true) {
    for (const auto& b : buttons) {
        const auto min = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
        const auto max = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
        if (b.UsagePage != kSensorPage || desired < min || desired > max) continue;
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=b.ReportID;
        HidD_GetFeature(handle, report.data(), static_cast<ULONG>(report.size()));
        const auto maximum=HidP_MaxUsageListLength(HidP_Feature,b.UsagePage,ppd);
        if(maximum){std::vector<USAGE> existing(maximum);ULONG existingCount=maximum;if(HidP_GetUsages(HidP_Feature,b.UsagePage,b.LinkCollection,existing.data(),&existingCount,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()))==HIDP_STATUS_SUCCESS&&existingCount)HidP_UnsetUsages(HidP_Feature,b.UsagePage,b.LinkCollection,existing.data(),&existingCount,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));}
        ULONG count=1; USAGE usage=desired;
        const auto parsed = HidP_SetUsages(HidP_Feature, b.UsagePage, b.LinkCollection, &usage, &count, ppd,
            reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size()));
        if (parsed != HIDP_STATUS_SUCCESS || !HidD_SetFeature(handle, report.data(), static_cast<ULONG>(report.size()))) {
            Logger::instance().write(LogLevel::error, std::format(L"Failed to set {} (parser=0x{:08X}, Win32={}: {})", label, static_cast<unsigned>(parsed), GetLastError(), windowsError(GetLastError())));
            return false;
        }
        Logger::instance().write(LogLevel::info, std::format(L"Set {} using descriptor report ID {}", label, b.ReportID)); return true;
    }
    if(warnIfMissing)Logger::instance().write(LogLevel::warning, std::format(L"Descriptor does not expose writable {} selector", label)); return false;
}

bool updateInterval(HANDLE handle, PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps, const std::vector<HIDP_VALUE_CAPS>& values) {
    for (const auto& c : values) {
        const auto usage=c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        if (c.UsagePage != kSensorPage || usage != kReportInterval) continue;
        const auto low=std::min(c.PhysicalMin,c.PhysicalMax), high=std::max(c.PhysicalMin,c.PhysicalMax);
        const auto exponent=decodeHidUnitExponent(c.UnitsExp);
        const auto unitScale=std::pow(10.0,exponent);
        const auto supportedLow=low*unitScale,supportedHigh=high*unitScale;
        auto targetSeconds=std::max(0.010,supportedLow);
        if(targetSeconds>0.020||supportedHigh<0.010){targetSeconds=supportedLow;Logger::instance().write(LogLevel::warning,std::format(L"Device interval range {:.3f}..{:.3f} ms does not support the protocol's 10..20 ms target; using fastest advertised interval {:.3f} ms",supportedLow*1000.0,supportedHigh*1000.0,targetSeconds*1000.0));}
        const LONG target=std::clamp<LONG>(static_cast<LONG>(std::llround(targetSeconds/unitScale)),low,high);
        std::vector<std::uint8_t> report(caps.FeatureReportByteLength); report[0]=c.ReportID;
        HidD_GetFeature(handle, report.data(), static_cast<ULONG>(report.size()));
        const auto status=HidP_SetScaledUsageValue(HidP_Feature,c.UsagePage,c.LinkCollection,usage,target,ppd,
            reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));
        if(status != HIDP_STATUS_SUCCESS || !HidD_SetFeature(handle,report.data(),static_cast<ULONG>(report.size()))) {
            Logger::instance().write(LogLevel::error,std::format(L"Failed setting report interval (parser=0x{:08X}, Win32={})",static_cast<unsigned>(status),GetLastError())); return false;
        }
        Logger::instance().write(LogLevel::info,std::format(L"Set report interval to {} x 10^{} seconds (report ID {})",target,exponent,c.ReportID)); return true;
    }
    Logger::instance().write(LogLevel::warning,L"Descriptor does not expose writable report interval"); return false;
}

bool configureHeadTrackerFeatures(HANDLE handle,PHIDP_PREPARSED_DATA ppd,const HIDP_CAPS& caps,const std::vector<HIDP_VALUE_CAPS>& values,const std::vector<HIDP_BUTTON_CAPS>& buttons){
    std::map<UCHAR,std::vector<std::uint8_t>> reports;auto ensure=[&](UCHAR id)->std::vector<std::uint8_t>&{auto& report=reports[id];if(report.empty()){report.resize(caps.FeatureReportByteLength);report[0]=id;}return report;};
    for(const auto& c:values){const auto usage=c.IsRange?c.Range.UsageMin:c.NotRange.Usage;if(c.UsagePage!=kSensorPage||usage!=kReportInterval)continue;auto& report=ensure(c.ReportID);const auto low=std::min(c.PhysicalMin,c.PhysicalMax),high=std::max(c.PhysicalMin,c.PhysicalMax);const auto exponent=decodeHidUnitExponent(c.UnitsExp);const auto scale=std::pow(10.0,exponent);const auto supportedLow=low*scale,supportedHigh=high*scale;auto targetSeconds=std::max(0.010,supportedLow);if(targetSeconds>0.020||supportedHigh<0.010){targetSeconds=supportedLow;Logger::instance().write(LogLevel::warning,std::format(L"Device interval range {:.3f}..{:.3f} ms is outside 10..20 ms; using {:.3f} ms",supportedLow*1000.0,supportedHigh*1000.0,targetSeconds*1000.0));}const auto target=std::clamp<LONG>(static_cast<LONG>(std::llround(targetSeconds/scale)),low,high);const auto status=HidP_SetScaledUsageValue(HidP_Feature,c.UsagePage,c.LinkCollection,usage,target,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));if(status!=HIDP_STATUS_SUCCESS){Logger::instance().write(LogLevel::error,std::format(L"Could not encode report interval (0x{:08X})",static_cast<unsigned>(status)));return false;}Logger::instance().write(LogLevel::info,std::format(L"Encoded interval {} x 10^{} seconds in report {}",target,exponent,c.ReportID));}
    const std::array<std::pair<USAGE,std::wstring_view>,3> desired{{{kTransportAcl,L"v2 ACL transport"},{kPowerFull,L"Full Power"},{kReportingAllEvents,L"All Events reporting"}}};
    for(const auto& [usage,label]:desired){bool exposed{};for(const auto& b:buttons){const auto min=b.IsRange?b.Range.UsageMin:b.NotRange.Usage,max=b.IsRange?b.Range.UsageMax:b.NotRange.Usage;if(b.UsagePage!=kSensorPage||usage<min||usage>max)continue;exposed=true;auto& report=ensure(b.ReportID);ULONG count=1;auto mutableUsage=usage;const auto status=HidP_SetUsages(HidP_Feature,b.UsagePage,b.LinkCollection,&mutableUsage,&count,ppd,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()));if(status!=HIDP_STATUS_SUCCESS){Logger::instance().write(LogLevel::error,std::format(L"Could not encode {} (0x{:08X})",label,static_cast<unsigned>(status)));return false;}Logger::instance().write(LogLevel::info,std::format(L"Encoded {} in report {}",label,b.ReportID));break;}if(!exposed&&usage!=kTransportAcl){Logger::instance().write(LogLevel::error,std::format(L"Descriptor lacks {}",label));return false;}}
    for(auto& [id,report]:reports){Logger::instance().write(LogLevel::info,std::format(L"Sending combined feature report {} ({} bytes)",id,report.size()));if(!HidD_SetFeature(handle,report.data(),static_cast<ULONG>(report.size()))){Logger::instance().write(LogLevel::error,std::format(L"SetFeature report {} failed: {}",id,windowsError(GetLastError())));return false;}Logger::instance().write(LogLevel::info,std::format(L"Feature report {} accepted",id));}return !reports.empty();
}

std::vector<double> usageArray(PHIDP_PREPARSED_DATA ppd, const HIDP_VALUE_CAPS& c, const std::vector<std::uint8_t>& report) {
    const auto usage=c.IsRange?c.Range.UsageMin:c.NotRange.Usage;
    const auto bytesCount=static_cast<USHORT>((static_cast<unsigned long long>(c.ReportCount)*c.BitSize+7)/8);
    std::vector<std::uint8_t> packed(bytesCount);
    if(HidP_GetUsageValueArray(HidP_Input,c.UsagePage,c.LinkCollection,usage,reinterpret_cast<PCHAR>(packed.data()),bytesCount,ppd,
        reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report.data())),static_cast<ULONG>(report.size())) != HIDP_STATUS_SUCCESS) return {};
    return decodePackedDescriptorValues(packed,makeField(c,false));
}

// Reads a single scaled scalar value, honouring the descriptor's logical/physical
// ranges and unit exponent. Used for the per-axis acceleration / gyro usages.
bool scalarValue(PHIDP_PREPARSED_DATA ppd, const HIDP_VALUE_CAPS& c, const std::vector<std::uint8_t>& report, double& out) {
    const auto usage=c.IsRange?c.Range.UsageMin:c.NotRange.Usage;
    LONG scaled{};
    if(HidP_GetScaledUsageValue(HidP_Input,c.UsagePage,c.LinkCollection,usage,&scaled,ppd,
        reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report.data())),static_cast<ULONG>(report.size()))==HIDP_STATUS_SUCCESS){out=static_cast<double>(scaled);return true;}
    ULONG raw{};
    if(HidP_GetUsageValue(HidP_Input,c.UsagePage,c.LinkCollection,usage,&raw,ppd,
        reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report.data())),static_cast<ULONG>(report.size()))!=HIDP_STATUS_SUCCESS) return false;
    std::int64_t value=static_cast<std::int64_t>(raw);
    if(c.LogicalMin<0&&c.BitSize&&c.BitSize<64){const auto sign=std::uint64_t{1}<<(c.BitSize-1);const auto mask=(std::uint64_t{1}<<c.BitSize)-1;value=static_cast<std::int64_t>(((static_cast<std::uint64_t>(raw)&mask)^sign)-sign);}
    out=descriptorScale(value,c.LogicalMin,c.LogicalMax,c.PhysicalMin,c.PhysicalMax,decodeHidUnitExponent(c.UnitsExp));
    return true;
}

} // namespace

struct HidBackend::Context {
    Handle handle;
    Preparsed ppd;
    HIDP_CAPS caps{};
    std::vector<HIDP_VALUE_CAPS> inputValues;
    RawCallback raw;
    SampleCallback sample;
    std::chrono::steady_clock::time_point rateStart{std::chrono::steady_clock::now()};
    std::uint64_t rateCount{};
    double rate{};
};

HidBackend::HidBackend() = default;
HidBackend::~HidBackend() { disconnect(); }

std::vector<DeviceInfo> HidBackend::enumerate(bool presentInterfacesOnly) {
    std::vector<DeviceInfo> devices;
    GUID guid{}; HidD_GetHidGuid(&guid);
    const auto flags=DIGCF_DEVICEINTERFACE|(presentInterfacesOnly?DIGCF_PRESENT:0);
    const auto set=SetupDiGetClassDevsW(&guid,nullptr,nullptr,flags);
    if(set==INVALID_HANDLE_VALUE) { Logger::instance().write(LogLevel::error,std::format(L"SetupAPI HID enumeration failed: {}",windowsError(GetLastError()))); return devices; }
    SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
    for(DWORD index=0;SetupDiEnumDeviceInterfaces(set,nullptr,&guid,index,&iface);++index) {
        DWORD needed{}; SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&needed,nullptr);
        std::vector<std::uint8_t> storage(needed); auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data()); detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA dev{}; dev.cbSize=sizeof(dev);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,needed,nullptr,&dev)) continue;
        DeviceInfo info; info.path=detail->DevicePath;
        wchar_t instance[MAX_DEVICE_ID_LEN]{}; if(CM_Get_Device_IDW(dev.DevInst,instance,MAX_DEVICE_ID_LEN,0)==CR_SUCCESS) info.instanceId=instance;
        Handle handle(CreateFileW(info.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
        if(handle.value==INVALID_HANDLE_VALUE) {
            const auto writeError=GetLastError(); handle=Handle(CreateFileW(info.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
            if(handle.value==INVALID_HANDLE_VALUE) handle=Handle(CreateFileW(info.path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
            if(handle.value==INVALID_HANDLE_VALUE) {
                info.accessDenied=writeError==ERROR_ACCESS_DENIED||GetLastError()==ERROR_ACCESS_DENIED;
                Logger::instance().write(LogLevel::error,std::format(L"Cannot open HID {}: {}",info.instanceId,windowsError(GetLastError()))); devices.push_back(std::move(info)); continue;
            }
            Logger::instance().write(LogLevel::warning,std::format(L"HID {} is not writable: {}",info.instanceId,windowsError(writeError)));
        }
        HIDD_ATTRIBUTES attributes{}; attributes.Size=sizeof(attributes); if(HidD_GetAttributes(handle.value,&attributes)) { info.vendorId=attributes.VendorID;info.productId=attributes.ProductID;info.version=attributes.VersionNumber; }
        info.product=hidString(handle.value,HidD_GetProductString); info.manufacturer=hidString(handle.value,HidD_GetManufacturerString);
        Preparsed ppd; if(!HidD_GetPreparsedData(handle.value,&ppd.value)) { devices.push_back(std::move(info)); continue; }
        HIDP_CAPS caps{}; if(HidP_GetCaps(ppd.value,&caps)!=HIDP_STATUS_SUCCESS) { devices.push_back(std::move(info)); continue; }
        info.usagePage=caps.UsagePage;info.usage=caps.Usage;info.inputReportBytes=caps.InputReportByteLength;info.featureReportBytes=caps.FeatureReportByteLength;
        auto inputs=getValueCaps(HidP_Input,ppd.value,caps.NumberInputValueCaps);
        auto features=getValueCaps(HidP_Feature,ppd.value,caps.NumberFeatureValueCaps);
        for(const auto& c:inputs) info.fields.push_back(makeField(c,false));
        for(const auto& c:features) info.fields.push_back(makeField(c,true));
        if(info.usagePage==kSensorPage&&info.usage==kOtherCustom) {
            info.sensorDescription=extractDescription(handle.value,ppd.value,caps,features,info.featureValues);
            info.androidHeadTracker=info.sensorDescription.starts_with(kMarker);
            info.bluetoothName=bluetoothNameForHidInstance(info.instanceId);
            Logger::instance().write(info.androidHeadTracker?LogLevel::info:LogLevel::warning,
                std::format(L"Candidate HID VID={:04X} PID={:04X}, headset='{}', description='{}'",info.vendorId,info.productId,
                    info.bluetoothName.empty()?L"(unresolved)":info.bluetoothName,
                    std::wstring(info.sensorDescription.begin(),info.sensorDescription.end())));
        }
        devices.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
    Logger::instance().write(LogLevel::info,std::format(L"SetupAPI discovered {} HID top-level collection(s)",devices.size()));
    return devices;
}

bool HidBackend::connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample) {
    disconnect(); auto ctx=std::make_unique<Context>();
    ctx->handle=Handle(CreateFileW(device.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    if(ctx->handle.value==INVALID_HANDLE_VALUE) { Logger::instance().write(LogLevel::error,std::format(L"Head tracker open failed: {}",windowsError(GetLastError()))); return false; }
    if(!HidD_GetPreparsedData(ctx->handle.value,&ctx->ppd.value)||HidP_GetCaps(ctx->ppd.value,&ctx->caps)!=HIDP_STATUS_SUCCESS) {
        Logger::instance().write(LogLevel::error,L"Could not obtain head tracker preparsed descriptor"); return false;
    }
    ctx->inputValues=getValueCaps(HidP_Input,ctx->ppd.value,ctx->caps.NumberInputValueCaps);
    const auto featureValues=getValueCaps(HidP_Feature,ctx->ppd.value,ctx->caps.NumberFeatureValueCaps);
    const auto featureButtons=getButtonCaps(HidP_Feature,ctx->ppd.value,ctx->caps.NumberFeatureButtonCaps);
    if(!configureHeadTrackerFeatures(ctx->handle.value,ctx->ppd.value,ctx->caps,featureValues,featureButtons)){Logger::instance().write(LogLevel::error,L"Head tracker feature configuration failed");return false;}
    ctx->raw=std::move(raw);ctx->sample=std::move(sample);context_=std::move(ctx);running_=true;
    reader_=std::jthread([this](std::stop_token stop) {
        auto* c=context_.get(); std::vector<std::uint8_t> report(c->caps.InputReportByteLength);
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        OVERLAPPED ov{}; ov.hEvent=event.value;
        while(!stop.stop_requested()) {
            DWORD bytes{}; ResetEvent(event.value);
            if(!ReadFile(c->handle.value,report.data(),static_cast<DWORD>(report.size()),&bytes,&ov)&&GetLastError()!=ERROR_IO_PENDING) {
                Logger::instance().write(LogLevel::error,std::format(L"HID read failed: {}",windowsError(GetLastError())));break;
            }
            while(!stop.stop_requested()) { const auto wait=WaitForSingleObject(event.value,100);if(wait==WAIT_OBJECT_0)break;if(wait==WAIT_FAILED)break; }
            if(stop.stop_requested()) { CancelIoEx(c->handle.value,&ov);break; }
            if(!GetOverlappedResult(c->handle.value,&ov,&bytes,FALSE)) { Logger::instance().write(LogLevel::error,std::format(L"HID asynchronous read failed: {}",windowsError(GetLastError())));break; }
            if(bytes==0)continue;report.resize(bytes);if(c->raw)c->raw(report);
            MotionSample s;s.receivedAt=std::chrono::steady_clock::now();bool gotRotation=false;
            Vec3 gyro{};bool gotGyro=false;Vec3 accel{};bool gotAccel=false;
            for(const auto& field:c->inputValues) {
                if(field.ReportID && report[0]!=field.ReportID)continue;
                const auto usage=field.IsRange?field.Range.UsageMin:field.NotRange.Usage;if(field.UsagePage!=kSensorPage)continue;
                if(usage==kRotation||usage==kAngularVelocity||usage==kAngularVelocityVector||usage==kAccelerationVector) {
                    // Vector-form fields: a packed array of three values.
                    const auto values=usageArray(c->ppd.value,field,report);if(values.size()<3)continue;
                    if(usage==kRotation){s.rotationVector={values[0],values[1],values[2]};gotRotation=true;}
                    else if(usage==kAccelerationVector){accel={values[0],values[1],values[2]};gotAccel=true;}
                    else{gyro={values[0],values[1],values[2]};gotGyro=true;} // 0x0545 / 0x0456
                } else if(usage==kAccelerationX||usage==kAccelerationY||usage==kAccelerationZ) {
                    double v{};if(scalarValue(c->ppd.value,field,report,v)){accel[usage-kAccelerationX]=v;gotAccel=true;}
                } else if(usage==kAngularVelocityX||usage==kAngularVelocityY||usage==kAngularVelocityZ) {
                    double v{};if(scalarValue(c->ppd.value,field,report,v)){gyro[usage-kAngularVelocityX]=v;gotGyro=true;}
                } else if(usage==kResetCounter) {
                    ULONG v{};if(HidP_GetUsageValue(HidP_Input,field.UsagePage,field.LinkCollection,usage,&v,c->ppd.value,reinterpret_cast<PCHAR>(report.data()),static_cast<ULONG>(report.size()))==HIDP_STATUS_SUCCESS)s.resetCounter=static_cast<std::uint8_t>(v);
                }
            }
            if(gotGyro)s.angularVelocity=gyro;
            if(gotAccel)s.acceleration=accel;
            ++c->rateCount;const auto elapsed=std::chrono::duration<double>(s.receivedAt-c->rateStart).count();if(elapsed>=1.0){c->rate=c->rateCount/elapsed;c->rateCount=0;c->rateStart=s.receivedAt;}s.packetsPerSecond=c->rate;s.receiveLatencyMs=-1.0;
            if(gotRotation&&c->sample)c->sample(std::move(s));
            report.resize(c->caps.InputReportByteLength);
        }
        running_=false;
    });
    Logger::instance().write(LogLevel::info,L"Asynchronous HID report reader started");return true;
}

void HidBackend::disconnect() {
    running_=false;if(reader_.joinable()){reader_.request_stop();if(context_&&context_->handle.value!=INVALID_HANDLE_VALUE)CancelIoEx(context_->handle.value,nullptr);reader_.join();}context_.reset();
}

std::wstring hexDump(const std::vector<std::uint8_t>& bytes) {
    std::wostringstream out;out<<std::hex<<std::uppercase<<std::setfill(L'0');for(std::size_t i=0;i<bytes.size();++i){if(i)out<<L' ';out<<std::setw(2)<<static_cast<unsigned>(bytes[i]);}return out.str();
}

} // namespace sony
