#!/usr/bin/env python3
"""Lift the initial E32 ARM/Thumb startup path into host-compilable C."""

import argparse
import json
from pathlib import Path
import struct


class Image:
    def __init__(self, data, base=None):
        self.data = data
        if data[16:20] != b'EPOC' or self.u32(0x1c):
            raise ValueError('expected uncompressed E32')
        linked_base = self.u32(0x4c)
        self.base = linked_base if base is None else base
        self.entry = self.base + self.u32(0x48)
        code_offset = self.u32(0x64)
        self.code_size = self.u32(0x30)
        self.code = bytearray(data[code_offset:code_offset + self.code_size])
        if len(self.code) != self.code_size:
            raise ValueError('truncated E32 code segment')
        self.relocated = []
        self.relocate_code(self.base - linked_base)
        self.heap_min, self.heap_max, self.stack_size = struct.unpack_from('<III', data, 0x38)
        self.imports = {}
        section = self.u32(0x6c)
        offset = section + 4
        for _ in range(self.u32(0x54)):
            name, count = struct.unpack_from('<II', data, offset)
            name += section
            dll = data[name:data.index(b'\0', name)].decode('ascii')
            offset += 8
            for _ in range(count):
                slot = self.base + self.u32(offset)
                offset += 4
                ordinal = self.load(slot)
                self.imports[slot] = dll, ordinal

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def relocate_code(self, delta):
        section = self.u32(0x70)
        if not section:
            return
        length, count = struct.unpack_from('<II', self.data, section)
        cursor = section + 8
        end = cursor + length
        if end > len(self.data):
            raise ValueError('truncated E32 relocation section')
        while cursor < end:
            page, length = struct.unpack_from('<II', self.data, cursor)
            if length < 8 or length % 4 or cursor + length > end or page % 4096:
                raise ValueError('invalid E32 relocation block')
            for (entry,) in struct.iter_unpack('<H', self.data[cursor + 8:cursor + length]):
                if entry == 0:
                    continue
                offset = page + (entry & 4095)
                if entry >> 12 != 1 or offset % 4 or offset + 4 > self.code_size:
                    raise ValueError(f'unsupported E32 code relocation {entry:#x} at {offset:#x}')
                value, = struct.unpack_from('<I', self.code, offset)
                struct.pack_into('<I', self.code, offset, (value + delta) & 0xffffffff)
                self.relocated.append(self.base + offset)
                count -= 1
            cursor += length
        if count:
            raise ValueError('E32 relocation count mismatch')

    def load(self, address, size=4):
        offset = address - self.base
        if offset < 0 or offset + size > self.code_size:
            raise ValueError(f'address outside E32 code: {address:#x}')
        return int.from_bytes(self.code[offset:offset + size], 'little')

    def export(self, ordinal):
        if not 1 <= ordinal <= self.u32(0x5c):
            raise ValueError(f'invalid E32 export ordinal {ordinal}')
        address = self.base + self.u32(0x58) - self.u32(0x64) + 4 * (ordinal - 1)
        target = self.load(address)
        if (target & ~1) - self.base not in range(self.code_size):
            raise ValueError(f'export {ordinal} is not in E32 code: {target:#x}')
        return target


def signed(x, bits):
    sign = 1 << (bits - 1)
    return (x ^ sign) - sign


# Data-processing opcodes that do not read or write flags when S is clear (TST..CMN need S).
DP_NO_FLAGS_OPCODES = (0, 1, 2, 3, 4, 12, 13, 14, 15)


def dp_expr(opcode, rn, shifted, left=None):
    """C expression for an ARM data-processing opcode without flags, operand2 already shifted."""
    left = left or f'r[{rn}]'
    return {0: f'{left} & ({shifted})', 1: f'{left} ^ ({shifted})', 2: f'{left} - ({shifted})',
            3: f'({shifted}) - {left}', 4: f'{left} + ({shifted})', 12: f'{left} | ({shifted})',
            13: f'({shifted})', 14: f'{left} & ~({shifted})', 15: f'~({shifted})'}[opcode]


# dfpaeabi ordinals the game imports, mapped to C statements over the EABI register pairs (doubles in r0:r1, r2:r3).
DFPAEABI_OPS = {
    8: 'aeabi_d2iz(r);', 12: 'put_dbl(r, dbl_arg(r, 0) + dbl_arg(r, 2));',
    13: 'r[0] = dbl_arg(r, 0) == dbl_arg(r, 2);', 14: 'r[0] = dbl_arg(r, 0) >= dbl_arg(r, 2);',
    15: 'r[0] = dbl_arg(r, 0) > dbl_arg(r, 2);', 16: 'r[0] = dbl_arg(r, 0) <= dbl_arg(r, 2);',
    17: 'r[0] = dbl_arg(r, 0) < dbl_arg(r, 2);', 18: 'r[0] = isnan(dbl_arg(r, 0)) || isnan(dbl_arg(r, 2));',
    19: 'put_dbl(r, dbl_arg(r, 0) / dbl_arg(r, 2));', 20: 'put_dbl(r, dbl_arg(r, 0) * dbl_arg(r, 2));',
    21: 'put_dbl(r, -dbl_arg(r, 0));', 22: 'put_dbl(r, dbl_arg(r, 2) - dbl_arg(r, 0));',
    23: 'put_dbl(r, dbl_arg(r, 0) - dbl_arg(r, 2));', 41: 'aeabi_i2d(r);',
    # Single precision: one float per core register.
    7: 'put_flt(r, (float)dbl_arg(r, 0));', 24: 'put_dbl(r, (double)flt_arg(r, 0));',
    25: 'r[0] = to_int32_sat(flt_arg(r, 0));', 27: 'r[0] = to_uint32_sat(flt_arg(r, 0));',
    29: 'put_flt(r, flt_arg(r, 0) + flt_arg(r, 1));', 30: 'r[0] = flt_arg(r, 0) == flt_arg(r, 1);',
    31: 'r[0] = flt_arg(r, 0) >= flt_arg(r, 1);', 32: 'r[0] = flt_arg(r, 0) > flt_arg(r, 1);',
    33: 'r[0] = flt_arg(r, 0) <= flt_arg(r, 1);', 34: 'r[0] = flt_arg(r, 0) < flt_arg(r, 1);',
    36: 'put_flt(r, flt_arg(r, 0) / flt_arg(r, 1));', 37: 'put_flt(r, flt_arg(r, 0) * flt_arg(r, 1));',
    40: 'put_flt(r, flt_arg(r, 0) - flt_arg(r, 1));', 42: 'put_flt(r, (float)(int32_t)r[0]);',
}

_FAIL = '{ return unsupported(r[14], 0); }'

# euser descriptor and memory primitives (euseru.def ordinal -> C statement). Symbian panics where these fail.
# Descriptor headers: EPtrC = 1 << 28 with the pointer at +4; EPtr = 2 << 28 with max length +4 and pointer +8.
EUSER_DESCRIPTOR_OPS = {
    595: 'r[0] = guest_strlen(stack, r[0]);',  # User::StringLength(const TUint8*)
    743: f'if (!desc_store(stack, r[0], 1, r[1], 1, r[2], 0)) {_FAIL}',  # TDes8::Copy(const TUint8*, TInt)
    744: f'if (!desc_store_desc(stack, r[0], 1, r[1], 1, 0)) {_FAIL}',  # TDes8::Copy(const TDesC8&)
    745: f'if (!desc_store_desc(stack, r[0], 1, r[1], 2, 0)) {_FAIL}',  # TDes8::Copy(const TDesC16&): low byte of each unit
    752: f'if (!desc_set_length(stack, r[0], 0)) {_FAIL}',  # TDes8::Zero()
    759: f'if (!desc_store(stack, r[0], 1, r[1], 1, r[2], 1)) {_FAIL}',  # TDes8::Append(const TUint8*, TInt)
    760: f'if (!desc_store_desc(stack, r[0], 1, r[1], 1, 1)) {_FAIL}',  # TDes8::Append(const TDesC8&)
    782: f'if (!desc_set_length(stack, r[0], r[1])) {_FAIL}',  # TDes8::SetLength(TInt)
    816: ('guest_write(stack, r[0], (UINT32_C(2) << 28) | (r[2] & 0xfffffff)); guest_write(stack, r[0] + 4, r[3]);'
          ' guest_write(stack, r[0] + 8, r[1]);'),  # TPtr8::TPtr8(TUint8*, TInt, TInt) (C1)
    951: f'if (!desc_store(stack, r[0], 2, r[1], 2, r[2], 0)) {_FAIL}',  # TDes16::Copy(const TUint16*, TInt)
    952: f'if (!desc_store_desc(stack, r[0], 2, r[1], 1, 0)) {_FAIL}',  # TDes16::Copy(const TDesC8&) widens
    953: f'if (!desc_store_desc(stack, r[0], 2, r[1], 2, 0)) {_FAIL}',  # TDes16::Copy(const TDesC16&)
    # TDes16::Append(TChar): one BMP character (the 16-bit build stores UTF-16 units).
    966: ('{ uint32_t length, text = text_data(stack, r[0], &length);'
          ' if (r[1] > 0xffff || !text || length >= guest_read(stack, r[0] + 4) || !desc_set_length(stack, r[0], length + 1)) '
          + _FAIL + ' guest_write_small(stack, text + 2 * length, r[1], 2); }'),
    967: f'if (!desc_store(stack, r[0], 2, r[1], 2, r[2], 1)) {_FAIL}',  # TDes16::Append(const TUint16*, TInt)
    968: f'if (!desc_store_desc(stack, r[0], 2, r[1], 2, 1)) {_FAIL}',  # TDes16::Append(const TDesC16&)
    767: f'if (!desc_format_int(stack, r[0], r[1], 1, (int32_t)r[2])) {_FAIL}',  # TDes8::Format: script commands
    973: f'if (!desc_format_int(stack, r[0], r[1], 2, (int32_t)r[2])) {_FAIL}',  # TDes16::Format: numbered paths
    988: f'if (!desc_set_length(stack, r[0], r[1])) {_FAIL}',  # TDes16::SetLength(TInt)
    1032: 'guest_write(stack, r[0], UINT32_C(0x10000000) | guest_strlen(stack, r[1])); guest_write(stack, r[0] + 4, r[1]);',
    1033: 'guest_write(stack, r[0], UINT32_C(0x10000000) | (r[2] & 0xfffffff)); guest_write(stack, r[0] + 4, r[1]);',
    1034: ('{ uint32_t length, text = text_data(stack, r[1], &length); if (!text) ' + _FAIL +
           ' guest_write(stack, r[0], UINT32_C(0x10000000) | length); guest_write(stack, r[0] + 4, text); }'),
    1035: 'guest_write(stack, r[0], UINT32_C(0x10000000)); guest_write(stack, r[0] + 4, 0);',  # TPtrC8::TPtrC8()
    1116: f'if (!hbufc16_des(stack, r[0], r[1])) {_FAIL}',  # HBufC16::Des(), hidden result pointer in r0
    1118: ('if (r[0] > 0x0fffffffu) ' + _FAIL +
           ' r[0] = heap_try_alloc(4 + 2 * r[0]); if (!r[0]) return unhandled_leave((uint32_t)-4, r[14]);'),
    1169: f'if (!tptrc16_from_z(stack, r[0], r[1])) {_FAIL}',  # TPtrC16(const TUint16*)
    1170: 'guest_write(stack, r[0], UINT32_C(0x10000000) | (r[2] & 0xfffffff)); guest_write(stack, r[0] + 4, r[1]);',
    1719: f'{{ uint32_t length; r[0] = text_data(stack, r[0], &length); if (!r[0]) {_FAIL} }}',  # TDesC8::Ptr()
    1806: ('{ uint32_t length, text = text_data(stack, r[0], &length); if (!text || r[1] >= length) ' + _FAIL +
           ' r[0] = text + 2 * r[1]; }'),  # TDesC16::AtC(TInt) returns const TUint16&
    # TBufBase16::TBufBase16(const TDesC16&, TInt aMaxLength) / TBufBase8::TBufBase8(const TDesC8&, TInt):
    # EBuf (3 << 28) header, iMaxLength at +4, inline data at +8, contents copied from the source.
    84: ('guest_write(stack, r[0], UINT32_C(0x30000000)); guest_write(stack, r[0] + 4, r[2]);'
         f' if (!desc_store_desc(stack, r[0], 2, r[1], 2, 0)) {_FAIL}'),
    1471: ('guest_write(stack, r[0], UINT32_C(0x30000000)); guest_write(stack, r[0] + 4, r[2]);'
           f' if (!desc_store_desc(stack, r[0], 1, r[1], 1, 0)) {_FAIL}'),
    1472: 'guest_write(stack, r[0], UINT32_C(0x30000000)); guest_write(stack, r[0] + 4, r[1]);',  # TBufBase8(TInt max)
    1473: (f'if (r[1] > r[2]) {_FAIL} guest_write(stack, r[0], UINT32_C(0x30000000) | r[1]);'
           ' guest_write(stack, r[0] + 4, r[2]);'),  # TBufBase8(TInt aLength, TInt aMaxLength)
    # TDes8/TDes16::FillZ(): zero-fill to MaxLength and set the length to MaxLength.
    756: ('{ uint32_t length, text = text_data(stack, r[0], &length), max = guest_read(stack, r[0] + 4);'
          f' if (!text || !desc_set_length(stack, r[0], max)) {_FAIL}'
          ' for (uint32_t i = 0; i < max; i++) guest_write_small(stack, text + i, 0, 1); }'),
    964: ('{ uint32_t length, text = text_data(stack, r[0], &length), max = guest_read(stack, r[0] + 4);'
          f' if (!text || !desc_set_length(stack, r[0], max)) {_FAIL}'
          ' for (uint32_t i = 0; i < max; i++) guest_write_small(stack, text + 2 * i, 0, 2); }'),
    # TRect is iTl.iX, iTl.iY, iBr.iX, iBr.iY; TSize is iWidth, iHeight.
    833: ('guest_write(stack, r[0], r[1]); guest_write(stack, r[0] + 4, r[2]); guest_write(stack, r[0] + 8, r[3]);'
          ' guest_write(stack, r[0] + 12, stack_read(stack, r[13]));'),  # TRect::SetRect(TInt, TInt, TInt, TInt)
    838: ('guest_write(stack, r[0], 0); guest_write(stack, r[0] + 4, 0);'
          ' guest_write(stack, r[0] + 8, guest_read(stack, r[1])); guest_write(stack, r[0] + 12, guest_read(stack, r[1] + 4));'),
    841: ('guest_write(stack, r[0], r[1]); guest_write(stack, r[0] + 4, r[2]); guest_write(stack, r[0] + 8, r[3]);'
          ' guest_write(stack, r[0] + 12, stack_read(stack, r[13]));'),  # TRect::TRect(TInt, TInt, TInt, TInt)
    848: 'guest_write(stack, r[0], r[1]); guest_write(stack, r[0] + 4, r[2]);',  # TSize::SetSize(TInt, TInt)
    # TRect::Size() const returns TSize through the hidden result pointer r0; this is r1.
    1648: ('guest_write(stack, r[0], guest_read(stack, r[1] + 8) - guest_read(stack, r[1]));'
           ' guest_write(stack, r[0] + 4, guest_read(stack, r[1] + 12) - guest_read(stack, r[1] + 4));'),
    1602: 'r[0] = guest_read(stack, r[0]) != guest_read(stack, r[1]);',  # TUid::operator!=(const TUid&) const
    723: f'if (!desc_append_fill(stack, r[0], 1, r[1], r[2])) {_FAIL}',  # TDes8::AppendFill(TChar, TInt)
    931: f'if (!desc_append_fill(stack, r[0], 2, r[1], r[2])) {_FAIL}',  # TDes16::AppendFill(TChar, TInt)
    # TDes16::AppendNumUC(TUint64, TRadix): the 64-bit value is aligned to r2:r3, the radix is on the stack.
    934: (f'if (!desc_append_num_uc(stack, r[0], (uint64_t)r[3] << 32 | r[2], stack_read(stack, r[13]))) {_FAIL}'),
    # TChar::IsDigit() const: the decimal-digit category; only ASCII is decided here, other characters stop.
    1622: (f'{{ uint32_t ch = guest_read(stack, r[0]); if (ch >= 0x80) {_FAIL} r[0] = ch >= 0x30 && ch <= 0x39; }}'),
    245: 'guest_write(stack, r[0] + ARRAY_COUNT, 0);',  # CArrayFixBase::Reset()
    250: f'if (!array_fix_insert(stack, r[0], r[1], r[2])) {_FAIL}',  # CArrayFixBase::InsertL(TInt, const TAny*)
    1553: f'r[0] = array_fix_at(stack, r[0], r[1]); if (!r[0]) {_FAIL}',  # CArrayFixBase::At(TInt) const
    1089: '',  # CActive::RunError(TInt aError) returns aError, already in r0
    2126: '',  # CBase::~CBase() (base object destructor) has an empty body and returns this
    # CBase/CActive::Extension_(TUint, TAny*& a0, TAny*): a0 = NULL; return KErrExtensionNotSupported (-47).
    2123: 'guest_write(stack, r[2], 0); r[0] = (uint32_t)-47;',
    2128: 'guest_write(stack, r[2], 0); r[0] = (uint32_t)-47;',
    1951: 'for (uint32_t i = 0; i < r[1]; i++) guest_write_small(stack, r[0] + i, 0, 1);',  # memclr(void*, TInt)
    1952: 'r[0] = mem_compare(stack, r[0], r[1], r[2], r[3]);',  # memcompare(const TUint8*, TInt, const TUint8*, TInt)
    1953: ('for (uint32_t i = 0; i < r[2]; i++)'
           ' guest_write_small(stack, r[0] + i, guest_read_small(stack, r[1] + i, 1), 1);'),  # memcpy, returns dest
}

# ESTLIB (Symbian C library) pure functions: ordinal -> C statement. Doubles travel in r0:r1 and r2:r3.
ESTLIB_OPS = {
    108: 'r[0] = (int32_t)r[0] < 0 ? 0u - r[0] : r[0];',  # abs
    113: 'r[0] = guest_atoi(stack, r[0]);',  # atoi
    145: 'if (!heap_release(r[0])) return unsupported(r[14], 0);',  # free
    198: 'r[0] = heap_try_alloc(r[0]);',  # malloc: NULL when the heap is exhausted
    203: 'r[0] = guest_memcmp(stack, r[0], r[1], r[2]);',  # memcmp
    245: 'r[0] = guest_strcat(stack, r[0], r[1]);',  # strcat
    247: 'r[0] = guest_strcmp(stack, r[0], r[1]);',  # strcmp
    254: 'r[0] = guest_strlen(stack, r[0]);',  # strlen
    260: 'r[0] = guest_strrchr(stack, r[0], r[1]);',  # strrchr
    262: 'r[0] = guest_strstr(stack, r[0], r[1]);',  # strstr
    370: 'put_dbl(r, cos(dbl_arg(r, 0)));',  # cos
    375: 'put_dbl(r, fabs(dbl_arg(r, 0)));',  # fabs
    385: 'put_dbl(r, pow(dbl_arg(r, 0), dbl_arg(r, 2)));',  # pow
    388: 'put_dbl(r, sin(dbl_arg(r, 0)));',  # sin
}

def _stream_read(size, convert='value'):
    """RReadStream::ReadXxxL into r0; a short read leaves with KErrEof."""
    return ('{ uint32_t value; int32_t rc = read_stream_bytes(stack, r[0], %d, &value);'
            ' if (rc) return unhandled_leave((uint32_t)rc, r[14]); r[0] = %s; }' % (size, convert))


def _stream_write(size, value='r[1]'):
    return (f'{{ int32_t rc = write_stream_bytes(stack, r[0], {size}, {value});'
            ' if (rc) return unhandled_leave((uint32_t)rc, r[14]); }')


# estor (estor3.def) stream imports reached by the game: ordinal -> C statement.
ESTOR_OPS = {
    334: f'if (!file_stream_attach(stack, r[0], r[1], r[2], STREAM_TOKEN)) {_FAIL}',  # RFileReadStream
    379: f'if (!file_stream_attach(stack, r[0], r[1], r[2], WRITE_STREAM_TOKEN)) {_FAIL}',  # RFileWriteStream
    102: 'cleanup_push_item(r[0], 0, CLEANUP_READ_STREAM);',  # RReadStream::PushL()
    112: 'read_stream_release(stack, r[0]);',  # RReadStream::Release()
    96: _stream_read(4),  # ReadInt32L
    97: _stream_read(1),  # ReadUint8L
    98: _stream_read(4),  # ReadReal32L: the float bits return in r0 (soft-float EABI)
    100: _stream_read(2),  # ReadUint16L
    113: _stream_read(1, '(uint32_t)(int32_t)(int8_t)value'),  # ReadInt8L
    160: _stream_write(1),  # WriteInt8L
    162: _stream_write(4),  # WriteInt32L
    163: _stream_write(1),  # WriteUint8L
    164: _stream_write(4, 'stream_real32_bits(r)'),  # WriteReal32L(TReal): aligned double in r2:r3
    166: _stream_write(2),  # WriteUint16L
    168: 'write_stream_release(stack, r[0], 1);',  # RWriteStream::Close()
    169: 'cleanup_push_item(r[0], 0, CLEANUP_WRITE_STREAM);',  # RWriteStream::PushL()
    179: 'write_stream_release(stack, r[0], 0);',  # RWriteStream::Release()
}

# ARM condition field to generated-C predicate; 14 (AL) is unconditional, 15 (NV) is not supported.
ARM_CONDITIONS = {0: 'z', 1: '!z', 2: 'c', 3: '!c', 4: 'n', 5: '!n', 6: 'v', 7: '!v', 8: 'c && !z',
                  9: '!c || z', 10: 'n == v', 11: 'n != v', 12: '!z && n == v', 13: 'z || n != v', 14: None}


def label(pc):
    return f'pc_{pc:08x}'


def arm_immediate(word):
    imm, rotate = word & 255, ((word >> 8) & 15) * 2
    return ((imm >> rotate) | (imm << (32 - rotate))) & 0xffffffff


