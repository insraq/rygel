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

#if __riscv_xlen == 64

#include "src/core/libcc/libcc.hh"
#include "ffi.hh"
#include "call.hh"
#include "util.hh"

#include <napi.h>

namespace RG {

struct A0A1Ret {
    uint64_t a0;
    uint64_t a1;
};
struct A0Fa0Ret {
    uint64_t a0;
    double fa0;
};
struct Fa0A0Ret {
    double fa0;
    uint64_t a0;
};
struct Fa0Fa1Ret {
    double fa0;
    double fa1;
};

struct BackRegisters {
    uint64_t a0;
    uint64_t a1;
    double fa0;
    double fa1;
};

extern "C" A0A1Ret ForwardCallGG(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" float ForwardCallF(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" Fa0A0Ret ForwardCallDG(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" A0Fa0Ret ForwardCallGD(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" Fa0Fa1Ret ForwardCallDD(const void *func, uint8_t *sp, uint8_t **out_old_sp);

extern "C" A0A1Ret ForwardCallXGG(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" float ForwardCallXF(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" Fa0A0Ret ForwardCallXDG(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" A0Fa0Ret ForwardCallXGD(const void *func, uint8_t *sp, uint8_t **out_old_sp);
extern "C" Fa0Fa1Ret ForwardCallXDD(const void *func, uint8_t *sp, uint8_t **out_old_sp);

extern "C" napi_value CallSwitchStack(Napi::Function *func, size_t argc, napi_value *argv,
                                      uint8_t *old_sp, Span<uint8_t> *new_stack,
                                      napi_value (*call)(Napi::Function *func, size_t argc, napi_value *argv));

#include "abi_trampolines.inc"

static void AnalyseParameter(ParameterInfo *param, int gpr_avail, int vec_avail)
{
    gpr_avail = std::min(2, gpr_avail);
    vec_avail = std::min(2, vec_avail);

    if (param->type->size > 16) {
        param->gpr_count = gpr_avail ? 1 : 0;
        param->use_memory = true;

        return;
    }

    int gpr_count = 0;
    int vec_count = 0;
    bool gpr_first = false;

    AnalyseFlat(param->type, [&](const TypeInfo *type, int offset, int count) {
#if defined(__riscv_float_abi_double)
        bool fp = IsFloat(type);
#elif defined(__riscv_float_abi_soft)
        bool fp = false;
#else
        #error The RISC-V single-precision float ABI (LP64F) is not supported
#endif

        if (fp) {
            vec_count += count;
        } else {
            gpr_count += count;
            gpr_first |= !vec_count;
        }
    });

    if (gpr_count == 1 && vec_count == 1 && gpr_avail && vec_avail) {
        param->gpr_count = 1;
        param->vec_count = 1;
        param->gpr_first = gpr_first;
    } else if (vec_count && !gpr_count && vec_count <= vec_avail) {
        param->vec_count = vec_count;
    } else if (gpr_avail) {
        param->gpr_count = (param->type->size + 7) / 8;
        param->gpr_first = true;
    }
}

bool AnalyseFunction(Napi::Env, InstanceData *, FunctionInfo *func)
{
    AnalyseParameter(&func->ret, 2, 2);

    int gpr_avail = 8 - func->ret.use_memory;
    int vec_avail = 8;

    for (ParameterInfo &param: func->parameters) {
        AnalyseParameter(&param, gpr_avail, !param.variadic ? vec_avail : 0);

        gpr_avail = std::max(0, gpr_avail - param.gpr_count);
        vec_avail = std::max(0, vec_avail - param.vec_count);
    }

    func->args_size = 8 * func->parameters.len;
    func->forward_fp = (vec_avail < 8);

    return true;
}

bool CallData::Prepare(const FunctionInfo *func, const Napi::CallbackInfo &info)
{
    uint64_t *args_ptr = nullptr;
    uint64_t *gpr_ptr = nullptr;
    uint64_t *vec_ptr = nullptr;

    // Return through registers unless it's too big
    if (RG_UNLIKELY(!AllocStack(func->args_size, 16, &args_ptr)))
        return false;
    if (RG_UNLIKELY(!AllocStack(8 * 8, 8, &gpr_ptr)))
        return false;
    if (RG_UNLIKELY(!AllocStack(8 * 8, 8, &vec_ptr)))
        return false;
    if (func->ret.use_memory) {
        return_ptr = AllocHeap(func->ret.type->size, 16);
        *(uint8_t **)(gpr_ptr++) = return_ptr;
    }

#define PUSH_INTEGER(CType) \
        do { \
            if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) { \
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value)); \
                return false; \
            } \
             \
            CType v = GetNumber<CType>(value); \
            *((param.gpr_count ? gpr_ptr : args_ptr)++) = (uint64_t)v; \
        } while (false)
#define PUSH_INTEGER_SWAP(CType) \
        do { \
            if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) { \
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value)); \
                return false; \
            } \
             \
            CType v = GetNumber<CType>(value); \
            *((param.gpr_count ? gpr_ptr : args_ptr)++) = (uint64_t)ReverseBytes(v); \
        } while (false)

    // Push arguments
    for (Size i = 0; i < func->parameters.len; i++) {
        const ParameterInfo &param = func->parameters[i];
        RG_ASSERT(param.directions >= 1 && param.directions <= 3);

        Napi::Value value = info[param.offset];

        switch (param.type->primitive) {
            case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

            case PrimitiveKind::Bool: {
                if (RG_UNLIKELY(!value.IsBoolean())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected boolean", GetValueType(instance, value));
                    return false;
                }

                bool b = value.As<Napi::Boolean>();
                *((param.gpr_count ? gpr_ptr : args_ptr)++) = (uint64_t)b;
            } break;
            case PrimitiveKind::Int8: { PUSH_INTEGER(int8_t); } break;
            case PrimitiveKind::UInt8: { PUSH_INTEGER(uint8_t); } break;
            case PrimitiveKind::Int16: { PUSH_INTEGER(int16_t); } break;
            case PrimitiveKind::Int16S: { PUSH_INTEGER_SWAP(int16_t); } break;
            case PrimitiveKind::UInt16: { PUSH_INTEGER(uint16_t); } break;
            case PrimitiveKind::UInt16S: { PUSH_INTEGER_SWAP(uint16_t); } break;
            case PrimitiveKind::Int32: { PUSH_INTEGER(int32_t); } break;
            case PrimitiveKind::Int32S: { PUSH_INTEGER_SWAP(int32_t); } break;
            case PrimitiveKind::UInt32: { PUSH_INTEGER(uint32_t); } break;
            case PrimitiveKind::UInt32S: { PUSH_INTEGER_SWAP(uint32_t); } break;
            case PrimitiveKind::Int64: { PUSH_INTEGER(int64_t); } break;
            case PrimitiveKind::Int64S: { PUSH_INTEGER_SWAP(int64_t); } break;
            case PrimitiveKind::UInt64: { PUSH_INTEGER(uint64_t); } break;
            case PrimitiveKind::UInt64S: { PUSH_INTEGER_SWAP(uint64_t); } break;
            case PrimitiveKind::String: {
                const char *str;
                if (RG_UNLIKELY(!PushString(value, param.directions, &str)))
                    return false;

                *(const char **)((param.gpr_count ? gpr_ptr : args_ptr)++) = str;
            } break;
            case PrimitiveKind::String16: {
                const char16_t *str16;
                if (RG_UNLIKELY(!PushString16(value, param.directions, &str16)))
                    return false;

                *(const char16_t **)((param.gpr_count ? gpr_ptr : args_ptr)++) = str16;
            } break;
            case PrimitiveKind::Pointer: {
                void *ptr;
                if (RG_UNLIKELY(!PushPointer(value, param.type, param.directions, &ptr)))
                    return false;

                *(void **)((param.gpr_count ? gpr_ptr : args_ptr)++) = ptr;
            } break;
            case PrimitiveKind::Record: {
                if (RG_UNLIKELY(!IsObject(value))) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected object", GetValueType(instance, value));
                    return false;
                }

                Napi::Object obj = value.As<Napi::Object>();

                if (!param.use_memory) {
                    RG_ASSERT(param.type->size <= 16);

                    // Split float or mixed int-float structs to registers
                    int realign = param.vec_count ? 8 : 0;

                    uint64_t buf[2] = { 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };
                    if (!PushObject(obj, param.type, (uint8_t *)buf, realign))
                        return false;
                    uint64_t *ptr = buf;

                    if (param.gpr_first) {
                        *(gpr_ptr++) = *(ptr++);
                        *((param.vec_count ? vec_ptr : gpr_ptr)++) = *(ptr++);
                        gpr_ptr -= (param.gpr_count == 1);
                    } else if (param.vec_count) {
                        *(vec_ptr++) = *(ptr++);
                        *((param.gpr_count ? gpr_ptr : vec_ptr)++) = *(ptr++);
                    } else {
                        RG_ASSERT(param.type->align <= 8);

                        memcpy_safe(args_ptr, ptr, param.type->size);
                        args_ptr += (param.type->size + 7) / 8;
                    }
                } else {
                    uint8_t *ptr = AllocHeap(param.type->size, 16);

                    if (param.gpr_count) {
                        RG_ASSERT(param.gpr_count == 1);
                        RG_ASSERT(param.vec_count == 0);

                        *(uint8_t **)(gpr_ptr++) = ptr;
                    } else {
                        *(uint8_t **)(args_ptr++) = ptr;
                    }

                    if (!PushObject(obj, param.type, ptr))
                        return false;
                }
            } break;
            case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
            case PrimitiveKind::Float32: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value));
                    return false;
                }

                float f = GetNumber<float>(value);

                if (RG_LIKELY(param.vec_count)) {
                    memset((uint8_t *)vec_ptr + 4, 0xFF, 4);
                    *(float *)(vec_ptr++) = f;
                } else if (param.gpr_count) {
                    memset((uint8_t *)gpr_ptr + 4, 0xFF, 4);
                    *(float *)(gpr_ptr++) = f;
                } else {
                    memset(args_ptr, 0xFF, 8);
                    *(float *)(args_ptr++) = f;
                }
            } break;
            case PrimitiveKind::Float64: {
                if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value));
                    return false;
                }

                double d = GetNumber<double>(value);

                if (RG_LIKELY(param.vec_count)) {
                    *(double *)(vec_ptr++) = d;
                } else if (param.gpr_count) {
                    *(double *)(gpr_ptr++) = d;
                } else {
                    *(double *)(args_ptr++) = d;
                }
            } break;
            case PrimitiveKind::Callback: {
                void *ptr;

                if (value.IsFunction()) {
                    Napi::Function func = value.As<Napi::Function>();

                    ptr = ReserveTrampoline(param.type->ref.proto, func);
                    if (RG_UNLIKELY(!ptr))
                        return false;
                } else if (CheckValueTag(instance, value, param.type->ref.marker)) {
                    ptr = value.As<Napi::External<void>>().Data();
                } else if (IsNullOrUndefined(value)) {
                    ptr = nullptr;
                } else {
                    ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected %2", GetValueType(instance, value), param.type->name);
                    return false;
                }

                *(void **)((param.gpr_count ? gpr_ptr : args_ptr)++) = ptr;
            } break;

            case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
        }
    }

