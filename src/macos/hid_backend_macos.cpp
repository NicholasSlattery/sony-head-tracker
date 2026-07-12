// hid_backend_macos.cpp
// macOS IOHID implementation. Task 5 starts with descriptor-driven discovery;
// report configuration and streaming are added in the following port tasks.
#include "sony_head_tracker/hid_backend.hpp"

#include "sony_head_tracker/hid_descriptor.hpp"
#include "sony_head_tracker/hid_usages.hpp"
#include "sony_head_tracker/logger.hpp"
#include "sony_head_tracker/macos_support.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDElement.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDValue.h>
#include <IOKit/hidsystem/IOHIDLib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sony {

namespace {

constexpr CFIndex kMaximumFeatureReportBytes = 64 * 1024;

class CfObject {
public:
    CfObject() = default;
    explicit CfObject(CFTypeRef value) : value_(value) {}
    ~CfObject() { reset(); }
    CfObject(const CfObject&) = delete;
    CfObject& operator=(const CfObject&) = delete;
    CfObject(CfObject&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    CfObject& operator=(CfObject&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    CFTypeRef get() const { return value_; }
    template <typename T> T as() const {
        return reinterpret_cast<T>(const_cast<void*>(value_));
    }
    void reset(CFTypeRef value = nullptr) {
        if (value_) CFRelease(value_);
        value_ = value;
    }

private:
    CFTypeRef value_{};
};

std::wstring cfString(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    auto string = static_cast<CFStringRef>(value);
    const auto length = CFStringGetLength(string);
    std::vector<UniChar> characters(static_cast<std::size_t>(length));
    if (length > 0) CFStringGetCharacters(string, CFRangeMake(0, length), characters.data());
    std::wstring result;
    result.reserve(characters.size());
    for (std::size_t index = 0; index < characters.size(); ++index) {
        std::uint32_t codePoint = characters[index];
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < characters.size()) {
            const auto low = characters[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000u + ((codePoint - 0xD800u) << 10u) + (low - 0xDC00u);
                ++index;
            }
        }
        result.push_back(static_cast<wchar_t>(codePoint));
    }
    return result;
}

std::uint64_t cfNumber(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return 0;
    std::uint64_t result{};
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result);
    return result;
}

std::wstring identifierFor(IOHIDDeviceRef device) {
    const auto registryID = cfNumber(IOHIDDeviceGetProperty(device, CFSTR("RegistryID")));
    if (registryID != 0) return std::format(L"iohid://{:016X}", registryID);
    const auto location = cfNumber(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDLocationIDKey)));
    return std::format(L"iohid-location://{:08X}", static_cast<std::uint32_t>(location));
}

DescriptorField makeField(IOHIDElementRef element, bool feature) {
    DescriptorField field;
    field.usagePage = static_cast<std::uint16_t>(IOHIDElementGetUsagePage(element));
    field.usage = static_cast<std::uint16_t>(IOHIDElementGetUsage(element));
    field.reportId = static_cast<std::uint8_t>(IOHIDElementGetReportID(element));
    const auto reportCount = IOHIDElementGetReportCount(element);
    const auto reportSize = IOHIDElementGetReportSize(element);
    field.reportCount = static_cast<std::uint16_t>(std::min<std::uint32_t>(
        reportCount, std::numeric_limits<std::uint16_t>::max()));
    // Apple defines ReportSize differently for a repeated non-array element:
    // it is the total size of all repeated usages, so the per-value size is
    // ReportSize / ReportCount. For an array, ReportSize is already per item.
    const auto perValueBits = !IOHIDElementIsArray(element) && reportCount > 1 &&
                              reportSize % reportCount == 0
        ? reportSize / reportCount
        : reportSize;
    field.bitSize = static_cast<std::uint16_t>(std::min<std::uint32_t>(
        perValueBits, std::numeric_limits<std::uint16_t>::max()));
    field.logicalMin = static_cast<std::int32_t>(IOHIDElementGetLogicalMin(element));
    field.logicalMax = static_cast<std::int32_t>(IOHIDElementGetLogicalMax(element));
    field.physicalMin = static_cast<std::int32_t>(IOHIDElementGetPhysicalMin(element));
    field.physicalMax = static_cast<std::int32_t>(IOHIDElementGetPhysicalMax(element));
    field.unitExponent = decodeHidUnitExponent(IOHIDElementGetUnitExponent(element));
    field.unit = IOHIDElementGetUnit(element);
    field.feature = feature;
    return field;
}

bool isFeature(IOHIDElementRef element) {
    return IOHIDElementGetType(element) == kIOHIDElementTypeFeature;
}

std::string findMarker(IOHIDDeviceRef device, CFArrayRef elements) {
    const auto count = elements ? CFArrayGetCount(elements) : 0;
    for (CFIndex index = 0; index < count; ++index) {
        auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements, index)));
        if (!isFeature(element) || IOHIDElementGetUsagePage(element) != kSensorPage ||
            IOHIDElementGetUsage(element) != kSensorDescription) continue;
        IOHIDValueRef value{};
        if (IOHIDDeviceGetValue(device, element, &value) != kIOReturnSuccess || !value) continue;
        const auto* bytes = IOHIDValueGetBytePtr(value);
        const auto length = IOHIDValueGetLength(value);
        std::string text(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
        while (!text.empty() && (text.back() == '\0' || static_cast<unsigned char>(text.back()) == 0xFF)) text.pop_back();
        const auto marker = text.find(kMarker);
        if (marker != std::string::npos) return text.substr(marker);
    }

    const auto maxSize = static_cast<CFIndex>(cfNumber(IOHIDDeviceGetProperty(
        device, CFSTR(kIOHIDMaxFeatureReportSizeKey))));
    if (maxSize <= 0 || maxSize > kMaximumFeatureReportBytes) return {};
    std::vector<std::uint32_t> reportIds;
    for (CFIndex index = 0; index < count; ++index) {
        auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements, index)));
        if (isFeature(element) && std::find(reportIds.begin(), reportIds.end(), IOHIDElementGetReportID(element)) == reportIds.end()) {
            reportIds.push_back(IOHIDElementGetReportID(element));
        }
    }
    std::vector<std::uint8_t> report(static_cast<std::size_t>(maxSize));
    for (const auto reportId : reportIds) {
        CFIndex length = maxSize;
        if (IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, reportId, report.data(), &length) != kIOReturnSuccess) continue;
        std::string text(reinterpret_cast<const char*>(report.data()), static_cast<std::size_t>(length));
        const auto marker = text.find(kMarker);
        if (marker != std::string::npos) return text.substr(marker);
    }
    return {};
}

