#pragma once

#include <switch.h>
#include "defines.hpp"

namespace sphaira::usb::api {

enum : u32 {
    MAGIC = 0x53504830,
    PACKET_SIZE = 16,
};

enum : u32 {
    CMD_QUIT = 0,
    CMD_OPEN = 1,
    CMD_EXPORT = 1,
};

enum : u32 {
    RESULT_OK = 0,
    RESULT_ERROR = 1,
};

enum : u32 {
    FLAG_NONE = 0,
    FLAG_STREAM = 1 << 0,
};

struct SendHeader {
    u32 magic;
    u32 cmd;
    u32 arg3;
    u32 arg4;

    Result Verify() const {
        R_UNLESS(magic == MAGIC, Result_UsbBadMagic);
        R_SUCCEED();
    }
};

struct ResultHeader {
    u32 magic;
    u32 result;
    u32 arg3;
    u32 arg4;

    Result Verify() const {
        R_UNLESS(magic == MAGIC, Result_UsbBadMagic);
        R_UNLESS(result == RESULT_OK, 1); // todo: create error code.
        R_SUCCEED();
    }
};

struct SendDataHeader {
    u64 offset;
    u32 size;
    u32 crc32c;
};

static_assert(sizeof(SendHeader) == PACKET_SIZE);
static_assert(sizeof(ResultHeader) == PACKET_SIZE);
static_assert(sizeof(SendDataHeader) == PACKET_SIZE);

} // namespace sphaira::usb::api
