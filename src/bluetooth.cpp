// bluetooth.cpp
// Read-only Bluetooth investigation + driver-rebind recovery. Windows-coupled.
#include "sony_head_tracker/windows_prelude.hpp"

#include "sony_head_tracker/bluetooth.hpp"

#include "sony_head_tracker/hid_descriptor.hpp"
#include "sony_head_tracker/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <format>
#include <iomanip>
#include <ostream>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sony {

namespace {

struct FindDeviceHandle {
    HBLUETOOTH_DEVICE_FIND value{};
    ~FindDeviceHandle(){if(value)BluetoothFindDeviceClose(value);}
};

struct FindRadioHandle {
    HBLUETOOTH_RADIO_FIND value{};
    ~FindRadioHandle(){if(value)BluetoothFindRadioClose(value);}
};

struct LookupHandle {
    HANDLE value{};
    ~LookupHandle(){if(value)WSALookupServiceEnd(value);}
};

struct WinHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~WinHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};

std::wstring lower(std::wstring_view text){std::wstring result(text);std::ranges::transform(result,result.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});return result;}
bool containsInsensitive(std::wstring_view text,std::wstring_view needle){return needle.empty()||lower(text).find(lower(needle))!=std::wstring::npos;}

std::wstring addressText(BLUETOOTH_ADDRESS address){
    return std::format(L"{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",address.rgBytes[5],address.rgBytes[4],address.rgBytes[3],address.rgBytes[2],address.rgBytes[1],address.rgBytes[0]);
}

