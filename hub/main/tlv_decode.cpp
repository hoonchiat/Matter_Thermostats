/* tlv_decode.cpp - see tlv_decode.h. */
#include "tlv_decode.h"

#include <lib/core/TLV.h>
#include <lib/core/TLVTags.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cinttypes>

using chip::TLV::TLVReader;
using chip::TLV::TLVType;

namespace {

/* Bounded appender: writes into a fixed buffer, never overflows, stays
 * NUL-terminated. Once full, further puts are dropped. */
struct Sink {
    char  *buf;
    size_t cap;
    size_t len;
    void put(const char *s)
    {
        while (*s && len + 1 < cap) buf[len++] = *s++;
        if (cap) buf[len] = '\0';
    }
    void putf(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char tmp[80];
        va_list ap; va_start(ap, fmt);
        vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        put(tmp);
    }
};

/* Print the context-tag field number ("3:") when the element carries one, so
 * struct/list fields are identifiable. Anonymous/array elements print nothing. */
void put_tag(TLVReader &r, Sink &s)
{
    chip::TLV::Tag tag = r.GetTag();
    if (chip::TLV::IsContextTag(tag))
        s.putf("%u:", (unsigned)chip::TLV::TagNumFromTag(tag));
}

void decode(TLVReader &r, Sink &s, int depth)
{
    if (depth > 6) { s.put("<...>"); return; }

    switch (r.GetType()) {
    case chip::TLV::kTLVType_SignedInteger: {
        int64_t v = 0; r.Get(v); s.putf("%" PRId64, v); break;
    }
    case chip::TLV::kTLVType_UnsignedInteger: {
        uint64_t v = 0; r.Get(v); s.putf("%" PRIu64, v); break;
    }
    case chip::TLV::kTLVType_Boolean: {
        bool b = false; r.Get(b); s.put(b ? "true" : "false"); break;
    }
    case chip::TLV::kTLVType_FloatingPointNumber: {
        double d = 0; r.Get(d); s.putf("%g", d); break;
    }
    case chip::TLV::kTLVType_UTF8String: {
        uint32_t slen = r.GetLength();
        char tmp[128];
        if (slen < sizeof(tmp) && r.GetString(tmp, sizeof(tmp)) == CHIP_NO_ERROR)
            s.putf("\"%s\"", tmp);
        else
            s.putf("\"<str %u B>\"", (unsigned)slen);
        break;
    }
    case chip::TLV::kTLVType_ByteString: {
        uint32_t blen = r.GetLength();
        uint8_t tmp[32];
        uint32_t take = blen < sizeof(tmp) ? blen : sizeof(tmp);
        if (r.GetBytes(tmp, take) == CHIP_NO_ERROR) {
            s.put("0x");
            for (uint32_t i = 0; i < take; ++i) s.putf("%02x", tmp[i]);
            if (blen > take) s.putf("…(%uB)", (unsigned)blen);
        } else {
            s.putf("<bytes %uB>", (unsigned)blen);
        }
        break;
    }
    case chip::TLV::kTLVType_Null:
        s.put("null");
        break;
    case chip::TLV::kTLVType_Structure:
    case chip::TLV::kTLVType_Array:
    case chip::TLV::kTLVType_List: {
        bool isStruct = (r.GetType() == chip::TLV::kTLVType_Structure);
        s.put(isStruct ? "{" : "[");
        TLVType outer;
        if (r.EnterContainer(outer) == CHIP_NO_ERROR) {
            bool first = true;
            while (r.Next() == CHIP_NO_ERROR) {
                if (!first) s.put(", ");
                first = false;
                put_tag(r, s);
                decode(r, s, depth + 1);
            }
            r.ExitContainer(outer);
        }
        s.put(isStruct ? "}" : "]");
        break;
    }
    default:
        s.put("<?>");
        break;
    }
}

} // namespace

void tlv_to_str(TLVReader &reader, char *buf, size_t cap)
{
    Sink s{ buf, cap, 0 };
    if (cap) buf[0] = '\0';
    decode(reader, s, 0);
}

bool tlv_get_scalar(TLVReader &reader, double &out)
{
    switch (reader.GetType()) {
    case chip::TLV::kTLVType_SignedInteger: {
        int64_t v = 0; if (reader.Get(v) != CHIP_NO_ERROR) return false;
        out = (double)v; return true;
    }
    case chip::TLV::kTLVType_UnsignedInteger: {
        uint64_t v = 0; if (reader.Get(v) != CHIP_NO_ERROR) return false;
        out = (double)v; return true;
    }
    case chip::TLV::kTLVType_FloatingPointNumber: {
        double d = 0; if (reader.Get(d) != CHIP_NO_ERROR) return false;
        out = d; return true;
    }
    case chip::TLV::kTLVType_Boolean: {
        bool b = false; if (reader.Get(b) != CHIP_NO_ERROR) return false;
        out = b ? 1.0 : 0.0; return true;   /* e.g. OnOff -> 1/0 */
    }
    default:
        return false;   /* null / string / container: not a scalar */
    }
}