CfObject matchingDictionary() {
    auto dictionary = CfObject(CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                                           &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks));
    const int page = kSensorPage;
    const int usage = kOtherCustom;
    CfObject pageNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page));
    CfObject usageNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage));
    CFDictionarySetValue(dictionary.as<CFMutableDictionaryRef>(), CFSTR(kIOHIDDeviceUsagePageKey), pageNumber.get());
    CFDictionarySetValue(dictionary.as<CFMutableDictionaryRef>(), CFSTR(kIOHIDDeviceUsageKey), usageNumber.get());
    return dictionary;
}

CfObject copyMatchingDevices(IOHIDManagerRef manager) {
    auto matching = matchingDictionary();
    IOHIDManagerSetDeviceMatching(manager, matching.as<CFDictionaryRef>());
    return CfObject(IOHIDManagerCopyDevices(manager));
}

DeviceInfo describeDevice(IOHIDDeviceRef device) {
    DeviceInfo info;
    info.path = identifierFor(device);
    info.instanceId = info.path;
    info.product = cfString(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey)));
    info.manufacturer = cfString(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDManufacturerKey)));
    info.bluetoothName = info.product;
    info.bluetoothAddress = cfString(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDSerialNumberKey)));
    info.usagePage = kSensorPage;
    info.usage = kOtherCustom;
    info.vendorId = static_cast<std::uint16_t>(cfNumber(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey))));
    info.productId = static_cast<std::uint16_t>(cfNumber(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey))));
    info.version = static_cast<std::uint16_t>(cfNumber(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVersionNumberKey))));
    info.inputReportBytes = static_cast<std::uint16_t>(cfNumber(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDMaxInputReportSizeKey))));
    info.featureReportBytes = static_cast<std::uint16_t>(cfNumber(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDMaxFeatureReportSizeKey))));

    CfObject elements(IOHIDDeviceCopyMatchingElements(device, nullptr, kIOHIDOptionsTypeNone));
    if (elements.get()) {
        const auto count = CFArrayGetCount(elements.as<CFArrayRef>());
        for (CFIndex index = 0; index < count; ++index) {
            auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements.as<CFArrayRef>(), index)));
            info.fields.push_back(makeField(element, isFeature(element)));
        }
        if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
            info.sensorDescription = findMarker(device, elements.as<CFArrayRef>());
            elements.reset();
            IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        }
    }
    info.androidHeadTracker = info.sensorDescription.starts_with(kMarker);
    return info;
}

} // namespace

struct ElementRecord {
    IOHIDElementRef element{};
    DescriptorField field{};
};

struct HidBackend::Context {
    IOHIDManagerRef manager{};
    IOHIDDeviceRef device{};
    CFArrayRef elements{};
    std::vector<ElementRecord> inputElements;
    std::vector<ElementRecord> featureElements;
    std::vector<std::uint8_t> reportBuffer;
    std::vector<std::uint8_t> rawBuffer;
    std::vector<std::uint8_t> packedScratch;
    std::vector<double> valueScratch;
    RawCallback raw;
    SampleCallback sample;
    std::chrono::steady_clock::time_point rateStart{std::chrono::steady_clock::now()};
    std::uint64_t rateCount{};
    double rate{};
    CFRunLoopRef runLoop{};
    bool loggedRawShape{};
    bool rawReportSeen{};
    bool loggedValueShape{};
    CFIndex reportIntervalRaw{-1};
    std::uint64_t valueTimestamp{};
    std::uint32_t valueReportId{};
    MotionSample valueSample{};
    bool valueGotRotation{};
    bool valueGotGyro{};
    bool valueGotAccel{};
    std::atomic_bool removed{};

    ~Context() {
        if (runLoop) CFRelease(runLoop);
        // IOHIDElement objects in the copied descriptor array are owned by the
        // device. Release the array before closing/releasing that device.
        if (elements) CFRelease(elements);
        if (device) {
            IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
            CFRelease(device);
        }
        if (manager) {
            IOHIDManagerClose(manager, kIOHIDManagerOptionNone);
            CFRelease(manager);
        }
    }
};

