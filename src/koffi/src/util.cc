// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "src/core/libcc/libcc.hh"
#include "call.hh"
#include "ffi.hh"
#include "util.hh"

#include <napi.h>

namespace RG {

const TypeInfo *ResolveType(Napi::Value value, int *out_directions)
{
    Napi::Env env = value.Env();
    InstanceData *instance = env.GetInstanceData<InstanceData>();

    if (value.IsString()) {
        std::string str = value.As<Napi::String>();
        const TypeInfo *type = ResolveType(instance, str.c_str(), out_directions);

        if (!type) {
            ThrowError<Napi::TypeError>(env, "Unknown or invalid type name '%1'", str.c_str());
            return nullptr;
        }

        return type;
    } else if (CheckValueTag(instance, value, &TypeInfoMarker)) {
        Napi::External<TypeInfo> external = value.As<Napi::External<TypeInfo>>();

        const TypeInfo *raw = external.Data();
        const TypeInfo *type = AlignDown(raw, 4);
        RG_ASSERT(type);

        if (out_directions) {
            Size delta = (uint8_t *)raw - (uint8_t *)type;
            *out_directions = 1 + (int)delta;
        }
        return type;
    } else {
        ThrowError<Napi::TypeError>(env, "Unexpected %1 value as type specifier, expected string or type", GetValueType(instance, value));
        return nullptr;
    }
}

const TypeInfo *ResolveType(InstanceData *instance, Span<const char> str, int *out_directions)
{
    Span<const char> remain = TrimStr(str);

    int indirect = 0;
    bool dispose = false;

    while (remain.len >= 6 && StartsWith(remain, "const") && IsAsciiWhite(remain[5])) {
        remain = remain.Take(6, remain.len - 6);
        remain = TrimStr(remain);
    }
    if (remain.len && remain[remain.len - 1] == '!') {
        dispose = true;

        remain = remain.Take(0, remain.len - 1);
        remain = TrimStr(remain);
    }
    while (remain.len) {
        if (remain[remain.len - 1] == '*') {
            remain = remain.Take(0, remain.len - 1);
            indirect++;
        } else if (remain.len >= 6 && EndsWith(remain, "const") && IsAsciiWhite(remain[remain.len - 6])) {
            remain = remain.Take(0, remain.len - 6);
        } else {
            break;
        }
        remain = TrimStr(remain);
    }

    const TypeInfo *type = instance->types_map.FindValue(remain, nullptr);

    if (!type) {
        // Try with cleaned up spaces
        if (remain.len < 256) {
            LocalArray<char, 256> buf;
            for (Size i = 0; i < remain.len; i++) {
                char c = remain[i];

                if (IsAsciiWhite(c)) {
                    buf.Append(' ');
                    while (++i < remain.len && IsAsciiWhite(remain[i]));
                    i--;
                } else {
                    buf.Append(c);
                }
            }

            type = instance->types_map.FindValue(buf, nullptr);
        }

        if (!type)
            return nullptr;
    }

    if (indirect) {
        type = MakePointerType(instance, type, indirect);
        RG_ASSERT(type);
    }

    if (dispose) {
        if (type->primitive != PrimitiveKind::String &&
                type->primitive != PrimitiveKind::String16 &&
                indirect != 1)
            return nullptr;

        TypeInfo *copy = instance->types.AppendDefault();

        memcpy((void *)copy, (const void *)type, RG_SIZE(*type));
        copy->name = "<anonymous>";
        copy->members.allocator = GetNullAllocator();
        copy->dispose = [](Napi::Env, const TypeInfo *, const void *ptr) { free((void *)ptr); };

        type = copy;
    }

    if (out_directions) {
        *out_directions = 1;
    }
    return type;
}

const TypeInfo *MakePointerType(InstanceData *instance, const TypeInfo *ref, int count)
{
    RG_ASSERT(count >= 1);

    for (int i = 0; i < count; i++) {
        char name_buf[256];
        Fmt(name_buf, "%1%2*", ref->name, EndsWith(ref->name, "*") ? "" : " ");

        TypeInfo *type = (TypeInfo *)instance->types_map.FindValue(name_buf, nullptr);

        if (!type) {
            type = instance->types.AppendDefault();

            type->name = DuplicateString(name_buf, &instance->str_alloc).ptr;

            if (ref->primitive != PrimitiveKind::Prototype) {
                type->primitive = PrimitiveKind::Pointer;
                type->size = RG_SIZE(void *);
                type->align = RG_SIZE(void *);
                type->ref.type = ref;
            } else {
                type->primitive = PrimitiveKind::Callback;
                type->size = RG_SIZE(void *);
                type->align = RG_SIZE(void *);
                type->ref.proto = ref->ref.proto;
            }

            instance->types_map.Set(type->name, type);
        }

        ref = type;
    }

    return ref;
}

static const TypeInfo *MakeArrayType(InstanceData *instance, const TypeInfo *ref, Size len,
                                     TypeInfo::ArrayHint hint, bool insert)
{
    RG_ASSERT(len > 0);
    RG_ASSERT(len <= instance->max_type_size / ref->size);

    TypeInfo *type = instance->types.AppendDefault();

    type->name = Fmt(&instance->str_alloc, "%1[%2]", ref->name, len).ptr;

    type->primitive = PrimitiveKind::Array;
    type->align = ref->align;
    type->size = (int32_t)(len * ref->size);
    type->ref.type = ref;
    type->hint = hint;

    if (insert) {
        bool inserted;
        type = (TypeInfo *)*instance->types_map.TrySet(type->name, type, &inserted);
        instance->types.RemoveLast(!inserted);
    }

    return type;
}

const TypeInfo *MakeArrayType(InstanceData *instance, const TypeInfo *ref, Size len)
{
    TypeInfo::ArrayHint hint = {};

    if (TestStr(ref->name, "char") || TestStr(ref->name, "char16") ||
                                             TestStr(ref->name, "char16_t")) {
        hint = TypeInfo::ArrayHint::String;
    } else {
        hint = TypeInfo::ArrayHint::TypedArray;
    }

    return MakeArrayType(instance, ref, len, hint, true);
}

const TypeInfo *MakeArrayType(InstanceData *instance, const TypeInfo *ref, Size len, TypeInfo::ArrayHint hint)
{
    return MakeArrayType(instance, ref, len, hint, false);
}

bool CanPassType(const TypeInfo *type, int directions)
{
    if (directions & 2) {
        if (type->primitive == PrimitiveKind::Pointer)
            return true;
        if (type->primitive == PrimitiveKind::String)
            return true;
        if (type->primitive == PrimitiveKind::String16)
            return true;

        return false;
    } else {
        if (type->primitive == PrimitiveKind::Void)
            return false;
        if (type->primitive == PrimitiveKind::Array)
            return false;
        if (type->primitive == PrimitiveKind::Prototype)
            return false;

        return true;
    }
}

bool CanReturnType(const TypeInfo *type)
{
    if (type->primitive == PrimitiveKind::Void && !TestStr(type->name, "void"))
        return false;
    if (type->primitive == PrimitiveKind::Array)
        return false;
    if (type->primitive == PrimitiveKind::Prototype)
        return false;

    return true;
}

bool CanStoreType(const TypeInfo *type)
{
    if (type->primitive == PrimitiveKind::Void)
        return false;
    if (type->primitive == PrimitiveKind::Prototype)
        return false;

    return true;
}

const char *GetValueType(const InstanceData *instance, Napi::Value value)
{
    if (CheckValueTag(instance, value, &CastMarker)) {
        Napi::External<ValueCast> external = value.As<Napi::External<ValueCast>>();
        ValueCast *cast = external.Data();

        return cast->type->name;
    }

    if (CheckValueTag(instance, value, &TypeInfoMarker))
        return "Type";
    for (const TypeInfo &type: instance->types) {
        if (type.ref.marker && CheckValueTag(instance, value, type.ref.marker))
            return type.name;
    }

    if (value.IsArray()) {
        return "Array";
    } else if (value.IsTypedArray()) {
        Napi::TypedArray array = value.As<Napi::TypedArray>();

        switch (array.TypedArrayType()) {
            case napi_int8_array: return "Int8Array";
            case napi_uint8_array: return "Uint8Array";
            case napi_uint8_clamped_array: return "Uint8ClampedArray";
            case napi_int16_array: return "Int16Array";
            case napi_uint16_array: return "Uint16Array";
            case napi_int32_array: return "Int32Array";
            case napi_uint32_array: return "Uint32Array";
            case napi_float32_array: return "Float32Array";
            case napi_float64_array: return "Float64Array";
            case napi_bigint64_array: return "BigInt64Array";
            case napi_biguint64_array: return "BigUint64Array";
        }
    } else if (value.IsArrayBuffer()) {
        return "ArrayBuffer";
    } else if (value.IsBuffer()) {
        return "Buffer";
    }

    switch (value.Type()) {
        case napi_undefined: return "Undefined";
        case napi_null: return "Null";
        case napi_boolean: return "Boolean";
        case napi_number: return "Number";
        case napi_string: return "String";
        case napi_symbol: return "Symbol";
        case napi_object: return "Object";
        case napi_function: return "Function";
        case napi_external: return "External";
        case napi_bigint: return "BigInt";
    }

    // This should not be possible, but who knows...
    return "Unknown";
}

void SetValueTag(const InstanceData *instance, Napi::Value value, const void *marker)
{
    RG_ASSERT(marker);

    napi_type_tag tag = { instance->tag_lower, (uint64_t)marker };
    napi_status status = napi_type_tag_object(value.Env(), value, &tag);
    RG_ASSERT(status == napi_ok);
}

bool CheckValueTag(const InstanceData *instance, Napi::Value value, const void *marker)
{
    RG_ASSERT(marker);

    bool match = false;

    if (!IsNullOrUndefined(value)) {
        napi_type_tag tag = { instance->tag_lower, (uint64_t)marker };
        napi_check_object_type_tag(value.Env(), value, &tag, &match);
    }

    return match;
}

int GetTypedArrayType(const TypeInfo *type)
{
    switch (type->primitive) {
        case PrimitiveKind::Int8: return napi_int8_array;
        case PrimitiveKind::UInt8: return napi_uint8_array;
        case PrimitiveKind::Int16: return napi_int16_array;
        case PrimitiveKind::UInt16: return napi_uint16_array;
        case PrimitiveKind::Int32: return napi_int32_array;
        case PrimitiveKind::UInt32: return napi_uint32_array;
        case PrimitiveKind::Float32: return napi_float32_array;
        case PrimitiveKind::Float64: return napi_float64_array;

        default: return -1;
    }

    RG_UNREACHABLE();
}

Napi::Object DecodeObject(Napi::Env env, const uint8_t *origin, const TypeInfo *type, int16_t realign)
{
    Napi::Object obj = Napi::Object::New(env);
    DecodeObject(obj, origin, type, realign);
    return obj;
}

void DecodeObject(Napi::Object obj, const uint8_t *origin, const TypeInfo *type, int16_t realign)
{
    Napi::Env env = obj.Env();
    InstanceData *instance = env.GetInstanceData<InstanceData>();

    RG_ASSERT(type->primitive == PrimitiveKind::Record);

    for (Size i = 0; i < type->members.len; i++) {
        const RecordMember &member = type->members[i];

        Size offset = realign ? (i * realign) : member.offset;
        const uint8_t *src = origin + offset;

        switch (member.type->primitive) {
            case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

            case PrimitiveKind::Bool: {
                bool b = *(bool *)src;
                obj.Set(member.name, Napi::Boolean::New(env, b));
            } break;
            case PrimitiveKind::Int8: {
                double d = (double)*(int8_t *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::UInt8: {
                double d = (double)*(uint8_t *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::Int16: {
                double d = (double)*(int16_t *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::Int16S: {
                int16_t v = *(int16_t *)src;
                double d = (double)ReverseBytes(v);

                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::UInt16: {
                double d = (double)*(uint16_t *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::UInt16S: {
                uint16_t v = *(uint16_t *)src;
                double d = (double)ReverseBytes(v);

                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::Int32: {
                double d = (double)*(int32_t *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::Int32S: {
                int32_t v = *(int32_t *)src;
                double d = (double)ReverseBytes(v);

                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::UInt32: {
                double d = (double)*(uint32_t *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::UInt32S: {
                uint32_t v = *(uint32_t *)src;
                double d = (double)ReverseBytes(v);

                obj.Set(member.name, Napi::Number::New(env, d));
            } break;
            case PrimitiveKind::Int64: {
                int64_t v = *(int64_t *)src;
                obj.Set(member.name, NewBigInt(env, v));
            } break;
            case PrimitiveKind::Int64S: {
                int64_t v = ReverseBytes(*(int64_t *)src);
                obj.Set(member.name, NewBigInt(env, v));
            } break;
            case PrimitiveKind::UInt64: {
                uint64_t v = *(uint64_t *)src;
                obj.Set(member.name, NewBigInt(env, v));
            } break;
            case PrimitiveKind::UInt64S: {
                uint64_t v = ReverseBytes(*(uint64_t *)src);
                obj.Set(member.name, NewBigInt(env, v));
            } break;
            case PrimitiveKind::String: {
                const char *str = *(const char **)src;
                obj.Set(member.name, str ? Napi::String::New(env, str) : env.Null());

                if (member.type->dispose) {
                    member.type->dispose(env, member.type, str);
                }
            } break;
            case PrimitiveKind::String16: {
                const char16_t *str16 = *(const char16_t **)src;
                obj.Set(member.name, str16 ? Napi::String::New(env, str16) : env.Null());

                if (member.type->dispose) {
                    member.type->dispose(env, member.type, str16);
                }
            } break;
            case PrimitiveKind::Pointer:
            case PrimitiveKind::Callback: {
                void *ptr2 = *(void **)src;

                if (ptr2) {
                    Napi::External<void> external = Napi::External<void>::New(env, ptr2);
                    SetValueTag(instance, external, member.type->ref.marker);

                    obj.Set(member.name, external);
                } else {
                    obj.Set(member.name, env.Null());
                }

                if (member.type->dispose) {
                    member.type->dispose(env, member.type, ptr2);
                }
            } break;
            case PrimitiveKind::Record: {
                Napi::Object obj2 = DecodeObject(env, src, member.type, realign);
                obj.Set(member.name, obj2);
            } break;
            case PrimitiveKind::Array: {
                Napi::Value value = DecodeArray(env, src, member.type, realign);
                obj.Set(member.name, value);
            } break;
            case PrimitiveKind::Float32: {
                float f = *(float *)src;
                obj.Set(member.name, Napi::Number::New(env, (double)f));
            } break;
            case PrimitiveKind::Float64: {
                double d = *(double *)src;
                obj.Set(member.name, Napi::Number::New(env, d));
            } break;

            case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
        }
    }
}

static Size WideStringLength(const char16_t *str16, Size max)
{
    Size len = 0;

    while (len < max && str16[len]) {
        len++;
    }

    return len;
}

Napi::Value DecodeArray(Napi::Env env, const uint8_t *origin, const TypeInfo *type, int16_t realign)
{
    InstanceData *instance = env.GetInstanceData<InstanceData>();

    RG_ASSERT(type->primitive == PrimitiveKind::Array);

    uint32_t len = type->size / type->ref.type->size;
    Size offset = 0;

#define POP_ARRAY(SetCode) \
        do { \
            Napi::Array array = Napi::Array::New(env); \
             \
            for (uint32_t i = 0; i < len; i++) { \
                int16_t align = std::max(realign, type->ref.type->align); \
                offset = AlignLen(offset, align); \
                 \
                const uint8_t *src = origin + offset; \
                 \
                SetCode \
                 \
                offset += type->ref.type->size; \
            } \
             \
            return array; \
        } while (false)
#define POP_NUMBER_ARRAY(TypedArrayType, CType) \
        do { \
            if (type->hint == TypeInfo::ArrayHint::Array) { \
                POP_ARRAY({ \
                    double d = (double)*(CType *)src; \
                    array.Set(i, Napi::Number::New(env, d)); \
                }); \
            } else { \
                Napi::TypedArrayType array = Napi::TypedArrayType::New(env, len); \
                Span<uint8_t> buffer = MakeSpan((uint8_t *)array.ArrayBuffer().Data(), (Size)len * RG_SIZE(CType)); \
                 \
                DecodeBuffer(buffer, origin, type->ref.type, realign); \
                 \
                return array; \
            } \
        } while (false)
#define POP_NUMBER_ARRAY_SWAP(TypedArrayType, CType) \
        do { \
            if (type->hint == TypeInfo::ArrayHint::Array) { \
                POP_ARRAY({ \
                    CType v = *(CType *)src; \
                    double d = (double)ReverseBytes(v); \
                    array.Set(i, Napi::Number::New(env, d)); \
                }); \
            } else { \
                Napi::TypedArrayType array = Napi::TypedArrayType::New(env, len); \
                Span<uint8_t> buffer = MakeSpan((uint8_t *)array.ArrayBuffer().Data(), (Size)len * RG_SIZE(CType)); \
                 \
                DecodeBuffer(buffer, origin, type->ref.type, realign); \
                 \
                return array; \
            } \
        } while (false)

    switch (type->ref.type->primitive) {
        case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

        case PrimitiveKind::Bool: {
            POP_ARRAY({
                bool b = *(bool *)src;
                array.Set(i, Napi::Boolean::New(env, b));
            });
        } break;
        case PrimitiveKind::Int8: {
            if (type->hint == TypeInfo::ArrayHint::String) {
                RG_ASSERT(!realign);

                const char *ptr = (const char *)origin;
                size_t count = strnlen(ptr, (size_t)len);

                Napi::String str = Napi::String::New(env, ptr, count);
                return str;
            }

            POP_NUMBER_ARRAY(Int8Array, int8_t);
        } break;
        case PrimitiveKind::UInt8: { POP_NUMBER_ARRAY(Uint8Array, uint8_t); } break;
        case PrimitiveKind::Int16: {
            if (type->hint == TypeInfo::ArrayHint::String) {
                RG_ASSERT(!realign);

                const char16_t *ptr = (const char16_t *)origin;
                Size count = WideStringLength(ptr, len);

                Napi::String str = Napi::String::New(env, ptr, count);
                return str;
            }

            POP_NUMBER_ARRAY(Int16Array, int16_t);
        } break;
        case PrimitiveKind::Int16S: { POP_NUMBER_ARRAY_SWAP(Int16Array, int16_t); } break;
        case PrimitiveKind::UInt16: { POP_NUMBER_ARRAY(Uint16Array, uint16_t); } break;
        case PrimitiveKind::UInt16S: { POP_NUMBER_ARRAY_SWAP(Uint16Array, uint16_t); } break;
        case PrimitiveKind::Int32: { POP_NUMBER_ARRAY(Int32Array, int32_t); } break;
        case PrimitiveKind::Int32S: { POP_NUMBER_ARRAY_SWAP(Int32Array, int32_t); } break;
        case PrimitiveKind::UInt32: { POP_NUMBER_ARRAY(Uint32Array, uint32_t); } break;
        case PrimitiveKind::UInt32S: { POP_NUMBER_ARRAY_SWAP(Uint32Array, uint32_t); } break;
        case PrimitiveKind::Int64: {
            POP_ARRAY({
                int64_t v = *(int64_t *)src;
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::Int64S: {
            POP_ARRAY({
                int64_t v = ReverseBytes(*(int64_t *)src);
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::UInt64: {
            POP_ARRAY({
                uint64_t v = *(uint64_t *)src;
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::UInt64S: {
            POP_ARRAY({
                uint64_t v = ReverseBytes(*(uint64_t *)src);
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::String: {
            POP_ARRAY({
                const char *str = *(const char **)src;
                array.Set(i, str ? Napi::String::New(env, str) : env.Null());
            });
        } break;
        case PrimitiveKind::String16: {
            POP_ARRAY({
                const char16_t *str16 = *(const char16_t **)src;
                array.Set(i, str16 ? Napi::String::New(env, str16) : env.Null());
            });
        } break;
        case PrimitiveKind::Pointer:
        case PrimitiveKind::Callback: {
            POP_ARRAY({
                void *ptr2 = *(void **)src;

                if (ptr2) {
                    Napi::External<void> external = Napi::External<void>::New(env, ptr2);
                    SetValueTag(instance, external, type->ref.type->ref.marker);

                    array.Set(i, external);
                } else {
                    array.Set(i, env.Null());
                }
            });
        } break;
        case PrimitiveKind::Record: {
            POP_ARRAY({
                Napi::Object obj = DecodeObject(env, src, type->ref.type, realign);
                array.Set(i, obj);
            });
        } break;
        case PrimitiveKind::Array: {
            POP_ARRAY({
                Napi::Value value = DecodeArray(env, src, type->ref.type, realign);
                array.Set(i, value);
            });
        } break;
        case PrimitiveKind::Float32: { POP_NUMBER_ARRAY(Float32Array, float); } break;
        case PrimitiveKind::Float64: { POP_NUMBER_ARRAY(Float64Array, double); } break;

        case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
    }

#undef POP_NUMBER_ARRAY_SWAP
#undef POP_NUMBER_ARRAY
#undef POP_ARRAY

    RG_UNREACHABLE();
}

void DecodeNormalArray(Napi::Array array, const uint8_t *origin, const TypeInfo *ref, int16_t realign)
{
    Napi::Env env = array.Env();
    InstanceData *instance = env.GetInstanceData<InstanceData>();

    RG_ASSERT(array.IsArray());

    Size offset = 0;
    uint32_t len = array.Length();

#define POP_ARRAY(SetCode) \
        do { \
            for (uint32_t i = 0; i < len; i++) { \
                int16_t align = std::max(realign, ref->align); \
                offset = AlignLen(offset, align); \
                 \
                const uint8_t *src = origin + offset; \
                 \
                SetCode \
                 \
                offset += ref->size; \
            } \
        } while (false)
#define POP_NUMBER_ARRAY(CType) \
        do { \
            POP_ARRAY({ \
                double d = (double)*(CType *)src; \
                array.Set(i, Napi::Number::New(env, d)); \
            }); \
        } while (false)
#define POP_NUMBER_ARRAY_SWAP(CType) \
        do { \
            POP_ARRAY({ \
                CType v = *(CType *)src; \
                double d = (double)ReverseBytes(v); \
                array.Set(i, Napi::Number::New(env, d)); \
            }); \
        } while (false)

    switch (ref->primitive) {
        case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

        case PrimitiveKind::Bool: {
            POP_ARRAY({
                bool b = *(bool *)src;
                array.Set(i, Napi::Boolean::New(env, b));
            });
        } break;
        case PrimitiveKind::Int8: { POP_NUMBER_ARRAY(int8_t); } break;
        case PrimitiveKind::UInt8: { POP_NUMBER_ARRAY(uint8_t); } break;
        case PrimitiveKind::Int16: { POP_NUMBER_ARRAY(int16_t); } break;
        case PrimitiveKind::Int16S: { POP_NUMBER_ARRAY_SWAP(int16_t); } break;
        case PrimitiveKind::UInt16: { POP_NUMBER_ARRAY(uint16_t); } break;
        case PrimitiveKind::UInt16S: { POP_NUMBER_ARRAY_SWAP(uint16_t); } break;
        case PrimitiveKind::Int32: { POP_NUMBER_ARRAY(int32_t); } break;
        case PrimitiveKind::Int32S: { POP_NUMBER_ARRAY_SWAP(int32_t); } break;
        case PrimitiveKind::UInt32: { POP_NUMBER_ARRAY(uint32_t); } break;
        case PrimitiveKind::UInt32S: { POP_NUMBER_ARRAY_SWAP(uint32_t); } break;
        case PrimitiveKind::Int64: {
            POP_ARRAY({
                int64_t v = *(int64_t *)src;
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::Int64S: {
            POP_ARRAY({
                int64_t v = ReverseBytes(*(int64_t *)src);
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::UInt64: {
            POP_ARRAY({
                uint64_t v = *(uint64_t *)src;
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::UInt64S: {
            POP_ARRAY({
                uint64_t v = ReverseBytes(*(uint64_t *)src);
                array.Set(i, NewBigInt(env, v));
            });
        } break;
        case PrimitiveKind::String: {
            POP_ARRAY({
                const char *str = *(const char **)src;
                array.Set(i, str ? Napi::String::New(env, str) : env.Null());

                if (ref->dispose) {
                    ref->dispose(env, ref, str);
                }
            });
        } break;
        case PrimitiveKind::String16: {
            POP_ARRAY({
                const char16_t *str16 = *(const char16_t **)src;
                array.Set(i, str16 ? Napi::String::New(env, str16) : env.Null());

                if (ref->dispose) {
                    ref->dispose(env, ref, str16);
                }
            });
        } break;
        case PrimitiveKind::Pointer:
        case PrimitiveKind::Callback: {
            POP_ARRAY({
                void *ptr2 = *(void **)src;

                if (ptr2) {
                    Napi::External<void> external = Napi::External<void>::New(env, ptr2);
                    SetValueTag(instance, external, ref->ref.marker);

                    array.Set(i, external);
                } else {
                    array.Set(i, env.Null());
                }

                if (ref->dispose) {
                    ref->dispose(env, ref, ptr2);
                }
            });
        } break;
        case PrimitiveKind::Record: {
            POP_ARRAY({
                Napi::Object obj = DecodeObject(env, src, ref, realign);
                array.Set(i, obj);
            });
        } break;
        case PrimitiveKind::Array: {
            POP_ARRAY({
                Napi::Value value = DecodeArray(env, src, ref, realign);
                array.Set(i, value);
            });
        } break;
        case PrimitiveKind::Float32: { POP_NUMBER_ARRAY(float); } break;
        case PrimitiveKind::Float64: { POP_NUMBER_ARRAY(double); } break;

        case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
    }

#undef POP_NUMBER_ARRAY_SWAP
#undef POP_NUMBER_ARRAY
#undef POP_ARRAY
}

void DecodeBuffer(Span<uint8_t> buffer, const uint8_t *origin, const TypeInfo *ref, int16_t realign)
{
    if (realign) {
        Size offset = 0;
        Size step = ref->size;

        for (Size i = 0; i < buffer.len; i += step) {
            offset = AlignLen(offset, realign);

            uint8_t *dest = buffer.ptr + i;
            const uint8_t *src = origin + offset;

            memcpy(dest, src, step);
            offset += step;
        }
    } else {
        memcpy_safe(buffer.ptr, origin, (size_t)buffer.len);
    }

#define SWAP(CType) \
        do { \
            CType *data = (CType *)buffer.ptr; \
            Size len = buffer.len / RG_SIZE(CType); \
             \
            for (Size i = 0; i < len; i++) { \
                data[i] = ReverseBytes(data[i]); \
            } \
        } while (false)

    if (ref->primitive == PrimitiveKind::Int16S || ref->primitive == PrimitiveKind::UInt16S) {
        SWAP(uint16_t);
    } else if (ref->primitive == PrimitiveKind::Int32S || ref->primitive == PrimitiveKind::UInt32S) {
        SWAP(uint32_t);
    } else if (ref->primitive == PrimitiveKind::Int64S || ref->primitive == PrimitiveKind::UInt64S) {
        SWAP(uint64_t);
    }

#undef SWAP
}

static int AnalyseFlatRec(const TypeInfo *type, int offset, int count, FunctionRef<void(const TypeInfo *type, int offset, int count)> func)
{
    if (type->primitive == PrimitiveKind::Record) {
        for (int i = 0; i < count; i++) {
            for (const RecordMember &member: type->members) {
                offset = AnalyseFlatRec(member.type, offset, 1, func);
            }
        }
    } else if (type->primitive == PrimitiveKind::Array) {
        count *= type->size / type->ref.type->size;
        offset = AnalyseFlatRec(type->ref.type, offset, count, func);
    } else {
        func(type, offset, count);
        offset += count;
    }

    return offset;
}

int AnalyseFlat(const TypeInfo *type, FunctionRef<void(const TypeInfo *type, int offset, int count)> func)
{
    return AnalyseFlatRec(type, 0, 1, func);
}

int IsHFA(const TypeInfo *type, int min, int max)
{
    uint32_t primitives = 0;
    int count = 0;

    count = AnalyseFlat(type, [&](const TypeInfo *type, int, int) {
        if (IsFloat(type)) {
            primitives |= 1u << (int)type->primitive;
        } else {
            primitives = UINT32_MAX;
        }
    });

    bool hfa = (count >= min && count <= max && PopCount(primitives) == 1);
    return hfa ? count : 0;
}

void DumpMemory(const char *type, Span<const uint8_t> bytes)
{
    if (bytes.len) {
        PrintLn(stderr, "%1 at 0x%2 (%3):", type, bytes.ptr, FmtMemSize(bytes.len));

        for (const uint8_t *ptr = bytes.begin(); ptr < bytes.end();) {
            Print(stderr, "  [0x%1 %2 %3]  ", FmtArg(ptr).Pad0(-16),
                                              FmtArg((ptr - bytes.begin()) / sizeof(void *)).Pad(-4),
                                              FmtArg(ptr - bytes.begin()).Pad(-4));
            for (int i = 0; ptr < bytes.end() && i < (int)sizeof(void *); i++, ptr++) {
                Print(stderr, " %1", FmtHex(*ptr).Pad0(-2));
            }
            PrintLn(stderr);
        }
    }
}

}