#undef PUSH_INTEGER_SWAP
#undef PUSH_INTEGER

    new_sp = mem->stack.end();

    return true;
}

void CallData::Execute(const FunctionInfo *func)
{
#define PERFORM_CALL(Suffix) \
        ([&]() { \
            auto ret = (func->forward_fp ? ForwardCallX ## Suffix(func->func, new_sp, &old_sp) \
                                         : ForwardCall ## Suffix(func->func, new_sp, &old_sp)); \
            return ret; \
        })()

    // Execute and convert return value
    switch (func->ret.type->primitive) {
        case PrimitiveKind::Void:
        case PrimitiveKind::Bool:
        case PrimitiveKind::Int8:
        case PrimitiveKind::UInt8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int16S:
        case PrimitiveKind::UInt16:
        case PrimitiveKind::UInt16S:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int32S:
        case PrimitiveKind::UInt32:
        case PrimitiveKind::UInt32S:
        case PrimitiveKind::Int64:
        case PrimitiveKind::Int64S:
        case PrimitiveKind::UInt64:
        case PrimitiveKind::UInt64S:
        case PrimitiveKind::String:
        case PrimitiveKind::String16:
        case PrimitiveKind::Pointer:
        case PrimitiveKind::Callback: { result.u64 = PERFORM_CALL(GG).a0; } break;
        case PrimitiveKind::Record: {
            if (func->ret.gpr_first && !func->ret.vec_count) {
                A0A1Ret ret = PERFORM_CALL(GG);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            } else if (func->ret.gpr_first) {
                A0Fa0Ret ret = PERFORM_CALL(GD);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            } else if (func->ret.vec_count == 2) {
                Fa0Fa1Ret ret = PERFORM_CALL(DD);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            } else {
                Fa0A0Ret ret = PERFORM_CALL(DG);
                memcpy(&result.buf, &ret, RG_SIZE(ret));
            }
        } break;
        case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
        case PrimitiveKind::Float32: { result.f = PERFORM_CALL(F); } break;
        case PrimitiveKind::Float64: { result.d = PERFORM_CALL(DD).fa0; } break;

        case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
    }

#undef PERFORM_CALL
}

Napi::Value CallData::Complete(const FunctionInfo *func)
{
    RG_DEFER {
       PopOutArguments();

        if (func->ret.type->dispose) {
            func->ret.type->dispose(env, func->ret.type, result.ptr);
        }
    };

    switch (func->ret.type->primitive) {
        case PrimitiveKind::Void: return env.Undefined();
        case PrimitiveKind::Bool: return Napi::Boolean::New(env, result.u32);
        case PrimitiveKind::Int8: return Napi::Number::New(env, (double)result.i8);
        case PrimitiveKind::UInt8: return Napi::Number::New(env, (double)result.u8);
        case PrimitiveKind::Int16: return Napi::Number::New(env, (double)result.i16);
        case PrimitiveKind::Int16S: return Napi::Number::New(env, (double)ReverseBytes(result.i16));
        case PrimitiveKind::UInt16: return Napi::Number::New(env, (double)result.u16);
        case PrimitiveKind::UInt16S: return Napi::Number::New(env, (double)ReverseBytes(result.u16));
        case PrimitiveKind::Int32: return Napi::Number::New(env, (double)result.i32);
        case PrimitiveKind::Int32S: return Napi::Number::New(env, (double)ReverseBytes(result.i32));
        case PrimitiveKind::UInt32: return Napi::Number::New(env, (double)result.u32);
        case PrimitiveKind::UInt32S: return Napi::Number::New(env, (double)ReverseBytes(result.u32));
        case PrimitiveKind::Int64: return NewBigInt(env, result.i64);
        case PrimitiveKind::Int64S: return NewBigInt(env, ReverseBytes(result.i64));
        case PrimitiveKind::UInt64: return NewBigInt(env, result.u64);
        case PrimitiveKind::UInt64S: return NewBigInt(env, ReverseBytes(result.u64));
        case PrimitiveKind::String: return result.ptr ? Napi::String::New(env, (const char *)result.ptr) : env.Null();
        case PrimitiveKind::String16: return result.ptr ? Napi::String::New(env, (const char16_t *)result.ptr) : env.Null();
        case PrimitiveKind::Pointer:
        case PrimitiveKind::Callback: {
            if (result.ptr) {
                Napi::External<void> external = Napi::External<void>::New(env, result.ptr);
                SetValueTag(instance, external, func->ret.type->ref.marker);

                return external;
            } else {
                return env.Null();
            }
        } break;
        case PrimitiveKind::Record: {
            if (func->ret.vec_count) { // HFA
                Napi::Object obj = DecodeObject(env, (const uint8_t *)&result.buf, func->ret.type, 8);
                return obj;
            } else {
                const uint8_t *ptr = return_ptr ? (const uint8_t *)return_ptr
                                                : (const uint8_t *)&result.buf;

                Napi::Object obj = DecodeObject(env, ptr, func->ret.type);
                return obj;
            }
        } break;
        case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
        case PrimitiveKind::Float32: return Napi::Number::New(env, (double)result.f);
        case PrimitiveKind::Float64: return Napi::Number::New(env, result.d);

        case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
    }

    RG_UNREACHABLE();
}

void CallData::Relay(Size idx, uint8_t *own_sp, uint8_t *caller_sp, bool async, BackRegisters *out_reg)
{
    if (RG_UNLIKELY(env.IsExceptionPending()))
        return;

    const TrampolineInfo &trampoline = shared.trampolines[idx];

    const FunctionInfo *proto = trampoline.proto;
    Napi::Function func = trampoline.func.Value();

    uint64_t *gpr_ptr = (uint64_t *)own_sp;
    uint64_t *vec_ptr = gpr_ptr + 8;
    uint64_t *args_ptr = (uint64_t *)caller_sp;

    uint8_t *return_ptr = proto->ret.use_memory ? (uint8_t *)gpr_ptr[0] : nullptr;
    gpr_ptr += proto->ret.use_memory;

    RG_DEFER_N(err_guard) { memset(out_reg, 0, RG_SIZE(*out_reg)); };

    if (RG_UNLIKELY(trampoline.generation >= 0 && trampoline.generation != (int32_t)mem->generation)) {
        ThrowError<Napi::Error>(env, "Cannot use non-registered callback beyond FFI call");
        return;
    }

    LocalArray<napi_value, MaxParameters + 1> arguments;

    arguments.Append(!trampoline.recv.IsEmpty() ? trampoline.recv.Value() : env.Undefined());

    // Convert to JS arguments
    for (Size i = 0; i < proto->parameters.len; i++) {
        const ParameterInfo &param = proto->parameters[i];
        RG_ASSERT(param.directions >= 1 && param.directions <= 3);

        switch (param.type->primitive) {
            case PrimitiveKind::Void: { RG_UNREACHABLE(); } break;

            case PrimitiveKind::Bool: {
                bool b = *(bool *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Boolean::New(env, b);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int8: {
                double d = (double)*(int8_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt8: {
                double d = (double)*(uint8_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int16: {
                double d = (double)*(int16_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int16S: {
                int16_t v = *(int16_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);
                double d = (double)ReverseBytes(v);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt16: {
                double d = (double)*(uint16_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt16S: {
                uint16_t v = *(uint16_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);
                double d = (double)ReverseBytes(v);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int32: {
                double d = (double)*(int32_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int32S: {
                int32_t v = *(int32_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);
                double d = (double)ReverseBytes(v);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt32: {
                double d = (double)*(uint32_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt32S: {
                uint32_t v = *(uint32_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);
                double d = (double)ReverseBytes(v);

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int64: {
                int64_t v = *(int64_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = NewBigInt(env, v);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Int64S: {
                int64_t v = *(int64_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = NewBigInt(env, ReverseBytes(v));
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt64: {
                uint64_t v = *(uint64_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = NewBigInt(env, v);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::UInt64S: {
                uint64_t v = *(uint64_t *)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = NewBigInt(env, ReverseBytes(v));
                arguments.Append(arg);
            } break;
            case PrimitiveKind::String: {
                const char *str = *(const char **)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = str ? Napi::String::New(env, str) : env.Null();
                arguments.Append(arg);

                if (param.type->dispose) {
                    param.type->dispose(env, param.type, str);
                }
            } break;
            case PrimitiveKind::String16: {
                const char16_t *str16 = *(const char16_t **)((param.gpr_count ? gpr_ptr : args_ptr)++);

                Napi::Value arg = str16 ? Napi::String::New(env, str16) : env.Null();
                arguments.Append(arg);

                if (param.type->dispose) {
                    param.type->dispose(env, param.type, str16);
                }
            } break;
            case PrimitiveKind::Pointer:
            case PrimitiveKind::Callback: {
                void *ptr2 = *(void **)((param.gpr_count ? gpr_ptr : args_ptr)++);

                if (ptr2) {
                    Napi::External<void> external = Napi::External<void>::New(env, ptr2);
                    SetValueTag(instance, external, param.type->ref.marker);

                    arguments.Append(external);
                } else {
                    arguments.Append(env.Null());
                }

                if (param.type->dispose) {
                    param.type->dispose(env, param.type, ptr2);
                }
            } break;
            case PrimitiveKind::Record: {
                if (!param.use_memory) {
                    uint64_t buf[2] = {};
                    uint64_t *ptr = buf;

                    if (param.gpr_first) {
                        *(ptr++) = *(gpr_ptr++);
                        *(ptr++) = *((param.vec_count ? vec_ptr : gpr_ptr)++);
                        gpr_ptr -= (param.gpr_count == 1);
                    } else if (param.vec_count) {
                        *(ptr++) = *(vec_ptr++);
                        *(ptr++) = *((param.gpr_count ? gpr_ptr : vec_ptr)++);
                    } else {
                        RG_ASSERT(param.type->align <= 8);

                        memcpy_safe(ptr, args_ptr, param.type->size);
                        args_ptr += (param.type->size + 7) / 8;
                    }

                    // Reassemble float or mixed int-float structs from registers
                    int realign = param.vec_count ? 8 : 0;

                    Napi::Object obj = DecodeObject(env, (const uint8_t *)buf, param.type, realign);
                    arguments.Append(obj);
                } else {
                    uint8_t *ptr = *(uint8_t **)((param.gpr_count ? gpr_ptr : args_ptr)++);

                    Napi::Object obj = DecodeObject(env, ptr, param.type);
                    arguments.Append(obj);
                }
            } break;
            case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
            case PrimitiveKind::Float32: {
                float f;
                if (RG_LIKELY(param.vec_count)) {
                    f = *(float *)(vec_ptr++);
                } else if (param.gpr_count) {
                    f = *(float *)(gpr_ptr++);
                } else {
                    f = *(float *)(args_ptr++);
                }

                Napi::Value arg = Napi::Number::New(env, (double)f);
                arguments.Append(arg);
            } break;
            case PrimitiveKind::Float64: {
                double d;
                if (RG_LIKELY(param.vec_count)) {
                    d = *(double *)(vec_ptr++);
                } else if (param.gpr_count) {
                    d = *(double *)(gpr_ptr++);
                } else {
                    d = *(double *)(args_ptr++);
                }

                Napi::Value arg = Napi::Number::New(env, d);
                arguments.Append(arg);
            } break;

            case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
        }
    }

    const TypeInfo *type = proto->ret.type;

    // Make the call
    napi_value ret;
    if (async) {
        ret = (napi_value)func.Call(arguments[0], arguments.len - 1, arguments.data + 1);
    } else {
        ret = CallSwitchStack(&func, (size_t)arguments.len, arguments.data, old_sp, &mem->stack,
                              [](Napi::Function *func, size_t argc, napi_value *argv) { return (napi_value)func->Call(argv[0], argc - 1, argv + 1); });
    }
    Napi::Value value(env, ret);

    if (RG_UNLIKELY(env.IsExceptionPending()))
        return;

#define RETURN_INTEGER(CType) \
        do { \
            if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) { \
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value)); \
                return; \
            } \
             \
            CType v = GetNumber<CType>(value); \
            out_reg->a0 = (uint64_t)v; \
        } while (false)
#define RETURN_INTEGER_SWAP(CType) \
        do { \
            if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) { \
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value)); \
                return; \
            } \
             \
            CType v = GetNumber<CType>(value); \
            out_reg->a0 = (uint64_t)ReverseBytes(v); \
        } while (false)

    // Convert the result
    switch (type->primitive) {
        case PrimitiveKind::Void: {} break;
        case PrimitiveKind::Bool: {
            if (RG_UNLIKELY(!value.IsBoolean())) {
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected boolean", GetValueType(instance, value));
                return;
            }

            bool b = value.As<Napi::Boolean>();
            out_reg->a0 = (uint64_t)b;
        } break;
        case PrimitiveKind::Int8: { RETURN_INTEGER(int8_t); } break;
        case PrimitiveKind::UInt8: { RETURN_INTEGER(uint8_t); } break;
        case PrimitiveKind::Int16: { RETURN_INTEGER(int16_t); } break;
        case PrimitiveKind::Int16S: { RETURN_INTEGER_SWAP(int16_t); } break;
        case PrimitiveKind::UInt16: { RETURN_INTEGER(uint16_t); } break;
        case PrimitiveKind::UInt16S: { RETURN_INTEGER_SWAP(uint16_t); } break;
        case PrimitiveKind::Int32: { RETURN_INTEGER(int32_t); } break;
        case PrimitiveKind::Int32S: { RETURN_INTEGER_SWAP(int32_t); } break;
        case PrimitiveKind::UInt32: { RETURN_INTEGER(uint32_t); } break;
        case PrimitiveKind::UInt32S: { RETURN_INTEGER_SWAP(uint32_t); } break;
        case PrimitiveKind::Int64: { RETURN_INTEGER(int64_t); } break;
        case PrimitiveKind::Int64S: { RETURN_INTEGER_SWAP(int64_t); } break;
        case PrimitiveKind::UInt64: { RETURN_INTEGER(uint64_t); } break;
        case PrimitiveKind::UInt64S: { RETURN_INTEGER_SWAP(uint64_t); } break;
        case PrimitiveKind::String: {
            const char *str;
            if (RG_UNLIKELY(!PushString(value, 1, &str)))
                return;

            out_reg->a0 = (uint64_t)str;
        } break;
        case PrimitiveKind::String16: {
            const char16_t *str16;
            if (RG_UNLIKELY(!PushString16(value, 1, &str16)))
                return;

            out_reg->a0 = (uint64_t)str16;
        } break;
        case PrimitiveKind::Pointer: {
            uint8_t *ptr;

            if (CheckValueTag(instance, value, type->ref.marker)) {
                ptr = value.As<Napi::External<uint8_t>>().Data();
            } else if (IsObject(value) && type->ref.type->primitive == PrimitiveKind::Record) {
                Napi::Object obj = value.As<Napi::Object>();

                ptr = AllocHeap(type->ref.type->size, 16);

                if (!PushObject(obj, type->ref.type, ptr))
                    return;
            } else if (IsNullOrUndefined(value)) {
                ptr = nullptr;
            } else {
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected %2", GetValueType(instance, value), type->name);
                return;
            }

            out_reg->a0 = (uint64_t)ptr;
        } break;
        case PrimitiveKind::Record: {
            if (RG_UNLIKELY(!IsObject(value))) {
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected object", GetValueType(instance, value));
                return;
            }

            Napi::Object obj = value.As<Napi::Object>();

            if (return_ptr) {
                if (!PushObject(obj, type, return_ptr))
                    return;
                out_reg->a0 = (uint64_t)return_ptr;
            } else if (proto->ret.vec_count) { // HFA
                PushObject(obj, type, (uint8_t *)&out_reg->fa0, 8);
            } else {
                PushObject(obj, type, (uint8_t *)&out_reg->a0);
            }
        } break;
        case PrimitiveKind::Array: { RG_UNREACHABLE(); } break;
        case PrimitiveKind::Float32: {
            if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value));
                return;
            }

            float f = GetNumber<float>(value);
            memset((uint8_t *)&out_reg->fa0 + 4, 0xFF, 4);
            memcpy(&out_reg->fa0, &f, 4);
        } break;
        case PrimitiveKind::Float64: {
            if (RG_UNLIKELY(!value.IsNumber() && !value.IsBigInt())) {
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected number", GetValueType(instance, value));
                return;
            }

            double d = GetNumber<double>(value);
            out_reg->fa0 = d;
        } break;
        case PrimitiveKind::Callback: {
            void *ptr;

            if (value.IsFunction()) {
                Napi::Function func2 = value.As<Napi::Function>();

                ptr = ReserveTrampoline(type->ref.proto, func2);
                if (RG_UNLIKELY(!ptr))
                    return;
            } else if (CheckValueTag(instance, value, type->ref.marker)) {
                ptr = value.As<Napi::External<void>>().Data();
            } else if (IsNullOrUndefined(value)) {
                ptr = nullptr;
            } else {
                ThrowError<Napi::TypeError>(env, "Unexpected %1 value, expected %2", GetValueType(instance, value), type->name);
                return;
            }

            out_reg->a0 = (uint64_t)ptr;
        } break;

        case PrimitiveKind::Prototype: { RG_UNREACHABLE(); } break;
    }

#undef RETURN_INTEGER_SWAP
#undef RETURN_INTEGER

    err_guard.Disable();
}

void *GetTrampoline(int16_t idx, const FunctionInfo *proto)
{
    bool fp = proto->forward_fp || proto->ret.vec_count;
    return Trampolines[idx][fp];
}

}

#endif