namespace {

bool relevantUsage(std::uint16_t usage) {
    return usage == kRotation || usage == kAngularVelocity || usage == kAngularVelocityVector ||
           usage == kAccelerationVector || usage == kAccelerationX || usage == kAccelerationY ||
           usage == kAccelerationZ || usage == kAngularVelocityX || usage == kAngularVelocityY ||
           usage == kAngularVelocityZ || usage == kResetCounter;
}

bool readValue(HidBackend::Context& context, const ElementRecord& record, IOHIDValueRef& value) {
    value = nullptr;
    if (IOHIDDeviceGetValueWithOptions(context.device, record.element, &value,
                                       kIOHIDDeviceGetValueWithoutUpdate) != kIOReturnSuccess || !value) {
        return false;
    }
    return true;
}

bool decodeVector(HidBackend::Context& context, const ElementRecord& record, IOHIDValueRef value, Vec3& out) {
    const auto* bytes = IOHIDValueGetBytePtr(value);
    const auto length = IOHIDValueGetLength(value);
    if (!bytes || length <= 0 || record.field.reportCount < 3) return false;
    decodePackedDescriptorValuesInto(context.valueScratch,
                                     std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(length)),
                                     record.field);
    if (context.valueScratch.size() < 3) return false;
    out = {context.valueScratch[0], context.valueScratch[1], context.valueScratch[2]};
    return true;
}

bool decodeScalar(const ElementRecord& record, IOHIDValueRef value, double& out) {
    const auto raw = static_cast<std::int64_t>(IOHIDValueGetIntegerValue(value));
    out = descriptorScale(raw, record.field.logicalMin, record.field.logicalMax,
                          record.field.physicalMin, record.field.physicalMax,
                          record.field.unitExponent);
    return true;
}

bool readMotionSample(HidBackend::Context& context, std::uint32_t reportId, MotionSample& sample) {
    bool gotRotation = false;
    bool gotGyro = false;
    bool gotAccel = false;
    Vec3 gyro{};
    Vec3 accel{};
    sample.receivedAt = std::chrono::steady_clock::now();
    for (const auto& record : context.inputElements) {
        if (record.field.usagePage != kSensorPage || !relevantUsage(record.field.usage)) continue;
        if (record.field.reportId != 0 && record.field.reportId != reportId) continue;
        IOHIDValueRef value = nullptr;
        if (!readValue(context, record, value)) continue;
        const auto usage = record.field.usage;
        if (usage == kRotation) {
            gotRotation = decodeVector(context, record, value, sample.rotationVector);
        } else if (usage == kAngularVelocity || usage == kAngularVelocityVector) {
            gotGyro = decodeVector(context, record, value, gyro);
        } else if (usage == kAccelerationVector) {
            gotAccel = decodeVector(context, record, value, accel);
        } else if (usage == kAngularVelocityX || usage == kAngularVelocityY || usage == kAngularVelocityZ) {
            double scalar{};
            if (decodeScalar(record, value, scalar)) {
                gyro[usage - kAngularVelocityX] = scalar;
                gotGyro = true;
            }
        } else if (usage == kAccelerationX || usage == kAccelerationY || usage == kAccelerationZ) {
            double scalar{};
            if (decodeScalar(record, value, scalar)) {
                accel[usage - kAccelerationX] = scalar;
                gotAccel = true;
            }
        } else if (usage == kResetCounter) {
            sample.resetCounter = static_cast<std::uint8_t>(IOHIDValueGetIntegerValue(value));
        }
    }
    if (gotGyro) sample.angularVelocity = gyro;
    if (gotAccel) sample.acceleration = accel;
    ++context.rateCount;
    const auto elapsed = std::chrono::duration<double>(sample.receivedAt - context.rateStart).count();
    if (elapsed >= 1.0) {
        context.rate = context.rateCount / elapsed;
        context.rateCount = 0;
        context.rateStart = sample.receivedAt;
    }
    sample.packetsPerSecond = context.rate;
    sample.receiveLatencyMs = -1.0;
    return gotRotation;
}

void emitValueSample(HidBackend::Context& context) {
    if (!context.valueGotRotation) return;
    if (context.valueGotGyro) context.valueSample.angularVelocity = context.valueSample.angularVelocity.value_or(Vec3{});
    if (context.valueGotAccel) context.valueSample.acceleration = context.valueSample.acceleration.value_or(Vec3{});
    ++context.rateCount;
    const auto elapsed = std::chrono::duration<double>(
        context.valueSample.receivedAt - context.rateStart).count();
    if (elapsed >= 1.0) {
        context.rate = context.rateCount / elapsed;
        context.rateCount = 0;
        context.rateStart = context.valueSample.receivedAt;
    }
    context.valueSample.packetsPerSecond = context.rate;
    context.valueSample.receiveLatencyMs = -1.0;
    if (context.sample) context.sample(std::move(context.valueSample));
}

void inputValueCallback(void* rawContext, IOReturn result, void*, IOHIDValueRef value) {
    auto* context = static_cast<HidBackend::Context*>(rawContext);
    if (!context || result != kIOReturnSuccess || !value || context->rawReportSeen) return;
    const auto element = IOHIDValueGetElement(value);
    if (!element || IOHIDElementGetUsagePage(element) != kSensorPage) return;
    const auto usage = static_cast<std::uint16_t>(IOHIDElementGetUsage(element));
    if (!relevantUsage(usage)) return;
    const auto record = std::find_if(
        context->inputElements.begin(), context->inputElements.end(),
        [&](const auto& candidate) { return candidate.element == element; });
    if (record == context->inputElements.end()) return;

    const auto reportId = static_cast<std::uint32_t>(IOHIDElementGetReportID(element));
    const auto timestamp = IOHIDValueGetTimeStamp(value);
    if (!context->loggedValueShape) {
        Logger::instance().write(
            LogLevel::info,
            std::format(L"First input value callback reportID={} usage=0x{:04X} "
                        L"timestamp={} valueBytes={} bitsPerValue={} count={}",
                        reportId, usage, timestamp, IOHIDValueGetLength(value),
                        record->field.bitSize, record->field.reportCount));
        context->loggedValueShape = true;
    }
    if (context->valueTimestamp != 0 &&
        (timestamp != context->valueTimestamp || reportId != context->valueReportId)) {
        emitValueSample(*context);
        context->valueSample = {};
        context->valueGotRotation = false;
        context->valueGotGyro = false;
        context->valueGotAccel = false;
    }
    if (context->valueTimestamp == 0 || timestamp != context->valueTimestamp ||
        reportId != context->valueReportId) {
        context->valueTimestamp = timestamp;
        context->valueReportId = reportId;
        context->valueSample.receivedAt = std::chrono::steady_clock::now();
    }

    if (usage == kRotation) {
        context->valueGotRotation = decodeVector(*context, *record, value,
                                                 context->valueSample.rotationVector);
    } else if (usage == kAngularVelocity || usage == kAngularVelocityVector) {
        Vec3 gyro{};
        if (decodeVector(*context, *record, value, gyro)) {
            context->valueSample.angularVelocity = gyro;
            context->valueGotGyro = true;
        }
    } else if (usage == kAccelerationVector) {
        Vec3 acceleration{};
        if (decodeVector(*context, *record, value, acceleration)) {
            context->valueSample.acceleration = acceleration;
            context->valueGotAccel = true;
        }
    } else if (usage == kResetCounter) {
        context->valueSample.resetCounter = static_cast<std::uint8_t>(
            IOHIDValueGetIntegerValue(value));
    }
}