def lift(image, entry, reason, attach=False, game_dlls=()):
    pending, blocks = [entry], {}
    modules = (image, *(dll for _, dll in game_dlls))
    for i, a in enumerate(modules):
        if any(a.base < b.base + b.code_size and b.base < a.base + a.code_size for b in modules[:i]):
            raise ValueError('overlapping guest code segments')
    dll_targets = {target for _, dll in game_dlls for target in (dll.entry, dll.export(1))}
    pending.extend(dll_targets)
    # SimMediaEngineDll registers its mixer thread function through the code word at
    # DLL offset 0xcd3c; RThread::Create passes it by value, so lift it explicitly.
    thread_targets = set()
    for name, dll in game_dlls:
        if name.lower() == 'simmediaenginedll.dll':
            thread_entry = dll.load(dll.base + 0xcd3c)
            if (thread_entry & ~1) - dll.base not in range(dll.code_size):
                raise ValueError(f'unexpected SimMediaEngineDll thread entry {thread_entry:#x}')
            thread_targets.add(thread_entry)
    pending.extend(thread_targets)
    # Virtual functions (RunL, DoCancel, ...) and callbacks (TCleanupItem, TCallBack) are reached only through
    # relocated code pointers, so every relocated word whose value lies in a module's code is a possible entry.
    indirect_targets = set()
    for module in modules:
        for slot in module.relocated:
            value = module.load(slot)
            if (value & ~1) - module.base in range(module.code_size) and (value & 1 or value % 4 == 0):
                indirect_targets.add(value)
    pending.extend(indirect_targets)
    call_returns = set()
    factory_targets = set()
    calls_factory = False
    app_vtable = image.base + 0x8a960
    document_vtable = image.base + 0x8ab58
    appui_vtable = image.base + 0x8a9d0
    control_vtable = image.base + 0x8ac10
    virtual_imports = {}
    app_profile = not attach and image.u32(8) == 0x2000bbe5
    if app_profile:
        pre_doc = image.imports.get(app_vtable + 12, ('', 0))
        create_doc = image.imports.get(app_vtable + 16, ('', 0))
        if (pre_doc[0].lower(), pre_doc[1], create_doc[0].lower(), create_doc[1]) != (
                'avkon{000a0000}[100056c6].dll', 686, 'eikcore{000a0000}[10004892].dll', 61):
            raise ValueError('unexpected Lament Island application vtable')
        lifecycle_targets = {image.load(vtable + slot) for vtable, slot in (
            (app_vtable, 20), (app_vtable, 68), (document_vtable, 76), (appui_vtable, 64))}
        virtual_imports[appui_vtable + 80] = ('avkon{000a0000}[100056c6].dll', 2997)
        if any((image.imports.get(slot, ('', 0))[0].lower(), image.imports.get(slot, ('', 0))[1]) != symbol
               for slot, symbol in virtual_imports.items()):
            raise ValueError('unexpected Lament Island virtual imports')
        lifecycle_targets.add(image.load(control_vtable + 0x0c))  # OfferKeyEventL
        # CCoeControl::SetExtent invokes the derived SizeChanged at slot 0x6c.
        lifecycle_targets.add(image.load(control_vtable + 0x6c))
        lifecycle_targets.add(image.load(control_vtable + 0xb0))
        lifecycle_targets.update((image.load(control_vtable + 0xd8), image.load(0x59d90), image.load(0x93ba0)))
        # Game timer RunL calls the engine's virtual update at vtable +12.
        lifecycle_targets.add(image.load(0x92e44))
        virtual_imports[control_vtable + 0x20] = ('cone{000a0000}[10003a41].dll', 61)
        if image.imports.get(control_vtable + 0x20, ('', 0))[1] != 61:
            raise ValueError('unexpected control activation slot')
        pending.extend(lifecycle_targets)
    else:
        lifecycle_targets = set()
    stack_size = 4096 if attach else image.stack_size

    def jump(target):
        target &= 0xffffffff
        pending.append(target)
        return f'goto {label(target)};'

    while pending:
        encoded = pending.pop()
        if encoded in blocks:
            continue
        pc, thumb = encoded & ~1, encoded & 1
        width = 2 if thumb else 4
        module = next((m for m in modules if m.base <= pc and pc + width <= m.base + m.code_size), None)
        if module is None:
            blocks[encoded] = [f'return unsupported({encoded:#x}, 0);']
            continue
        # This menu action launches the Symbian browser via AppArc. Keep the original
        # menu logic and descriptor, but hand off to the desktop browser instead.
        if app_profile and module is image and encoded == 0x6b49c and image.load(pc) == 0xe1a0c00d:
            blocks[encoded] = ['open_official_website(stack, r[1]);', 'target = r[14]; goto dispatch;']
            continue
        w = module.load(pc, width)
        next_pc = encoded + width
        predicate = None
        if not thumb and w >> 28 != 15 and w & 0x0e000000 != 0x0a000000:
            predicate = ARM_CONDITIONS[w >> 28]
            w = (w & 0x0fffffff) | 0xe0000000
        if thumb and w & 0xfe00 == 0xb400:
            mask = (w & 255) | ((w & 256) << 6)
            regs = [r for r in range(15) if mask & (1 << r)]
            ops = [f'r[13] -= {4 * len(regs)};']
            ops += [f'stack_write(stack, r[13] + {i * 4}, r[{r}]);' for i, r in enumerate(regs)]
            ops.append(jump(next_pc))
        elif thumb and w == 0x4778:  # BX PC interworking veneer
            ops = [jump((pc + 4) & ~3)]
        elif thumb and w & 0xfe00 == 0xbc00:
            regs = [r for r in range(8) if w & (1 << r)]
            ops = [f'r[{r}] = stack_read(stack, r[13] + {i * 4});' for i, r in enumerate(regs)]
            if w & 256:
                ops += [f'target = stack_read(stack, r[13] + {len(regs) * 4});']
            ops += [f'r[13] += {4 * (len(regs) + bool(w & 256))};', 'goto dispatch;' if w & 256 else jump(next_pc)]
        elif thumb and w & 0xf800 == 0xe000:
            ops = [jump(encoded + 4 + signed(w & 0x7ff, 11) * 2)]
        elif thumb and w & 0xf800 == 0x4800:
            literal = module.load(((pc + 4) & ~3) + (w & 255) * 4)
            ops = [f'r[{(w >> 8) & 7}] = UINT32_C({literal:#x});', jump(next_pc)]
        elif thumb and w & 0xf800 == 0x2000:
            imm = w & 255
            ops = [f'r[{(w >> 8) & 7}] = {imm}; z = {int(imm == 0)};', jump(next_pc)]
        elif thumb and w & 0xf800 == 0xf000:
            tail = module.load(pc + 2, 2)
            if tail & 0xf800 not in (0xe800, 0xf800):
                raise ValueError(f'unsupported Thumb call at {pc:#x}')
            target = pc + 4 + signed(w & 0x7ff, 11) * 4096 + (tail & 0x7ff) * 2
            target = (target | 1) if tail & 0x1000 else (target & ~3)
            ret = encoded + 4
            call_returns.add(ret)
            pending.append(ret)
            ops = [f'r[14] = {ret:#x};', jump(target)]
        elif thumb and w & 0xf800 == 0x2800:
            rn, imm = (w >> 8) & 7, w & 255
            carry = f'r[{rn}] >= {imm}' if imm else '1'
            ops = [f'z = r[{rn}] == {imm}; c = {carry};', jump(next_pc)]
        elif thumb and w & 0xff00 in (0xd000, 0xd100):
            condition = '!z' if w & 0x100 else 'z'
            target = encoded + 4 + signed(w & 255, 8) * 2
            ops = [f'if ({condition}) {{ {jump(target)} }}', jump(next_pc)]
        elif thumb and w & 0xf800 == 0x6800:
            rd, rn, offset = w & 7, (w >> 3) & 7, ((w >> 6) & 31) * 4
            ops = [f'r[{rd}] = stack_read(stack, r[{rn}] + {offset});', jump(next_pc)]
        elif thumb and w & 0xffc0 == 0:
            rd, rs = w & 7, (w >> 3) & 7
            ops = [f'r[{rd}] = r[{rs}]; z = r[{rd}] == 0;', jump(next_pc)]
        elif not thumb and w == 0xe51ff004 and pc + 4 in module.imports:
            dll, ordinal = module.imports[pc + 4]
            if (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1360):
                ops = ['r[0] = setup_thread_heap(stack, r[1]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('mediaclientaudiostream{000a0000}[10003996].dll', 3):
                ops = ['r[0] = audio_new(stack, r[0], r[1]);',
                       'if (!r[0]) return unhandled_leave((uint32_t)-4, r[14]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('drtaeabi{000a0000}.dll', 135):
                ops = ['if (!aeabi_idivmod(r)) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('drtaeabi{000a0000}.dll', 139), ('drtaeabi{000a0000}.dll', 155)):
                helper = 'aeabi_ldivmod' if ordinal == 139 else 'aeabi_uidivmod'
                ops = [f'if (!{helper}(r)) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('drtaeabi{000a0000}.dll', 204):
                ops = ['init_exception_globals(stack, r[0]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 585):
                ops = ['init_process();', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('apparc{000a0000}[10003a3d].dll', 171):
                ops = ['stack_write(stack, r[0], 0);', 'stack_write(stack, r[0] + 4, r[1]);',
                       'stack_write(stack, r[0] + 8, 0);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('eikcore{000a0000}[10004892].dll', 394):
                calls_factory = True
                ops = ['if (process_state != PROCESS_INITIALIZED || r[0] || !r[1] || r[2])',
                       f'    return missing_import({pc:#x}, {json.dumps(dll)}, {ordinal}, r);',
                       'start_application();', 'target = r[1]; r[14] = 0xff000010; goto dispatch;']
            elif (dll.lower(), ordinal) == ('eikcore{000a0000}[10004892].dll', 64):
                ops = ['construct_eik_application(stack, r[0]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) in (('scppnwdl{000a0000}.dll', 5), ('scppnwdl{000a0000}.dll', 7)):
                ops = ['r[0] = heap_try_alloc(r[0]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) in (('scppnwdl{000a0000}.dll', 1), ('scppnwdl{000a0000}.dll', 3)):
                ops = ['if (!heap_release(r[0])) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'fbscli{000a0000}[10003a15].dll' and ordinal in (31, 32):
                ops = ['if (!fbs_bitmap_construct(stack, r[0])) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'fbscli{000a0000}[10003a15].dll' and ordinal in (25, 33, 34, 35):
                ops = ['if (!fbs_bitmap_reset(r[0])) return unsupported(r[14], 0);']
                if ordinal == 33:
                    ops.append('if (!heap_release(r[0])) return unsupported(r[14], 0);')
                ops.append('target = r[14]; goto dispatch;')
            elif app_profile and (dll.lower(), ordinal) == ('bitgdi{000a0000}[10003b18].dll', 44):
                ops = ['if (!fbs_device_new(stack, r[0], &r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('bitgdi{000a0000}[10003b18].dll', 57):
                ops = ['if (!fbs_screen_device_new(stack, r[0], r[1], &r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('bitgdi{000a0000}[10003b18].dll', 132):
                ops = ['if (!fbs_bitgc_new(stack, &r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('bitgdi{000a0000}[10003b18].dll', 148):
                ops = ['guest_write(stack, r[0] + 4, r[1]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('fbscli{000a0000}[10003a15].dll', 108),
                                                             ('fbscli{000a0000}[10003a15].dll', 133)):
                ops = ['if (!fbs_data_address(r[0], &r[0])) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('fbscli{000a0000}[10003a15].dll', 117):
                ops = ['if (!fbs_size_in_pixels(stack, r[0], r[1])) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('fbscli{000a0000}[10003a15].dll', 14):
                ops = [f'if (!fbs_scanline(r[0], r[1], &r[0])) return missing_import({pc:#x}, {json.dumps(dll)}, {ordinal}, r);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('fbscli{000a0000}[10003a15].dll', 107):
                ops = ['if (!fbs_unlock_heap(r[0])) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 842):
                # TRect::TRect() initialises iTl(0,0) and iBr(0,0) (us_graph.cpp).
                ops = ['for (uint32_t i = 0; i < 4; i++) guest_write(stack, r[0] + 4 * i, 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 93):
                # RFile::Open(RFs&, const TDesC16&, TUint): this=r0, fs=r1, name=r2, mode=r3.
                ops = ['{ int rc = rfile_open(stack, r[0], r[1], r[2], r[3]);',
                       f'  if (rc == 1) return missing_import({pc:#x}, {json.dumps(dll)}, {ordinal}, r);',
                       '  r[0] = (uint32_t)rc; }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 300):
                ops = ['rfile_close(stack, r[0]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 96):
                ops = ['r[0] = (uint32_t)rfile_flush(stack, r[0]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 99):
                ops = ['r[0] = (uint32_t)rfile_write(stack, r[0], r[1], r[2]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 109):
                ops = ['r[0] = (uint32_t)rfile_set_size(stack, r[0], (int32_t)r[1]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('efsrv{000a0000}[100039e4].dll', 255),
                                                            ('efsrv{000a0000}[100039e4].dll', 257)):
                # RFile::Read(TDes8&) / Read(TDes8&, TInt) const.
                ops = [f'if (!rfile_read(stack, r[0], r[1], r[2], {int(ordinal == 255)})) return unsupported(r[14], 0);',
                       'r[0] = 0; target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 263):
                ops = ['if (!rfile_seek(stack, r[0], r[1], r[2])) return unsupported(r[14], 0);',
                       'r[0] = 0; target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 264):
                ops = ['if (!rfile_size(stack, r[0], r[1])) return unsupported(r[14], 0);',
                       'r[0] = 0; target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('ezlib{000a0000}[10005706].dll', 49),
                                                            ('ezlib{000a0000}[10005706].dll', 69)):
                # compress / uncompress(dest, &destLen, source, sourceLen) from ezlib2.def.
                ops = [f'if (!ezlib_oneshot(stack, r, {int(ordinal == 69)})) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('msgs{000a0000}[10004e66].dll', 254):
                # CMsvEntrySelection::CMsvEntrySelection(): CArrayFixFlat<TMsvId>(KMessageSelectionGranularity = 32).
                ops = ['{ uint32_t vtable = host_cbase_vtable(stack); if (!vtable) return unsupported(r[14], 0);',
                       '  array_fix_init(stack, r[0], vtable, 4, 32); }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('msgs{000a0000}[10004e66].dll', 34),
                                                            ('msgs{000a0000}[10004e66].dll', 18)):
                # CMsvSession::OpenSyncL / OpenAsyncL(MMsvSessionObserver&): this host has no message server, so the
                # session is a CBase object over an empty message store. OpenAsyncL's observer never receives
                # EMsvServerReady, as with a server that has not started. Methods are bound as the trace reaches them.
                ops = ['{ uint32_t vtable = host_cbase_vtable(stack), session = heap_try_alloc(64);',
                       '  if (!vtable || !session) return unsupported(r[14], 0);',
                       '  guest_write(stack, session, vtable); r[0] = session; }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('msgs{000a0000}[10004e66].dll', 249),
                                                            ('msgs{000a0000}[10004e66].dll', 250)):
                # CClientMtmRegistry::NewL(CMsvSession&, TTimeIntervalMicroSeconds32) and NewMtmL(TUid): the registry
                # is a host CBase object; an MTM is a host CBaseMtm whose messaging virtuals stop with a diagnostic.
                vtable = 'host_cbase_vtable' if ordinal == 249 else 'host_mtm_vtable'
                ops = [f'{{ uint32_t vtable = {vtable}(stack), object = heap_try_alloc(128);',
                       '  if (!vtable || !object) return unsupported(r[14], 0);',
                       '  guest_write(stack, object, vtable); r[0] = object; }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('efsrv{000a0000}[100039e4].dll', 68):
                ops = ['if (!rfs_connect(stack, r[0])) return unsupported(r[14], 0);', 'r[0] = 0;',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 831):
                ops = ['rect_set_rect(stack, r[0], r[1], r[2]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 674):
                ops = ['r[0] = user_tick_count();', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('hal{000a0000}[100039e8].dll', 1):
                # HAL::Get(ESystemTickPeriod, TInt&) only. The host timer resolves microseconds but the scheduler
                # clock is millisecond-based, so the tick is reported as 1000 us.
                ops = ['if (r[0] != 14) return unsupported(r[14], 0);', 'guest_write(stack, r[1], 1000); r[0] = 0;',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('fbscli{000a0000}[10003a15].dll', 56):
                ops = ['fbs_util_init(stack, r[0], r[1]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1955):
                ops = ['for (uint32_t i = 0; i < r[2]; i++) guest_write_small(stack, r[0] + i, r[1] & 0xff, 1);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'euser{000a0000}[100039e5].dll' and ordinal in EUSER_DESCRIPTOR_OPS:
                ops = [EUSER_DESCRIPTOR_OPS[ordinal], 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1954):
                # memmove(dest, src, n): copy direction follows the overlap; returns dest in r0.
                ops = ['if (r[0] < r[1]) { for (uint32_t i = 0; i < r[2]; i++)',
                       '    guest_write_small(stack, r[0] + i, guest_read_small(stack, r[1] + i, 1), 1); }',
                       'else { for (uint32_t i = r[2]; i-- > 0;)',
                       '    guest_write_small(stack, r[0] + i, guest_read_small(stack, r[1] + i, 1), 1); }',
                       'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1485):
                ops = ['guest_write_small(stack, r[0], r[1], 1);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('fbscli{000a0000}[10003a15].dll', 26):
                ops = ['if (!fbs_bitmap_create(stack, r[0], r[1], r[2])) return 3;', 'r[0] = 0;',
                       'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 2060):
                ops = ['r[0] = cleanup_mark();', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 196):
                ops = ['r[0] = trap_cleanup_new(stack);', 'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'euser{000a0000}[100039e5].dll' and ordinal in (425, 426, 428, 430, 433):
                if ordinal == 430:  # CActiveScheduler::CActiveScheduler()
                    ops = ['if (!worker_scheduler_construct(stack, r[0])) return unsupported(r[14], 0);']
                elif ordinal == 428:
                    ops = ['if (!worker_scheduler_install(stack, r[0])) return unsupported(r[14], 0);']
                elif ordinal == 426:
                    ops = ['if (!is_guest_thread(ctx) || !ctx->scheduler) return unsupported(r[14], 0);',
                           'ctx->scheduler_running = 1;',
                           'target = WORKER_SCHEDULER_STEP; goto dispatch;']
                elif ordinal == 425:
                    ops = ['if (!current_worker || !current_worker->scheduler_running) return unsupported(r[14], 0);',
                           'current_worker->scheduler_running = 0;']
                else:
                    ops = ['if (!worker_scheduler_destroy(stack, r[0])) return unsupported(r[14], 0);']
                if ordinal != 426:
                    ops += ['target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'euser{000a0000}[100039e5].dll' and ordinal in (199, 200, 201):
                ops = ['if (!trap_cleanup_destroy(stack, r[0])) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 2061):
                ops = ['cleanup_unmark(r[0]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) in (('euser{000a0000}[100039e5].dll', 651), ('euser{000a0000}[100039e5].dll', 654)):
                ops = ['r[0] = heap_alloc_zeroed(r[0]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 85):
                ops = ['guest_write(stack, r[0], 0x30000000u);',  # TBuf16: EBuf, length zero
                       'guest_write(stack, r[0] + 4, r[1]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 960):
                ops = ['if (guest_read(stack, r[0]) >> 28 != 3) return unsupported(r[14], 0);',
                       'guest_write(stack, r[0], 0x30000000u);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1091):
                ops = ['guest_write(stack, r[0] + 20, r[1]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 424):
                ops = ['if (!add_active(stack, r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 925):
                ops = ['if (!create_timer(stack, r[0])) return 3;', 'r[0] = 0; target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 928):
                ops = ['if (!schedule_timer(stack, r[0], r[1], r[2])) return 3;', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1090):
                ops = ['if (!set_active(stack, r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 634):
                ops = ['if (clock_gettime(CLOCK_MONOTONIC, &framework.last_activity) != 0) return 3;',
                       'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1844):
                ops = ['if (guest_read(stack, r[0]) != 0xffff8000u) return unsupported(r[14], 0);',
                       'framework.process_priority = r[1]; target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1027):
                ops = ['if (r[2] > r[3] || r[3] > 0x0fffffffu) return 3;',
                       'guest_write(stack, r[0], 0x20000000u | r[2]);',
                       'guest_write(stack, r[0] + 4, r[3]); guest_write(stack, r[0] + 8, r[1]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1169):
                ops = ['for (address = 0; guest_read_small(stack, r[1] + address * 2, 2); address++) {}',
                       'guest_write(stack, r[0], 0x10000000u | address); guest_write(stack, r[0] + 4, r[1]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 953):
                ops = ['if (!copy_text(stack, r[0], r[1])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'estlib{000a0000}[10003b0b].dll' and ordinal in (131, 140, 144, 148, 152):
                ops = [f'if (!game_stdio(stack, r, {ordinal})) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'estor{000a0000}[10003b0d].dll' and ordinal in ESTOR_OPS:
                ops = [ESTOR_OPS[ordinal], 'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'estlib{000a0000}[10003b0b].dll' and ordinal in ESTLIB_OPS:
                ops = [ESTLIB_OPS[ordinal], 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('gdi{000a0000}[10003b15].dll', 311),
                                                           ('gdi{000a0000}[10003b15].dll', 314)):
                arg = 'r[1]' if ordinal == 314 else '0'
                ops = [f'if (!font_measure(stack, r[0], {arg}, &r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1379):
                ops = ['r[0] = new_periodic(stack, r[0]);',
                       'if (!r[0]) return 3;', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1381):
                ops = ['if (!start_periodic(stack, r[0], r[1], r[2], r[3], guest_read(stack, r[13]))) return 3;',
                       'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 593):
                ops = ['if (r[0] & 0x80000000u) return unhandled_leave(r[0], r[14]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 548):
                # Math::Rand(TInt64&): SDK um_rand.cpp, with 64-bit wraparound.
                ops = ['{ const uint64_t seed = guest_read64(stack, r[0]) * 214013u + 2531011u;',
                       '  guest_write64(stack, r[0], seed); r[0] = (seed >> 16) & 0x7fffffffu; }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 649):
                ops = ['return unhandled_leave(r[0], r[14]);']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 665):
                ops = ['r[0] = user_allocator(stack);',
                       'if (!r[0]) return unhandled_leave((uint32_t)-4, r[14]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 859):
                ops = ['if (!home_time(stack, r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 914):
                ops = [f'if (!debug_print(stack, r)) return missing_import({pc:#x}, {json.dumps(dll)}, {ordinal}, r);',
                       'r[0] = 0; target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1677):
                ops = ['guest_write64(stack, r[0], guest_read64(stack, r[1]) - ((uint64_t)r[3] << 32 | r[2]));',
                       'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1679):
                # TTime::DateTime() const: hidden result pointer in r0, this in r1.
                ops = ['time_date_time(stack, r[0], r[1]);', 'target = r[14]; goto dispatch;']
            elif game_dlls and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1308):
                ops = ['target = load_library(stack, r[0], r[1], r[14]);',
                       'if (!target) return 2;',
                       'if (target == r[14]) r[0] = 0;',
                       'else { r[0] = 2; r[14] = 0xff000028; }', 'goto dispatch;']
            elif game_dlls and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1838):
                ops = ['r[0] = lookup_export(stack, r[0], r[1]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 532):
                ops = ['r[0] = (uint32_t)math_unary(stack, r[0], r[1], cos);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 544):
                ops = ['r[0] = (uint32_t)math_unary(stack, r[0], r[1], atan);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 540):
                ops = ['r[0] = (uint32_t)math_unary(stack, r[0], r[1], sin);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 549):
                ops = ['r[0] = (uint32_t)math_unary_domain(stack, r[0], r[1], sqrt, 0.0, INFINITY);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 542):
                ops = ['r[0] = (uint32_t)math_unary_domain(stack, r[0], r[1], acos, -1.0, 1.0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'dfpaeabi{000a0000}.dll' and ordinal in DFPAEABI_OPS:
                ops = [DFPAEABI_OPS[ordinal], 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 968):
                ops = ['if (!desc_append(stack, r[0], r[1])) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 2106):
                ops = ['if (!desc_left(stack, r[0], r[1], r[2])) return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('avkon{000a0000}[100056c6].dll', 14):
                ops = ['r[0] = (uint32_t)complete_with_app_path(stack, r[0]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('avkon{000a0000}[100056c6].dll', 131):
                ops = ['construct_akn_document(stack, r[0], r[1]);', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) == ('avkon{000a0000}[100056c6].dll', 218):
                ops = ['construct_akn_appui(stack, r[0]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('avkon{000a0000}[100056c6].dll', 2924):
                ops = ['if (!base_construct_appui(stack, r[0], r[1])) return 2;',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 65):
                ops = ['if (!construct_control(stack, r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 21):
                ops = ['if (r[0] != framework.control || r[1] != framework.appui + 40) return 3;',
                       'guest_write(stack, r[0] + 48, r[1]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 50):
                ops = ['if (r[0] != framework.control || guest_read(stack, r[0] + 40)) return 3;',
                       'set_extent(stack, r[0], framework.screen_rect);',
                       'target = guest_read(stack, guest_read(stack, r[0]) + 0x6c); goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 25):
                ops = ['if (!create_control_window(stack, r[0])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 57):
                ops = ['if (r[0] != framework.control || !framework.window) return 3;',
                       'for (unsigned i = 0; i < 4; i++) framework.window_rect[i] = guest_read(stack, r[1] + i * 4);',
                       'set_extent(stack, r[0], framework.window_rect);',
                       'target = guest_read(stack, guest_read(stack, r[0]) + 0x6c); goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 165):
                ops = ['if (r[0] != framework.coe_env || framework.foreground_observer || !r[1]) return 3;',
                       'framework.foreground_observer = r[1]; target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('ws32{000a0000}[10003b21].dll', 132):
                ops = ['r[0] = capture_key(r[0], r[1], r[2], r[3]);',
                       'if (!r[0]) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 202):
                ops = ['if (r[0] != framework.appui || r[1] != framework.control || r[2] || r[3] ||',
                       '    guest_read(stack, framework.control_stack)) return 3;',
                       'guest_write(stack, framework.control_stack, r[1]); target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('avkon{000a0000}[100056c6].dll', 2927):
                ops = ['if (r[0] != framework.appui || r[1] != 1) return 3;',
                       'framework.key_block_mode = r[1]; target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 182):
                ops = ['r[0] = framework.coe_env;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('gdi{000a0000}[10003b15].dll', 312):
                ops = ['if (!font_spec_in_twips(stack, r[0], r[1])) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 153):
                ops = ['if (r[0] != framework.coe_env) return 3;',
                       'r[0] = screen_font(stack, r[1]);', 'if (!r[0]) return 3;', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('cone{000a0000}[10003a41].dll', 279),
                                                          ('cone{000a0000}[10003a41].dll', 287)):
                field = 'window' if ordinal == 279 else 'system_gc'
                ops = [f'if (r[0] != framework.control || !framework.{field}) return 3;',
                       f'r[0] = framework.{field};', 'target = r[14]; goto dispatch;']
            elif (dll.lower(), ordinal) in (('euser{000a0000}[100039e5].dll', 1649),
                                            ('euser{000a0000}[100039e5].dll', 1651)):
                axis = 0 if ordinal == 1649 else 4
                ops = [f'r[0] = guest_read(stack, r[0] + {axis + 8}) - guest_read(stack, r[0] + {axis});',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 660):
                # MCoeForegroundObserver holds only the vptr overwritten by the derived ctor.
                ops = ['target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('cone{000a0000}[10003a41].dll', 664):
                # This secondary base has no state other than its vptr,
                # which the derived constructor replaces before its first use.
                ops = ['target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('euser{000a0000}[100039e5].dll', 208),
                                                          ('euser{000a0000}[100039e5].dll', 205)):
                ops = [f'{"cleanup_push(r[0])" if ordinal == 208 else "cleanup_pop()"};',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 207):
                # CleanupStack::PushL(TCleanupItem): the item travels by value as operation r0, pointer r1.
                ops = ['cleanup_push_item(r[1], r[0], CLEANUP_ITEM);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 209):
                ops = ['cleanup_push_item(r[0], 0, CLEANUP_ANY);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 204):
                ops = ['cleanup_pop_count(r[0]);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('euser{000a0000}[100039e5].dll', 202),
                                                            ('euser{000a0000}[100039e5].dll', 203)):
                count = 'r[0]' if ordinal == 202 else '1'
                ops = [f'if (!cleanup_pop_and_destroy(stack, {count}, r[13])) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'euser{000a0000}[100039e5].dll' and ordinal in (1158, 1159):
                # RThread::Create; r0 is the RThread object receiving the handle. 1158 takes
                # RAllocator* at [sp], 1159 heap sizes at [sp]/[sp+4]; the arg follows.
                arg_slot, owner_slot = (4, 8) if ordinal == 1158 else (8, 12)
                ops = [f'r[0] = (uint32_t)thread_create(stack, r[1], r[2], r[3], '
                       f'stack_read(stack, r[13]), stack_read(stack, r[13] + {arg_slot}), '
                       f'stack_read(stack, r[13] + {owner_slot}), r[0]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1795):
                ops = ['(void)thread_resume(stack, guest_read(stack, r[0]));',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1790):
                ops = ['if (!thread_request_complete(stack, guest_read(stack, r[0]), r[1], r[2]))',
                       '    return unsupported(r[14], 0);', 'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1154):
                ops = ['r[0] = (uint32_t)thread_kill(stack, guest_read(stack, r[0]));',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 120):
                ops = ['{ const uint32_t handle = guest_read(stack, r[0]);',
                       '  if (handle == ETEL_EMPTY_SESSION || sem_close(handle) || thread_close(stack, handle) == 0)',
                       '      guest_write(stack, r[0], 0); }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 64):
                # RSemaphore::CreateLocal(TInt, TOwnerType): r0=this, r1=count.
                ops = ['r[0] = (uint32_t)sem_create_local(stack, r[0], (int32_t)r[1]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 70):
                # RSemaphore::Signal(TInt): r0=this, r1=count.
                ops = ['(void)sem_signal(guest_read(stack, r[0]), (int32_t)r[1]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 71):
                # RSemaphore::Signal(): r0=this.
                ops = ['(void)sem_signal(guest_read(stack, r[0]), 1);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'euser{000a0000}[100039e5].dll' and ordinal in (69, 922):
                indirect_targets.add(pc)
                ops = ['{ int status = sem_wait(stack, ctx, guest_read(stack, r[0]), 1);',
                       f'   if (status == THREAD_YIELD) {{ target = {pc:#x}; return status; }}',
                       f'   if (status == SEM_BAD_WAIT) return missing_import({pc:#x}, {json.dumps(dll)}, {ordinal}, r);',
                       '   if (status) return status; }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 918):
                # RMutex::CreateLocal(TOwnerType): r0=this.
                ops = ['r[0] = (uint32_t)mutex_create_local(stack, r[0]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 923):
                # RMutex::Signal(): r0=this.
                ops = ['(void)sem_signal(guest_read(stack, r[0]), 1);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 508):
                # CActiveSchedulerWait::CActiveSchedulerWait(): empty body in euser/cbase/ub_act.cpp.
                # The object is CBase (vptr +0) with iLoop at +4; the euser vtable is not part of this
                # process, so the vptr stays zero and a virtual destroy faults with a diagnostic.
                # iLoop is zeroed by the CBase zero-initialising allocator.
                ops = ['target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 815):
                # TPtr8::TPtr8(TUint8 *aBuf, TInt aMaxLength): empty EPtr descriptor over aBuf.
                ops = ['guest_write(stack, r[0], UINT32_C(2) << 28);',
                       'guest_write(stack, r[0] + 4, r[2]);',
                       'guest_write(stack, r[0] + 8, r[1]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 819):
                # TPtr8::TPtr8(TUint8 *aBuf, TInt aLength, TInt aMaxLength): this=r0, buf=r1, len=r2, max=r3.
                # Layout (e32des8.h): iLength@0 holds EPtr (2) << KShiftDesType(28) | length;
                # iMaxLength@4; iPtr@8 (TPtr8 member).
                ops = ['guest_write(stack, r[0], (UINT32_C(2) << 28) | ((uint32_t)r[2] & 0xfffffff));',
                       'guest_write(stack, r[0] + 4, r[3]);',
                       'guest_write(stack, r[0] + 8, r[1]);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('etel3rdparty{000a0000}[10004268].dll', 55):
                # CTelephony::GetPhoneId(TRequestStatus&, TDes8&) const: no phone backend on this host,
                # so the request completes with KErrNotSupported (-5) when the scheduler next runs,
                # after the caller's SetActive(). TRequestStatus::iStatus is at offset 0.
                ops = ['if (!defer_completion(r[1], -5)) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 505):
                # CActiveSchedulerWait::Start(): this->iLoop (+4) is nonzero while the loop runs.
                # Each dispatched RunL runs nested below the caller's sp until AsyncStop clears iLoop.
                ops = ['if (guest_read(stack, r[0] + 4)) return unsupported(r[14], 0);',
                       'guest_write(stack, r[0] + 4, 1);',
                       'while (guest_read(stack, r[0] + 4)) {',
                       '    uint32_t argument = 0, event = 0;',
                       '    if (!next_timer_event(stack, &argument, &event)) return unsupported(r[14], 0);',
                       '    if (!run_nested_event(stack, event, argument, r[13])) return unsupported(r[14], 0);',
                       '}',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 947):
                # TDes16::Num(TInt64): this=r0, the EABI 64-bit argument is aligned to r2:r3 (low word first).
                ops = ['if (!desc_num(stack, r[0], (int64_t)((uint64_t)r[3] << 32 | r[2]))) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1807):
                # TDesC16::Ptr() const: address of the first character for any descriptor type.
                ops = ['{ uint32_t length; r[0] = text_data(stack, r[0], &length); }',
                       'if (!r[0]) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1171):
                # TPtrC16::TPtrC16(const TDesC16&): iLength@0 = EPtrC (1) << 28 | length, iPtr@4; returns this.
                ops = ['{ uint32_t length, text = text_data(stack, r[1], &length);',
                       '  if (!text) return unsupported(r[14], 0);',
                       '  guest_write(stack, r[0], UINT32_C(0x10000000) | length);',
                       '  guest_write(stack, r[0] + 4, text); }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1088):
                # CActive::Cancel().
                ops = ['if (!cancel_active(stack, r[0], r[13])) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('euser{000a0000}[100039e5].dll', 1093),
                                                            ('euser{000a0000}[100039e5].dll', 1094)):
                # CActive::~CActive() complete/base object destructors; r0 is returned as this.
                ops = ['if (!remove_active(stack, r[0])) return unsupported(r[14], 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 511):
                # CActiveSchedulerWait::~CActiveSchedulerWait(): AsyncStop() if still started (iLoop at +4).
                ops = ['guest_write(stack, r[0] + 4, 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 507):
                # CActiveSchedulerWait::AsyncStop(): clears iLoop (+4) so the owning Start() returns.
                ops = ['guest_write(stack, r[0] + 4, 0);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and dll.lower() == 'etel{000a0000}[10003d46].dll' and ordinal in (29, 30, 167):
                # RTelServer is an RSessionBase. This host has an empty inventory, not an emulated handset.
                if ordinal == 30:  # RTelServer::RTelServer()
                    ops = ['guest_write(stack, r[0], 0);']
                elif ordinal == 29:  # RTelServer::Connect(TInt)
                    ops = ['if ((int32_t)r[1] < -1) r[0] = (uint32_t)-6;',
                           'else if (guest_read(stack, r[0])) r[0] = (uint32_t)-11;',
                           'else { guest_write(stack, r[0], ETEL_EMPTY_SESSION); r[0] = 0; }']
                else:  # RTelServer::EnumeratePhones(TInt&)
                    ops = ['if (guest_read(stack, r[0]) != ETEL_EMPTY_SESSION) r[0] = (uint32_t)-8;',
                           'else { guest_write(stack, r[1], 0); r[0] = 0; }']
                ops += ['target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('etel3rdparty{000a0000}[10004268].dll', 53):
                # CTelephony::NewL(): this PC host has no cellular modem. The object exists with a
                # null iTelephonyFunctions (CBase vptr + one pointer). Its methods must report
                # "no telephony" explicitly; they are bound one by one as the trace reaches them.
                # Its vptr points at host destructor slots (D1 at +0, deleting D0 at +4).
                ops = ['r[0] = heap_alloc_zeroed(8);',
                       '{ uint32_t vtable = heap_alloc_zeroed(8);',
                       '  guest_write(stack, vtable, TELEPHONY_BASE);',
                       '  guest_write(stack, vtable + 4, TELEPHONY_BASE + 4);',
                       '  guest_write(stack, r[0], vtable); }',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('etel3rdparty{000a0000}[10004268].dll', 1):
                # CTelephony::TPhoneIdV1::TPhoneIdV1(): the base TEtelISVType constructor sets
                # iVersionId (offset 0) to KEtelISVV1 = 1. Per the agreed scope, nothing else is written.
                ops = ['guest_write(stack, r[0], 1);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 1786):
                # RThread::SetPriority: scheduling priority is not modelled, but the handle must exist.
                ops = ['r[0] = (uint32_t)(guest_thread_find(guest_read(stack, r[0])) ? 0 : -8);',
                       'target = r[14]; goto dispatch;']
            elif app_profile and (dll.lower(), ordinal) in (('euser{000a0000}[100039e5].dll', 1783),
                                                          ('euser{000a0000}[100039e5].dll', 1794)):
                # RThread Rendezvous/Logon: blocking waits yield to the scheduler at the caller's LR.
                ops = ['target = r[14];', 'return THREAD_YIELD;']
            elif app_profile and (dll.lower(), ordinal) == ('euser{000a0000}[100039e5].dll', 645):
                # User::After(TTimeIntervalMicroSeconds32): a guest thread yields to the scheduler, which runs it
                # again on a later pass; the main thread has nothing else runnable and sleeps on the host.
                ops = ['if (is_guest_thread(ctx)) { target = r[14]; return THREAD_YIELD; }',
                       '{ struct timespec delay = { (time_t)(r[0] / 1000000), (long)(r[0] % 1000000) * 1000 };',
                       '  if ((int32_t)r[0] > 0) nanosleep(&delay, NULL); }',
                       'target = r[14]; goto dispatch;']
            else:
                ops = [f'return missing_import({pc:#x}, {json.dumps(dll)}, {ordinal}, r);']
        elif not thumb and w & 0xffff0000 == 0xe92d0000:
            regs = [r for r in range(16) if w & (1 << r)]
            values = [f'r[{r}]' if r < 15 else f'{pc + 12:#x}' for r in regs]
            ops = [f'r[13] -= {4 * len(regs)};']
            ops += [f'stack_write(stack, r[13] + {i * 4}, {v});' for i, v in enumerate(values)]
            ops.append(jump(next_pc))
        elif not thumb and w & 0x0fff0000 == 0x08bd0000 and w >> 28 in (0, 14) and not w & (1 << 13):
            regs = [r for r in range(16) if w & (1 << r)]
            ops = [f'{"target" if r == 15 else f"r[{r}]"} = stack_read(stack, r[13] + {i * 4});' for i, r in enumerate(regs)]
            ops += [f'r[13] += {len(regs) * 4};', 'goto dispatch;' if 15 in regs else jump(next_pc)]
            if w >> 28 == 0:
                ops = ['if (z) {', *ops, '}', jump(next_pc)]
        elif not thumb and w & 0x0e400000 == 0x08000000 and w & 0xffff and (w >> 16) & 15 != 15:
            # LDM/STM with all four addressing modes; S-bit (user-bank/exception-return) forms are not handled.
            load, writeback, up, before = bool(w & (1 << 20)), bool(w & (1 << 21)), bool(w & (1 << 23)), bool(w & (1 << 24))
            rn = (w >> 16) & 15
            regs = [r for r in range(16) if w & (1 << r)]
            size = 4 * len(regs)
            base = (4 if before else 0) if up else -size + (0 if before else 4)
            if writeback and w & (1 << rn) and load:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                ops = [f'address = r[{rn}] + UINT32_C({base & 0xffffffff:#x});']
                for i, r in enumerate(regs):
                    if load:
                        ops.append(f'{"target" if r == 15 else f"r[{r}]"} = guest_read(stack, address + {4 * i});')
                    else:
                        ops.append(f'guest_write(stack, address + {4 * i}, {f"{pc + 12:#x}" if r == 15 else f"r[{r}]"});')
                if writeback:
                    ops.append(f'r[{rn}] = r[{rn}] + UINT32_C({(size if up else -size) & 0xffffffff:#x});')
                ops.append('goto dispatch;' if load and 15 in regs else jump(next_pc))
        elif not thumb and w == 0xe12fff1e:
            ops = ['target = r[14]; goto dispatch;']
        elif not thumb and w & 0xfffffff0 == 0xe12fff30 and w & 15 != 15:
            call_returns.add(next_pc)
            pending.append(next_pc)
            ops = [f'target = r[{w & 15}]; r[14] = {next_pc:#x}; goto dispatch;']
        elif not thumb and w & 0x0e000000 == 0x0a000000 and w >> 28 != 15:
            target = pc + 8 + signed(w & 0xffffff, 24) * 4
            predicate = ARM_CONDITIONS[w >> 28]
            link = bool(w & 0x01000000)
            if link:
                call_returns.add(next_pc)
                pending.append(next_pc)
            transfer = (f'r[14] = {next_pc:#x}; ' if link else '') + jump(target)
            ops = [transfer] if predicate is None else [f'if ({predicate}) {{ {transfer} }}', jump(next_pc)]
        elif not thumb and w & 0xfe100000 == 0xe2000000 and (w >> 21) & 15 in DP_NO_FLAGS_OPCODES:
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            value = arm_immediate(w)
            if rd == 15:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                left = f'UINT32_C({pc + 8:#x})' if rn == 15 else None
                ops = [f'r[{rd}] = {dp_expr((w >> 21) & 15, rn, f"UINT32_C({value})", left)};', jump(next_pc)]
        elif not thumb and w & 0xffff0000 == 0xe3a00000 and (w >> 12) & 15 != 15:
            value = arm_immediate(w)
            ops = [f'r[{(w >> 12) & 15}] = UINT32_C({value});', jump(next_pc)]
        elif not thumb and w & 0xffff0000 == 0xe3e00000 and (w >> 12) & 15 != 15:
            ops = [f'r[{(w >> 12) & 15}] = ~UINT32_C({arm_immediate(w)});', jump(next_pc)]
        elif not thumb and w & 0x0ff0f090 == 0x01500010 \
                and 15 not in ((w >> 16) & 15, (w >> 8) & 15, w & 15) and (w >> 5) & 3 != 3:
            rn, rs, rm, kind = (w >> 16) & 15, (w >> 8) & 15, w & 15, (w >> 5) & 3
            ops = [f'a = r[{rn}]; b = arm_shift(r[{rm}], {kind}, r[{rs}]); result = a - b;',
                   'z = result == 0; n = result >> 31; c = a >= b; v = ((a ^ b) & (a ^ result)) >> 31;', jump(next_pc)]
        elif not thumb and ((w & 0xfe000000 == 0xe2000000) or
                            (w & 0xfe000010 == 0xe0000000 and (w >> 5) & 3 != 3)) \
                and ((w >> 21) & 15) in (2, 3, 4, 5, 6, 7, 10, 11):
            opcode, rn, rd = (w >> 21) & 15, (w >> 16) & 15, (w >> 12) & 15
            subtract = opcode in (2, 3, 6, 7, 10)
            immediate = bool(w & 0x02000000)
            left = f'UINT32_C({pc + 8:#x})' if rn == 15 else f'r[{rn}]'
            right = f'UINT32_C({arm_immediate(w)})' if immediate else (
                f'UINT32_C({pc + 8:#x})' if w & 15 == 15 else f'r[{w & 15}]')
            if not immediate:
                kind, amount = (w >> 5) & 3, (w >> 7) & 31
                if kind or amount:
                    right = f'arm_shift({right}, {kind}, {amount or 32})'
            if opcode in (3, 7):
                left, right = right, left
            extra = 'c' if opcode == 5 else '1 - c' if opcode in (6, 7) else '0'
            if (opcode in (10, 11) and (not w & (1 << 20) or rd)) or rd == 15:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                ops = [f'a = {left}; b = {right};', f'wide = (uint64_t)a {"-" if subtract else "+"} b {"-" if subtract else "+"} ({extra});',
                       'result = (uint32_t)wide;']
                if w & (1 << 20):
                    overflow = '(a ^ b)' if subtract else '~(a ^ b)'
                    carry = '(wide >> 32) == 0' if subtract else '(wide >> 32) != 0'
                    ops.append(f'z = result == 0; n = result >> 31; c = {carry}; v = ({overflow} & (a ^ result)) >> 31;')
                if opcode not in (10, 11):
                    ops.append(f'r[{rd}] = result;')
                ops.append(jump(next_pc))
        elif not thumb and w & 0xfff00ff0 == 0xe0600000 and 15 not in ((w >> 16) & 15, (w >> 12) & 15, w & 15):
            rn, rd, rm = (w >> 16) & 15, (w >> 12) & 15, w & 15
            ops = [f'r[{rd}] = r[{rm}] - r[{rn}];', jump(next_pc)]
        elif not thumb and w & 0x0f8000f0 == 0x00800090 and not w & (1 << 20) \
                and 15 not in ((w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15) and (w >> 16) & 15 != (w >> 12) & 15:
            hi, lo, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
            if w & (1 << 22):
                product = f'(uint64_t)((int64_t)(int32_t)r[{rm}] * (int64_t)(int32_t)r[{rs}])'  # SMULL/SMLAL
            else:
                product = f'(uint64_t)r[{rm}] * r[{rs}]'  # UMULL/UMLAL
            if w & (1 << 21):
                product = f'{product} + (((uint64_t)r[{hi}] << 32) | r[{lo}])'
            ops = [f'{{ uint64_t product = {product}; r[{lo}] = (uint32_t)product; r[{hi}] = (uint32_t)(product >> 32); }}',
                   jump(next_pc)]
        elif not thumb and w & 0xffe0f0f0 == 0xe0000090 and 15 not in ((w >> 16) & 15, (w >> 8) & 15, w & 15):
            rd, rs, rm = (w >> 16) & 15, (w >> 8) & 15, w & 15
            ops = [f'r[{rd}] = r[{rm}] * r[{rs}];']
            if w & (1 << 20):
                ops.append(f'z = r[{rd}] == 0; n = r[{rd}] >> 31;')
            ops.append(jump(next_pc))
        elif not thumb and w & 0xfff000f0 == 0xe0200090 and 15 not in ((w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15):
            rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15  # MLA Rd = Rm * Rs + Rn
            ops = [f'r[{rd}] = r[{rm}] * r[{rs}] + r[{rn}];', jump(next_pc)]
        elif not thumb and w & 0x0e100090 == 0x00100010 and (w >> 21) & 15 in (0, 1, 8, 9, 12, 13, 14, 15) \
                and (w >> 5) & 3 != 3 and 15 not in ((w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15):
            opcode, rn, rd, rs, rm, kind = (w >> 21) & 15, (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15, (w >> 5) & 3
            if opcode in (8, 9) and rd != 0:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                carry = ('b <= 32 ? (a >> (32 - b)) & 1 : 0',
                         'b <= 32 ? (a >> (b - 1)) & 1 : 0',
                         '(a >> (b >= 32 ? 31 : b - 1)) & 1')[kind]
                expr = dp_expr({8: 0, 9: 1}.get(opcode, opcode), rn, f'arm_shift(a, {kind}, b)')
                ops = [f'a = r[{rm}]; b = r[{rs}] & 255;', f'if (b) c = {carry};',
                       f'result = {expr}; z = result == 0; n = result >> 31;']
                if opcode not in (8, 9):
                    ops.append(f'r[{rd}] = result;')
                ops.append(jump(next_pc))
        elif not thumb and w & 0x0e100ff0 == 0x00100000 and (w >> 21) & 15 in (0, 1, 8, 9, 12, 13, 14, 15) \
                and 15 not in ((w >> 16) & 15, (w >> 12) & 15, w & 15):
            opcode, rn, rd, rm = (w >> 21) & 15, (w >> 16) & 15, (w >> 12) & 15, w & 15
            if opcode in (8, 9) and rd != 0:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                expr = dp_expr({8: 0, 9: 1}.get(opcode, opcode), rn, f'r[{rm}]')
                ops = [f'result = {expr}; z = result == 0; n = result >> 31;']
                if opcode not in (8, 9):
                    ops.append(f'r[{rd}] = result;')
                ops.append(jump(next_pc))
        elif not thumb and w & 0xfff0f000 == 0xe3100000 and (w >> 16) & 15 != 15:
            imm, rn = arm_immediate(w), (w >> 16) & 15
            ops = [f'result = r[{rn}] & UINT32_C({imm}); z = result == 0; n = result >> 31;']
            if w & 0xf00:
                ops.append(f'c = {imm >> 31};')
            ops.append(jump(next_pc))
        elif not thumb and w & 0xfff00000 == 0xe2100000 and 15 not in ((w >> 16) & 15, (w >> 12) & 15):
            imm, rn, rd = arm_immediate(w), (w >> 16) & 15, (w >> 12) & 15
            ops = [f'r[{rd}] = r[{rn}] & UINT32_C({imm}); z = r[{rd}] == 0; n = r[{rd}] >> 31;']
            if w & 0xf00:
                ops.append(f'c = {imm >> 31};')
            ops.append(jump(next_pc))
        elif not thumb and w & 0x0fff0010 == 0x01b00000 and (w >> 5) & 3 != 3 \
                and 15 not in ((w >> 12) & 15, w & 15):
            rd, rm, kind, amount = (w >> 12) & 15, w & 15, (w >> 5) & 3, (w >> 7) & 31
            amount = amount or (32 if kind else 0)
            ops = [f'a = r[{rm}];']
            if amount:
                bit = 32 - amount if kind == 0 else amount - 1
                ops.append(f'c = (a >> {bit}) & 1;')
            ops += [f'r[{rd}] = arm_shift(a, {kind}, {amount}); z = r[{rd}] == 0; n = r[{rd}] >> 31;', jump(next_pc)]
        elif not thumb and w & 0x0e000090 == 0x00000010 and not w & (1 << 20) and (w >> 21) & 15 in DP_NO_FLAGS_OPCODES \
                and (w >> 16) & 15 != 15 and (w >> 12) & 15 != 15 and (w >> 8) & 15 != 15 and w & 15 != 15 \
                and (w >> 5) & 3 != 3:
            opcode, rn, rd, rm, rs, kind = (w >> 21) & 15, (w >> 16) & 15, (w >> 12) & 15, w & 15, (w >> 8) & 15, (w >> 5) & 3
            ops = [f'r[{rd}] = {dp_expr(opcode, rn, f"arm_shift(r[{rm}], {kind}, r[{rs}])")};', jump(next_pc)]
        elif not thumb and w & 0x0e000010 == 0 and not w & (1 << 20) and (w >> 21) & 15 in DP_NO_FLAGS_OPCODES \
                and (w >> 16) & 15 != 15 and (w >> 12) & 15 != 15 and w & 15 != 15 and (w >> 5) & 3 != 3:
            opcode, rn, rd, rm, kind, amount = (w >> 21) & 15, (w >> 16) & 15, (w >> 12) & 15, w & 15, (w >> 5) & 3, (w >> 7) & 31
            if kind == 0:
                shifted = f'r[{rm}] << {amount}' if amount else f'r[{rm}]'
            elif kind == 1:
                shifted = f'r[{rm}] >> {amount}' if amount else '0u'  # LSR #32 clears the register
            else:
                shifted = f'(uint32_t)((int32_t)r[{rm}] >> {amount or 31})'  # ASR #32 sign-fills
            ops = [f'r[{rd}] = {dp_expr(opcode, rn, shifted)};', jump(next_pc)]
        elif not thumb and w & 0x0fff0ff0 == 0x01a00000 and (w >> 12) & 15 != 15:
            source = f'UINT32_C({pc + 8:#x})' if w & 15 == 15 else f'r[{w & 15}]'
            ops = [f'r[{(w >> 12) & 15}] = {source};', jump(next_pc)]
        elif not thumb and w == 0xe31f0000:
            # TST #0 sets Z; the following CMP overwrites Z and C before use.
            ops = ['z = 1;', jump(next_pc)]
        elif not thumb and w & 0xffff0070 == 0xe1a00040 and 15 not in ((w >> 12) & 15, w & 15):
            rd, rm, shift = (w >> 12) & 15, w & 15, (w >> 7) & 31
            # ASR #0 encodes a shift by 32; use unsigned operations.
            src = f'r[{rm}]'
            sign = f'(0u - ({src} >> 31))'
            value = f'(({src} >> {shift}) | ({sign} << {32 - shift}))' if shift else sign
            ops = [f'r[{rd}] = {value};', jump(next_pc)]
        elif not thumb and w & 0xfffffff0 == 0xe79ff100 and predicate == ARM_CONDITIONS[9] and w & 15 != 15:
            # CMP Rm,#limit; LDRLS pc,[pc,Rm,LSL #2]; B default; relocated table.
            rm = w & 15
            compare = module.load(pc - 4)
            count = arm_immediate(compare) + 1
            table = pc + 8
            if (compare & 0xfffff000 == (0xe3500000 | rm << 16)
                    and module.load(pc + 4) >> 24 == 0xea
                    and table + count * 4 <= module.base + module.code_size
                    and all(slot in module.relocated for slot in range(table, table + count * 4, 4))):
                cases = {}
                for index in range(count):
                    cases.setdefault(module.load(table + 4 * index), []).append(index)
                ops = [f'switch (r[{rm}]) {{']
                for dest, indices in cases.items():
                    ops.append(' '.join(f'case {index}:' for index in indices) + ' ' + jump(dest))
                ops.extend([f'default: return unsupported({encoded:#x}, {w:#x});', '}'])
            else:
                ops = [f'return unsupported({encoded:#x}, {w:#x});']
        elif not thumb and w == 0xe51ff004 and pc + 4 in module.relocated:
            ops = [jump(module.load(pc + 4))]
        elif not thumb and w & 0xff7f0000 == 0xe51f0000 and (w >> 12) & 15 != 15:
            offset = w & 0xfff
            literal = module.load(pc + 8 + (offset if w & (1 << 23) else -offset))
            rd = (w >> 12) & 15
            # The function-form application factory is passed in r1 immediately
            # before the constructor call. Seed that callback at build time.
            call_insn = module.load(pc + 12) if rd == 1 and pc + 16 <= module.base + module.code_size else 0
            if rd == 1 and call_insn >> 24 == 0xeb:
                veneer = pc + 20 + signed(call_insn & 0xffffff, 24) * 4
                dll, ordinal = module.imports.get(veneer + 4, ('', 0))
                if (dll.lower(), ordinal) == ('apparc{000a0000}[10003a3d].dll', 171):
                    factory_targets.add(literal)
                    pending.append(literal)
            ops = [f'r[{rd}] = UINT32_C({literal:#x});', jump(next_pc)]
        elif not thumb and w & 0x0e000090 == 0x00000090 and (w >> 5) & 3 != 0 and (w >> 16) & 15 != 15 \
                and (w >> 12) & 15 != 15 and (w & (1 << 20) or (w >> 5) & 3 == 1) and (w & (1 << 24) or not w & (1 << 21)):
            # Halfword and signed-byte/halfword transfers (LDRH/STRH/LDRSB/LDRSH), immediate or register offset.
            # Post-indexed forms may not write back, and ARMv5 has no LDRD/STRD, so those stay unsupported.
            sh, load, pre, up = (w >> 5) & 3, bool(w & (1 << 20)), bool(w & (1 << 24)), bool(w & (1 << 23))
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            offset = f'r[{w & 15}]' if not w & (1 << 22) else f'UINT32_C({((w >> 8) & 15) << 4 | (w & 15)})'
            sign = '+' if up else '-'
            moved = f'r[{rn}] {sign} {offset}'
            if sh == 1 and load:
                value = f'guest_read_small(stack, address, 2)'
            elif sh == 1:
                value = None
            elif sh == 2:
                value = f'(uint32_t)(int32_t)(int8_t)guest_read_small(stack, address, 1)'
            else:
                value = f'(uint32_t)(int32_t)(int16_t)guest_read_small(stack, address, 2)'
            if not load and sh != 1:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                ops = [f'address = {f"r[{rn}] {sign} {offset}" if pre else f"r[{rn}]"};']
                ops.append(f'r[{rd}] = {value};' if load else f'guest_write_small(stack, address, r[{rd}], 2);')
                if not pre or w & (1 << 21):
                    ops.append(f'r[{rn}] = {"address" if pre else moved};')
                ops.append(jump(next_pc))
        elif not thumb and w & 0xff6000f0 == 0xe14000b0 and (w >> 16) & 15 != 15 and (w >> 12) & 15 != 15:
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            offset = ((w >> 4) & 0xf0) | (w & 15)
            addr = f'r[{rn}] {"+" if w & (1 << 23) else "-"} {offset}'
            memory = f'r[{rd}] = guest_read_small(stack, {addr}, 2);' if w & (1 << 20) else \
                     f'guest_write_small(stack, {addr}, r[{rd}], 2);'
            ops = [memory, jump(next_pc)]
        elif not thumb and w & 0xfe400000 == 0xe4400000 and (w >> 16) & 15 != 15 and (w >> 12) & 15 != 15 \
                and (w & (1 << 24) or not w & (1 << 21)):
            # LDRB/STRB with an immediate offset: offset, pre-indexed with writeback, or post-indexed (LDRBT/STRBT,
            # post-indexed with W set, are rejected by the mask). Writeback with Rn == Rd is unpredictable.
            rn, rd, pre = (w >> 16) & 15, (w >> 12) & 15, bool(w & (1 << 24))
            writeback = not pre or bool(w & (1 << 21))
            moved = f'r[{rn}] {"+" if w & (1 << 23) else "-"} {w & 0xfff}'
            memory = f'r[{rd}] = guest_read_small(stack, address, 1);' if w & (1 << 20) else \
                     f'guest_write_small(stack, address, r[{rd}], 1);'
            if writeback and rn == rd:
                ops = [f'return unsupported({pc:#x}, {w:#x});']
            else:
                ops = [f'address = {moved if pre else f"r[{rn}]"};', memory]
                if writeback:
                    ops.append(f'r[{rn}] = {"address" if pre else moved};')
                ops.append(jump(next_pc))
        elif not thumb and w & 0xfff0f070 == 0xe790f000 and 15 not in ((w >> 16) & 15, w & 15):
            rn, rm, amount = (w >> 16) & 15, w & 15, (w >> 7) & 31
            if module.load(pc - 4) == 0xe1a0e00f:  # MOV lr, pc
                call_returns.add(next_pc)
                pending.append(next_pc)
            ops = [f'target = guest_read(stack, r[{rn}] + (r[{rm}] << {amount})); goto dispatch;']
        elif not thumb and w & 0x0e000010 == 0x06000000 and (w >> 16) & 15 != 15 and (w >> 12) & 15 != 15 \
                and w & 15 != 15 and (w & (1 << 24) or not w & (1 << 21)) and not (w >> 5 & 3 == 3 and not w >> 7 & 31):
            # Word/byte transfers with a register offset and immediate shift; RRX and post-indexed writeback are handled
            # by the same expression. Post-indexed forms may not write back (ARM ARM), so those are rejected by the mask.
            rn, rd, rm, kind, amount = (w >> 16) & 15, (w >> 12) & 15, w & 15, (w >> 5) & 3, (w >> 7) & 31
            pre, up, writeback = bool(w & (1 << 24)), bool(w & (1 << 23)), bool(w & (1 << 21))
            if kind == 0:
                shifted = f'r[{rm}] << {amount}' if amount else f'r[{rm}]'
            elif kind == 1:
                shifted = f'r[{rm}] >> {amount}' if amount else '0u'
            elif kind == 2:
                shifted = f'(uint32_t)((int32_t)r[{rm}] >> {amount or 31})'
            else:
                shifted = f'(r[{rm}] >> {amount}) | (r[{rm}] << (32 - {amount}))'  # ROR #imm, amount 1..31
            sign = '+' if up else '-'
            moved = f'r[{rn}] {sign} ({shifted})'
            byte = bool(w & (1 << 22))
            if byte:
                memory = f'r[{rd}] = guest_read_small(stack, address, 1);' if w & (1 << 20) else \
                         f'guest_write_small(stack, address, r[{rd}], 1);'
            else:
                memory = f'r[{rd}] = guest_read(stack, address);' if w & (1 << 20) else \
                         f'guest_write(stack, address, r[{rd}]);'
            ops = [f'address = {moved if pre else f"r[{rn}]"};', memory]
            if writeback or not pre:
                ops.append(f'r[{rn}] = {"address" if pre else moved};')
            ops.append(jump(next_pc))
        elif not thumb and w == 0xe49df004:  # LDR pc, [sp], #4 (single-register POP)
            ops = ['target = stack_read(stack, r[13]);', 'r[13] += 4;', 'goto dispatch;']
        elif not thumb and w & 0x0f600000 == 0x04000000 and (w >> 16) & 15 != 15 and (w >> 12) & 15 != 15 \
                and (w >> 16) & 15 != (w >> 12) & 15:
            rn, rd, offset = (w >> 16) & 15, (w >> 12) & 15, w & 0xfff
            memory = f'r[{rd}] = guest_read(stack, address);' if w & (1 << 20) else \
                     f'guest_write(stack, address, r[{rd}]);'
            sign = '+' if w & (1 << 23) else '-'
            ops = [f'address = r[{rn}];', memory, f'r[{rn}] = r[{rn}] {sign} {offset};', jump(next_pc)]
        elif not thumb and w & 0xff400000 == 0xe5000000 and (w >> 16) & 15 != 15:
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            offset = w & 0xfff
            addr = f'r[{rn}] {"+" if w & (1 << 23) else "-"} {offset}'
            if w & (1 << 21):
                if rd == rn or rd == 15:
                    ops = [f'return unsupported({pc:#x}, {w:#x});']
                else:
                    mem = f'r[{rd}] = guest_read(stack, address);' if w & (1 << 20) else \
                          f'guest_write(stack, address, r[{rd}]);'
                    ops = [f'address = {addr};', mem, f'r[{rn}] = address;', jump(next_pc)]
            elif w & (1 << 20):
                if rd == 15:
                    if pc >= module.base + 4 and module.load(pc - 4) == 0xe1a0e00f:
                        # MOV lr,pc immediately before LDR pc sets LR to the
                        # following instruction, not to the LDR itself.
                        call_returns.add(next_pc)
                        pending.append(next_pc)
                    ops = [f'address = {addr};',
                           f'if (address < 0x1000) return unsupported({pc:#x}, {w:#x});',
                           'target = guest_read(stack, address);']
                    ops += [f'if (address == {slot:#x} && target == {ordinal}) target = address | 0x80000000u;'
                            for slot, (_, ordinal) in virtual_imports.items()]
                    ops.append('goto dispatch;')
                else:
                    ops = [f'r[{rd}] = guest_read(stack, {addr});', jump(next_pc)]
            else:
                value = f'{pc + 12:#x}' if rd == 15 else f'r[{rd}]'
                ops = [f'guest_write(stack, {addr}, {value});', jump(next_pc)]
        elif not thumb and w == 0xe08cc00f:
            ops = [f'r[12] += UINT32_C({pc + 8:#x});', jump(next_pc)]
        elif not thumb and w == 0xe12fff1c:
            # Resolve this literal-loaded interworking veneer at build time.
            if module.load(pc - 4) == 0xe59fc004:
                dest = module.load(pc + 8)
            elif module.load(pc - 8) == 0xe59fc004 and module.load(pc - 4) == 0xe08cc00f:
                dest = (module.load(pc + 4) + pc + 4) & 0xffffffff
            else:
                dest = None
            ops = [f'if (r[12] != {dest:#x}) return unsupported(r[12], 0);', jump(dest)] if dest is not None else [f'return unsupported({encoded:#x}, {w:#x});']
        else:
            ops = [f'return unsupported({encoded:#x}, {w:#x});']
        if predicate:
            ops = [f'if (!({predicate})) {{ {jump(next_pc)} }}', *ops]
        if not ops:
            raise ValueError(f'unsupported instruction at {encoded:#x}: {w:#x}')
        blocks[encoded] = ops

    lines = ['    goto dispatch;']
    for pc, ops in sorted(blocks.items()):
        lines.append(f'{label(pc)}:')
        lines.extend(f'    {op}' for op in ops)
    lines.extend(['dispatch:', '    switch (target) {'])
    lines.extend(f'    case {ret:#x}: goto {label(ret)};' for ret in sorted(call_returns))
    lines.extend(f'    case {pc:#x}: goto {label(pc)};'
                 for pc in sorted((factory_targets | lifecycle_targets | dll_targets | thread_targets
                                   | indirect_targets | {entry}) - call_returns))
    if calls_factory and app_profile:
        lines += ['    case AUDIO_BASE + 4: case AUDIO_BASE + 0x10: case AUDIO_BASE + 0x14:',
                  '    case AUDIO_BASE + 0x18: case AUDIO_BASE + 0x1c: case AUDIO_BASE + 0x24: case AUDIO_BASE + 0x28:',
                  '        if (r[0] != audio.object || current_worker != audio.owner) return unsupported(r[14], target);',
                  '        switch (target - AUDIO_BASE) {',
                  '        case 4: audio_destroy(); break;',
                  '        case 0x10: audio_open(stack, r[1]); break;',
                  '        case 0x14: r[0] = SDL_MIX_MAXVOLUME; break;',
                  '        case 0x18: r[0] = (uint32_t)audio.volume; break;',
                  '        case 0x1c: audio_set_volume((int32_t)r[1]); break;',
                  '        case 0x24: { int error = audio_write(stack, r[1]);',
                  '                     if (error) return unhandled_leave((uint32_t)error, r[14]); break; }',
                  '        case 0x28: if (!audio_stop(stack, r[13])) return 3; break;',
                  '        }',
                  '        target = r[14]; goto dispatch;',
                  '    case WORKER_SCHEDULER_STEP:',
                  '        if (ctx->scheduler_running) {',
                  '            if (!worker_scheduler_step(stack, ctx)) return 3;',
                  '            if (ctx->scheduler_running) return THREAD_YIELD;',
                  '        }',
                  '        target = r[14]; goto dispatch;',
                  '    case WORKER_SCHEDULER_DESTROY:',
                  '        if (!worker_scheduler_destroy(stack, r[0])) return unsupported(r[14], 0);',
                  '        target = r[14]; goto dispatch;',
                  '    case TRAP_CLEANUP_DESTROY:',
                  '        if (!trap_cleanup_destroy(stack, r[0])) return unsupported(r[14], 0);',
                  '        target = r[14]; goto dispatch;',
                  '    case SCREEN_DISPLAY_MODE:',
                  '        if (r[0] != framework.screen_device) return 3;',
                  '        r[0] = DISPLAY_RGB565; target = r[14]; goto dispatch;',
                  '    case SCREEN_NEAREST_FONT:',
                  '        if (r[0] != framework.screen_device + 4) return 3;',
                  '        address = screen_font(stack, r[2]);',
                  '        if (!address) return 3;',
                  '        guest_write(stack, r[1], address);',
                  '        r[0] = 0; target = r[14]; goto dispatch;',
                  '    case GC_BASE + 0x24: case GC_BASE + 0x3c: case GC_BASE + 0x40:',
                  '    case GC_BASE + 0x48: case GC_BASE + 0x4c: case GC_BASE + 0x88: case GC_BASE + 0xac:',
                  '    case GC_BASE + 0xe4: case GC_BASE + 0x110: case GC_BASE + 0x114:',
                  '        if (!window_gc(stack, target - GC_BASE, r[0], r[1], r[2], r[14] - 4)) return 3;',
                  '        target = r[14]; goto dispatch;',
                  f'    case {0x80000000 | (control_vtable + 0x20):#x}:',
                  '        if (!activate_control(stack, r[0])) return 3;',
                  '        target = r[14]; goto dispatch;',
                  f'    case {0x80000000 | (appui_vtable + 80):#x}:',
                  '        if (r[1] != framework.appui || !framework.control_stack) return 3;',
                  '        for (unsigned i = 0; i < 4; i++) guest_write(stack, r[0] + 4 * i, framework.screen_rect[i]);',
                  '        target = r[14]; goto dispatch;',
                  '    case 0xff000010:',
                  f'        if (!verify_factory(stack, r[0], {app_vtable:#x})) return 3;',
                  '        target = guest_read(stack, guest_read(stack, r[0]) + 20);',
                  '        r[14] = 0xff000014; goto dispatch;',
                  '    case 0xff000014:',
                  f'        if (r[0] != {image.u32(8):#x} || !pre_doc_construct(stack, resource_root, r[0])) return 2;',
                  '        r[0] = framework.application;',
                  '        target = guest_read(stack, guest_read(stack, r[0]) + 68);',
                  '        r[14] = 0xff000018; goto dispatch;',
                  '    case 0xff000018:',
                  f'        if (!verify_document(stack, r[0], {document_vtable:#x})) return 3;',
                  '        target = guest_read(stack, guest_read(stack, r[0]) + 76);',
                  '        r[14] = 0xff00001c; goto dispatch;',
                  '    case 0xff00001c:',
                  f'        if (!bind_appui(stack, r[0], {appui_vtable:#x})) return 3;',
                  '        target = guest_read(stack, guest_read(stack, r[0]) + 64);',
                  '        r[14] = 0xff000020; goto dispatch;',
                  '    case 0xff000020:',
                  '        r[0] = framework.foreground_observer;',
                  '        if (!r[0] || !guest_read(stack, framework.control_stack)) return 3;',
                  '        target = guest_read(stack, guest_read(stack, r[0]));',
                  '        fprintf(stderr, "AppUi constructed; foreground callback at 0x%08x\\n", target);',
                  '        r[14] = 0xff000030; goto dispatch;',
                  '    case 0xff000030:',
                  '        { int status = pump_window_input(stack, r[13]); if (status) return status < 0 ? 0 : status; }',
                  '        { int status = threads_run_ready(stack); if (status) return status; }',
                  '        if (!next_timer_event(stack, &r[0], &target)) return 3;',
                  '        r[14] = 0xff000030; goto dispatch;']
    elif calls_factory:
        lines += ['    case 0xff000010: return finish_factory(stack, r[0]);']
    if attach:
        lines.append('    case 0xff000000: printf("DLL process attach returned 0x%08x\\n", r[0]); return r[0] != 0;')
    if game_dlls:
        lines += ['    case 0xff000028:',
                  '        target = finish_library_attach(stack, r[0]);',
                  '        if (!target) return 2;',
                  '        goto dispatch;']
    if app_profile:
        # A guest thread that returns reaches GUEST_THREAD_SENTINEL in r14.
        lines += ['    case 0xff00ff00:', '        return 0;']
        lines += [f'    case ALLOCATOR_BASE + {slot:#x}:' for slot in range(0, 0x28, 4)]
        lines += ['        if (target != ALLOCATOR_BASE + 0x18 || r[0] != allocator_object)',
                  '            return trap_slot("RAllocator", target - ALLOCATOR_BASE, r[0]);',
                  '        r[0] = heap_alloc_size(stack, r[1]); target = r[14]; goto dispatch;']
        lines += ['    case FBS_BITMAP_BASE: case FBS_BITMAP_BASE + 4:',
                  '        if (!fbs_bitmap_reset(r[0]) || (target == FBS_BITMAP_BASE + 4 && !heap_release(r[0])))',
                  '            return trap_slot("CFbsBitmap", target - FBS_BITMAP_BASE, r[0]);',
                  '        target = r[14]; goto dispatch;']
    # Trap vtables for objects whose methods are not implemented yet; each call is reported by class, slot and receiver.
    lines += [f'    case DEVICE_BASE + {slot:#x}:' for slot in range(0, 0xa0, 4)]
    lines += ['        return trap_slot("CFbsBitmapDevice", target - DEVICE_BASE, r[0]);']
    if app_profile:
        # ~CTelephony(): the object owns no backend (iTelephonyFunctions is null) and the guest heap
        # never frees, so both destructor variants return.
        lines += ['    case TELEPHONY_BASE: case TELEPHONY_BASE + 4:',
                  '        target = r[14]; goto dispatch;',
                  # Host CBase destructors (heap does not free) and CBase::Extension_.
                  '    case CBASE_VTABLE_BASE: case CBASE_VTABLE_BASE + 4:',
                  '        target = r[14]; goto dispatch;',
                  '    case CBASE_VTABLE_BASE + 8:',
                  '        guest_write(stack, r[2], 0); r[0] = (uint32_t)-47; target = r[14]; goto dispatch;',
                  # Host CPeriodic: erase its timer and queue entry; RunError keeps r0; RunL is
                  # unreachable because the scheduler invokes the periodic TCallBack directly.
                  '    case PERIODIC_BASE: case PERIODIC_BASE + 4:',
                  '        if (!periodic_destroy(stack, r[0])) return 3;',
                  '        target = r[14]; goto dispatch;',
                  '    case PERIODIC_BASE + 8: case PERIODIC_BASE + 16:',
                  '        target = r[14]; goto dispatch;',
                  '    case PERIODIC_BASE + 12:',
                  '        if (!periodic_cancel(stack, r[0])) return 3;',
                  '        target = r[14]; goto dispatch;']
        lines += [f'    case MTM_BASE + {slot:#x}:' for slot in range(12, 4 * 64, 4)]
        lines += ['        return trap_slot("CBaseMtm", target - MTM_BASE, r[0]);']
    lines += [f'    case BITGC_BASE + {slot:#x}:' for slot in range(0, 0x11c, 4)]
    lines += ['        if (!bitgc_slot(stack, target - BITGC_BASE, r)) return trap_slot("CFbsBitGc", target - BITGC_BASE, r[0]);',
              '        target = r[14]; goto dispatch;']
    lines.extend(['    default: return unsupported(target, 0);', '    }'])
    body = '\n'.join(lines)
    image_arrays = '\n'.join(
        f'static const unsigned char image_{m.base:x}[] = {{\n' + '\n'.join(
            '    ' + ','.join(map(str, m.code[i:i + 32])) + ',' for i in range(0, m.code_size, 32)) + '\n};'
        for m in modules)
    image_reads = '\n'.join(
        f'    if (!bytes && address - {m.base:#x}u <= sizeof image_{m.base:x} - 4)\n'
        f'        bytes = image_{m.base:x} + (address - {m.base:#x}u);' for m in modules)
    library_runtime = ''
    if game_dlls:
        library_entries = '\n'.join(f'    {{ {json.dumps(name.lower())}, {dll.entry:#x}, {dll.export(1):#x}, 0 }},'
                                    for name, dll in game_dlls)
        library_runtime = f'''
static struct {{ const char *name; uint32_t entry, export_one; int attached; }} libraries[] = {{
{library_entries}
}};
enum {{ library_count = sizeof libraries / sizeof libraries[0] }};
static struct {{ unsigned id; uint32_t object, caller; }} loading;

static int matches_dll_name(const uint32_t *stack, uint32_t descriptor, const char *name) {{
    uint32_t header = guest_read(stack, descriptor);
    unsigned type = header >> 28;
    if ((header & 0x0fffffffu) != strlen(name)) return 0;
    uint32_t text;
    switch (type) {{
    case 0: text = descriptor + 4; break; /* EBufC */
    case 1: text = guest_read(stack, descriptor + 4); break; /* EPtrC */
    case 2: text = guest_read(stack, descriptor + 8); break; /* EPtr */
    case 3: text = descriptor + 8; break; /* EBuf */
    default: return 0;
    }}
    for (; *name; ++name, text += 2) {{
        unsigned ch = guest_read_small(stack, text, 2);
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)*name) return 0;
    }}
    return 1;
}}

static uint32_t load_library(uint32_t *stack, uint32_t object, uint32_t name, uint32_t caller) {{
    if (loading.id || guest_read(stack, object)) {{
        fputs("nested load or open RLibrary handle\\n", stderr);
        return 0;
    }}
    for (unsigned i = 0; i < library_count; ++i) {{
        if (!matches_dll_name(stack, name, libraries[i].name)) continue;
        /* The two bundled names resolve in the local executable's module set. */
        if (libraries[i].attached) {{
            guest_write(stack, object, i + 1);
            return caller;
        }}
        loading.id = i + 1;
        loading.object = object;
        loading.caller = caller;
        return libraries[i].entry;
    }}
    fprintf(stderr, "unsupported RLibrary name at 0x%08x (caller 0x%08x)\\n", name, caller);
    return 0;
}}

static uint32_t finish_library_attach(uint32_t *stack, uint32_t status) {{
    if (!loading.id || status) {{
        fprintf(stderr, "game DLL attach failed: 0x%08x\\n", status);
        return 0;
    }}
    libraries[loading.id - 1].attached = 1;
    guest_write(stack, loading.object, loading.id);
    fprintf(stderr, "translated process attach: %s at 0x%08x (caller 0x%08x)\\n",
            libraries[loading.id - 1].name, libraries[loading.id - 1].entry, loading.caller);
    loading.id = 0;
    return loading.caller;
}}

static uint32_t lookup_export(const uint32_t *stack, uint32_t object, uint32_t ordinal) {{
    uint32_t id = guest_read(stack, object);
    if (!id || id > library_count || ordinal != 1 || !libraries[id - 1].attached) return 0;
    return libraries[id - 1].export_one;
}}
'''
    return f'''/* Local build output; do not distribute. */
#define _GNU_SOURCE
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

enum {{ stack_base = 0x100000, stack_size = {stack_size}, thread_info_size = 64, heap_base = 0x20000000 }};
static struct {{ unsigned char *backing; uint32_t committed, limit, used, free_head; }} thread_heap;
{image_arrays}

static uint32_t stack_index(uint32_t address) {{
    uint32_t offset = address - stack_base;
    if (offset >= stack_size || offset % 4) {{
        fprintf(stderr, "invalid guest stack access at 0x%08x\\n", address);
        exit(3);
    }}
    return offset / 4;
}}

static uint32_t guest_read(const uint32_t *stack, uint32_t address);
static void guest_write(uint32_t *stack, uint32_t address, uint32_t value);

static uint32_t stack_read(const uint32_t *stack, uint32_t address) {{
    /* Thread stacks are heap allocations, so stack loads may fall outside stack[]. */
    if (address - (uint32_t)stack_base >= stack_size) return guest_read(stack, address);
    return stack[stack_index(address)];
}}

static void stack_write(uint32_t *stack, uint32_t address, uint32_t value) {{
    if (address - (uint32_t)stack_base >= stack_size) {{
        guest_write(stack, address, value);
        return;
    }}
    stack[stack_index(address)] = value;
}}

static void invalid_word(uint32_t address) {{
    fprintf(stderr, "invalid guest word access at 0x%08x\\n", address);
    exit(3);
}}

static unsigned char *heap_word(uint32_t address) {{
    uint32_t offset = address - heap_base;
    return thread_heap.used >= 4 && offset <= thread_heap.used - 4
        ? thread_heap.backing + offset : NULL;
}}

static uint32_t guest_read(const uint32_t *stack, uint32_t address) {{
    if (address % 4) invalid_word(address);
    if (address - (uint32_t)stack_base < stack_size) return stack_read(stack, address);
    const unsigned char *bytes = heap_word(address);
{image_reads}
    if (!bytes) invalid_word(address);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}}

static void guest_write(uint32_t *stack, uint32_t address, uint32_t value) {{
    if (address % 4) invalid_word(address);
    if (address - (uint32_t)stack_base < stack_size) {{
        stack_write(stack, address, value);
        return;
    }}
    unsigned char *bytes = heap_word(address);
    if (!bytes) invalid_word(address);
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}}

static uint32_t guest_read_small(const uint32_t *stack, uint32_t address, unsigned size) {{
    if (address & (size - 1)) invalid_word(address);
    unsigned shift = (address & 3) * 8;
    return (guest_read(stack, address & ~3u) >> shift) & ((1u << (8 * size)) - 1);
}}

static uint64_t guest_read64(const uint32_t *stack, uint32_t address) {{
    return (uint64_t)guest_read(stack, address + 4) << 32 | guest_read(stack, address);
}}

static void guest_write64(uint32_t *stack, uint32_t address, uint64_t value) {{
    guest_write(stack, address, (uint32_t)value);
    guest_write(stack, address + 4, (uint32_t)(value >> 32));
}}

static int home_time(uint32_t *stack, uint32_t address) {{
    struct timespec now;
    struct tm local;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC || !localtime_r(&now.tv_sec, &local)) {{
        fputs("unable to read local wall clock\\n", stderr);
        return 0;
    }}
    /* Symbian TTime counts local microseconds from 1 January, year 0. */
    int64_t seconds = INT64_C(62168256000) + now.tv_sec + local.tm_gmtoff;
    guest_write64(stack, address, (uint64_t)(seconds * INT64_C(1000000) + now.tv_nsec / 1000));
    return 1;
}}

static int time_leap_year(int year) {{
    return year % 4 == 0 && (year <= 1600 || year % 100 != 0 || year % 400 == 0);
}}

static int time_year_start(int year) {{
    /* Count leap years before year, with year 0 a leap year. */
    int leaps;
    if (year <= 0) leaps = year / 4;
    else if (year <= 1600) leaps = 1 + (year - 1) / 4;
    else {{
        int n = year - 1601;
        leaps = 401 + n / 4 - n / 100 + n / 400;
    }}
    return year * 365 + leaps;
}}

static void time_date_time(uint32_t *stack, uint32_t result, uint32_t self) {{
    const int64_t day_us = INT64_C(86400000000);
    int64_t time = (int64_t)guest_read64(stack, self);
    int days = (int)(time / day_us);
    int64_t within = time % day_us;
    if (within < 0) {{ --days; within += day_us; }}

    int year = days * 4 / 1461;
    while (days < time_year_start(year)) --year;
    while (days >= time_year_start(year + 1)) ++year;
    int day = days - time_year_start(year);
    static const int month_days[] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    int month = 0;
    for (;;) {{
        int length = month_days[month] + (month == 1 && time_leap_year(year));
        if (day < length) break;
        day -= length;
        ++month;
    }}

    uint32_t seconds = (uint32_t)(within / 1000000);
    uint32_t fields[] = {{(uint32_t)year, (uint32_t)month, (uint32_t)day,
                        seconds / 3600, seconds / 60 % 60, seconds % 60,
                        (uint32_t)(within % 1000000)}};
    for (unsigned i = 0; i < 7; ++i) guest_write(stack, result + 4 * i, fields[i]);
}}

static void guest_write_small(uint32_t *stack, uint32_t address, uint32_t value, unsigned size) {{
    if (address & (size - 1)) invalid_word(address);
    unsigned shift = (address & 3) * 8;
    uint32_t mask = (1u << (8 * size)) - 1;
    uint32_t word = guest_read(stack, address & ~3u);
    guest_write(stack, address & ~3u, (word & ~(mask << shift)) | ((value & mask) << shift));
}}

{library_runtime}
static uint32_t setup_thread_heap(const uint32_t *stack, uint32_t info) {{
    /* ARM SStdEpocThreadCreateInfo extends the 48-byte SThreadCreateInfo. */
    uint32_t allocator = stack_read(stack, info + 48);
    uint32_t initial = stack_read(stack, info + 52);
    uint32_t limit = stack_read(stack, info + 56);
    if (allocator || initial > limit || thread_heap.backing) {{
        fputs("unsupported thread heap setup\\n", stderr);
        exit(3);
    }}
    if (initial == 0) return 0;
    thread_heap.backing = calloc(1, initial);
    if (!thread_heap.backing) return UINT32_C(0xfffffffc); /* KErrNoMemory */
    thread_heap.committed = initial;
    thread_heap.limit = limit;
    thread_heap.free_head = UINT32_MAX;
    return 0;
}}

/* Each aligned block starts with its total size and the next free offset (or its live marker). */
#define HEAP_LIVE (UINT32_MAX - 1)
static uint32_t *heap_block(uint32_t offset) {{
    return (uint32_t *)(thread_heap.backing + offset);
}}

/* Returns 0 when the heap cannot satisfy the request (User::Alloc / operator new[] semantics). */
static uint32_t heap_try_alloc(uint32_t size) {{
    if (!size) size = 1; /* zero-size allocations still need a distinct address */
    if (!thread_heap.backing || size > thread_heap.limit || thread_heap.limit > UINT32_MAX - heap_base ||
        size > UINT32_MAX - 15) return 0;
    uint32_t length = ((size + 7) & ~UINT32_C(7)) + 8;
    uint32_t *link = &thread_heap.free_head;
    while (*link != UINT32_MAX) {{
        uint32_t start = *link, *block = heap_block(start);
        if (block[0] >= length) {{
            if (block[0] - length >= 16) {{
                uint32_t *rest = heap_block(start + length);
                rest[0] = block[0] - length;
                rest[1] = block[1];
                *link = start + length;
                block[0] = length;
            }} else *link = block[1];
            block[1] = HEAP_LIVE;
            memset(block + 2, 0, size);
            return heap_base + start + 8;
        }}
        link = &block[1];
    }}
    if (length > thread_heap.limit - thread_heap.used) return 0;
    uint32_t start = thread_heap.used;
    uint32_t end = start + length;
    if (end > thread_heap.committed) {{
        unsigned char *grown = realloc(thread_heap.backing, end);
        if (!grown) {{
            fputs("guest AllocZL: host allocation failed\\n", stderr);
            exit(2);
        }}
        thread_heap.backing = grown;
        thread_heap.committed = end;
    }}
    uint32_t *block = heap_block(start);
    block[0] = length;
    block[1] = HEAP_LIVE;
    memset(block + 2, 0, size);
    thread_heap.used = end;
    return heap_base + start + 8;
}}

static uint32_t heap_alloc_zeroed(uint32_t size) {{
    uint32_t address = heap_try_alloc(size);
    if (!address) {{
        fputs("guest AllocZL: out of memory; leave is not implemented\\n", stderr);
        exit(2);
    }}
    return address;
}}

#define ALLOCATOR_BASE UINT32_C(0xff00c000)
static uint32_t allocator_object;

static uint32_t user_allocator(uint32_t *stack) {{
    if (!allocator_object && (allocator_object = heap_try_alloc(4 + 0x28))) {{
        guest_write(stack, allocator_object, allocator_object + 4);
        for (uint32_t slot = 0; slot < 0x28; slot += 4)
            guest_write(stack, allocator_object + 4 + slot, ALLOCATOR_BASE + slot);
    }}
    return allocator_object;
}}

/* MAllocator::AllocSize(TInt&): return live cell count and write live payload bytes. */
static uint32_t heap_alloc_size(uint32_t *stack, uint32_t out) {{
    uint32_t count = 0, bytes = 0;
    for (uint32_t offset = 0; offset < thread_heap.used;) {{
        const uint32_t *block = heap_block(offset);
        if (block[1] == HEAP_LIVE && heap_base + offset + 8 != allocator_object) {{
            count++;
            bytes += block[0] - 8;
        }}
        offset += block[0];
    }}
    guest_write(stack, out, bytes);
    return count;
}}

static int heap_release(uint32_t address) {{
    if (!address) return 1;
    uint32_t start = address - heap_base - 8;
    if (start >= thread_heap.used || (start & 7)) return 0;
    uint32_t *block = heap_block(start);
    if (block[1] != HEAP_LIVE || block[0] < 16 || block[0] > thread_heap.used - start || (block[0] & 7)) return 0;
    uint32_t *link = &thread_heap.free_head, *prev = NULL;
    while (*link < start) {{
        prev = heap_block(*link);
        link = &prev[1];
    }}
    block[1] = *link;
    if (start + block[0] == block[1]) {{
        uint32_t *next = heap_block(block[1]);
        block[0] += next[0];
        block[1] = next[1];
    }}
    if (prev && prev + prev[0] / sizeof *prev == block) {{
        prev[0] += block[0];
        prev[1] = block[1];
    }} else *link = start;
    return 1;
}}

/* Register-specified shifts use the bottom byte; amounts of 32 or more saturate (ARM ARM). kind 3 (ROR) is not used here. */
static uint32_t arm_shift(uint32_t value, unsigned kind, uint32_t amount) {{
    amount &= 0xff;
    if (!amount) return value;
    switch (kind) {{
    case 0: return amount < 32 ? value << amount : 0;
    case 1: return amount < 32 ? value >> amount : 0;
    default: return (uint32_t)((int32_t)value >> (amount < 32 ? amount : 31));
    }}
}}

static uint32_t exception_tls;
static enum {{ PROCESS_BOOTING, PROCESS_INITIALIZED }} process_state;
static struct {{
    uint32_t coe_env, process, application, document, appui, window_group_uid, resource_offset;
    uint32_t control, control_stack, screen_rect[4], menu_bar_id, cba_id;
    uint32_t window, window_rect[4], screen_device, system_gc, normal_font, foreground_observer;
    uint32_t active_queue, next_timer, key_block_mode, process_priority;
    struct timespec last_activity;
    int screen_change_events;
    FILE *resource;
}} framework;
/* periodic: the owning CPeriodic object, or 0 for a plain RTimer. */
static struct {{ uint64_t deadline; uint32_t status, periodic; }} timers[32];
static struct {{ uint32_t key, mask, modifiers; }} captures[16];
static unsigned capture_count;
#define SCREEN_DISPLAY_MODE UINT32_C(0xff000024)
#define SCREEN_NEAREST_FONT UINT32_C(0xff00002c)
#define GC_BASE UINT32_C(0xff001000)
#define DEVICE_BASE UINT32_C(0xff004000)
#define BITGC_BASE UINT32_C(0xff005000)
#define TELEPHONY_BASE UINT32_C(0xff006000)
#define TRAP_CLEANUP_DESTROY UINT32_C(0xff009000)
#define WORKER_SCHEDULER_STEP UINT32_C(0xff009004)
#define WORKER_SCHEDULER_DESTROY UINT32_C(0xff009008)
#define PERIODIC_BASE UINT32_C(0xff00d000)
{(Path(__file__).with_name('window_gc.h').read_text() if app_profile else '')}
{(Path(__file__).with_name('fbs_bitmap.h').read_text() if app_profile else '')}
{(Path(__file__).with_name('game_files.h').read_text() if app_profile else '')}
{(Path(__file__).with_name('guest_threads.h').read_text() if app_profile else '')}
{(Path(__file__).with_name('guest_sync.h').read_text() if app_profile else '')}
{(Path(__file__).with_name('guest_audio.h').read_text() if app_profile else '')}

#if {int(bool(app_profile))}
static int window_gc(uint32_t *stack, uint32_t slot, uint32_t context, uint32_t arg, uint32_t point, uint32_t pc) {{
    if (context != framework.system_gc || (slot != 0x110 && !gc.window)) return 0;
    switch (slot) {{
    case 0x24: if (!arg) return 0; gc.font = arg; break;
    case 0x3c: gc.pen_color = guest_read(stack, arg); break;
    case 0x40: gc.pen_style = arg; break;
    case 0x48: gc.brush_color = guest_read(stack, arg); break;
    case 0x4c: gc.brush_style = arg; break;
    case 0x88: return draw_rect(stack, arg);
    case 0xac: return draw_text(stack, arg, point, pc);
    case 0xe4: {{ /* CWindowGc::BitBlt(const TPoint&, const CFbsBitmap*) */
        const struct fbs_bitmap_entry *bitmap = fbs_bitmap_find(point);
        if (!bitmap || bitmap->mode != FBS_EColor64K) return 0;
        int64_t left = (int32_t)guest_read(stack, arg), top = (int32_t)guest_read(stack, arg + 4);
        unsigned changed = 0;
        for (int y = 0; y < window_height; y++) {{
            int64_t sy = y - top;
            if (sy < 0 || sy >= bitmap->height) continue;
            uint32_t row = bitmap->data + (uint32_t)sy * bitmap->stride;
            for (int x = 0; x < window_width; x++) {{
                int64_t sx = x - left;
                if (sx < 0 || sx >= bitmap->width) continue;
                uint16_t value = guest_read_small(stack, row + (uint32_t)sx * 2, 2);
                changed += pixels[y][x] != value;
                pixels[y][x] = value;
            }}
        }}
        if (changed) {{
            gc.draw_pc = pc;
            fprintf(stderr, "guest BitBlt pc=%08x changed_pixels=%u\\n", pc, changed);
        }}
        break;
    }}
    case 0x110:
        if (arg != framework.window || gc.window) return 0;
        gc.window = arg;
        break;
    case 0x114: gc.window = 0; return publish_window(pc);
    default: return 0;
    }}
    return 1;
}}
#endif
enum {{ DISPLAY_RGB565 = 7 }}; /* EColor64K */
#define ETEL_EMPTY_SESSION UINT32_C(0x0000e7e1)
enum {{ font_spec_size = 72, font_spec_height = 56 }};
/* CleanupStack entries: a CBase* (destroyed through its deleting destructor), a TAny* (User::Free), a
   TCleanupItem (operation called with the pointer), or an RReadStream (CleanupReleasePushL: Release()). */
enum {{ CLEANUP_CBASE, CLEANUP_ANY, CLEANUP_ITEM, CLEANUP_READ_STREAM, CLEANUP_WRITE_STREAM }};
static struct guest_cleanup main_cleanup;
#define cleanup_items ((current_worker ? &current_worker->cleanup : &main_cleanup)->items)
#define cleanup_depth ((current_worker ? &current_worker->cleanup : &main_cleanup)->depth)
#define cleanup_handler ((current_worker ? &current_worker->cleanup : &main_cleanup)->handler)

/* The translated TRAP sites track cleanup depth directly rather than invoking a guest TTrapHandler. */
static uint32_t trap_cleanup_new(uint32_t *stack) {{
    static uint32_t vtable;
    if (!vtable) {{
        vtable = heap_try_alloc(8);
        if (!vtable) return 0;
        guest_write(stack, vtable, TRAP_CLEANUP_DESTROY);
        guest_write(stack, vtable + 4, TRAP_CLEANUP_DESTROY);
    }}
    uint32_t object = heap_try_alloc(12);
    if (!object) return 0;
    guest_write(stack, object, vtable);
    guest_write(stack, object + 4, cleanup_handler);
    guest_write(stack, object + 8, cleanup_depth);
    cleanup_handler = object;
    return object;
}}

static int trap_cleanup_destroy(uint32_t *stack, uint32_t object) {{
    if (!object || object != cleanup_handler || guest_read(stack, object + 8) != cleanup_depth) return 0;
    cleanup_handler = guest_read(stack, object + 4);
    return 1;
}}

static void start_application(void) {{
    if (framework.coe_env) {{
        fputs("application environment is already active\\n", stderr);
        exit(3);
    }}
    /* Host-owned environment with fields exposed where the game inlines
       accessors. Other CCoeEnv methods go through host bindings. */
    framework.coe_env = heap_alloc_zeroed(0x4c);
    framework.process = heap_alloc_zeroed(4);
}}

static void construct_eik_application(uint32_t *stack, uint32_t application) {{
    if (!framework.coe_env || framework.application || !heap_word(application + 16)) {{
        fputs("invalid CEikApplication construction\\n", stderr);
        exit(3);
    }}
    /* ARM32 CApaApplication: vptr, filename reference, ECOM key, reserved int.
       Its empty constructor leaves the zeroed fields untouched. The game
       installs the derived vptr immediately after this base constructor. */
    guest_write(stack, application + 16, framework.coe_env);
    framework.application = application;
}}

static int finish_factory(const uint32_t *stack, uint32_t application) {{
    if (!application || application != framework.application ||
        guest_read(stack, application + 16) != framework.coe_env) {{
        fputs("application factory returned an invalid object\\n", stderr);
        return 3;
    }}
    uint32_t vtable = guest_read(stack, application);
    if (!vtable || !guest_read(stack, vtable)) {{
        fputs("application factory did not install a vtable\\n", stderr);
        return 3;
    }}
    fprintf(stderr, "application factory returned 0x%08x (vtable 0x%08x); PreDocConstructL not implemented\\n",
            application, vtable);
    return 2;
}}

static int verify_factory(const uint32_t *stack, uint32_t application, uint32_t expected_vptr) {{
    if (!application || application != framework.application ||
        guest_read(stack, application + 16) != framework.coe_env ||
        guest_read(stack, application) != expected_vptr) {{
        fputs("application factory returned an invalid object\\n", stderr);
        return 0;
    }}
    fprintf(stderr, "application factory returned 0x%08x (vtable 0x%08x)\\n", application, expected_vptr);
    return 1;
}}

static int read_resource_bytes(FILE *file, long offset, unsigned char *out, size_t length) {{
    return fseek(file, offset, SEEK_SET) == 0 && fread(out, 1, length, file) == length;
}}

static unsigned le16(const unsigned char *bytes) {{
    return bytes[0] | (unsigned)bytes[1] << 8;
}}

static uint32_t le32(const unsigned char *bytes) {{
    return le16(bytes) | (uint32_t)le16(bytes + 2) << 16;
}}

static int read_uncompressed_resource(FILE *file, unsigned id, unsigned char *out, size_t length) {{
    unsigned char word[8];
    if (!id || fseek(file, 0, SEEK_END) != 0) return 0;
    long size = ftell(file);
    if (size < 25 || size > 65535 || !read_resource_bytes(file, 0, word, 4) ||
        le32(word) != UINT32_C(0x101f4a6b) || !read_resource_bytes(file, size - 2, word, 2)) return 0;
    unsigned index = le16(word);
    if (index < 19 || index > (unsigned)size - 4 || (size - index) % 2) return 0;
    unsigned resources = (unsigned)(size - index) / 2 - 1;
    unsigned bit = id - 1;
    if (id > resources || !read_resource_bytes(file, 19 + bit / 8, word, 1) ||
        (word[0] & (1u << (bit % 8))) || !read_resource_bytes(file, index + bit * 2, word, 4)) return 0;
    unsigned first = le16(word), second = le16(word + 2);
    return first >= 19 + (resources + 7) / 8 && second <= index && second >= first &&
           second - first == length && read_resource_bytes(file, first, out, length);
}}

static uint32_t resource_signature(FILE *file) {{
    unsigned char word[8];
    if (!read_uncompressed_resource(file, 1, word, sizeof word)) return 0;
    uint32_t self_link = le32(word + 4);
    return (self_link & 0xfff) == 1 ? self_link & ~UINT32_C(0xfff) : 0;
}}

static int pre_doc_construct(uint32_t *stack, const char *root, uint32_t uid) {{
    if (!root) {{
        fputs("pass the extracted Symbian resource root as the first argument\\n", stderr);
        return 0;
    }}
    /* Standalone foreground profile: one root group, one AppArc process,
       default language. Register the installed resource before creating the document. */
    const char relative[] = "/resource/apps/IsLand_3rd.rsc";
    size_t n = strlen(root);
    if (n > SIZE_MAX - sizeof relative) return 0;
    char *path = malloc(n + sizeof relative);
    if (!path) return 0;
    memcpy(path, root, n);
    memcpy(path + n, relative, sizeof relative);
    FILE *file = fopen(path, "rb");
    if (!file) perror(path);
    free(path);
    if (!file) return 0;
    uint32_t offset = resource_signature(file);
    if (!offset) {{
        fclose(file);
        fputs("application resource signature is invalid or unsupported\\n", stderr);
        return 0;
    }}
    framework.resource = file;
    framework.resource_offset = offset;
    framework.window_group_uid = uid;
    guest_write(stack, framework.application + 20, offset); /* iResourceFileOffset */
    guest_write(stack, framework.application + 24, framework.process); /* iProcess */
    fprintf(stderr, "registered application resource offset 0x%08x for UID 0x%08x\\n", offset, uid);
    return 1;
}}

static void construct_akn_document(uint32_t *stack, uint32_t document, uint32_t application) {{
    if (!framework.resource || application != framework.application || !heap_word(document + 12)) {{
        fputs("invalid CAknDocument construction\\n", stderr);
        exit(3);
    }}
    /* CApaDocument places its embedded observer before the app and process. */
    guest_write(stack, document + 8, application);
    guest_write(stack, document + 12, guest_read(stack, application + 24));
    framework.document = document;
}}

static void cleanup_push_item(uint32_t pointer, uint32_t operation, uint32_t kind) {{
    if (cleanup_depth >= sizeof cleanup_items / sizeof cleanup_items[0]) {{
        fputs("guest cleanup stack is full\\n", stderr);
        exit(3);
    }}
    cleanup_items[cleanup_depth].pointer = pointer;
    cleanup_items[cleanup_depth].operation = operation;
    cleanup_items[cleanup_depth].kind = kind;
    cleanup_depth++;
}}

static void cleanup_push(uint32_t object) {{
    cleanup_push_item(object, 0, CLEANUP_CBASE);
}}

static void cleanup_pop_count(uint32_t count) {{
    if (count > cleanup_depth) {{
        fputs("guest cleanup stack underflow\\n", stderr);
        exit(3);
    }}
    while (count--) {{
        --cleanup_depth;
        cleanup_items[cleanup_depth].pointer = cleanup_items[cleanup_depth].operation = 0;
    }}
}}

static void cleanup_pop(void) {{
    cleanup_pop_count(1);
}}

/* Host TRAP handlers are not installed and leaves are return values, so a mark records only the cleanup depth. */
static uint32_t cleanup_mark(void) {{
    return cleanup_depth + 1;
}}

static void cleanup_unmark(uint32_t mark) {{
    if (mark != cleanup_depth + 1) {{
        fprintf(stderr, "guest cleanup stack unbalanced at unmark: mark %u depth %u\\n", mark, cleanup_depth);
        exit(3);
    }}
}}

static int verify_document(const uint32_t *stack, uint32_t document, uint32_t expected_vptr) {{
    if (document != framework.document || !document || cleanup_depth ||
        guest_read(stack, document + 8) != framework.application ||
        guest_read(stack, document + 12) != framework.process ||
        guest_read(stack, document) != expected_vptr) return 0;
    fprintf(stderr, "document factory returned 0x%08x (vtable 0x%08x)\\n", document, expected_vptr);
    return 1;
}}

static void construct_akn_appui(uint32_t *stack, uint32_t appui) {{
    if (!framework.document || framework.appui || !heap_word(appui + 68)) {{
        fputs("invalid CAknAppUiBase construction\\n", stderr);
        exit(3);
    }}
    guest_write(stack, appui + 4, framework.coe_env); /* CCoeAppUi::iCoeEnv */
    guest_write(stack, appui + 68, 1u << 4); /* CAknAppUiBase::iAknFlags[EFullScreen] */
    framework.appui = appui;
}}

static int bind_appui(uint32_t *stack, uint32_t appui, uint32_t expected_vptr) {{
    if (!appui || appui != framework.appui || guest_read(stack, appui) != expected_vptr) return 0;
    /* CEikDocument::PrepareToEditL: set iAppUi, SetDocument(), and leave the
       embedded observer/read-only fields zero for the standalone launch. */
    guest_write(stack, framework.document + 24, appui);
    guest_write(stack, appui + 44, framework.document);
    guest_write(stack, framework.coe_env + 0x24, appui);
    fprintf(stderr, "AppUi factory returned 0x%08x (vtable 0x%08x)\\n", appui, expected_vptr);
    return 1;
}}

static uint32_t screen_font(uint32_t *stack, uint32_t spec) {{
    uint32_t height = guest_read(stack, spec + font_spec_height);
    if (!height || height > 1440) {{
        fprintf(stderr, "unsupported screen font height %u twips\\n", height);
        return 0;
    }}
    /* The standalone screen uses a scalable default sans-serif typeface.
       Preserve the requested logical spec; drawing binds glyphs separately. */
    uint32_t font = heap_alloc_zeroed(font_spec_size);
    for (unsigned i = 0; i < font_spec_size; i += 4)
        guest_write(stack, font + i, guest_read(stack, spec + i));
    return font;
}}

static int font_spec_in_twips(uint32_t *stack, uint32_t dest, uint32_t font) {{
    if (font != framework.normal_font) return 0;
    for (unsigned i = 0; i < font_spec_size; i += 4)
        guest_write(stack, dest + i, guest_read(stack, font + i));
    return 1;
}}

static int base_construct_appui(uint32_t *stack, uint32_t appui, uint32_t flags) {{
    /* Foreground, non-embedded S60 layout. Keep system chrome out of the
       game surface; retain its resource IDs for the UI ABI. */
    unsigned char info[28], titles[6];
    if (!framework.resource || appui != framework.appui || flags || framework.control_stack ||
        guest_read(stack, appui + 44) != framework.document || !(guest_read(stack, appui + 68) & 16) ||
        !read_uncompressed_resource(framework.resource, 3, info, sizeof info) ||
        le32(info) || le32(info + 8) || le32(info + 12) || le32(info + 20) || le32(info + 24)) {{
        fputs("unsupported foreground EIK_APP_INFO configuration\\n", stderr);
        return 0;
    }}
    uint32_t menu = le32(info + 4);
    if ((menu & ~UINT32_C(0xfff)) != framework.resource_offset ||
        !read_uncompressed_resource(framework.resource, menu & 0xfff, titles, sizeof titles) ||
        le16(titles)) {{
        fputs("unsupported Eikon menu title list\\n", stderr);
        return 0;
    }}
    framework.menu_bar_id = menu;
    framework.cba_id = le32(info + 16);
    framework.control_stack = heap_alloc_zeroed(4); /* Host-owned CONE control stack. */
    guest_write(stack, appui + 12, framework.control_stack);
    /* The original app has a 176x208 content path (also used as a viewport
       of its 352x416 mode). Expose the former as the standalone device. */
    framework.screen_rect[2] = 176;
    framework.screen_rect[3] = 208;
    framework.screen_device = heap_alloc_zeroed(8);
    uint32_t screen_vtable = heap_alloc_zeroed(16);
    guest_write(stack, screen_vtable + 12, SCREEN_DISPLAY_MODE);
    guest_write(stack, framework.screen_device, screen_vtable);
    uint32_t screen_map_vtable = heap_alloc_zeroed(28);
    guest_write(stack, screen_map_vtable + 24, SCREEN_NEAREST_FONT);
    guest_write(stack, framework.screen_device + 4, screen_map_vtable);
    guest_write(stack, framework.coe_env + 0x48, framework.screen_device); /* ScreenDevice() */
    framework.normal_font = heap_alloc_zeroed(font_spec_size);
    guest_write(stack, framework.normal_font + font_spec_height, 200);
    guest_write(stack, framework.coe_env + 0x44, framework.normal_font); /* NormalFont() */
    framework.system_gc = heap_alloc_zeroed(4);
    uint32_t gc_vtable = heap_alloc_zeroed(0x118);
    static const uint32_t slots[] = {{ 0x24, 0x3c, 0x40, 0x48, 0x4c, 0x88, 0xac, 0xe4, 0x110, 0x114 }};
    for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++)
        guest_write(stack, gc_vtable + slots[i], GC_BASE + slots[i]);
    guest_write(stack, framework.system_gc, gc_vtable);
    framework.screen_change_events = 1;
    fprintf(stderr, "initialized fullscreen AppUi with %ux%u screen (menu 0x%08x, CBA 0x%08x)\\n",
            framework.screen_rect[2], framework.screen_rect[3], framework.menu_bar_id, framework.cba_id);
    return 1;
}}

static int add_active(uint32_t *stack, uint32_t object) {{
    uint32_t *queue = current_worker ? &current_worker->active_queue : &framework.active_queue;
    if ((current_worker ? !current_worker->scheduler : !framework.control_stack) ||
        !object || guest_read(stack, object + 12)) {{
        fputs("invalid CActiveScheduler::Add\\n", stderr);
        return 0;
    }}
    if (!*queue) {{
        *queue = heap_alloc_zeroed(8);
        guest_write(stack, *queue, *queue);
        guest_write(stack, *queue + 4, *queue);
    }}
    /* TRequestStatus at +4 (status and flags); TPriQueLink at +12. */
    uint32_t link = object + 12, head = *queue;
    uint32_t next = guest_read(stack, head);
    uint32_t priority = guest_read(stack, link + 8) ^ 0x80000000u;
    while (next != head && (guest_read(stack, next + 8) ^ 0x80000000u) >= priority)
        next = guest_read(stack, next);
    uint32_t prev = guest_read(stack, next + 4);
    guest_write(stack, link, next);
    guest_write(stack, link + 4, prev);
    guest_write(stack, prev, link);
    guest_write(stack, next + 4, link);
    return 1;
}}

static int create_timer(uint32_t *stack, uint32_t handle) {{
    if (!framework.control_stack || guest_read(stack, handle) || framework.next_timer >= sizeof timers / sizeof timers[0]) {{
        fputs("invalid RTimer::CreateLocal\\n", stderr);
        return 0;
    }}
    guest_write(stack, handle, ++framework.next_timer);
    return 1;
}}

static uint32_t periodic_host_vtable(uint32_t *stack) {{
    /* CPeriodic lives in euser, which is not translated, so the host builds its CActive vtable:
       destructors (+0/+4), RunError (+8, returns the error in r0), DoCancel (+12) and RunL (+16).
       The scheduler calls a periodic object's TCallBack directly, so RunL is only a safe placeholder. */
    static uint32_t vtable;
    if (!vtable) {{
        vtable = heap_try_alloc(20);
        if (!vtable) return 0;
        for (uint32_t slot = 0; slot < 20; slot += 4) guest_write(stack, vtable + slot, PERIODIC_BASE + slot);
    }}
    return vtable;
}}

static uint32_t new_periodic(uint32_t *stack, uint32_t priority) {{
    /* CActive (28 bytes), RTimer (4), interval and TCallBack (12). */
    uint32_t vtable = periodic_host_vtable(stack), timer = heap_alloc_zeroed(44);
    if (!vtable) return 0;
    guest_write(stack, timer, vtable);
    guest_write(stack, timer + 20, priority);
    return create_timer(stack, timer + 28) && add_active(stack, timer) ? timer : 0;
}}

static int schedule_timer(uint32_t *stack, uint32_t handle, uint32_t status, uint32_t delay) {{
    uint32_t id = guest_read(stack, handle);
    struct timespec now;
    if (!id || id > framework.next_timer || timers[id - 1].status || delay > INT32_MAX ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0) {{
        fprintf(stderr, "invalid RTimer::After request (handle 0x%08x id %u, status 0x%08x, outstanding 0x%08x)\\n",
                handle, id, status, id && id <= framework.next_timer ? timers[id - 1].status : 0);
        return 0;
    }}
    timers[id - 1].deadline = (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000 + delay;
    timers[id - 1].status = status;
    guest_write(stack, status, 0x80000001u); /* KRequestPending */
    guest_write(stack, status + 4, guest_read(stack, status + 4) | 2);
    return 1;
}}

static int set_active(uint32_t *stack, uint32_t object) {{
    uint32_t flags = guest_read(stack, object + 8);
    if (!guest_read(stack, object + 12) || (flags & 1)) return 0;
    guest_write(stack, object + 8, flags | 1);
    return 1;
}}

static int start_periodic(uint32_t *stack, uint32_t object, uint32_t delay, uint32_t interval,
                          uint32_t callback, uint32_t argument) {{
    if (interval > INT32_MAX || !callback || !schedule_timer(stack, object + 28, object + 4, delay)) return 0;
    timers[guest_read(stack, object + 28) - 1].periodic = object;
    guest_write(stack, object + 32, interval);
    guest_write(stack, object + 36, callback);
    guest_write(stack, object + 40, argument);
    return set_active(stack, object);
}}

/* Asynchronous completions issued by imports (for example CTelephony::GetPhoneId). They are
   applied when the scheduler next runs, so they happen after the caller has called SetActive(),
   the same ordering as a request completed by the server. */
#define DEFERRED_MAX 8
static uint32_t deferred_status[DEFERRED_MAX];
static int32_t deferred_value[DEFERRED_MAX];
static uint32_t deferred_count;

static int defer_completion(uint32_t status, int32_t value) {{
    if (deferred_count >= DEFERRED_MAX) return 0;
    deferred_status[deferred_count] = status;
    deferred_value[deferred_count] = value;
    deferred_count++;
    return 1;
}}

static void complete_deferred(uint32_t *stack) {{
    for (uint32_t i = 0; i < deferred_count; i++) {{
        guest_write(stack, deferred_status[i], (uint32_t)deferred_value[i]);
        guest_write(stack, deferred_status[i] + 4, guest_read(stack, deferred_status[i] + 4) & ~2u);
    }}
    deferred_count = 0;
}}

/* Runs one dispatched RunL as a nested guest call on the caller's stack below sp. It returns
   when the callee reaches the sentinel; any other exit is a failure. */
static int run_nested_event(uint32_t *stack, uint32_t target, uint32_t argument, uint32_t sp) {{
    struct guest_thread nested = {{0}};
    nested.entry = target;
    nested.arg = argument;
    nested.target = target;
    nested.r[0] = argument;
    nested.r[13] = sp;
    nested.r[14] = GUEST_THREAD_SENTINEL;
    return guest_run(&nested, stack) == 0;
}}

/* PC input surface: physical keys and pointer events enter here. The original key codes stay
   authoritative: every host path ends in an EEventKeyDown/Up pair carrying a code the phone build
   already emits, so no host route can call an item script, allocate an item id or mutate the
   inventory on its own. Host-only PC actions (Q/E/F/Tab/pointer) are dispatched by explicit input
   context and only route when a hook installed for that exact context returns a verified code; an
   unrouted action resolves to 0 and is dropped.

   Visibility and configuration: ISLAND_PC_INPUT="key=value,key=value" overrides the effective
   layer once at startup and the resolved values are logged to stderr. Keys: ok, back, items
   (original softkey codes), drag (drag threshold in logical px), pointer/right/tab/q/e/f (0/1),
   tabkey/qkey/ekey/fkey (SDL scancodes) and context (scene|dialogue|menu|inventory|item).
   Unknown or invalid entries are ignored and counted; key-code collisions fall back to the
   verified defaults. The guest context source is not yet proven (the inventory state and cursor
   hit-test are unresolved), so context is never inferred from game state: only a verified caller
   through push/pop or the explicit diagnostic override moves it. */
enum {{ PC_INPUT_SCENE, PC_INPUT_DIALOGUE, PC_INPUT_MENU, PC_INPUT_INVENTORY,
        PC_INPUT_ITEM_PUZZLE, PC_INPUT_CONTEXT_COUNT }};
enum {{ PC_ACTION_SELECT_PICKAXE, PC_ACTION_SELECT_SLINGSHOT, PC_ACTION_SELECT_CONTEXT_ITEM,
        PC_ACTION_TOGGLE_INVENTORY, PC_ACTION_POINTER_MOVE, PC_ACTION_POINTER_PRIMARY,
        PC_ACTION_POINTER_SECONDARY, PC_ACTION_POINTER_SCROLL, PC_ACTION_POINTER_DRAG,
        PC_ACTION_COUNT }};
typedef uint32_t (*pc_input_hook)(int context, int action, int32_t x, int32_t y, int32_t detail);

#define PC_INPUT_CONTEXT_MAX 4
#define PC_INPUT_PENDING 32
#define PC_INPUT_ENV "ISLAND_PC_INPUT"

static const char *const pc_input_context_names[PC_INPUT_CONTEXT_COUNT] = {{
    "scene", "dialogue", "menu", "inventory", "item"
}};

/* Every value here either names an original phone key code or gates a PC-only convenience. */
struct pc_input_config {{
    uint32_t ok_key, back_key, items_key; /* original EScancode values */
    int32_t drag_px;
    int pointer_confirm, right_button_back, tab_items;
    int q_select, e_select, f_context;
    uint32_t tab_scancode, q_scancode, e_scancode, f_scancode;
    int initial_context;
}};
static struct pc_input_config pc_cfg = {{
    0xa7, 0xa5, 0xa4, /* OK / right softkey / left softkey, verified for the first level */
    4, 1, 1, 1, 1, 1, 1,
    SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_F,
    PC_INPUT_SCENE
}};

static pc_input_hook pc_input_hooks[PC_INPUT_CONTEXT_COUNT][PC_ACTION_COUNT];
static int pc_input_stack[PC_INPUT_CONTEXT_MAX] = {{ PC_INPUT_SCENE }};
static unsigned pc_input_depth = 1;
static int pc_input_configured;
static int32_t pc_pointer_x, pc_pointer_y, pc_pointer_press_x, pc_pointer_press_y;
static int pc_pointer_down, pc_pointer_dragged, pc_pointer_right_down;
static uint32_t pc_pointer_right_scan; /* code latched on press; released whatever the context */
static struct pc_pending_key {{ uint32_t scan, type; unsigned char host; }} pc_pending_keys[PC_INPUT_PENDING];
static unsigned pc_pending_count;
static uint32_t pc_held_keys[PC_INPUT_PENDING];
static unsigned pc_held_key_count;

static int pc_queue_key(uint32_t scan, uint32_t type, int host) {{
    if (!scan || pc_pending_count >= PC_INPUT_PENDING) return 0;
    pc_pending_keys[pc_pending_count].scan = scan;
    pc_pending_keys[pc_pending_count].type = type;
    pc_pending_keys[pc_pending_count].host = (unsigned char)(host != 0);
    pc_pending_count++;
    return 1;
}}

/* A release must not be lost to a queue full of synthetic taps: drop the oldest tap for room. */
static int pc_queue_key_force(uint32_t scan, uint32_t type) {{
    if (!scan) return 0;
    if (pc_pending_count >= PC_INPUT_PENDING) {{
        unsigned victim = 0;
        for (unsigned i = 0; i < pc_pending_count; i++)
            if (pc_pending_keys[i].host) {{ victim = i; break; }}
        memmove(&pc_pending_keys[victim], &pc_pending_keys[victim + 1],
                (pc_pending_count - victim - 1) * sizeof pc_pending_keys[0]);
        pc_pending_count--;
    }}
    return pc_queue_key(scan, type, 0);
}}

/* A synthetic confirm/cancel is only useful as a matched down/up pair, and one still in flight
   must not be queued twice (a duplicate click before the guest update is one activation). */
static int pc_queue_tap(uint32_t scan) {{
    if (!scan || pc_pending_count + 2 > PC_INPUT_PENDING) return 0;
    for (unsigned i = 0; i + 1 < pc_pending_count; i++)
        if (pc_pending_keys[i].scan == scan && pc_pending_keys[i].type == 3 &&
            pc_pending_keys[i + 1].scan == scan && pc_pending_keys[i + 1].type == 2) return 0;
    return pc_queue_key(scan, 3, 1) && pc_queue_key(scan, 2, 1);
}}

static int pc_key_held(uint32_t scan) {{
    for (unsigned i = 0; i < pc_held_key_count; i++)
        if (pc_held_keys[i] == scan) return 1;
    return 0;
}}

/* Returns 0 for a duplicate key-down, which is not forwarded (event dedupe). */
static int pc_note_key(uint32_t scan, uint32_t type) {{
    if (type == 3) {{
        if (pc_key_held(scan)) return 0;
        if (pc_held_key_count < PC_INPUT_PENDING) pc_held_keys[pc_held_key_count++] = scan;
        return 1;
    }}
    for (unsigned i = 0; i < pc_held_key_count; i++)
        if (pc_held_keys[i] == scan) {{
            pc_held_keys[i] = pc_held_keys[--pc_held_key_count];
            break;
        }}
    return 1;
}}

/* Queue releases for every physical key still down, so crossing a UI boundary cannot latch a
   movement/confirm key in the next context. */
static void pc_release_held_keys(void) {{
    while (pc_held_key_count) {{
        uint32_t scan = pc_held_keys[--pc_held_key_count];
        if (!pc_queue_key_force(scan, 2)) break;
    }}
    pc_held_key_count = 0;
}}

static void pc_release_pointer_latch(void) {{
    if (pc_pointer_right_down) {{
        pc_pointer_right_down = 0;
        (void)pc_queue_key_force(pc_pointer_right_scan, 2);
    }}
    pc_pointer_right_scan = 0;
    pc_pointer_down = 0;
    pc_pointer_dragged = 0;
}}

/* A context transition or focus loss cancels host activations not yet delivered: a click aimed
   at the scene must not arrive after an inventory/menu takes the input, and Alt+Tab must not
   leave a queued confirm behind. Physical releases are kept. */
static void pc_drop_pending_taps(void) {{
    unsigned keep = 0;
    for (unsigned i = 0; i < pc_pending_count; i++)
        if (!pc_pending_keys[i].host) pc_pending_keys[keep++] = pc_pending_keys[i];
    pc_pending_count = keep;
}}

/* Only context transitions and focus loss need this; they are rare, so the log stays readable. */
static void pc_input_isolate(void) {{
    pc_drop_pending_taps();
    pc_release_held_keys();
    pc_release_pointer_latch();
}}

static int pc_input_top(void) {{ return pc_input_stack[pc_input_depth ? pc_input_depth - 1 : 0]; }}

static int pc_input_set_context(int context) {{
    if (context < 0 || context >= PC_INPUT_CONTEXT_COUNT) return pc_input_top();
    int previous = pc_input_top();
    if (context == previous) return previous;
    pc_input_isolate();
    pc_input_stack[pc_input_depth - 1] = context;
    fprintf(stderr, "pc-input: context %s -> %s\\n",
            pc_input_context_names[previous], pc_input_context_names[context]);
    return previous;
}}

static int pc_input_push_context(int context) {{
    if (context < 0 || context >= PC_INPUT_CONTEXT_COUNT) return pc_input_top();
    if (context == pc_input_top()) return context;
    if (pc_input_depth >= PC_INPUT_CONTEXT_MAX) {{
        fputs("pc-input: context stack full; push ignored\\n", stderr);
        return pc_input_top();
    }}
    pc_input_isolate();
    pc_input_stack[pc_input_depth++] = context;
    fprintf(stderr, "pc-input: context push %s\\n", pc_input_context_names[context]);
    return context;
}}

static int pc_input_pop_context(void) {{
    if (pc_input_depth <= 1) return pc_input_top();
    pc_input_isolate();
    int context = pc_input_stack[--pc_input_depth];
    fprintf(stderr, "pc-input: context pop %s -> %s\\n",
            pc_input_context_names[context], pc_input_context_names[pc_input_top()]);
    return pc_input_top();
}}

static int pc_parse_bool(const char *text, int *out) {{
    if (!strcmp(text, "1") || !strcmp(text, "on") || !strcmp(text, "yes") || !strcmp(text, "true")) {{
        *out = 1;
        return 1;
    }}
    if (!strcmp(text, "0") || !strcmp(text, "off") || !strcmp(text, "no") || !strcmp(text, "false")) {{
        *out = 0;
        return 1;
    }}
    return 0;
}}

static int pc_parse_u32(const char *text, uint32_t *out) {{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end || errno || !value || (uint64_t)value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}}

static int pc_parse_scancode(const char *text, uint32_t *out) {{
    uint32_t value;
    if (!pc_parse_u32(text, &value) || value >= (uint32_t)SDL_NUM_SCANCODES) return 0;
    *out = value;
    return 1;
}}

static int pc_parse_context(const char *text, int *out) {{
    for (int i = 0; i < PC_INPUT_CONTEXT_COUNT; i++)
        if (!strcmp(text, pc_input_context_names[i])) {{
            *out = i;
            return 1;
        }}
    if (text[0] >= '0' && text[0] <= '9' && !text[1]) {{
        int value = text[0] - '0';
        if (value < PC_INPUT_CONTEXT_COUNT) {{
            *out = value;
            return 1;
        }}
    }}
    return 0;
}}

/* A PC action must not shadow a key that already carries a verified phone meaning. */
static int pc_scancode_reserved(uint32_t scancode) {{
    switch (scancode) {{
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_SPACE: case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_KP_1: case SDL_SCANCODE_KP_2: case SDL_SCANCODE_KP_3:
    case SDL_SCANCODE_KP_4: case SDL_SCANCODE_KP_5: case SDL_SCANCODE_KP_6:
    case SDL_SCANCODE_KP_7: case SDL_SCANCODE_KP_8: case SDL_SCANCODE_KP_9:
    case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_PAGEDOWN:
        return 1;
    default:
        return 0;
    }}
}}

/* Apply ISLAND_PC_INPUT once. Bad input falls back to the verified defaults and is reported but
   never fatal, so a typo cannot prevent startup. */
static void pc_input_configure(void) {{
    const char *spec = getenv(PC_INPUT_ENV);
    unsigned applied = 0, rejected = 0;
    if (spec && *spec) {{
        char copy[256];
        size_t length = strlen(spec);
        if (length >= sizeof copy) {{
            length = sizeof copy - 1;
            rejected++;
        }}
        memcpy(copy, spec, length);
        copy[length] = 0;
        for (char *token = strtok(copy, ",; \\t\\r\\n"); token; token = strtok(NULL, ",; \\t\\r\\n")) {{
            char *equals = strchr(token, '=');
            if (!equals) {{
                rejected++;
                continue;
            }}
            *equals = 0;
            const char *key = token, *value = equals + 1;
            uint32_t code;
            int ok = 0;
            if (!strcmp(key, "ok")) {{
                ok = pc_parse_u32(value, &code);
                if (ok) pc_cfg.ok_key = code;
            }} else if (!strcmp(key, "back")) {{
                ok = pc_parse_u32(value, &code);
                if (ok) pc_cfg.back_key = code;
            }} else if (!strcmp(key, "items")) {{
                ok = pc_parse_u32(value, &code);
                if (ok) pc_cfg.items_key = code;
            }} else if (!strcmp(key, "drag")) {{
                ok = pc_parse_u32(value, &code) && code <= 64;
                if (ok) pc_cfg.drag_px = (int32_t)code;
            }} else if (!strcmp(key, "pointer")) {{
                ok = pc_parse_bool(value, &pc_cfg.pointer_confirm);
            }} else if (!strcmp(key, "right")) {{
                ok = pc_parse_bool(value, &pc_cfg.right_button_back);
            }} else if (!strcmp(key, "tab")) {{
                ok = pc_parse_bool(value, &pc_cfg.tab_items);
            }} else if (!strcmp(key, "q")) {{
                ok = pc_parse_bool(value, &pc_cfg.q_select);
            }} else if (!strcmp(key, "e")) {{
                ok = pc_parse_bool(value, &pc_cfg.e_select);
            }} else if (!strcmp(key, "f")) {{
                ok = pc_parse_bool(value, &pc_cfg.f_context);
            }} else if (!strcmp(key, "tabkey")) {{
                ok = pc_parse_scancode(value, &pc_cfg.tab_scancode);
            }} else if (!strcmp(key, "qkey")) {{
                ok = pc_parse_scancode(value, &pc_cfg.q_scancode);
            }} else if (!strcmp(key, "ekey")) {{
                ok = pc_parse_scancode(value, &pc_cfg.e_scancode);
            }} else if (!strcmp(key, "fkey")) {{
                ok = pc_parse_scancode(value, &pc_cfg.f_scancode);
            }} else if (!strcmp(key, "context")) {{
                ok = pc_parse_context(value, &pc_cfg.initial_context);
            }}
            if (ok) applied++;
            else rejected++;
        }}
    }}
    if (pc_cfg.ok_key == pc_cfg.back_key || pc_cfg.ok_key == pc_cfg.items_key ||
        pc_cfg.back_key == pc_cfg.items_key) {{
        fputs("pc-input: softkey codes collide; restoring 0xa7/0xa5/0xa4\\n", stderr);
        pc_cfg.ok_key = 0xa7;
        pc_cfg.back_key = 0xa5;
        pc_cfg.items_key = 0xa4;
        rejected++;
    }}
    if (pc_cfg.tab_scancode == pc_cfg.q_scancode || pc_cfg.tab_scancode == pc_cfg.e_scancode ||
        pc_cfg.tab_scancode == pc_cfg.f_scancode || pc_cfg.q_scancode == pc_cfg.e_scancode ||
        pc_cfg.q_scancode == pc_cfg.f_scancode || pc_cfg.e_scancode == pc_cfg.f_scancode ||
        pc_scancode_reserved(pc_cfg.tab_scancode) || pc_scancode_reserved(pc_cfg.q_scancode) ||
        pc_scancode_reserved(pc_cfg.e_scancode) || pc_scancode_reserved(pc_cfg.f_scancode)) {{
        fputs("pc-input: action scancodes collide or shadow a phone key; restoring Tab/Q/E/F\\n", stderr);
        pc_cfg.tab_scancode = SDL_SCANCODE_TAB;
        pc_cfg.q_scancode = SDL_SCANCODE_Q;
        pc_cfg.e_scancode = SDL_SCANCODE_E;
        pc_cfg.f_scancode = SDL_SCANCODE_F;
        rejected++;
    }}
    (void)pc_input_set_context(pc_cfg.initial_context);
    fprintf(stderr, "pc-input: ready ok=0x%02x back=0x%02x items=0x%02x drag=%dpx "
            "pointer=%d right=%d tab=%d select=%d%d%d scancodes=%u/%u/%u/%u context=%s "
            "entries=%u rejected=%u source=%s\\n",
            (unsigned)pc_cfg.ok_key, (unsigned)pc_cfg.back_key, (unsigned)pc_cfg.items_key,
            (int)pc_cfg.drag_px, pc_cfg.pointer_confirm, pc_cfg.right_button_back, pc_cfg.tab_items,
            pc_cfg.q_select, pc_cfg.e_select, pc_cfg.f_context,
            (unsigned)pc_cfg.tab_scancode, (unsigned)pc_cfg.q_scancode,
            (unsigned)pc_cfg.e_scancode, (unsigned)pc_cfg.f_scancode,
            pc_input_context_names[pc_input_top()], applied, rejected,
            (spec && *spec) ? "env" : "default");
    pc_input_configured = 1;
}}

static void pc_input_install_hook(int context, int action, pc_input_hook hook) {{
    if (context >= 0 && context < PC_INPUT_CONTEXT_COUNT && action >= 0 && action < PC_ACTION_COUNT)
        pc_input_hooks[context][action] = hook;
}}

/* Default routes are the original key codes accepted for this build, scoped per context so a UI
   cannot fall through to a scene-only action (notably Tab opening inventory from inside a UI).
   Anything without a verified code (item selection, pointer move/scroll/drag) returns 0 unless a
   hook installed for that exact context supplies one. */
static uint32_t pc_input_default_route(int context, int action) {{
    switch (action) {{
    case PC_ACTION_TOGGLE_INVENTORY:
        return context == PC_INPUT_SCENE && pc_cfg.tab_items ? pc_cfg.items_key : 0;
    case PC_ACTION_POINTER_PRIMARY:
        return pc_cfg.pointer_confirm ? pc_cfg.ok_key : 0;
    case PC_ACTION_POINTER_SECONDARY:
        return pc_cfg.right_button_back ? pc_cfg.back_key : 0;
    default:
        return 0;
    }}
}}

static uint32_t pc_input_route(int action, int32_t x, int32_t y, int32_t detail) {{
    if (action < 0 || action >= PC_ACTION_COUNT) return 0;
    int context = pc_input_top();
    pc_input_hook hook = pc_input_hooks[context][action];
    if (hook) return hook(context, action, x, y, detail);
    return pc_input_default_route(context, action);
}}

static int pc_send_key(uint32_t *stack, uint32_t sp, uint32_t scan, uint32_t type) {{
    uint32_t key_event = sp - 16;
    guest_write(stack, key_event, 0); /* iCode is only set for EEventKey, which the game ignores. */
    guest_write(stack, key_event + 4, scan);
    guest_write(stack, key_event + 8, 0);  /* iModifiers */
    guest_write(stack, key_event + 12, 0); /* iRepeats */
    struct guest_thread nested = {{0}};
    nested.target = guest_read(stack, guest_read(stack, framework.control) + 0x0c);
    nested.r[0] = framework.control;
    nested.r[1] = key_event;
    nested.r[2] = type; /* EEventKeyDown/Up */
    nested.r[13] = key_event;
    nested.r[14] = GUEST_THREAD_SENTINEL;
    return guest_run(&nested, stack);
}}

/* Map a window-pixel pointer position into the 176x208 logical frame. The game window owns only
   the frame lease here; no pointer coordinates are written into guest memory. */
static void pc_logical_point(uint32_t window_id, int wx, int wy, int32_t *x, int32_t *y) {{
    int ww = window_width, wh = window_height;
    SDL_Window *window = SDL_GetWindowFromID(window_id);
    if (window) SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0) ww = window_width;
    if (wh <= 0) wh = window_height;
    int32_t lx = (int32_t)((int64_t)wx * window_width / ww);
    int32_t ly = (int32_t)((int64_t)wy * window_height / wh);
    *x = lx < 0 ? 0 : lx >= window_width ? window_width - 1 : lx;
    *y = ly < 0 ? 0 : ly >= window_height ? window_height - 1 : ly;
}}

/* Deliver one SDL transition per scheduler pass so a short tap reaches the guest update. Only
   matching down/up pairs are ever injected: physical keys keep their held state, synthetic taps
   are queued as a pair, and a latched pointer button releases the exact code it pressed. */
static int pump_window_input(uint32_t *stack, uint32_t sp) {{
    if (!framework.control || !SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    if (!pc_input_configured) pc_input_configure();

    /* Host-generated transitions (a click becomes an OK tap, Tab becomes the items softkey) are
       delivered one per scheduler pass, like physical key transitions. */
    if (pc_pending_count) {{
        struct pc_pending_key pending = pc_pending_keys[0];
        memmove(&pc_pending_keys[0], &pc_pending_keys[1],
                (pc_pending_count - 1) * sizeof pc_pending_keys[0]);
        pc_pending_count--;
        return pc_send_key(stack, sp, pending.scan, pending.type);
    }}

    SDL_Event event;
    SDL_PumpEvents();
    int queued;
    while ((queued = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) > 0) {{
        if (event.type == SDL_QUIT) return -1;
        if (event.type == SDL_WINDOWEVENT) {{
            /* Lost focus or a minimize must not leave a movement, confirm or pointer button
               latched; the releases are queued for the next scheduler pass. */
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                event.window.event == SDL_WINDOWEVENT_MINIMIZED) {{
                pc_input_isolate();
            }}
            continue;
        }}
        if (event.type == SDL_MOUSEMOTION) {{
            int32_t x, y;
            pc_logical_point(event.motion.windowID, event.motion.x, event.motion.y, &x, &y);
            pc_pointer_x = x;
            pc_pointer_y = y;
            if (pc_pointer_down) {{
                int32_t dx = x - pc_pointer_press_x, dy = y - pc_pointer_press_y;
                if (dx > pc_cfg.drag_px || dx < -pc_cfg.drag_px ||
                    dy > pc_cfg.drag_px || dy < -pc_cfg.drag_px) pc_pointer_dragged = 1;
            }}
            /* Cursor positioning has no verified guest field or hit-test; the hook decides. */
            (void)pc_input_route(PC_ACTION_POINTER_MOVE, x, y, 0);
            continue;
        }}
        if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {{
            int32_t x, y;
            pc_logical_point(event.button.windowID, event.button.x, event.button.y, &x, &y);
            pc_pointer_x = x;
            pc_pointer_y = y;
            if (event.button.button == SDL_BUTTON_LEFT) {{
                if (event.type == SDL_MOUSEBUTTONDOWN) {{
                    if (pc_pointer_down) continue; /* duplicate press: already tracked */
                    pc_pointer_down = 1;
                    pc_pointer_dragged = 0;
                    pc_pointer_press_x = x;
                    pc_pointer_press_y = y;
                }} else if (pc_pointer_down) {{
                    int32_t dx = x - pc_pointer_press_x, dy = y - pc_pointer_press_y;
                    int dragged = pc_pointer_dragged ||
                        dx > pc_cfg.drag_px || dx < -pc_cfg.drag_px ||
                        dy > pc_cfg.drag_px || dy < -pc_cfg.drag_px;
                    pc_pointer_down = 0;
                    pc_pointer_dragged = 0;
                    if (dragged) {{
                        /* Drag/drop needs the original two-slot combine path (unresolved). */
                        (void)pc_input_route(PC_ACTION_POINTER_DRAG, x, y, 0);
                    }} else {{
                        (void)pc_queue_tap(pc_input_route(PC_ACTION_POINTER_PRIMARY, x, y, 0));
                    }}
                }}
                continue;
            }}
            if (event.button.button == SDL_BUTTON_RIGHT) {{
                if (event.type == SDL_MOUSEBUTTONDOWN) {{
                    /* Latch the code chosen at press time and release exactly that code even if
                       the input context changed while the button was held. */
                    if (!pc_pointer_right_down) {{
                        uint32_t scan = pc_input_route(PC_ACTION_POINTER_SECONDARY, x, y, 0);
                        if (scan && pc_queue_key_force(scan, 3)) {{
                            pc_pointer_right_down = 1;
                            pc_pointer_right_scan = scan;
                        }}
                    }}
                }} else if (pc_pointer_right_down) {{
                    pc_pointer_right_down = 0;
                    (void)pc_queue_key_force(pc_pointer_right_scan, 2);
                    pc_pointer_right_scan = 0;
                }}
                continue;
            }}
            continue;
        }}
        if (event.type == SDL_MOUSEWHEEL) {{
            /* No original list-scroll input is proven; the hook decides once one is. */
            if (event.wheel.y)
                (void)pc_input_route(PC_ACTION_POINTER_SCROLL, pc_pointer_x, pc_pointer_y, event.wheel.y);
            continue;
        }}
        if ((event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) || event.key.repeat) continue;

        /* PC-only actions never fall through to the phone key table. Their scancodes are
           configurable; collisions and shadows were rejected when the layer was configured. */
        uint32_t scancode = (uint32_t)event.key.keysym.scancode;
        int action = -1;
        if (scancode == pc_cfg.q_scancode && pc_cfg.q_select) action = PC_ACTION_SELECT_PICKAXE;
        else if (scancode == pc_cfg.e_scancode && pc_cfg.e_select) action = PC_ACTION_SELECT_SLINGSHOT;
        else if (scancode == pc_cfg.f_scancode && pc_cfg.f_context) action = PC_ACTION_SELECT_CONTEXT_ITEM;
        else if (scancode == pc_cfg.tab_scancode && pc_cfg.tab_items) action = PC_ACTION_TOGGLE_INVENTORY;
        if (scancode == pc_cfg.q_scancode || scancode == pc_cfg.e_scancode ||
            scancode == pc_cfg.f_scancode || scancode == pc_cfg.tab_scancode) {{
            /* A disabled action stays swallowed rather than becoming a phone key. */
            if (event.type == SDL_KEYDOWN && action >= 0)
                (void)pc_queue_tap(pc_input_route(action, pc_pointer_x, pc_pointer_y, 0));
            continue;
        }}

        uint32_t scan;
        /* Physical keypad positions retain their phone mapping with Num Lock off. */
        switch (scancode) {{
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: scan = pc_cfg.items_key; break; /* left softkey */
        case SDL_SCANCODE_ESCAPE: scan = pc_cfg.back_key; break; /* right softkey */
        case SDL_SCANCODE_SPACE: case SDL_SCANCODE_RSHIFT: scan = pc_cfg.ok_key; break; /* D-Pad center / OK */
        case SDL_SCANCODE_LEFT:   scan = 0x0e; break;
        case SDL_SCANCODE_RIGHT:  scan = 0x0f; break;
        case SDL_SCANCODE_UP:     scan = 0x10; break;
        case SDL_SCANCODE_DOWN:   scan = 0x11; break;
        case SDL_SCANCODE_KP_7: scan = '1'; break;
        case SDL_SCANCODE_KP_8: scan = '2'; break;
        case SDL_SCANCODE_KP_9: scan = '3'; break;
        case SDL_SCANCODE_KP_4: scan = '4'; break;
        case SDL_SCANCODE_KP_5: scan = '5'; break;
        case SDL_SCANCODE_KP_6: scan = '6'; break;
        case SDL_SCANCODE_KP_1: scan = '7'; break;
        case SDL_SCANCODE_KP_2: scan = '8'; break;
        case SDL_SCANCODE_KP_3: scan = '9'; break;
        case SDL_SCANCODE_PAGEUP:   scan = 0x7f; break; /* # */
        case SDL_SCANCODE_PAGEDOWN: scan = 0x85; break; /* * */
        default: continue;
        }}
        uint32_t type = event.type == SDL_KEYDOWN ? 3 : 2; /* EEventKeyDown/Up */
        if (!pc_note_key(scan, type)) continue; /* duplicate down: already delivered */
        return pc_send_key(stack, sp, scan, type);
    }}
    if (queued < 0) {{ fprintf(stderr, "SDL keyboard events: %s\\n", SDL_GetError()); return 3; }}
    return 0;
}}

/* CleanupStack::PopAndDestroy(count): pops and destroys from the top. A CBase is deleted through its deleting
   destructor (vtable +4) and a TCleanupItem runs its operation, both as nested guest calls below sp; a TAny* is
   released by User::Free, which the non-freeing guest heap makes a no-op. */
static int cleanup_pop_and_destroy(uint32_t *stack, uint32_t count, uint32_t sp) {{
    if (count > cleanup_depth) {{
        fputs("guest cleanup stack underflow\\n", stderr);
        return 0;
    }}
    while (count--) {{
        uint32_t pointer = cleanup_items[cleanup_depth - 1].pointer;
        uint32_t operation = cleanup_items[cleanup_depth - 1].operation, kind = cleanup_items[cleanup_depth - 1].kind;
        cleanup_pop_count(1);
        if (kind == CLEANUP_CBASE && pointer &&
            !run_nested_event(stack, guest_read(stack, guest_read(stack, pointer) + 4), pointer, sp)) return 0;
        if (kind == CLEANUP_ITEM && !run_nested_event(stack, operation, pointer, sp)) return 0;
        if (kind == CLEANUP_READ_STREAM) read_stream_release(stack, pointer);
        if (kind == CLEANUP_WRITE_STREAM) write_stream_release(stack, pointer, 0);
    }}
    return 1;
}}

/* User::WaitForRequest(status) after DoCancel(): a deferred import completion is applied now. A timer that DoCancel
   left outstanding would block forever on a device, so it is reported instead. */
static int wait_for_request(uint32_t *stack, uint32_t status) {{
    for (uint32_t i = 0; i < deferred_count; i++)
        if (deferred_status[i] == status) {{
            guest_write(stack, status, (uint32_t)deferred_value[i]);
            deferred_count--;
            deferred_status[i] = deferred_status[deferred_count];
            deferred_value[i] = deferred_value[deferred_count];
            return 1;
        }}
    for (uint32_t i = 0; i < framework.next_timer; i++)
        if (timers[i].status == status) {{
            fprintf(stderr, "DoCancel left timer %u outstanding (status 0x%08x)\\n", i + 1, status);
            return 0;
        }}
    return 1;
}}

/* CActive::Cancel() (ub_act.cpp): if EActive, DoCancel() (vtable +12), wait for the request, clear EActive and
   ERequestPending. DoCancel runs as a nested guest call below the caller's sp. */
static int cancel_active(uint32_t *stack, uint32_t object, uint32_t sp) {{
    if (!(guest_read(stack, object + 8) & 1)) return 1;
    if (!run_nested_event(stack, guest_read(stack, guest_read(stack, object) + 12), object, sp) ||
        !wait_for_request(stack, object + 4)) return 0;
    guest_write(stack, object + 8, guest_read(stack, object + 8) & ~3u);
    return 1;
}}

/* CActive::~CActive() (ub_act.cpp): an active request panics; an added object leaves the scheduler queue. */
static int remove_active(uint32_t *stack, uint32_t object) {{
    uint32_t link = object + 12, next = guest_read(stack, link);
    if (guest_read(stack, object + 8) & 1) return 0;
    if (!next) return 1;
    uint32_t prev = guest_read(stack, link + 4);
    guest_write(stack, prev, next);
    guest_write(stack, next + 4, prev);
    guest_write(stack, link, 0);
    guest_write(stack, link + 4, 0);
    return 1;
}}

/* Host CPeriodic DoCancel: CTimer::DoCancel() calls RTimer::Cancel(), which completes an outstanding
   request with KErrCancel and leaves none for User::WaitForRequest after CActive::Cancel(). */
static int periodic_cancel(uint32_t *stack, uint32_t object) {{
    uint32_t id = guest_read(stack, object + 28), status = object + 4;
    if (!id || id > framework.next_timer) return 1;
    if (timers[id - 1].status == status) {{
        timers[id - 1].status = 0;
        guest_write(stack, status, (uint32_t)-3);
        guest_write(stack, status + 4, guest_read(stack, status + 4) & ~2u);
    }}
    if (timers[id - 1].periodic == object) timers[id - 1].periodic = 0;
    return 1;
}}

/* Host CPeriodic destructor: cancel the timer (as CTimer does), then leave the scheduler queue. */
static int periodic_destroy(uint32_t *stack, uint32_t object) {{
    uint32_t flags = guest_read(stack, object + 8);
    if (flags & 1) {{
        if (!periodic_cancel(stack, object)) return 0;
        guest_write(stack, object + 8, flags & ~3u);
    }}
    return remove_active(stack, object);
}}

static int worker_scheduler_construct(uint32_t *stack, uint32_t object) {{
    static uint32_t vtable;
    if (!current_worker || !object) return 0;
    if (!vtable) {{
        vtable = heap_try_alloc(8);
        if (!vtable) return 0;
        guest_write(stack, vtable, WORKER_SCHEDULER_DESTROY);
        guest_write(stack, vtable + 4, WORKER_SCHEDULER_DESTROY);
    }}
    guest_write(stack, object, vtable);
    return 1;
}}

static int worker_scheduler_install(uint32_t *stack, uint32_t object) {{
    if (!current_worker || current_worker->scheduler_running || (object && current_worker->scheduler)) return 0;
    (void)stack;
    current_worker->scheduler = object;
    return 1;
}}

static int worker_scheduler_destroy(uint32_t *stack, uint32_t object) {{
    if (!current_worker || !object || current_worker->scheduler_running ||
        (current_worker->scheduler && current_worker->scheduler != object)) return 0;
    uint32_t head = current_worker->active_queue;
    while (head && guest_read(stack, head) != head)
        if (!remove_active(stack, guest_read(stack, head) - 12)) return 0;
    current_worker->scheduler = current_worker->active_queue = 0;
    return 1;
}}

static int worker_scheduler_step(uint32_t *stack, struct guest_thread *ctx) {{
    complete_deferred(stack);
    if (!audio_dispatch(stack, ctx)) return 0;
    uint32_t head = ctx->active_queue;
    if (!head) return 1;
    for (uint32_t link = guest_read(stack, head); link != head; link = guest_read(stack, link)) {{
        uint32_t object = link - 12, flags = guest_read(stack, object + 8);
        if ((flags & 3) != 1 || guest_read(stack, object + 4) == UINT32_C(0x80000001)) continue;
        guest_write(stack, object + 8, flags & ~1u);
        return run_nested_event(stack, guest_read(stack, guest_read(stack, object) + 16), object, ctx->r[13]);
    }}
    return 1;
}}

static int next_timer_event(uint32_t *stack, uint32_t *argument, uint32_t *target) {{
    complete_deferred(stack);
    for (;;) {{
        uint64_t earliest = UINT64_MAX;
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
        uint64_t us = (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
        for (uint32_t i = 0; i < framework.next_timer; i++) {{
            uint32_t status = timers[i].status;
            if (!status) continue;
            if (timers[i].deadline <= us) {{
                guest_write(stack, status, 0);
                guest_write(stack, status + 4, guest_read(stack, status + 4) & ~2u);
                timers[i].status = 0;
            }} else if (timers[i].deadline < earliest) earliest = timers[i].deadline;
        }}
        uint32_t head = framework.active_queue;
        if (!head) return 0;
        for (uint32_t link = guest_read(stack, head); link != head; link = guest_read(stack, link)) {{
            uint32_t object = link - 12, flags = guest_read(stack, object + 8);
            if ((flags & 3) != 1) continue;
            guest_write(stack, object + 8, flags & ~1u);
            uint32_t id = guest_read(stack, object + 28);
            if (id && id <= framework.next_timer && timers[id - 1].periodic == object) {{
                if (!schedule_timer(stack, object + 28, object + 4, guest_read(stack, object + 32)) ||
                    !set_active(stack, object)) return 0;
                *argument = guest_read(stack, object + 40);
                *target = guest_read(stack, object + 36);
            }} else {{
                *argument = object;
                *target = guest_read(stack, guest_read(stack, object) + 16);
            }}
            return 1;
        }}
        if (earliest == UINT64_MAX) {{ fputs("no pending timer events\\n", stderr); return 0; }}
        struct timespec wake = {{ .tv_sec = earliest / 1000000, .tv_nsec = earliest % 1000000 * 1000 }};
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
        if (rc && rc != EINTR) return 0;
    }}
}}

static int unhandled_leave(uint32_t error, uint32_t caller) {{
    fprintf(stderr, "unhandled Symbian leave 0x%08x at 0x%08x\\n", error, caller);
    return 2;
}}

static int construct_control(uint32_t *stack, uint32_t control) {{
    if (!framework.control_stack || framework.control || !heap_word(control + 48)) return 0;
    /* The game's S60 control ABI has iCoeEnv at +12 (used by the
       guest's inline ControlEnv access). Default controls are focusable. */
    guest_write(stack, control + 12, framework.coe_env);
    guest_write(stack, control + 44, heap_alloc_zeroed(4)); /* Opaque CCoeControlStorage. */
    framework.control = control;
    return 1;
}}

static void set_extent(uint32_t *stack, uint32_t control, const uint32_t rect[4]) {{
    guest_write(stack, control + 20, rect[0]);
    guest_write(stack, control + 24, rect[1]);
    guest_write(stack, control + 28, rect[2] - rect[0]);
    guest_write(stack, control + 32, rect[3] - rect[1]);
}}

static int create_control_window(uint32_t *stack, uint32_t control) {{
    if (control != framework.control || framework.window || !framework.window_group_uid) return 0;
    /* RWindow identity; its drawing and visibility are owned by the host. */
    framework.window = heap_alloc_zeroed(4);
    guest_write(stack, control + 40, framework.window);
    guest_write(stack, control + 36, guest_read(stack, control + 36) | 4); /* EOwnsWindow */
    return 1;
}}

static int activate_control(uint32_t *stack, uint32_t control) {{
    if (control != framework.control || !framework.window ||
        framework.window_rect[2] <= framework.window_rect[0] ||
        framework.window_rect[3] <= framework.window_rect[1]) return 0;
    /* This control owns a normal (not backed-up) window and has no children. */
    uint32_t flags = guest_read(stack, control + 36);
    if (flags != 4) return 0;
    guest_write(stack, control + 36, flags | 0x10); /* EActivated */
    return 1;
}}

static uint32_t capture_key(uint32_t group, uint32_t key, uint32_t mask, uint32_t modifiers) {{
    if (group != framework.coe_env + 0x38 || !framework.window_group_uid ||
        capture_count >= sizeof captures / sizeof captures[0]) return 0;
    captures[capture_count].key = key;
    captures[capture_count].mask = mask;
    captures[capture_count].modifiers = modifiers;
    return ++capture_count;
}}

static void init_exception_globals(uint32_t *stack, uint32_t address) {{
    uint32_t offset = address - stack_base;
    if (offset > stack_size - 40 || offset % 8 || exception_tls) {{
        fputs("invalid exception globals\\n", stderr);
        exit(3);
    }}
    uint32_t *globals = stack + offset / 4;
    globals[0] = 0;                      /* uncaughtExceptions */
    globals[1] = UINT32_C(0xff000004);    /* guest default unexpected handler */
    globals[2] = UINT32_C(0xff000008);    /* guest default terminate handler */
    globals[3] = 0;                      /* termination state */
    globals[4] = 0;                      /* call hook */
    globals[5] = 0;                      /* caught chain */
    globals[6] = 0;                      /* propagating chain */
    globals[7] = address + 32;           /* emergency buffer */
    globals[8] = 0;                      /* not in use */
    globals[9] = 0;                      /* no second emergency buffer */
    exception_tls = address;
}}

static void init_process(void) {{
    if (process_state != PROCESS_BOOTING || !exception_tls || !thread_heap.backing) {{
        fputs("process startup prerequisites not met\\n", stderr);
        exit(3);
    }}
    /* The image imports only platform DLLs. Implemented services are native
       and initialized above; there are no statically attached guest DLLs. */
    process_state = PROCESS_INITIALIZED;
}}

static int missing_import(uint32_t pc, const char *dll, uint32_t ordinal, const uint32_t r[16]) {{
    fprintf(stderr, "unimplemented Symbian import at 0x%08x: %s ordinal %u (r0=%u, r1=0x%08x, lr=0x%08x)\\n",
            pc, dll, ordinal, r[0], r[1], r[14]);
    return 2;
}}

static int unsupported(uint32_t pc, uint32_t op) {{
    fprintf(stderr, "untranslated instruction at 0x%08x: 0x%08x\\n", pc, op);
    return 3;
}}

static const char *resource_root;

#if {int(bool(app_profile))}
static int guest_run(struct guest_thread *ctx, uint32_t *stack) {{
    uint32_t *r = ctx->r;
#define target (ctx->target) /* the per-thread dispatch target lives in the context */
    int z = 0, c = 0, n = 0, v = 0;
    uint32_t a = 0, b = 0, result = 0;
    uint64_t wide = 0;
    uint32_t address = 0;
    (void)n; (void)v; (void)a; (void)b; (void)result; (void)wide;
    (void)c;
    (void)address;
    (void)guest_read;
    (void)guest_write;
    (void)guest_read_small;
    (void)guest_write_small;
    (void)guest_read64;
    (void)home_time;
    (void)time_date_time;
    (void)start_application;
    (void)construct_eik_application;
    (void)finish_factory;
    (void)verify_factory;
    (void)pre_doc_construct;
    (void)construct_akn_document;
    (void)cleanup_push;
    (void)cleanup_pop;
    (void)verify_document;
    (void)construct_akn_appui;
    (void)bind_appui;
    (void)base_construct_appui;
    (void)screen_font;
    (void)font_spec_in_twips;
    (void)construct_control;
    (void)set_extent;
    (void)create_control_window;
    (void)activate_control;
    (void)capture_key;
    (void)add_active;
    (void)create_timer;
    (void)new_periodic;
    (void)start_periodic;
    (void)next_timer_event;
    (void)unhandled_leave;
    (void)heap_alloc_zeroed;
    (void)stack_write;
    (void)setup_thread_heap;
    (void)init_exception_globals;
    (void)init_process;
    (void)missing_import;
    (void)thread_create; (void)thread_resume; (void)thread_kill; (void)thread_close;
    {"(void)load_library; (void)lookup_export;" if game_dlls else ""}
    goto dispatch;
{body}
#undef target
    return 0;
}}
#endif

int main(int argc, char **argv) {{
    resource_root = argc > 1 ? argv[1] : NULL;
#if {int(bool(app_profile))}
    if (argc > 4 || (argc == 4 && strcmp(argv[3], "--first-frame"))) {{
        fprintf(stderr, "usage: %s RESOURCE_ROOT [SIMLIFE_ROOT [--first-frame]]\\n", argv[0]);
        return 2;
    }}
    simlife_root = argc > 2 ? argv[2] : NULL;
    install_root = resource_root; /* RESOURCE_ROOT is the installation drive */
    hold_first_frame = argc == 4;
#endif
    uint32_t stack[stack_size / 4] = {{0}};
#if {int(bool(app_profile))}
    struct guest_thread main_thread;
    memset(&main_thread, 0, sizeof main_thread);
    main_thread.entry = {entry};
    main_thread.stack_size = {stack_size};
    main_thread.target = {entry};
    main_thread.r[0] = {2 if attach else 0};
    main_thread.r[4] = {reason};
    main_thread.r[13] = stack_base + stack_size - thread_info_size;
    main_thread.r[14] = {0xff000000 if attach else 0};
    uint32_t *info = &stack[(main_thread.r[13] - stack_base) / 4];
#else
    uint32_t r[16] = {{ [0] = {2 if attach else 0}, [4] = {reason},
        [13] = stack_base + stack_size - thread_info_size, [14] = {0xff000000 if attach else 0} }};
    uint32_t *info = &stack[(r[13] - stack_base) / 4];
#endif
    info[6] = stack_base;
    info[7] = stack_size;
    info[11] = thread_info_size;
    info[13] = {image.heap_min};
    info[14] = {image.heap_max};
#if {int(bool(app_profile))}
    (void)resource_root;
    int status = guest_run(&main_thread, stack);
    audio_destroy();
    return status;
#else
    int z = 0, c = 0, n = 0, v = 0;
    uint32_t a = 0, b = 0, result = 0;
    uint64_t wide = 0;
    (void)n; (void)v; (void)a; (void)b; (void)result; (void)wide;
    uint32_t target = {entry};
    uint32_t address = 0;
    (void)c;
    (void)address;
    (void)guest_read;
    (void)guest_write;
    (void)guest_read_small;
    (void)guest_write_small;
    (void)guest_read64;
    (void)home_time;
    (void)time_date_time;
    (void)start_application;
    (void)construct_eik_application;
    (void)finish_factory;
    (void)verify_factory;
    (void)pre_doc_construct;
    (void)construct_akn_document;
    (void)cleanup_push;
    (void)cleanup_pop;
    (void)verify_document;
    (void)construct_akn_appui;
    (void)bind_appui;
    (void)base_construct_appui;
    (void)screen_font;
    (void)font_spec_in_twips;
    (void)construct_control;
    (void)set_extent;
    (void)create_control_window;
    (void)activate_control;
    (void)capture_key;
    (void)add_active;
    (void)create_timer;
    (void)new_periodic;
    (void)start_periodic;
    (void)next_timer_event;
    (void)unhandled_leave;
    (void)resource_root;
    (void)heap_alloc_zeroed;
    (void)stack_write;
    (void)setup_thread_heap;
    (void)init_exception_globals;
    (void)init_process;
    (void)missing_import;
    {"(void)load_library; (void)lookup_export;" if game_dlls else ""}
    if (r[13] == 0) return unsupported(0, 0);
{body}
#endif
}}
'''


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--reason', type=int, default=0)
    parser.add_argument('--attach', action='store_true', help='probe a DLL process-attach entry in isolation')
    parser.add_argument('--base', type=lambda v: int(v, 0), help='guest code base')
    parser.add_argument('--game-dlls', type=Path, metavar='DIR', help='compile both local game DLLs into the EXE build')
    args = parser.parse_args()
    image = Image(args.image.read_bytes(), args.base)
    dlls = []
    if args.game_dlls:
        if args.attach or image.u32(8) != 0x2000bbe5:
            parser.error('--game-dlls requires the Lament Island EXE')
        for name, uid, base in (('SimDeviceEngineDll.dll', 0x2000bbe7, 0x200000),
                                ('SimMediaEngineDll.dll', 0x2000bbe6, 0x300000)):
            dll = Image((args.game_dlls / name).read_bytes(), base)
            if dll.u32(8) != uid or any(dll.u32(offset) for offset in (0x34, 0x44)):
                raise ValueError(f'unexpected DLL UID or writable image segments: {name}')
            dlls.append((name, dll))
    args.output.write_text(lift(image, image.entry, args.reason, args.attach, dlls))
