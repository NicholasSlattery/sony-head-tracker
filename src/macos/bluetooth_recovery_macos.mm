// bluetooth_recovery_macos.mm
// Uses public IOBluetooth APIs to reconnect the exact paired headset and
// refresh its SDP services so macOS can recreate the Bluetooth HID collection.
#include "sony_head_tracker/bluetooth_recovery.hpp"
#include "sony_head_tracker/macos_support.hpp"

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

namespace sony {
namespace {

std::string normalizedAddress(std::wstring_view value) {
    std::string result;
    result.reserve(12);
    for (const auto character : value) {
        if (character >= L'0' && character <= L'9') {
            result.push_back(static_cast<char>(character));
        } else if (character >= L'a' && character <= L'f') {
            result.push_back(static_cast<char>(character - L'a' + 'A'));
        } else if (character >= L'A' && character <= L'F') {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

std::string normalizedAddress(NSString* value) {
    if (!value) return {};
    const auto* utf8 = [value UTF8String];
    if (!utf8) return {};
    std::string result;
    for (const auto* cursor = reinterpret_cast<const unsigned char*>(utf8); *cursor; ++cursor) {
        if (std::isxdigit(*cursor)) {
            result.push_back(static_cast<char>(std::toupper(*cursor)));
        }
    }
    return result;
}

NSString* stringFromWide(std::wstring_view value) {
    if (value.empty()) return nil;
    static_assert(sizeof(wchar_t) == 4);
    return [[[NSString alloc] initWithBytes:value.data()
                                     length:value.size() * sizeof(wchar_t)
                                   encoding:NSUTF32LittleEndianStringEncoding] autorelease];
}

IOBluetoothDevice* exactPairedDevice(std::wstring_view address,
                                     std::wstring_view fallbackName) {
    NSArray* paired = [IOBluetoothDevice pairedDevices];
    if (!paired) return nil;
    const auto targetAddress = normalizedAddress(address);
    if (!targetAddress.empty()) {
        for (IOBluetoothDevice* device in paired) {
            if (normalizedAddress(device.addressString) == targetAddress) return device;
        }
        return nil;
    }

    NSString* targetName = stringFromWide(fallbackName);
    if (!targetName) return nil;
    IOBluetoothDevice* match = nil;
    for (IOBluetoothDevice* device in paired) {
        if (device.name && [device.name caseInsensitiveCompare:targetName] == NSOrderedSame) {
            if (match) return nil; // Ambiguous names must never select an arbitrary device.
            match = device;
        }
    }
    return match;
}

bool exactHidCollectionVisible(const std::string& address, NSString* fallbackName) {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                                  kIOHIDManagerOptionNone);
    if (!manager) return false;

    CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const int usagePage = 0x20;
    const int usage = 0xE1;
    CFNumberRef pageNumber = CFNumberCreate(kCFAllocatorDefault,
                                             kCFNumberIntType, &usagePage);
    CFNumberRef usageNumber = CFNumberCreate(kCFAllocatorDefault,
                                              kCFNumberIntType, &usage);
    CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsagePageKey), pageNumber);
    CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsageKey), usageNumber);
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(usageNumber);
    CFRelease(pageNumber);
    CFRelease(matching);

    bool visible{};
    bool ambiguousName{};
    if (IOHIDManagerOpen(manager, kIOHIDManagerOptionNone) == kIOReturnSuccess) {
        CFSetRef devices = IOHIDManagerCopyDevices(manager);
        if (devices) {
            const auto count = CFSetGetCount(devices);
            std::vector<const void*> values(static_cast<std::size_t>(count));
            CFSetGetValues(devices, values.data());
            for (const auto raw : values) {
                auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(raw));
                if (!IOHIDDeviceConformsTo(device, usagePage, usage)) continue;
                if (!address.empty()) {
                    auto serial = (NSString*)IOHIDDeviceGetProperty(
                        device, CFSTR(kIOHIDSerialNumberKey));
                    if (normalizedAddress(serial) == address) {
                        visible = true;
                        break;
                    }
                    continue;
                }
                auto product = (NSString*)IOHIDDeviceGetProperty(
                    device, CFSTR(kIOHIDProductKey));
                if (!fallbackName || !product ||
                    [product caseInsensitiveCompare:fallbackName] != NSOrderedSame) {
                    continue;
                }
                if (visible) {
                    ambiguousName = true;
                    break;
                }
                visible = true;
            }
            CFRelease(devices);
        }
        IOHIDManagerClose(manager, kIOHIDManagerOptionNone);
    }
    CFRelease(manager);
    return visible && !ambiguousName;
}

void runLoopFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
}

BluetoothConnectionWaitResult waitForConnection(IOBluetoothDevice* device,
                                                std::stop_token stop) {
    return waitForBluetoothConnection(
        true,
        [&] { return device.isConnected; },
        [&](std::chrono::milliseconds duration) { runLoopFor(duration); },
        [&] { return stop.stop_requested(); },
        [] { return std::chrono::steady_clock::now(); });
}

NSURL* recoveryIdentityURL(bool createDirectory) {
    NSFileManager* files = [NSFileManager defaultManager];
    NSURL* applicationSupport = [[files URLsForDirectory:NSApplicationSupportDirectory
                                                inDomains:NSUserDomainMask] firstObject];
    if (!applicationSupport) return nil;
    NSURL* directory = [applicationSupport URLByAppendingPathComponent:@"SonyHeadTracker"
                                                            isDirectory:YES];
    if (createDirectory && ![files createDirectoryAtURL:directory
                            withIntermediateDirectories:YES
                                             attributes:@{ NSFilePosixPermissions: @0700 }
                                                  error:nil]) {
        return nil;
    }
    if (createDirectory && ![files setAttributes:@{ NSFilePosixPermissions: @0700 }
                                      ofItemAtPath:directory.path error:nil]) {
        return nil;
    }
    return [directory URLByAppendingPathComponent:@"last-tracker.plist" isDirectory:NO];
}

} // namespace