void inputReportCallback(void* rawContext, IOReturn result, void*, IOHIDReportType type,
                         uint32_t reportId, uint8_t* report, CFIndex reportLength) {
    auto* context = static_cast<HidBackend::Context*>(rawContext);
    if (!context || result != kIOReturnSuccess || type != kIOHIDReportTypeInput || reportLength <= 0) return;
    context->rawReportSeen = true;
    if (!context->loggedRawShape) {
        Logger::instance().write(
            LogLevel::info,
            std::format(L"First input callback reportID={} length={} byte0=0x{:02X}",
                        reportId, reportLength, static_cast<unsigned>(report[0])));
        context->loggedRawShape = true;
    }
    // Preserve exactly what IOHID supplied until the first-device diagnostic
    // establishes whether this buffer already contains the report ID.
    context->rawBuffer.assign(report, report + reportLength);
    if (context->raw) context->raw(context->rawBuffer);
    MotionSample sample;
    if (context->sample && readMotionSample(*context, reportId, sample)) context->sample(std::move(sample));
}

void removalCallback(void* rawContext, IOReturn, void*) {
    auto* context = static_cast<HidBackend::Context*>(rawContext);
    if (!context) return;
    context->removed = true;
    if (context->runLoop) CFRunLoopStop(context->runLoop);
}

void collectElements(HidBackend::Context& context) {
    const auto count = context.elements ? CFArrayGetCount(context.elements) : 0;
    for (CFIndex index = 0; index < count; ++index) {
        auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(context.elements, index)));
        const bool feature = isFeature(element);
        ElementRecord record{element, makeField(element, feature)};
        (feature ? context.featureElements : context.inputElements).push_back(record);
    }
}

bool revalidateFeatureWriteTarget(HidBackend::Context& context) {
    if (!context.device || !context.elements ||
        !IOHIDDeviceConformsTo(context.device, kSensorPage, kOtherCustom)) {
        Logger::instance().write(LogLevel::error,
                                 L"Reacquired device no longer exposes the Android Head Tracker usage");
        return false;
    }
    const auto marker = findMarker(context.device, context.elements);
    if (!marker.starts_with(kMarker)) {
        Logger::instance().write(LogLevel::error,
                                 L"Reacquired device no longer exposes a verified Android Head Tracker marker");
        return false;
    }
    const auto maxFeatureBytes = static_cast<CFIndex>(cfNumber(
        IOHIDDeviceGetProperty(context.device, CFSTR(kIOHIDMaxFeatureReportSizeKey))));
    if (maxFeatureBytes <= 0 || maxFeatureBytes > kMaximumFeatureReportBytes ||
        context.featureElements.empty()) {
        Logger::instance().write(LogLevel::error,
                                 L"Reacquired device exposes an unsafe or incomplete feature-report layout");
        return false;
    }
    DeviceInfo candidate;
    candidate.usagePage = kSensorPage;
    candidate.usage = kOtherCustom;
    candidate.androidHeadTracker = true;
    candidate.fields.reserve(context.featureElements.size());
    for (const auto& record : context.featureElements) candidate.fields.push_back(record.field);
    if (!canConfigureFeatureReports(candidate)) {
        Logger::instance().write(LogLevel::error,
                                 L"Reacquired device exposes an incomplete or ambiguous feature-report layout");
        return false;
    }
    return true;
}

bool setFeatureInteger(HidBackend::Context& context, std::uint16_t usage, CFIndex rawValue,
                       std::wstring_view label, bool required) {
    if (!revalidateFeatureWriteTarget(context)) return false;
    for (const auto& record : context.featureElements) {
        if (record.field.usagePage != kSensorPage || record.field.usage != usage) continue;
        IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault, record.element, 0, rawValue);
        if (!value) return false;
        const auto result = IOHIDDeviceSetValue(context.device, record.element, value);
        CFRelease(value);
        if (result != kIOReturnSuccess) {
            Logger::instance().write(LogLevel::error,
                                     std::format(L"Failed setting {} (IOKit=0x{:08X})", label,
                                                 static_cast<unsigned>(result)));
            return false;
        }
        IOHIDValueRef readback = nullptr;
        const auto readResult = IOHIDDeviceGetValueWithOptions(
            context.device, record.element, &readback, kIOHIDDeviceGetValueWithoutUpdate);
        const auto readValue = readResult == kIOReturnSuccess && readback
            ? IOHIDValueGetIntegerValue(readback) : -1;
        Logger::instance().write(LogLevel::info,
                                 std::format(L"Set {} raw={} readback={} report={}", label,
                                             rawValue, readValue, record.field.reportId));
        return readResult == kIOReturnSuccess && readValue == rawValue;
    }
    if (required) Logger::instance().write(LogLevel::error,
                                           std::format(L"Descriptor does not expose writable {}", label));
    else Logger::instance().write(LogLevel::info,
                                  std::format(L"Descriptor does not expose optional {}", label));
    return !required;
}

