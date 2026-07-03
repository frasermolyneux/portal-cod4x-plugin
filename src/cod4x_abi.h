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

typedef enum
{
    qfalse,
    qtrue
} qboolean;

constexpr int kMaxStringChars = 1024;

typedef enum
{
    NA_BAD = 0,
    NA_BOT = 0,
    NA_LOOPBACK = 2,
    NA_BROADCAST = 3,
    NA_IP = 4,
    NA_IP6 = 5,
    NA_TCP = 6,
    NA_TCP6 = 7,
    NA_MULTICAST6 = 8,
    NA_UNSPEC = 9,
    NA_DOWN = 10,
} netadrtype_t;

typedef struct
{
    qboolean overflowed;
    qboolean readonly;
    byte* data;
    byte* splitData;
    int maxsize;
    int cursize;
    int splitSize;
    int readcount;
    int bit;
    int lastRefEntity;
} msg_t;

typedef struct
{
    netadrtype_t type;
    int scope_id;
    unsigned short port;
    unsigned short pad;
    int sock;
    union
    {
        byte ip[4];
        byte ipx[10];
        byte ip6[16];
    } address;
} netadr_t;

typedef enum
{
    FT_PROTO_HTTP,
    FT_PROTO_HTTPS,
    FT_PROTO_FTP
} ftprotocols_t;

typedef struct
{
    qboolean lock;
    qboolean active;
    qboolean transferactive;
    int transferStartTime;
    int socket;
    int transfersocket;
    int sentBytes;
    int finallen;
    int totalreceivedbytes;
    int transfertotalreceivedbytes;
    msg_t* extrecvmsg;
    msg_t* extsendmsg;
    msg_t sendmsg;
    msg_t recvmsg;
    msg_t transfermsg;
    qboolean complete;
    int code;
    int version;
    char status[32];
    char url[kMaxStringChars];
    char address[kMaxStringChars];
    char username[256];
    char password[256];
    char contentType[64];
    char cookie[kMaxStringChars];
    int mode;
    int headerLength;
    int contentLength;
    int contentLengthArrived;
    int currentChunkLength;
    int currentChunkReadOffset;
    int chunkedEncoding;
    int startTime;
    int stage;
    ftprotocols_t protocol;
    netadr_t remote;
    qboolean socketReady;
    void* tls;
} ftRequest_t;

extern "C" void COD4X_CALL Plugin_Printf(const char* fmt, ...);
extern "C" void COD4X_CALL Plugin_ChatPrintf(int slot, const char* fmt, ...);
extern "C" void COD4X_CALL Plugin_Cbuf_AddText(const char* text);
extern "C" ftRequest_t* COD4X_CALL Plugin_HTTP_MakeHttpRequest(
    const char* url,
    const char* method,
    byte* requestpayload,
    int payloadlen,
    const char* additionalheaderlines);
extern "C" int COD4X_CALL Plugin_HTTP_SendReceiveData(ftRequest_t* request);
extern "C" void COD4X_CALL Plugin_HTTP_FreeObj(ftRequest_t* request);

#endif
