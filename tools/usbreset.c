// usbreset.c — 对指定序列号的 USB 设备发起总线复位（相当于拔插）
// clang -framework IOKit -framework CoreFoundation usbreset.c -o usbreset
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <serial>\n", argv[0]); return 1; }
    CFMutableDictionaryRef dict = IOServiceMatching(kIOUSBDeviceClassName);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, dict, &it) != KERN_SUCCESS) {
        fprintf(stderr, "no usb devices\n"); return 1;
    }
    io_service_t svc;
    int found = 0;
    while ((svc = IOIteratorNext(it))) {
        CFTypeRef snRef = IORegistryEntryCreateCFProperty(svc, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
        if (snRef) {
            char sn[256] = {0};
            if (CFGetTypeID(snRef) == CFStringGetTypeID())
                CFStringGetCString(snRef, sn, sizeof(sn), kCFStringEncodingUTF8);
            CFRelease(snRef);
            if (strcmp(sn, argv[1]) == 0) {
                found = 1;
                IOCFPlugInInterface **plugIn = NULL;
                SInt32 score = 0;
                kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceInterfaceID, kIOCFPlugInInterfaceID, &plugIn, &score);
                if (kr == KERN_SUCCESS && plugIn) {
                    IOUSBDeviceInterface **dev = NULL;
                    if ((*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (void **)&dev) == KERN_SUCCESS) {
                        IOReturn rc = (*dev)->ResetDevice(dev);
                        printf("ResetDevice -> 0x%x (%s)\n", rc, rc == KERN_SUCCESS ? "OK" : "FAILED");
                        (*dev)->Release(dev);
                    } else fprintf(stderr, "QueryInterface failed\n");
                    (*plugIn)->Release(plugIn);
                } else fprintf(stderr, "create plugin failed: 0x%x\n", kr);
                IOObjectRelease(svc);
                break;
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (!found) { fprintf(stderr, "device with serial %s not found\n", argv[1]); return 1; }
    return 0;
}