// Extracts the 48-bit Bluetooth address embedded in a BTHENUM PnP instance ID.
// Device nodes look like BTHENUM\Dev_F8DF15AABBCC\...; service child nodes end
// with ...&0&F8DF15AABBCC_C00000000. Service GUIDs also contain 12-hex-digit
// runs, so the match is anchored to a "Dev_" prefix or a '&' delimiter.
bool addressFromBthenumId(std::wstring_view id,BLUETOOTH_ADDRESS& address){
    std::wstring text(id);std::ranges::transform(text,text.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    const auto isHex=[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F');};
    const auto tryParse=[&](std::size_t pos){
        if(pos+12>text.size())return false;
        for(std::size_t i=0;i<12;++i)if(!isHex(text[pos+i]))return false;
        if(pos+12<text.size()&&isHex(text[pos+12]))return false;
        std::uint64_t value{};for(std::size_t i=0;i<12;++i){const auto c=text[pos+i];value=(value<<4)|static_cast<std::uint64_t>(c<=L'9'?c-L'0':c-L'A'+10);}
        if(!value)return false;address.ullLong=value;return true;
    };
    if(const auto dev=text.find(L"DEV_");dev!=std::wstring::npos&&tryParse(dev+4))return true;
    for(auto pos=text.find(L'&');pos!=std::wstring::npos;pos=text.find(L'&',pos+1))if(tryParse(pos+1))return true;
    return false;
}

// Enumerates paired Classic Bluetooth devices across all local radios.
std::vector<BLUETOOTH_DEVICE_INFO> pairedClassicDevices(){
    std::vector<BLUETOOTH_DEVICE_INFO> result;
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;search.fReturnConnected=TRUE;
    BLUETOOTH_DEVICE_INFO device{};device.dwSize=sizeof(device);
    FindDeviceHandle find;find.value=BluetoothFindFirstDevice(&search,&device);
    if(!find.value)return result;
    do{result.push_back(device);device={};device.dwSize=sizeof(device);}while(BluetoothFindNextDevice(find.value,&device));
    return result;
}

bool hasPresentBluetoothHidChild(BLUETOOTH_ADDRESS address){
    auto compactAddress=addressText(address);
    std::erase(compactAddress,L':');
    const auto set=SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    if(set==INVALID_HANDLE_VALUE)return false;
    bool found{};
    for(DWORD index=0;!found;++index){
        SP_DEVINFO_DATA dev{};dev.cbSize=sizeof(dev);
        if(!SetupDiEnumDeviceInfo(set,index,&dev)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;continue;}
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(set,&dev,instance,MAX_DEVICE_ID_LEN,nullptr))continue;
        const std::wstring_view id(instance);
        if(!id.starts_with(L"HID\\"))continue;
        DEVINST parent{};
        if(CM_Get_Parent(&parent,dev.DevInst,0)!=CR_SUCCESS)continue;
        wchar_t parentId[MAX_DEVICE_ID_LEN]{};
        if(CM_Get_Device_IDW(parent,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)continue;
        const std::wstring_view parentText(parentId);
        found=containsInsensitive(parentText,L"BTHENUM\\{00001124-0000-1000-8000-00805F9B34FB}")&&containsInsensitive(parentText,compactAddress);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

std::wstring uuidText(const BTH_LE_UUID& uuid){
    if(uuid.IsShortUuid)return std::format(L"0x{:04X}",uuid.Value.ShortUuid);
    wchar_t buffer[40]{};StringFromGUID2(uuid.Value.LongUuid,buffer,40);return buffer;
}

std::wstring bytesText(const std::uint8_t* data,std::size_t size,std::size_t limit=256){
    std::wostringstream out;out<<std::hex<<std::uppercase<<std::setfill(L'0');
    const auto shown=std::min(size,limit);for(std::size_t i=0;i<shown;++i){if(i)out<<L' ';out<<std::setw(2)<<static_cast<unsigned>(data[i]);}
    if(shown<size)out<<std::format(L" … ({} bytes total)",size);return out.str();
}

// hidSigned lives in hid_descriptor (pure); describeHidReport calls sony::hidSigned.

struct HidDescriptorSummary {
    bool sensorApplication{};
    bool hasDescription{};
    bool hasRotation{};
    bool hasVelocity{};
    bool hasReset{};
    std::set<unsigned> reportIds;
};

HidDescriptorSummary describeHidReport(const std::uint8_t* data,std::size_t size,std::wostream& out){
    HidDescriptorSummary summary;std::uint32_t usagePage{},usage{};std::size_t i{};
    out<<L"      HID report descriptor ("<<size<<L" bytes):\n";
    while(i<size){const auto offset=i;const auto prefix=data[i++];if(prefix==0xFE){if(i+2>size)break;const auto length=data[i++];const auto tag=data[i++];out<<std::format(L"        {:04X}: long item tag=0x{:02X} length={}\n",offset,tag,length);i=std::min(size,i+length);continue;}
        const auto sizeCode=prefix&3u;const unsigned length=sizeCode==3?4:sizeCode;const auto type=(prefix>>2)&3u;const auto tag=(prefix>>4)&0xFu;if(i+length>size)break;std::uint32_t value{};for(unsigned b=0;b<length;++b)value|=static_cast<std::uint32_t>(data[i+b])<<(8*b);i+=length;
        if(type==1&&tag==0){usagePage=value;out<<std::format(L"        {:04X}: Usage Page 0x{:X}\n",offset,value);}
        else if(type==2&&tag==0){usage=length==4?value:((usagePage<<16)|value);const auto page=usage>>16,u=usage&0xffff;out<<std::format(L"        {:04X}: Usage 0x{:04X}:0x{:04X}\n",offset,page,u);if(page==0x20&&u==0x0308)summary.hasDescription=true;if(page==0x20&&u==0x0544)summary.hasRotation=true;if(page==0x20&&u==0x0545)summary.hasVelocity=true;if(page==0x20&&u==0x0546)summary.hasReset=true;}
        else if(type==0&&tag==10){const auto page=usage>>16,u=usage&0xffff;out<<std::format(L"        {:04X}: Collection {} for 0x{:04X}:0x{:04X}\n",offset,value,page,u);if(value==1&&page==0x20&&u==0x00E1)summary.sensorApplication=true;usage=0;}
        else if(type==0&&tag==12)out<<std::format(L"        {:04X}: End Collection\n",offset);
        else if(type==1&&tag==8){summary.reportIds.insert(value);out<<std::format(L"        {:04X}: Report ID {}\n",offset,value);}
        else if(type==1&&tag==7)out<<std::format(L"        {:04X}: Report Size {}\n",offset,value);
        else if(type==1&&tag==9)out<<std::format(L"        {:04X}: Report Count {}\n",offset,value);
        else if(type==1&&tag==1)out<<std::format(L"        {:04X}: Logical Minimum {}\n",offset,hidSigned(value,length));
        else if(type==1&&tag==2)out<<std::format(L"        {:04X}: Logical Maximum {}\n",offset,hidSigned(value,length));
        else if(type==1&&tag==3)out<<std::format(L"        {:04X}: Physical Minimum {}\n",offset,hidSigned(value,length));
        else if(type==1&&tag==4)out<<std::format(L"        {:04X}: Physical Maximum {}\n",offset,hidSigned(value,length));
        else if(type==0&&(tag==8||tag==9||tag==11))out<<std::format(L"        {:04X}: {} flags=0x{:X}\n",offset,tag==8?L"Input":tag==9?L"Output":L"Feature",value);
    }
    out<<std::format(L"      Android fields: collection={} description={} rotation={} velocity={} reset={}\n",summary.sensorApplication,summary.hasDescription,summary.hasRotation,summary.hasVelocity,summary.hasReset);
    return summary;
}

void collectSdpStrings(LPBYTE stream,ULONG length,std::vector<std::vector<std::uint8_t>>& strings){
    SDP_ELEMENT_DATA data{};if(BluetoothSdpGetElementData(stream,length,&data)!=ERROR_SUCCESS)return;
    if(data.type==SDP_TYPE_STRING){strings.emplace_back(data.data.string.value,data.data.string.value+data.data.string.length);return;}
    LPBYTE nested=nullptr;ULONG nestedLength{};if(data.type==SDP_TYPE_SEQUENCE){nested=data.data.sequence.value;nestedLength=data.data.sequence.length;}else if(data.type==SDP_TYPE_ALTERNATIVE){nested=data.data.alternative.value;nestedLength=data.data.alternative.length;}else return;
    HBLUETOOTH_CONTAINER_ELEMENT element{};while(true){SDP_ELEMENT_DATA child{};const auto status=BluetoothSdpGetContainerElementData(nested,nestedLength,&element,&child);if(status==ERROR_NO_MORE_ITEMS)break;if(status!=ERROR_SUCCESS)break;if(child.type==SDP_TYPE_STRING)strings.emplace_back(child.data.string.value,child.data.string.value+child.data.string.length);else if(child.type==SDP_TYPE_SEQUENCE)collectSdpStrings(child.data.sequence.value,child.data.sequence.length,strings);else if(child.type==SDP_TYPE_ALTERNATIVE)collectSdpStrings(child.data.alternative.value,child.data.alternative.length,strings);}
}

struct SdpAttributeContext {std::wostream* output{};bool foundDescriptor{};bool androidDescriptor{};};

BOOL CALLBACK sdpAttribute(ULONG id,LPBYTE value,ULONG length,LPVOID rawContext){
    auto& context=*static_cast<SdpAttributeContext*>(rawContext);*context.output<<std::format(L"    SDP attribute 0x{:04X}, {} bytes: {}\n",id,length,bytesText(value,length,id==0x0206?4096:128));
    if(id!=0x0206)return TRUE;std::vector<std::vector<std::uint8_t>> strings;collectSdpStrings(value,length,strings);
    for(const auto& candidate:strings){if(candidate.size()<4)continue;context.foundDescriptor=true;const auto summary=describeHidReport(candidate.data(),candidate.size(),*context.output);context.androidDescriptor|=summary.sensorApplication&&summary.hasDescription&&summary.hasRotation&&summary.hasVelocity&&summary.hasReset;}
    return TRUE;
}

struct ClassicSdpSummary{bool recordReturned{};bool androidDescriptor{};};

ClassicSdpSummary queryClassicSdp(const BLUETOOTH_DEVICE_INFO& device,std::wostream& out){
    ClassicSdpSummary summary;
    SOCKADDR_BTH socketAddress{};socketAddress.addressFamily=AF_BTH;socketAddress.btAddr=device.Address.ullLong;wchar_t contextAddress[64]{};DWORD contextLength=64;
    if(WSAAddressToStringW(reinterpret_cast<LPSOCKADDR>(&socketAddress),sizeof(socketAddress),nullptr,contextAddress,&contextLength)!=0){out<<L"  SDP address formatting failed: "<<WSAGetLastError()<<L"\n";return summary;}
    GUID hidService{0x00001124,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};WSAQUERYSETW query{};query.dwSize=sizeof(query);query.dwNameSpace=NS_BTH;query.lpServiceClassId=&hidService;query.lpszContext=contextAddress;
    LookupHandle lookup;if(WSALookupServiceBeginW(&query,LUP_FLUSHCACHE,&lookup.value)!=0){out<<L"  Live HID SDP query failed: WSA "<<WSAGetLastError()<<L"\n";return summary;}
    std::vector<std::uint8_t> buffer(64*1024);
    while(true){std::ranges::fill(buffer,std::uint8_t{});auto* result=reinterpret_cast<WSAQUERYSETW*>(buffer.data());result->dwSize=sizeof(*result);DWORD bytes=static_cast<DWORD>(buffer.size());
        const DWORD flags=LUP_RETURN_NAME|LUP_RETURN_COMMENT|LUP_RETURN_ADDR|LUP_RETURN_BLOB;if(WSALookupServiceNextW(lookup.value,flags,&bytes,result)!=0){const auto error=WSAGetLastError();if(error==WSAEFAULT&&bytes>buffer.size()){buffer.resize(bytes);continue;}if(error!=WSA_E_NO_MORE&&error!=WSASERVICE_NOT_FOUND)out<<L"  SDP result error: WSA "<<error<<L"\n";break;}
        summary.recordReturned=true;out<<L"  HID SDP service: "<<(result->lpszServiceInstanceName?result->lpszServiceInstanceName:L"(unnamed)")<<L"\n";
        if(!result->lpBlob||!result->lpBlob->pBlobData){out<<L"    no raw SDP record returned\n";continue;}const auto* record=result->lpBlob->pBlobData;const auto recordSize=result->lpBlob->cbSize;out<<L"    raw record: "<<bytesText(record,recordSize,4096)<<L"\n";SdpAttributeContext attribute{&out};
        if(!BluetoothSdpEnumAttributes(const_cast<LPBYTE>(record),recordSize,sdpAttribute,&attribute))out<<L"    SDP attribute parser failed: "<<GetLastError()<<L"\n";
        out<<std::format(L"    HID descriptor present={} Android Head Tracker descriptor={}\n",attribute.foundDescriptor,attribute.androidDescriptor);
        summary.androidDescriptor|=attribute.androidDescriptor;
    }
    if(!summary.recordReturned)out<<L"  No live HID SDP record was returned.\n";return summary;
}

// Quiet variant used for auto-detection: true when the device's live SDP record
// carries a verified Android Head Tracker HID descriptor. Read-only.
bool deviceAdvertisesAndroidHeadTracker(const BLUETOOTH_DEVICE_INFO& device){
    std::wostringstream sink;return queryClassicSdp(device,sink).androidDescriptor;
}

std::wstring setupString(HDEVINFO set,SP_DEVINFO_DATA& dev,DWORD property){DWORD type{},needed{};SetupDiGetDeviceRegistryPropertyW(set,&dev,property,&type,nullptr,0,&needed);if(!needed)return {};std::vector<std::uint8_t> data(needed);if(!SetupDiGetDeviceRegistryPropertyW(set,&dev,property,&type,data.data(),needed,nullptr))return {};return reinterpret_cast<wchar_t*>(data.data());}

std::vector<BTH_LE_GATT_SERVICE> services(HANDLE device){USHORT count{};auto hr=BluetoothGATTGetServices(device,0,nullptr,&count,BLUETOOTH_GATT_FLAG_NONE);if(hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!count)return {};std::vector<BTH_LE_GATT_SERVICE> values(count);if(FAILED(BluetoothGATTGetServices(device,count,values.data(),&count,BLUETOOTH_GATT_FLAG_NONE)))return {};values.resize(count);return values;}
std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics(HANDLE device,BTH_LE_GATT_SERVICE& service){USHORT count{};auto hr=BluetoothGATTGetCharacteristics(device,&service,0,nullptr,&count,BLUETOOTH_GATT_FLAG_NONE);if(hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!count)return {};std::vector<BTH_LE_GATT_CHARACTERISTIC> values(count);if(FAILED(BluetoothGATTGetCharacteristics(device,&service,count,values.data(),&count,BLUETOOTH_GATT_FLAG_NONE)))return {};values.resize(count);return values;}
std::vector<BTH_LE_GATT_DESCRIPTOR> descriptors(HANDLE device,BTH_LE_GATT_CHARACTERISTIC& characteristic){USHORT count{};auto hr=BluetoothGATTGetDescriptors(device,&characteristic,0,nullptr,&count,BLUETOOTH_GATT_FLAG_NONE);if(hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!count)return {};std::vector<BTH_LE_GATT_DESCRIPTOR> values(count);if(FAILED(BluetoothGATTGetDescriptors(device,&characteristic,count,values.data(),&count,BLUETOOTH_GATT_FLAG_NONE)))return {};values.resize(count);return values;}

std::vector<std::uint8_t> readCharacteristic(HANDLE device,BTH_LE_GATT_CHARACTERISTIC& characteristic,HRESULT& status){USHORT needed{};status=BluetoothGATTGetCharacteristicValue(device,&characteristic,0,nullptr,&needed,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(status!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!needed)return {};std::vector<std::uint8_t> storage(needed);status=BluetoothGATTGetCharacteristicValue(device,&characteristic,needed,reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(storage.data()),nullptr,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(FAILED(status))return {};auto* value=reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(storage.data());return {value->Data,value->Data+value->DataSize};}
std::vector<std::uint8_t> readDescriptor(HANDLE device,BTH_LE_GATT_DESCRIPTOR& descriptor,HRESULT& status){USHORT needed{};status=BluetoothGATTGetDescriptorValue(device,&descriptor,0,nullptr,&needed,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(status!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)||!needed)return {};std::vector<std::uint8_t> storage(needed);status=BluetoothGATTGetDescriptorValue(device,&descriptor,needed,reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(storage.data()),nullptr,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE);if(FAILED(status))return {};auto* value=reinterpret_cast<PBTH_LE_GATT_DESCRIPTOR_VALUE>(storage.data());return {value->Data,value->Data+value->DataSize};}

void probeGatt(const BluetoothProbeOptions& options,std::wostream& out){
    const auto set=SetupDiGetClassDevsW(&GUID_BLUETOOTHLE_DEVICE_INTERFACE,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);if(set==INVALID_HANDLE_VALUE){out<<L"BLE interface enumeration failed: "<<GetLastError()<<L"\n";return;}SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);unsigned found{};
    for(DWORD index=0;SetupDiEnumDeviceInterfaces(set,nullptr,&GUID_BLUETOOTHLE_DEVICE_INTERFACE,index,&iface);++index){DWORD needed{};SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&needed,nullptr);std::vector<std::uint8_t> detailStorage(needed);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());detail->cbSize=sizeof(*detail);SP_DEVINFO_DATA dev{};dev.cbSize=sizeof(dev);if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,needed,nullptr,&dev))continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};SetupDiGetDeviceInstanceIdW(set,&dev,instance,MAX_DEVICE_ID_LEN,nullptr);auto name=setupString(set,dev,SPDRP_FRIENDLYNAME);if(name.empty())name=setupString(set,dev,SPDRP_DEVICEDESC);const bool selected=options.probeAllLeDevices||(!options.nameFilter.empty()&&(containsInsensitive(name,options.nameFilter)||containsInsensitive(instance,options.nameFilter)));out<<std::format(L"BLE device {}: {}\n  instance: {}\n  selected for reads: {}\n",index,name,instance,selected);++found;if(!selected)continue;
        WinHandle handle;handle.value=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);if(handle.value==INVALID_HANDLE_VALUE)handle.value=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);if(handle.value==INVALID_HANDLE_VALUE){out<<L"  open failed: "<<GetLastError()<<L"\n";continue;}
        auto serviceValues=services(handle.value);out<<L"  primary services: "<<serviceValues.size()<<L"\n";for(auto& service:serviceValues){out<<std::format(L"    service {} handle=0x{:04X}\n",uuidText(service.ServiceUuid),service.AttributeHandle);auto chars=characteristics(handle.value,service);for(auto& characteristic:chars){out<<std::format(L"      characteristic {} attr=0x{:04X} value=0x{:04X} properties={}{}{}{}{}\n",uuidText(characteristic.CharacteristicUuid),characteristic.AttributeHandle,characteristic.CharacteristicValueHandle,characteristic.IsReadable?L"R":L"",characteristic.IsWritable?L"W":L"",characteristic.IsWritableWithoutResponse?L"w":L"",characteristic.IsNotifiable?L"N":L"",characteristic.IsIndicatable?L"I":L"");
                if(characteristic.IsReadable){HRESULT hr{};const auto value=readCharacteristic(handle.value,characteristic,hr);out<<L"        read "<<(SUCCEEDED(hr)?L"ok":std::format(L"failed 0x{:08X}",static_cast<unsigned>(hr)))<<L": "<<bytesText(value.data(),value.size(),1024)<<L"\n";}
                auto descs=descriptors(handle.value,characteristic);for(auto& descriptor:descs){out<<std::format(L"        descriptor {} type={} attr=0x{:04X}",uuidText(descriptor.DescriptorUuid),static_cast<unsigned>(descriptor.DescriptorType),descriptor.AttributeHandle);HRESULT hr{};const auto value=readDescriptor(handle.value,descriptor,hr);if(SUCCEEDED(hr))out<<L" value="<<bytesText(value.data(),value.size(),512);else out<<std::format(L" read=0x{:08X}",static_cast<unsigned>(hr));out<<L"\n";}
            }}
    }
    SetupDiDestroyDeviceInfoList(set);if(!found)out<<L"No present BLE device interfaces were exposed by Windows.\n";
}

} // namespace