bool setSelector(HidBackend::Context& context, std::uint16_t desired,
                 std::uint16_t groupMin, std::uint16_t groupMax,
                 std::wstring_view label) {
    if (!revalidateFeatureWriteTarget(context)) return false;
    const ElementRecord* selected = nullptr;
    for (const auto& record : context.featureElements) {
        if (record.field.usagePage == kSensorPage && record.field.usage == desired) {
            selected = &record;
            break;
        }
    }
    if (!selected) {
        Logger::instance().write(LogLevel::error,
                                 std::format(L"Descriptor does not expose {}", label));
        return false;
    }

    const auto parent = IOHIDElementGetParent(selected->element);
    Logger::instance().write(
        LogLevel::info,
        std::format(L"{} selector report={} isArray={} parent=0x{:04X}:0x{:04X} parentIsArray={}",
                    label, selected->field.reportId,
                    IOHIDElementIsArray(selected->element) ? L"yes" : L"no",
                    parent ? IOHIDElementGetUsagePage(parent) : 0,
                    parent ? IOHIDElementGetUsage(parent) : 0,
                    parent && IOHIDElementIsArray(parent) ? L"yes" : L"no"));
    CfObject values(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks));
    std::vector<const ElementRecord*> siblings;
    for (const auto& record : context.featureElements) {
        if (record.field.usagePage != kSensorPage ||
            record.field.reportId != selected->field.reportId ||
            record.field.usage < groupMin || record.field.usage > groupMax ||
            IOHIDElementGetParent(record.element) != parent) {
            continue;
        }
        const CFIndex raw = record.field.usage == desired ? 1 : 0;
        IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(
            kCFAllocatorDefault, record.element, 0, raw);
        if (!value) return false;
        CFDictionarySetValue(values.as<CFMutableDictionaryRef>(), record.element, value);
        CFRelease(value);
        siblings.push_back(&record);
    }
    if (siblings.empty()) return false;

    const auto result = IOHIDDeviceSetValueMultiple(
        context.device, values.as<CFDictionaryRef>());
    if (result != kIOReturnSuccess) {
        Logger::instance().write(LogLevel::error,
                                 std::format(L"Failed setting {} (IOKit=0x{:08X})", label,
                                             static_cast<unsigned>(result)));
        return false;
    }

    bool accepted = true;
    for (const auto* sibling : siblings) {
        IOHIDValueRef readback = nullptr;
        const auto readResult = IOHIDDeviceGetValueWithOptions(
            context.device, sibling->element, &readback,
            kIOHIDDeviceGetValueWithoutUpdate);
        const auto expected = sibling->field.usage == desired ? 1 : 0;
        const auto actual = readResult == kIOReturnSuccess && readback
            ? IOHIDValueGetIntegerValue(readback) : -1;
        accepted = accepted && actual == expected;
        Logger::instance().write(
            LogLevel::info,
            std::format(L"{} selector usage=0x{:04X} expected={} readback={}",
                        label, sibling->field.usage, expected, actual));
    }
    return accepted;
}

bool verifyFeatureInteger(HidBackend::Context& context, std::uint16_t usage,
                          CFIndex expected, std::wstring_view label) {
    for (const auto& record : context.featureElements) {
        if (record.field.usagePage != kSensorPage || record.field.usage != usage) continue;
        IOHIDValueRef readback = nullptr;
        const auto result = IOHIDDeviceGetValueWithOptions(
            context.device, record.element, &readback,
            kIOHIDDeviceGetValueWithoutUpdate);
        const auto actual = result == kIOReturnSuccess && readback
            ? IOHIDValueGetIntegerValue(readback) : -1;
        Logger::instance().write(
            actual == expected ? LogLevel::info : LogLevel::error,
            std::format(L"Final {} expected={} readback={} report={}",
                        label, expected, actual, record.field.reportId));
        return actual == expected;
    }
    Logger::instance().write(LogLevel::error,
                             std::format(L"Final verification could not find {}", label));
    return false;
}

bool readFeatureReport(HidBackend::Context& context, std::uint8_t reportId,
                       std::vector<std::uint8_t>& report) {
    const auto maxSize = static_cast<CFIndex>(cfNumber(IOHIDDeviceGetProperty(
        context.device, CFSTR(kIOHIDMaxFeatureReportSizeKey))));
    if (maxSize <= 0 || maxSize > kMaximumFeatureReportBytes) {
        Logger::instance().write(LogLevel::error,
                                 L"Feature report size is missing or exceeds the safety limit");
        return false;
    }
    report.assign(static_cast<std::size_t>(maxSize), 0);
    CFIndex length = maxSize;
    const auto result = IOHIDDeviceGetReport(
        context.device, kIOHIDReportTypeFeature, reportId,
        report.data(), &length);
    if (result != kIOReturnSuccess || length <= 0 || length > maxSize) {
        Logger::instance().write(
            LogLevel::error,
            std::format(L"Feature report {} read failed (IOKit=0x{:08X})",
                        reportId, static_cast<unsigned>(result)));
        return false;
    }
    report.resize(static_cast<std::size_t>(length));
    return true;
}

struct RawFeatureField {
    const void* identity{};
    std::uint8_t reportId{};
    std::size_t bitOffset{};
    std::size_t bitSize{};
    std::vector<const ElementRecord*> records;
};