std::wstring loadLastVerifiedBluetoothAddress() {
    @autoreleasepool {
        NSURL* url = recoveryIdentityURL(false);
        if (!url) return {};
        NSDictionary* identity = [NSDictionary dictionaryWithContentsOfURL:url];
        NSString* address = [identity objectForKey:@"address"];
        const auto normalized = normalizedAddress(address);
        if (normalized.size() != 12) return {};
        return std::wstring(normalized.begin(), normalized.end());
    }
}

bool saveLastVerifiedBluetoothAddress(std::wstring_view bluetoothAddress) {
    const auto normalized = normalizedAddress(bluetoothAddress);
    if (normalized.size() != 12) return false;
    @autoreleasepool {
        NSURL* url = recoveryIdentityURL(true);
        if (!url) return false;
        NSString* address = [NSString stringWithUTF8String:normalized.c_str()];
        NSDictionary* identity = @{ @"address": address, @"verifiedMarker": @"#AndroidHeadTracker#" };
        if (![identity writeToURL:url atomically:YES]) return false;
        NSFileManager* files = [NSFileManager defaultManager];
        if (![files setAttributes:@{ NSFilePosixPermissions: @0600 }
                       ofItemAtPath:url.path error:nil]) {
            [files removeItemAtURL:url error:nil];
            return false;
        }
        NSDictionary* attributes = [files attributesOfItemAtPath:url.path error:nil];
        return [[attributes objectForKey:NSFilePosixPermissions] unsignedShortValue] == 0600;
    }
}

BluetoothRecoveryResult recoverPairedBluetoothHid(
    std::wstring_view bluetoothAddress,
    std::wstring_view fallbackProductName,
    bool forceBasebandReconnect,
    std::stop_token stop) {
    BluetoothRecoveryResult result;
    @autoreleasepool {
        IOBluetoothDevice* device = exactPairedDevice(
            bluetoothAddress, fallbackProductName);
        if (!device || !device.isPaired) return result;
        result.pairedDeviceFound = true;
        result.wasConnected = device.isConnected;
        result.forcedBasebandReconnect = forceBasebandReconnect;

        if (forceBasebandReconnect && device.isConnected) {
            result.closeStatus = static_cast<std::int32_t>([device closeConnection]);
            const auto disconnectDeadline = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(2);
            while (!stop.stop_requested() && device.isConnected &&
                   std::chrono::steady_clock::now() < disconnectDeadline) {
                runLoopFor(std::chrono::milliseconds(50));
            }
            if (stop.stop_requested()) {
                result.cancelled = true;
                return result;
            }
        }

        bool opened = true;
        if (!device.isConnected) {
            result.openStatus = static_cast<std::int32_t>([device openConnection]);
            opened = result.openStatus == kIOReturnSuccess;
        }
        const auto connectionResult = opened
            ? waitForConnection(device, stop)
            : BluetoothConnectionWaitResult::openFailed;
        result.connected = connectionResult == BluetoothConnectionWaitResult::connected;
        result.connectionTimedOut = connectionResult == BluetoothConnectionWaitResult::timedOut;
        result.cancelled = connectionResult == BluetoothConnectionWaitResult::cancelled;
        if (!bluetoothConnectionConfirmed(connectionResult, device.isConnected)) {
            result.connected = false;
            return result;
        }

        // Refresh the paired device's published services through SDP. Recovery
        // deliberately does not change HID ignore state or driver bindings.
        NSDate* before = [device getLastServicesUpdate];
        const auto beforeTime = before ? before.timeIntervalSinceReferenceDate : -1.0;
        if (stop.stop_requested() ||
            !bluetoothConnectionConfirmed(connectionResult, device.isConnected)) return result;
        result.sdpStartStatus = static_cast<std::int32_t>([device performSDPQuery:nil]);
        result.sdpQueryStarted = result.sdpStartStatus == kIOReturnSuccess;
        if (result.sdpQueryStarted) {
            const auto sdpDeadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(5);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < sdpDeadline) {
                runLoopFor(std::chrono::milliseconds(50));
                NSDate* updated = [device getLastServicesUpdate];
                if (updated && updated.timeIntervalSinceReferenceDate > beforeTime) {
                    result.sdpQueryCompleted = true;
                    break;
                }
            }
            if (stop.stop_requested()) {
                result.cancelled = true;
                return result;
            }
        }
        runLoopFor(std::chrono::milliseconds(500));
    }
    return result;
}

TrackerAvailability queryPairedTrackerAvailability(
    std::wstring_view bluetoothAddress,
    std::wstring_view fallbackProductName) {
    TrackerAvailability result;
    @autoreleasepool {
        IOBluetoothDevice* device = exactPairedDevice(
            bluetoothAddress, fallbackProductName);
        if (!device || !device.isPaired) return result;
        result.pairedDeviceFound = true;
        result.bluetoothConnected = device.isConnected;
        const auto address = normalizedAddress(bluetoothAddress);
        NSString* name = device.name;
        if (!name) name = stringFromWide(fallbackProductName);
        result.hidCollectionVisible = exactHidCollectionVisible(address, name);
    }
    return result;
}

} // namespace sony
