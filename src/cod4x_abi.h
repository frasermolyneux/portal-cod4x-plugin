#pragma once

#include <cstddef>

#if defined(PORTAL_COD4X_USE_EXTERNAL_SDK)
#if !defined(PLUGIN_HANDLER_VERSION_MAJOR)
#define PLUGIN_HANDLER_VERSION_MAJOR 4
#endif

#if !defined(PLUGIN_HANDLER_VERSION_MINOR)
#define PLUGIN_HANDLER_VERSION_MINOR 0
#endif

#include "pinc.h"

#if !defined(COD4X_CALL)
#if defined(_MSC_VER)
#define COD4X_CALL __cdecl
#else
#define COD4X_CALL __attribute__((cdecl))
#endif
#endif
#else

#if defined(_MSC_VER)
#define COD4X_CALL __cdecl
#else
#define COD4X_CALL __attribute__((cdecl))
#endif

// NOTE: The calling convention (COD4X_CALL) must appear between the return type
// and the function name, so it is applied at each declaration rather than baked
// into the PCL prefix. Placing __cdecl before the return type is rejected by MSVC.
#if defined(_WIN32)
#if defined(__GNUC__)
#define PCL extern "C" __attribute__((dllexport))
#else
#define PCL extern "C" __declspec(dllexport)
#endif
#else
#define PCL extern "C" __attribute__((visibility("default")))
#endif

#define PLUGIN_HANDLER_VERSION_MAJOR 4
#define PLUGIN_HANDLER_VERSION_MINOR 0

typedef struct
{
    int major;
    int minor;
} version_t;

typedef struct
{
    version_t handlerVersion;
    version_t pluginVersion;
    char fullName[64];
    char shortDescription[128];
    char longDescription[1024];
} pluginInfo_t;

typedef unsigned char byte;

typedef struct
{
    byte* data;
    int cursize;
} msg_t;

typedef struct
{
    int code;
    int headerLength;
    int contentLength;
    int contentLengthArrived;
    msg_t* extrecvmsg;
} ftRequest_t;

extern "C" void COD4X_CALL Plugin_Printf(const char* fmt, ...);
extern "C" void COD4X_CALL Plugin_ChatPrintf(int slot, const char* fmt, ...);
extern "C" void COD4X_CALL Plugin_Cbuf_AddText(const char* text);
extern "C" ftRequest_t* COD4X_CALL Plugin_HTTP_Request(
    const char* url,
    const char* method,
    byte* requestpayload,
    int payloadlen,
    const char* additionalheaderlines);
extern "C" void COD4X_CALL Plugin_HTTP_FreeObj(ftRequest_t* request);

#endif