std::vector<RawFeatureField> featureLayout(HidBackend::Context& context) {
    std::vector<RawFeatureField> layout;
    std::map<std::uint8_t, std::size_t> nextBit;
    for (const auto& record : context.featureElements) {
        const auto parent = IOHIDElementGetParent(record.element);
        const void* identity = IOHIDElementIsArray(record.element) && parent
            ? static_cast<const void*>(parent)
            : static_cast<const void*>(record.element);
        const auto existing = std::find_if(
            layout.begin(), layout.end(),
            [&](const auto& field) {
                return field.reportId == record.field.reportId && field.identity == identity;
            });
        if (existing != layout.end()) {
            existing->records.push_back(&record);
            continue;
        }
        const auto count = std::max<std::size_t>(record.field.reportCount, 1);
        if (record.field.bitSize != 0 &&
            count > std::numeric_limits<std::size_t>::max() / record.field.bitSize) {
            return {};
        }
        const auto bits = static_cast<std::size_t>(record.field.bitSize) * count;
        if (bits == 0) continue;
        auto& offset = nextBit[record.field.reportId];
        if (bits > std::numeric_limits<std::size_t>::max() - offset) return {};
        layout.push_back({identity, record.field.reportId, offset, bits, {&record}});
        offset += bits;
    }
    return layout;
}

const RawFeatureField* fieldForUsage(const std::vector<RawFeatureField>& layout,
                                     std::uint16_t usage) {
    for (const auto& field : layout) {
        if (std::any_of(field.records.begin(), field.records.end(),
                        [&](const auto* record) {
                            return record->field.usagePage == kSensorPage &&
                                   record->field.usage == usage;
                        })) {
            return &field;
        }
    }
    return nullptr;
}

struct RawAssignment {
    std::uint8_t reportId{};
    std::size_t bitOffset{};
    std::size_t bitSize{};
    std::uint64_t value{};
    std::wstring label;
};

bool appendSelectorAssignment(const std::vector<RawFeatureField>& layout,
                              std::uint16_t desired, std::uint16_t groupMin,
                              std::uint16_t groupMax, std::wstring_view label,
                              std::vector<RawAssignment>& assignments) {
    const auto* field = fieldForUsage(layout, desired);
    if (!field || field->bitSize > 64) return false;
    std::size_t selectorIndex{};
    bool found{};
    const ElementRecord* desiredRecord = nullptr;
    for (const auto* record : field->records) {
        if (record->field.usage < groupMin || record->field.usage > groupMax) continue;
        if (record->field.usage == desired) {
            found = true;
            desiredRecord = record;
            break;
        }
        ++selectorIndex;
    }
    if (!found || !desiredRecord) return false;
    const auto raw = static_cast<std::int64_t>(desiredRecord->field.logicalMin) +
                     static_cast<std::int64_t>(selectorIndex);
    if (raw < desiredRecord->field.logicalMin || raw > desiredRecord->field.logicalMax) return false;
    assignments.push_back({field->reportId, field->bitOffset, field->bitSize,
                           static_cast<std::uint64_t>(raw), std::wstring(label)});
    return true;
}

std::optional<FeatureReportLayout> explicitReportLayout(
    const std::vector<RawFeatureField>& fields, std::uint8_t reportId) {
    std::vector<DescriptorField> descriptorFields;
    for (const auto& field : fields) {
        for (const auto* record : field.records) descriptorFields.push_back(record->field);
    }
    return featureReportLayoutFor(descriptorFields, reportId);
}

bool ensureRawFeatureConfiguration(HidBackend::Context& context) {
    if (context.reportIntervalRaw < 0) return false;
    const auto layout = featureLayout(context);
    std::vector<RawAssignment> assignments;
    if (!appendSelectorAssignment(layout, kPowerFull, 0x0850, 0x0855,
                                  L"Full Power", assignments) ||
        !appendSelectorAssignment(layout, kReportingAllEvents, 0x0840, 0x0845,
                                  L"All Events", assignments)) {
        Logger::instance().write(LogLevel::error,
                                 L"Could not derive selector locations from the IOHID element tree");
        return false;
    }
    const auto* interval = fieldForUsage(layout, kReportInterval);
    if (!interval || interval->bitSize > 64) return false;
    assignments.push_back({interval->reportId, interval->bitOffset, interval->bitSize,
                           static_cast<std::uint64_t>(context.reportIntervalRaw),
                           L"Report Interval"});

    std::map<std::uint8_t, std::vector<RawAssignment>> byReport;
    for (const auto& assignment : assignments) byReport[assignment.reportId].push_back(assignment);
    for (const auto& [reportId, reportAssignments] : byReport) {
        const auto reportLayout = explicitReportLayout(layout, reportId);
        if (!reportLayout) {
            Logger::instance().write(LogLevel::error,
                                     std::format(L"Feature report {} has ambiguous report-ID metadata", reportId));
            return false;
        }
        std::vector<std::uint8_t> before;
        if (!readFeatureReport(context, reportId, before)) return false;
        auto after = before;
        if (reportLayout->hasReportIdPrefix &&
            (before.empty() || before.front() != reportId)) {
            Logger::instance().write(LogLevel::error,
                                     std::format(L"Feature report {} is missing its descriptor-declared ID prefix", reportId));
            return false;
        }
        for (const auto& assignment : reportAssignments) {
            // Prove each requested range fits the original report before any
            // modification, not merely the copy that will be written.
            if (!readFeatureBits(before, *reportLayout, assignment.bitOffset,
                                 assignment.bitSize).has_value() ||
                !writeFeatureBits(after, *reportLayout, assignment.bitOffset,
                                  assignment.bitSize, assignment.value)) {
                Logger::instance().write(LogLevel::error,
                                         std::format(L"Raw feature field {} is outside report {}",
                                                     assignment.label, reportId));
                return false;
            }
        }
        if (after != before) {
            if (!revalidateFeatureWriteTarget(context)) return false;
            const auto result = IOHIDDeviceSetReport(
                context.device, kIOHIDReportTypeFeature, reportId,
                after.data(), static_cast<CFIndex>(after.size()));
            if (result != kIOReturnSuccess) {
                Logger::instance().write(LogLevel::error,
                                         std::format(L"Raw feature report {} write failed (IOKit=0x{:08X})",
                                                     reportId, static_cast<unsigned>(result)));
                return false;
            }
            Logger::instance().write(
                LogLevel::info,
                std::format(L"Raw feature fallback report {} before={} requested={}",
                            reportId, hexDump(before), hexDump(after)));
        }
        std::vector<std::uint8_t> verified;
        if (!readFeatureReport(context, reportId, verified)) return false;
        if (reportLayout->hasReportIdPrefix &&
            (verified.empty() || verified.front() != reportId)) {
            Logger::instance().write(LogLevel::error,
                                     std::format(L"Feature read-back {} is missing its descriptor-declared ID prefix", reportId));
            return false;
        }
        for (const auto& assignment : reportAssignments) {
            const auto readback = readFeatureBits(verified, *reportLayout,
                                                  assignment.bitOffset, assignment.bitSize);
            if (!readback || *readback != assignment.value) {
                Logger::instance().write(LogLevel::error,
                                         std::format(L"Raw feature verification failed for {}",
                                                     assignment.label));
                return false;
            }
        }
        Logger::instance().write(
            LogLevel::info,
            std::format(L"Feature report {} length={} bytes={}",
                        reportId, verified.size(), hexDump(verified)));
    }
    return true;
}