// Resolves the paired Bluetooth headset name owning a head-tracker HID node by
// walking the PnP parent chain to the BTHENUM node and matching its address
// against the paired-device list.
std::wstring bluetoothNameForHidInstance(std::wstring_view instanceId){
    if(instanceId.empty())return {};
    std::wstring id(instanceId);DEVINST node{};
    if(CM_Locate_DevNodeW(&node,id.data(),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS)return {};
    BLUETOOTH_ADDRESS address{};bool resolved{};
    for(int depth=0;depth<6&&!resolved;++depth){
        DEVINST parent{};if(CM_Get_Parent(&parent,node,0)!=CR_SUCCESS)break;node=parent;
        wchar_t parentId[MAX_DEVICE_ID_LEN]{};if(CM_Get_Device_IDW(node,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS)continue;
        const std::wstring_view text(parentId);
        if(!text.starts_with(L"BTHENUM\\"))continue;
        resolved=addressFromBthenumId(text,address);
    }
    if(!resolved)return {};
    for(const auto& device:pairedClassicDevices())if(device.Address.ullLong==address.ullLong)return device.szName;
    return {};
}

std::vector<std::wstring> pairedBluetoothDeviceNames(){
    std::vector<std::wstring> names;
    for(const auto& device:pairedClassicDevices())names.emplace_back(device.szName);
    return names;
}

int runBluetoothProbe(const BluetoothProbeOptions& options,std::wostream& output){
    WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0){output<<L"Winsock initialization failed.\n";return 3;}
    output<<L"Read-only Bluetooth investigation\n=================================\n";BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;search.fReturnConnected=TRUE;search.fReturnUnknown=FALSE;search.fIssueInquiry=FALSE;search.cTimeoutMultiplier=2;BLUETOOTH_DEVICE_INFO device{};device.dwSize=sizeof(device);FindDeviceHandle find;find.value=BluetoothFindFirstDevice(&search,&device);unsigned matched{};
    if(find.value){do{if(!containsInsensitive(device.szName,options.nameFilter))continue;++matched;output<<std::format(L"\nClassic device: {} [{}] connected={} authenticated={} remembered={}\n",device.szName,addressText(device.Address),device.fConnected!=FALSE,device.fAuthenticated!=FALSE,device.fRemembered!=FALSE);
        const auto sdp=queryClassicSdp(device,output);
        if(sdp.androidDescriptor)output<<L"  => "<<device.szName<<L" advertises the Android Head Tracker service. This headset should work.\n";
        else if(containsInsensitive(device.szName,L"AirPods"))output<<L"  => AirPods use Apple's proprietary accessory protocol (L2CAP PSM 0x1001), which\n     Windows does not expose to applications. Head tracking cannot be read from\n     AirPods on Windows without a third-party kernel driver. See README > Compatibility.\n";
        device={};device.dwSize=sizeof(device);}while(BluetoothFindNextDevice(find.value,&device));}
    if(!matched)output<<(options.nameFilter.empty()?std::wstring(L"No paired Classic Bluetooth devices were found.\n"):std::format(L"No paired Classic Bluetooth device matched '{}'.\n",options.nameFilter));
    output<<L"\nBLE GATT interfaces\n===================\n";probeGatt(options,output);output<<L"\nNo configuration values were written and no notification subscriptions were enabled.\n";WSACleanup();return matched?0:2;
}

int rebindBluetoothHid(std::wstring_view nameFilter,std::wostream& output){
    BLUETOOTH_FIND_RADIO_PARAMS radioSearch{sizeof(radioSearch)};WinHandle radio;FindRadioHandle findRadio;findRadio.value=BluetoothFindFirstRadio(&radioSearch,&radio.value);
    if(!findRadio.value){output<<L"No Bluetooth radio is available (Win32 "<<GetLastError()<<L").\n";return 3;}
    // With no name filter, auto-detect via read-only SDP which paired device(s)
    // actually advertise the Android Head Tracker descriptor, so the service
    // cycle below never touches an unrelated HID device (mouse, keyboard, ...).
    const bool autoDetect=nameFilter.empty();std::set<unsigned long long> targets;
    if(autoDetect){
        WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0){output<<L"Winsock initialization failed; pass --name to select the headset explicitly.\n";return 3;}
        output<<L"Auto-detecting paired devices that advertise the Android Head Tracker service...\n";
        for(const auto& candidate:pairedClassicDevices())if(deviceAdvertisesAndroidHeadTracker(candidate)){targets.insert(candidate.Address.ullLong);output<<L"  found: "<<candidate.szName<<L" ["<<addressText(candidate.Address)<<L"]\n";}
        WSACleanup();
        if(targets.empty()){output<<L"No paired device advertises the Android Head Tracker service over SDP.\n  - Make sure the headphones are connected and powered on, then retry.\n  - Or pass --name \"<Bluetooth device name>\" to select the headset explicitly.\n  - AirPods and other Apple headphones do not implement this protocol and cannot work.\n";return 2;}
    }
    // Never toggle a live HID service. If Bluetooth's database says the service
    // is enabled but no PnP node exists, disable/enable repairs that stale state.
    GUID hidService{0x00001124,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB}};bool matched{};DWORD enableResult=ERROR_NOT_FOUND;
    do{
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;search.fReturnConnected=TRUE;search.fReturnUnknown=FALSE;search.fIssueInquiry=FALSE;search.hRadio=radio.value;BLUETOOTH_DEVICE_INFO device{};device.dwSize=sizeof(device);FindDeviceHandle find;find.value=BluetoothFindFirstDevice(&search,&device);
        if(find.value){do{const bool wanted=autoDetect?targets.contains(device.Address.ullLong):containsInsensitive(device.szName,nameFilter);if(!wanted){device={};device.dwSize=sizeof(device);continue;}matched=true;const bool liveNode=hasPresentBluetoothHidChild(device.Address);output<<L"Requesting HID service for "<<device.szName<<L" ["<<addressText(device.Address)<<L"]\n  live HID child: "<<(liveNode?L"yes":L"no")<<L"\n";enableResult=BluetoothSetServiceState(radio.value,&device,&hidService,BLUETOOTH_SERVICE_ENABLE);output<<L"  enable result: "<<enableResult<<L"\n";
            if(!liveNode&&(enableResult==ERROR_INVALID_PARAMETER||enableResult==static_cast<DWORD>(E_INVALIDARG))){output<<L"  stale enabled state detected; cycling only the absent HID service\n";const auto disableResult=BluetoothSetServiceState(radio.value,&device,&hidService,BLUETOOTH_SERVICE_DISABLE);output<<L"  disable result: "<<disableResult<<L"\n";if(disableResult==ERROR_SUCCESS||disableResult==ERROR_INVALID_PARAMETER||disableResult==static_cast<DWORD>(E_INVALIDARG)){Sleep(1500);enableResult=BluetoothSetServiceState(radio.value,&device,&hidService,BLUETOOTH_SERVICE_ENABLE);output<<L"  recovery enable result: "<<enableResult<<L"\n";}}
            device={};device.dwSize=sizeof(device);}while(BluetoothFindNextDevice(find.value,&device));}
        if(matched)break;CloseHandle(radio.value);radio.value=INVALID_HANDLE_VALUE;
    }while(BluetoothFindNextRadio(findRadio.value,&radio.value));
    if(!matched){output<<(autoDetect?std::wstring(L"The auto-detected headset was not found during the rebind pass.\n"):std::format(L"No paired Bluetooth device matched '{}'.\n",nameFilter));return 2;}
    if(enableResult==ERROR_SUCCESS){output<<L"HID service was requested; waiting for Plug and Play enumeration.\n";Sleep(5000);return 0;}
    if(enableResult==ERROR_INVALID_PARAMETER||enableResult==static_cast<DWORD>(E_INVALIDARG)){output<<L"HID service is already enabled. No device was removed; power-cycle the headphones.\n";return 0;}
    output<<L"HID service enable failed. No existing device was removed.\n";return 4;
}

