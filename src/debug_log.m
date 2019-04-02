
#ifdef __IPHONEOS__
#include <CoreFoundation/CoreFoundation.h>

extern void iOSLog(const char* format, ...) 
{
	
    va_list args;
    va_start(args, format);
    NSLogv(CFStringCreateWithCString(NULL, format, kCFStringEncodingUTF8), args);
    va_end(args);
}

#endif

#if 0
#ifdef __IPHONEOS__
#include <stdarg.h>
#include <CoreFoundation/CoreFoundation.h>
//#include <Foundation/Foundation.h>
extern void NSLog(CFStringRef format, ...);
//FOUNDATION_EXPORT void NSLogv(NSString *format, va_list args) NS_FORMAT_FUNCTION(1,0) NS_NO_TAIL_CALL;
extern void NSLogv(CFStringRef format, va_list) NS_FORMAT_FUNCTION(1,0);
extern void iOSLog(const char* format, ...) 
{
	
    va_list args;
    va_start(args, format);
    NSLogv(CFStringCreateWithCString(NULL, format, kCFStringEncodingUTF8), args);
    va_end(args);
}
#endif
#endif