bool setReportInterval(HidBackend::Context& context) {
    for (const auto& record : context.featureElements) {
        const auto& field = record.field;
        if (field.usagePage != kSensorPage || field.usage != kReportInterval) continue;
        const auto choice = chooseReportInterval(field);
        if (!choice) return false;
        const auto raw = static_cast<CFIndex>(choice->raw);
        const auto milliseconds = choice->seconds * 1000.0;
        Logger::instance().write(LogLevel::info,
                                 std::format(L"Configuring report interval {:.3f} ms as raw={} (unit exponent 10^{})",
                                             milliseconds, raw, field.unitExponent));
        context.reportIntervalRaw = raw;
        return setFeatureInteger(context, kReportInterval, raw, L"report interval", true);
    }
    Logger::instance().write(LogLevel::error, L"Descriptor does not expose report interval");
    return false;
}

bool configureFeatures(HidBackend::Context& context) {
    // ULT WEAR exposes no vendor-reserved LE transport selector, so there is no
    // transport write to perform. If a future descriptor exposes one, add it by
    // its actual element usage before power/reporting configuration.
    Logger::instance().write(LogLevel::info, L"transport: ACL selector not exposed; skipping");
    if (!setSelector(context, kPowerFull, 0x0850, 0x0855, L"Full Power")) return false;
    if (!setSelector(context, kReportingAllEvents, 0x0840, 0x0845, L"All Events")) return false;
    if (!setReportInterval(context)) return false;
    if (!ensureRawFeatureConfiguration(context)) return false;
    const bool fullPower = verifyFeatureInteger(context, kPowerFull, 1, L"Full Power");
    const bool powerOff = verifyFeatureInteger(context, 0x0855, 0, L"Power Off");
    const bool allEvents = verifyFeatureInteger(context, kReportingAllEvents, 1, L"All Events");
    const bool noEvents = verifyFeatureInteger(context, 0x0840, 0, L"No Events");
    return fullPower && powerOff && allEvents && noEvents;
}

IOHIDDeviceRef findDeviceByIdentifier(const DeviceInfo& target,
                                      IOHIDManagerRef& retainedManager) {
    CfObject manager(IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDManagerOptionNone));
    if (!manager.get()) return nullptr;
    if (IOHIDManagerOpen(manager.as<IOHIDManagerRef>(), kIOHIDManagerOptionNone) != kIOReturnSuccess) return nullptr;
    auto devices = copyMatchingDevices(manager.as<IOHIDManagerRef>());
    IOHIDDeviceRef selected = nullptr;
    if (devices.get()) {
        const auto count = CFSetGetCount(devices.as<CFSetRef>());
        std::vector<const void*> values(static_cast<std::size_t>(count));
        CFSetGetValues(devices.as<CFSetRef>(), values.data());
        for (const auto raw : values) {
            auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(raw));
            if (IOHIDDeviceConformsTo(device, kSensorPage, kOtherCustom) && identifierFor(device) == target.path) {
                selected = reinterpret_cast<IOHIDDeviceRef>(const_cast<void*>(CFRetain(device)));
                break;
            }
        }
    }
    devices.reset();
    if (selected) {
        retainedManager = reinterpret_cast<IOHIDManagerRef>(
            const_cast<void*>(CFRetain(manager.get())));
    } else {
        IOHIDManagerClose(manager.as<IOHIDManagerRef>(), kIOHIDManagerOptionNone);
    }
    return selected;
}

} // namespace

HidBackend::HidBackend() = default;
HidBackend::~HidBackend() { disconnect(); }