int useGenericHidDriver(std::wostream& output){
    const auto set=SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT);if(set==INVALID_HANDLE_VALUE){output<<L"PnP enumeration failed: "<<GetLastError()<<L"\n";return 3;}SP_DEVINFO_DATA selected{};std::wstring selectedInstance,selectedHardwareId;unsigned matches{};
    for(DWORD index=0;;++index){SP_DEVINFO_DATA dev{};dev.cbSize=sizeof(dev);if(!SetupDiEnumDeviceInfo(set,index,&dev)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;continue;}ULONG status{},problem{};if(CM_Get_DevNode_Status(&status,&problem,dev.DevInst,0)!=CR_SUCCESS||problem!=CM_PROB_FAILED_START)continue;
        DEVINST parent{};if(CM_Get_Parent(&parent,dev.DevInst,0)!=CR_SUCCESS)continue;wchar_t parentId[MAX_DEVICE_ID_LEN]{};if(CM_Get_Device_IDW(parent,parentId,MAX_DEVICE_ID_LEN,0)!=CR_SUCCESS||!std::wstring_view(parentId).starts_with(L"BTHENUM\\"))continue;
        DWORD type{},needed{};SetupDiGetDeviceRegistryPropertyW(set,&dev,SPDRP_HARDWAREID,&type,nullptr,0,&needed);if(!needed)continue;std::vector<std::uint8_t> ids(needed);if(!SetupDiGetDeviceRegistryPropertyW(set,&dev,SPDRP_HARDWAREID,&type,ids.data(),needed,nullptr))continue;const auto* current=reinterpret_cast<const wchar_t*>(ids.data());bool headTracker{};std::wstring first;for(;*current;current+=wcslen(current)+1){if(first.empty())first=current;if(containsInsensitive(current,L"UP:0020_U:00E1"))headTracker=true;}if(!headTracker)continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};if(!SetupDiGetDeviceInstanceIdW(set,&dev,instance,MAX_DEVICE_ID_LEN,nullptr))continue;++matches;selected=dev;selectedInstance=instance;selectedHardwareId=first;
    }
    SetupDiDestroyDeviceInfoList(set);if(matches!=1){output<<L"Expected exactly one failed Bluetooth Android Head Tracker node; found "<<matches<<L". No driver binding changed.\n";return 2;}
    wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);const auto inputInf=std::wstring(windows)+L"\\INF\\input.inf";BOOL reboot{};output<<L"Binding exact device "<<selectedInstance<<L"\n  hardware ID: "<<selectedHardwareId<<L"\n  inbox INF: "<<inputInf<<L"\n";
    if(!UpdateDriverForPlugAndPlayDevicesW(nullptr,selectedHardwareId.c_str(),inputInf.c_str(),INSTALLFLAG_FORCE|INSTALLFLAG_NONINTERACTIVE,&reboot)){const auto error=GetLastError();output<<L"Generic HID binding failed: "<<error<<L"\n";return error==ERROR_ACCESS_DENIED?5:4;}
    output<<L"Generic HID binding succeeded; reboot required="<<(reboot?L"yes":L"no")<<L"\n";return reboot?1:0;
}

} // namespace sony
