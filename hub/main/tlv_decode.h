/*
 * tlv_decode.h - generic Matter TLV value -> text decoder.
 *
 * This is what makes the inspector work for ANY device: attribute values arrive
 * as typed TLV (chip::TLV::TLVReader). tlv_to_str() walks the reader and renders
 * a human-readable representation (ints, floats, bools, strings, byte strings,
 * and nested structs/arrays/lists), so the console can print the value of any
 * attribute without cluster-specific decoding. Modelled on chip-tool's
 * DataModelLogger. The reader is consumed by the call.
 */
#pragma once

#include <lib/core/TLV.h>
#include <cstddef>

/* Render the current TLV element (and any nested content) into buf.
 * buf is always NUL-terminated. Safe to pass a reader positioned on any element. */
void tlv_to_str(chip::TLV::TLVReader &reader, char *buf, size_t cap);

/* Extract a scalar numeric value (signed int, unsigned int, float, or boolean->
 * 1/0) from the element the reader is positioned on, as a double. Returns false
 * for null, containers, strings. Used by the logic engine (MeasuredValue compare)
 * and the dash overview (OnOff / battery). Consumes the current element. */
bool tlv_get_scalar(chip::TLV::TLVReader &reader, double &out);