std::vector<DeviceInfo> HidBackend::enumerate(bool /*presentInterfacesOnly*/) {
    std::vector<DeviceInfo> devices;
    const auto access = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    if (access == kIOHIDAccessTypeDenied) {
        Logger::instance().write(LogLevel::error, L"macOS denied IOHID listen access; enable Input Monitoring and retry");
        return devices;
    }
    if (access != kIOHIDAccessTypeGranted && !IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)) {
        Logger::instance().write(LogLevel::error, L"macOS IOHID listen access was not granted");
        return devices;
    }
    CfObject manager(IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDManagerOptionNone));
    if (!manager.get()) return devices;
    const auto openResult = IOHIDManagerOpen(manager.as<IOHIDManagerRef>(), kIOHIDManagerOptionNone);
    if (openResult != kIOReturnSuccess) {
        Logger::instance().write(LogLevel::error, L"IOHIDManagerOpen failed");
        return devices;
    }
    auto matchingDevices = copyMatchingDevices(manager.as<IOHIDManagerRef>());
    if (matchingDevices.get()) {
        const auto count = CFSetGetCount(matchingDevices.as<CFSetRef>());
        std::vector<const void*> values(static_cast<std::size_t>(count));
        CFSetGetValues(matchingDevices.as<CFSetRef>(), values.data());
        for (const auto raw : values) {
            auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(raw));
            if (!IOHIDDeviceConformsTo(device, kSensorPage, kOtherCustom)) continue;
            auto info = describeDevice(device);
            Logger::instance().write(info.androidHeadTracker ? LogLevel::info : LogLevel::warning,
                                     std::format(L"macOS HID candidate product='{}' usage=0x{:04X}:0x{:04X} marker={}",
                                                 info.product, info.usagePage, info.usage,
                                                 info.androidHeadTracker ? L"yes" : L"no"));
            devices.push_back(std::move(info));
        }
    }
    matchingDevices.reset();
    IOHIDManagerClose(manager.as<IOHIDManagerRef>(), kIOHIDManagerOptionNone);
    return devices;
}

bool HidBackend::connect(const DeviceInfo& device, RawCallback raw, SampleCallback sample) {
    disconnect();
    IOHIDManagerRef retainedManager = nullptr;
    auto selected = findDeviceByIdentifier(device, retainedManager);
    if (!selected) {
        Logger::instance().write(LogLevel::error, L"Could not reacquire the selected macOS HID device");
        return false;
    }
    auto context = std::make_unique<Context>();
    context->manager = retainedManager;
    context->device = selected;
    context->elements = IOHIDDeviceCopyMatchingElements(selected, nullptr, kIOHIDOptionsTypeNone);
    if (!context->elements || IOHIDDeviceOpen(selected, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        Logger::instance().write(LogLevel::error, L"Could not open the selected macOS HID device");
        return false;
    }
    collectElements(*context);
    const auto reportSize = cfNumber(IOHIDDeviceGetProperty(selected, CFSTR(kIOHIDMaxInputReportSizeKey)));
    if (reportSize == 0) {
        Logger::instance().write(LogLevel::error, L"Descriptor exposes no input report size");
        return false;
    }
    context->reportBuffer.resize(static_cast<std::size_t>(reportSize));
    context->rawBuffer.reserve(static_cast<std::size_t>(reportSize) + 1);
    context->valueScratch.reserve(8);
    context->raw = std::move(raw);
    context->sample = std::move(sample);
    context_ = std::move(context);
    running_ = true;
    std::promise<void> scheduled;
    auto scheduledFuture = scheduled.get_future();
    reader_ = std::jthread([this, scheduled = std::move(scheduled)](std::stop_token stop) mutable {
        auto* current = context_.get();
        current->runLoop = reinterpret_cast<CFRunLoopRef>(const_cast<void*>(CFRetain(CFRunLoopGetCurrent())));
        Logger::instance().write(
            LogLevel::info,
            std::format(L"Registering input report callback with {}-byte buffer",
                        current->reportBuffer.size()));
        IOHIDDeviceRegisterInputReportCallback(current->device, current->reportBuffer.data(),
                                               static_cast<CFIndex>(current->reportBuffer.size()),
                                               inputReportCallback, current);
        IOHIDDeviceRegisterInputValueCallback(current->device, inputValueCallback, current);
        IOHIDDeviceRegisterRemovalCallback(current->device, removalCallback, current);
        IOHIDManagerScheduleWithRunLoop(current->manager, current->runLoop, kCFRunLoopDefaultMode);
        IOHIDDeviceScheduleWithRunLoop(current->device, current->runLoop, kCFRunLoopDefaultMode);
        scheduled.set_value();
        bool firstRun = true;
        while (!stop.stop_requested() && !current->removed) {
            const auto result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
            if (firstRun) {
                Logger::instance().write(
                    LogLevel::info,
                    std::format(L"Input run loop first result={}", static_cast<long>(result)));
                firstRun = false;
            }
        }
        IOHIDDeviceUnscheduleFromRunLoop(current->device, current->runLoop, kCFRunLoopDefaultMode);
        IOHIDManagerUnscheduleFromRunLoop(current->manager, current->runLoop, kCFRunLoopDefaultMode);
        running_ = false;
    });
    scheduledFuture.get();
    if (!configureFeatures(*context_)) {
        Logger::instance().write(LogLevel::error, L"Head tracker feature configuration failed");
        disconnect();
        return false;
    }
    Logger::instance().write(LogLevel::info, L"macOS head tracker feature configuration accepted");
    return true;
}

void HidBackend::disconnect() {
    running_ = false;
    if (reader_.joinable()) {
        reader_.request_stop();
        if (context_ && context_->runLoop) CFRunLoopStop(context_->runLoop);
        reader_.join();
    }
    context_.reset();
}

std::wstring hexDump(const std::vector<std::uint8_t>& bytes) {
    std::wostringstream out;
    out << std::hex << std::uppercase << std::setfill(L'0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index) out << L' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return out.str();
}

} // namespace sony
