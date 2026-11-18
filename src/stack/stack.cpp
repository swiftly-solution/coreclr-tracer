#include "stack.h"

#include <cstddef>
#include <utility>

#include "corerror.h"

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

constexpr ULONG kMaxSignatureParams = 1024;
constexpr size_t kMaxGenericParams = 256;

static std::wstring FromMetadata(const WCHAR* s)
{
    if (s == nullptr)
    {
        return std::wstring();
    }
    if (sizeof(WCHAR) == sizeof(wchar_t))
    {
        return std::wstring(reinterpret_cast<const wchar_t*>(s));
    }

    std::wstring out;
    for (const WCHAR* p = s; *p != 0; ++p)
    {
        char32_t c = static_cast<char32_t>(*p);
        if (c >= 0xD800 && c <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF)
        {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(p[1]) - 0xDC00);
            ++p;
        }
        out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}

class MetaDataHolder
{
public:
    MetaDataHolder(ICorProfilerInfo2* info, ModuleID moduleId)
    {
        if (FAILED(info->GetModuleMetaData(moduleId, ofRead, IID_IMetaDataImport2, reinterpret_cast<IUnknown**>(&import_))))
        {
            import_ = nullptr;
        }
    }
    ~MetaDataHolder()
    {
        if (import_)
        {
            import_->Release();
        }
    }
    MetaDataHolder(const MetaDataHolder&) = delete;
    MetaDataHolder& operator=(const MetaDataHolder&) = delete;

    IMetaDataImport2* get() const
    {
        return import_;
    }
    IMetaDataImport2* operator->() const
    {
        return import_;
    }
    explicit operator bool() const
    {
        return import_ != nullptr;
    }

private:
    IMetaDataImport2* import_ = nullptr;
};

static std::wstring Angle(const std::vector<std::wstring>& items)
{
    if (items.empty())
    {
        return L"";
    }
    std::wstring out = L"<";
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
        {
            out += L", ";
        }
        out += items[i];
    }
    out += L">";
    return out;
}

static std::wstring StripArity(const std::wstring& name)
{
    auto tick = name.find(L'`');
    return tick == std::wstring::npos ? name : name.substr(0, tick);
}

static std::wstring TypeDefFullName(IMetaDataImport2* import, mdTypeDef typeDef)
{
    WCHAR buf[1024] = {};
    ULONG len = 0;
    DWORD flags = 0;
    mdToken extends = 0;
    if (FAILED(import->GetTypeDefProps(typeDef, buf, _countof(buf), &len, &flags, &extends)))
    {
        return L"<unknown>";
    }

    std::wstring name = StripArity(FromMetadata(buf));
    mdTypeDef enclosing = mdTypeDefNil;
    if (SUCCEEDED(import->GetNestedClassProps(typeDef, &enclosing)) && enclosing != mdTypeDefNil)
    {
        return TypeDefFullName(import, enclosing) + L"+" + name;
    }
    return name;
}

static std::wstring TypeDefLocalName(IMetaDataImport2* import, mdTypeDef typeDef)
{
    WCHAR buf[1024] = {};
    ULONG len = 0;
    DWORD flags = 0;
    mdToken extends = 0;
    if (FAILED(import->GetTypeDefProps(typeDef, buf, _countof(buf), &len, &flags, &extends)))
    {
        return L"<unknown>";
    }
    return StripArity(FromMetadata(buf));
}

static std::wstring TypeRefFullName(IMetaDataImport2* import, mdTypeRef typeRef)
{
    WCHAR buf[1024] = {};
    ULONG len = 0;
    mdToken scope = 0;
    if (FAILED(import->GetTypeRefProps(typeRef, &scope, buf, _countof(buf), &len)))
    {
        return L"<unknown>";
    }
    return StripArity(FromMetadata(buf));
}

static std::vector<std::wstring> GenericParamNames(IMetaDataImport2* import, mdToken owner)
{
    std::vector<std::wstring> names;
    HCORENUM en = nullptr;
    mdGenericParam params[32];
    for (;;)
    {
        ULONG got = 0;
        if (FAILED(import->EnumGenericParams(&en, owner, params, _countof(params), &got)) || got == 0)
        {
            break;
        }
        for (ULONG i = 0; i < got; ++i)
        {
            WCHAR buf[256] = {};
            ULONG seq = 0;
            ULONG len = 0;
            mdToken ownerOut = 0;
            DWORD flags = 0;
            DWORD reserved = 0;
            if (SUCCEEDED(import->GetGenericParamProps(params[i], &seq, &flags, &ownerOut, &reserved, buf, _countof(buf), &len)) && seq < kMaxGenericParams)
            {
                if (names.size() <= seq)
                {
                    names.resize(seq + 1);
                }
                names[seq] = FromMetadata(buf);
            }
        }
    }
    if (en)
    {
        import->CloseEnum(en);
    }
    return names;
}

static std::wstring NestedName(IMetaDataImport2* import, mdTypeDef typeDef, const std::vector<std::wstring>& args, bool definition)
{
    std::vector<mdTypeDef> chain;
    mdTypeDef current = typeDef;
    while (current != mdTypeDefNil && chain.size() < 32)
    {
        chain.push_back(current);
        mdTypeDef enclosing = mdTypeDefNil;
        if (FAILED(import->GetNestedClassProps(current, &enclosing)))
        {
            break;
        }
        current = enclosing;
    }

    std::wstring result;
    size_t consumed = 0;
    for (size_t i = chain.size(); i-- > 0;)
    {
        std::vector<std::wstring> params = GenericParamNames(import, chain[i]);
        std::vector<std::wstring> slice;
        for (size_t k = consumed; k < params.size(); ++k)
        {
            if (definition)
            {
                slice.push_back(params[k]);
            }
            else if (k < args.size())
            {
                slice.push_back(args[k]);
            }
        }
        if (!result.empty())
        {
            result += L"+";
        }
        result += TypeDefLocalName(import, chain[i]) + Angle(slice);
        if (params.size() > consumed)
        {
            consumed = params.size();
        }
    }
    return result;
}

static const wchar_t* PrimitiveTypeName(ULONG et)
{
    switch (et)
    {
    case ELEMENT_TYPE_VOID:
        return L"void";
    case ELEMENT_TYPE_BOOLEAN:
        return L"System.Boolean";
    case ELEMENT_TYPE_CHAR:
        return L"System.Char";
    case ELEMENT_TYPE_I1:
        return L"System.SByte";
    case ELEMENT_TYPE_U1:
        return L"System.Byte";
    case ELEMENT_TYPE_I2:
        return L"System.Int16";
    case ELEMENT_TYPE_U2:
        return L"System.UInt16";
    case ELEMENT_TYPE_I4:
        return L"System.Int32";
    case ELEMENT_TYPE_U4:
        return L"System.UInt32";
    case ELEMENT_TYPE_I8:
        return L"System.Int64";
    case ELEMENT_TYPE_U8:
        return L"System.UInt64";
    case ELEMENT_TYPE_R4:
        return L"System.Single";
    case ELEMENT_TYPE_R8:
        return L"System.Double";
    case ELEMENT_TYPE_I:
        return L"System.IntPtr";
    case ELEMENT_TYPE_U:
        return L"System.UIntPtr";
    case ELEMENT_TYPE_STRING:
        return L"System.String";
    case ELEMENT_TYPE_OBJECT:
        return L"System.Object";
    case ELEMENT_TYPE_TYPEDBYREF:
        return L"System.TypedReference";
    default:
        return nullptr;
    }
}

static std::vector<std::wstring> ParamNames(IMetaDataImport2* import, mdMethodDef method, size_t slots)
{
    std::vector<std::wstring> names(slots);
    HCORENUM en = nullptr;
    mdParamDef params[32];
    for (;;)
    {
        ULONG got = 0;
        if (FAILED(import->EnumParams(&en, method, params, _countof(params), &got)) || got == 0)
        {
            break;
        }
        for (ULONG i = 0; i < got; ++i)
        {
            WCHAR buf[256] = {};
            ULONG seq = 0;
            ULONG len = 0;
            mdMethodDef owner = 0;
            DWORD attrs = 0;
            DWORD cplusType = 0;
            UVCP_CONSTANT value = nullptr;
            ULONG valueLen = 0;
            if (SUCCEEDED(import->GetParamProps(params[i], &owner, &seq, buf, _countof(buf), &len, &attrs, &cplusType, &value, &valueLen)) && seq < slots)
            {
                names[seq] = FromMetadata(buf);
            }
        }
    }
    if (en)
    {
        import->CloseEnum(en);
    }
    return names;
}

class SignatureReader
{
public:
    SignatureReader(IMetaDataImport2* import, PCCOR_SIGNATURE sig, ULONG size, const std::vector<std::wstring>& classParams, const std::vector<std::wstring>& methodParams)
        : import_(import), cur_(sig), end_(sig + size), classParams_(classParams), methodParams_(methodParams)
    {
    }

    bool ok() const
    {
        return ok_;
    }

    BYTE ReadByte()
    {
        if (cur_ >= end_)
        {
            ok_ = false;
            return 0;
        }
        return *cur_++;
    }

    BYTE PeekByte() const
    {
        return cur_ < end_ ? *cur_ : 0;
    }

    ULONG ReadCompressed()
    {
        BYTE b = ReadByte();
        if (!ok_)
        {
            return 0;
        }
        if ((b & 0x80) == 0)
        {
            return b;
        }
        if ((b & 0xC0) == 0x80)
        {
            BYTE b1 = ReadByte();
            return (static_cast<ULONG>(b & 0x3F) << 8) | b1;
        }
        if ((b & 0xE0) == 0xC0)
        {
            BYTE b1 = ReadByte();
            BYTE b2 = ReadByte();
            BYTE b3 = ReadByte();
            return (static_cast<ULONG>(b & 0x1F) << 24) | (static_cast<ULONG>(b1) << 16) | (static_cast<ULONG>(b2) << 8) | b3;
        }
        ok_ = false;
        return 0;
    }

    std::wstring ReadTypeDefOrRef()
    {
        ULONG coded = ReadCompressed();
        if (!ok_)
        {
            return L"";
        }
        return NameFromCoded(coded, std::vector<std::wstring>());
    }

    std::wstring ReadType()
    {
        BYTE et = ReadByte();
        if (!ok_)
        {
            return L"";
        }

        if (const wchar_t* primitive = PrimitiveTypeName(et))
        {
            return primitive;
        }

        switch (et)
        {
        case ELEMENT_TYPE_CLASS:
        case ELEMENT_TYPE_VALUETYPE:
            return ReadTypeDefOrRef();

        case ELEMENT_TYPE_VAR:
        {
            ULONG index = ReadCompressed();
            if (index < classParams_.size())
            {
                return classParams_[index];
            }
            return L"!" + std::to_wstring(index);
        }
        case ELEMENT_TYPE_MVAR:
        {
            ULONG index = ReadCompressed();
            if (index < methodParams_.size())
            {
                return methodParams_[index];
            }
            return L"!!" + std::to_wstring(index);
        }

        case ELEMENT_TYPE_GENERICINST:
        {
            ReadByte();
            ULONG coded = ReadCompressed();
            ULONG argCount = ReadCompressed();
            std::vector<std::wstring> args;
            for (ULONG i = 0; i < argCount && ok_; ++i)
            {
                args.push_back(ReadType());
            }
            if (!ok_)
            {
                return L"";
            }
            return NameFromCoded(coded, args);
        }

        case ELEMENT_TYPE_SZARRAY:
            return ReadType() + L"[]";

        case ELEMENT_TYPE_ARRAY:
        {
            std::wstring elem = ReadType();
            ULONG rank = ReadCompressed();
            if (!ok_ || rank == 0 || rank > 32)
            {
                ok_ = false;
                return L"";
            }
            ULONG sizes = ReadCompressed();
            for (ULONG i = 0; i < sizes && ok_; ++i)
            {
                ReadCompressed();
            }
            ULONG lower = ReadCompressed();
            for (ULONG i = 0; i < lower && ok_; ++i)
            {
                ReadCompressed();
            }
            std::wstring dims = L"[";
            for (ULONG i = 1; i < rank; ++i)
            {
                dims += L",";
            }
            dims += L"]";
            return elem + dims;
        }

        case ELEMENT_TYPE_BYREF:
            return ReadType() + L"&";
        case ELEMENT_TYPE_PTR:
            return ReadType() + L"*";
        case ELEMENT_TYPE_PINNED:
            return ReadType();

        case ELEMENT_TYPE_CMOD_OPT:
        case ELEMENT_TYPE_CMOD_REQD:
            ReadCompressed();
            return ReadType();

        case ELEMENT_TYPE_FNPTR:
            return ReadFunctionPointer();

        default:
            ok_ = false;
            return L"";
        }
    }

private:
    std::wstring NameFromCoded(ULONG coded, const std::vector<std::wstring>& genericArgs)
    {
        ULONG rid = coded >> 2;
        switch (coded & 0x3)
        {
        case 0:
        {
            mdTypeDef td = static_cast<mdTypeDef>(mdtTypeDef | rid);
            return genericArgs.empty() ? TypeDefFullName(import_, td) : NestedName(import_, td, genericArgs, false);
        }
        case 1:
            return TypeRefFullName(import_, static_cast<mdTypeRef>(mdtTypeRef | rid)) + Angle(genericArgs);
        case 2:
            return TypeSpecName(static_cast<mdTypeSpec>(mdtTypeSpec | rid)) + Angle(genericArgs);
        default:
            ok_ = false;
            return L"";
        }
    }

    std::wstring ReadFunctionPointer()
    {
        BYTE callConv = ReadByte();
        if (callConv & IMAGE_CEE_CS_CALLCONV_GENERIC)
        {
            ReadCompressed();
        }
        ULONG count = ReadCompressed();
        if (!ok_ || count > kMaxSignatureParams)
        {
            ok_ = false;
            return L"";
        }

        std::wstring prefix = L"delegate*";
        switch (callConv & 0x0F)
        {
        case 0x1:
            prefix += L" unmanaged[Cdecl]";
            break;
        case 0x2:
            prefix += L" unmanaged[Stdcall]";
            break;
        case 0x3:
            prefix += L" unmanaged[Thiscall]";
            break;
        case 0x4:
            prefix += L" unmanaged[Fastcall]";
            break;
        case 0x9:
            prefix += L" unmanaged";
            break;
        default:
            break;
        }

        std::wstring ret = ReadType();
        std::vector<std::wstring> parts;
        for (ULONG i = 0; i < count && ok_; ++i)
        {
            if (PeekByte() == ELEMENT_TYPE_SENTINEL)
            {
                ReadByte();
            }
            parts.push_back(ReadType());
        }
        parts.push_back(ret);
        return prefix + Angle(parts);
    }

    std::wstring TypeSpecName(mdTypeSpec spec)
    {
        PCCOR_SIGNATURE sig = nullptr;
        ULONG size = 0;
        if (FAILED(import_->GetTypeSpecFromToken(spec, &sig, &size)))
        {
            ok_ = false;
            return L"";
        }
        SignatureReader nested(import_, sig, size, classParams_, methodParams_);
        std::wstring name = nested.ReadType();
        if (!nested.ok())
        {
            ok_ = false;
        }
        return name;
    }

    IMetaDataImport2* import_;
    PCCOR_SIGNATURE cur_;
    PCCOR_SIGNATURE end_;
    const std::vector<std::wstring>& classParams_;
    const std::vector<std::wstring>& methodParams_;
    bool ok_ = true;
};

static std::wstring ClassNameImpl(ICorProfilerInfo2* info, ClassID classId, bool definition);

static bool ArrayClassParts(ICorProfilerInfo2* info, ClassID classId, bool definition, std::wstring& base, std::wstring& suffix)
{
    CorElementType elemType = ELEMENT_TYPE_END;
    ClassID elemClass = 0;
    ULONG rank = 0;
    if (info->IsArrayClass(classId, &elemType, &elemClass, &rank) != S_OK)
    {
        return false;
    }

    std::wstring dims = L"[";
    for (ULONG i = 1; i < rank; ++i)
    {
        dims += L",";
    }
    dims += L"]";

    std::wstring innerBase;
    std::wstring innerSuffix;
    if (elemClass != 0 && ArrayClassParts(info, elemClass, definition, innerBase, innerSuffix))
    {
        base = innerBase;
        suffix = dims + innerSuffix;
        return true;
    }

    if (elemType == ELEMENT_TYPE_PTR)
    {
        base = L"<pointer>";
    }
    else if (elemType == ELEMENT_TYPE_FNPTR)
    {
        base = L"<fnptr>";
    }
    else if (elemClass != 0)
    {
        base = ClassNameImpl(info, elemClass, definition);
    }
    else if (const wchar_t* primitive = PrimitiveTypeName(elemType))
    {
        base = primitive;
    }
    else
    {
        base = L"<unknown>";
    }
    suffix = dims;
    return true;
}

static std::wstring ArrayClassName(ICorProfilerInfo2* info, ClassID classId, bool definition)
{
    std::wstring base;
    std::wstring suffix;
    if (!ArrayClassParts(info, classId, definition, base, suffix))
    {
        return L"<array>";
    }
    return base + suffix;
}

static std::wstring ClassNameImpl(ICorProfilerInfo2* info, ClassID classId, bool definition)
{
    ModuleID moduleId = 0;
    mdTypeDef typeDef = mdTypeDefNil;
    ClassID parent = 0;
    ULONG32 argCount = 0;

    HRESULT hr = info->GetClassIDInfo2(classId, &moduleId, &typeDef, &parent, 0, &argCount, nullptr);
    if (hr == CORPROF_E_CLASSID_IS_ARRAY)
    {
        return ArrayClassName(info, classId, definition);
    }
    if (FAILED(hr))
    {
        return L"<unknown>";
    }

    std::vector<ClassID> argIds(argCount);
    if (argCount > 0)
    {
        hr = info->GetClassIDInfo2(classId, &moduleId, &typeDef, &parent, argCount, &argCount, argIds.data());
        if (FAILED(hr))
        {
            return L"<unknown>";
        }
    }

    MetaDataHolder import(info, moduleId);
    if (!import)
    {
        return L"<unknown>";
    }

    std::vector<std::wstring> args;
    if (!definition)
    {
        args.reserve(argIds.size());
        for (ClassID id : argIds)
        {
            args.push_back(ClassNameImpl(info, id, false));
        }
    }
    return NestedName(import.get(), typeDef, args, definition);
}

std::wstring GetClassName(ICorProfilerInfo2* info, ClassID classId)
{
    return ClassNameImpl(info, classId, false);
}

std::wstring GetClassDefinitionName(ICorProfilerInfo2* info, ClassID classId)
{
    return ClassNameImpl(info, classId, true);
}

std::wstring GetTypeDefinitionName(IMetaDataImport2* import, mdTypeDef typeDef)
{
    return NestedName(import, typeDef, std::vector<std::wstring>(), true);
}

std::wstring GetTypeSimpleName(IMetaDataImport2* import, mdTypeDef typeDef)
{
    return TypeDefLocalName(import, typeDef);
}

HRESULT GetFunctionInfo(ICorProfilerInfo2* info, FunctionID functionId, FunctionInfo& out, COR_PRF_FRAME_INFO frameInfo)
{
    out = FunctionInfo{};
    out.functionId = functionId;

    ULONG32 typeArgCount = 0;
    HRESULT hr = info->GetFunctionInfo2(functionId, frameInfo, &out.classId, &out.moduleId, &out.token, 0, &typeArgCount, nullptr);
    if (FAILED(hr))
    {
        return hr;
    }
    std::vector<ClassID> typeArgIds(typeArgCount);
    if (typeArgCount > 0)
    {
        hr = info->GetFunctionInfo2(functionId, frameInfo, &out.classId, &out.moduleId, &out.token, typeArgCount, &typeArgCount, typeArgIds.data());
        if (FAILED(hr))
        {
            return hr;
        }
    }
    for (ClassID id : typeArgIds)
    {
        out.methodTypeArgs.push_back(ClassNameImpl(info, id, false));
    }

    MetaDataHolder import(info, out.moduleId);
    if (!import)
    {
        return E_FAIL;
    }

    WCHAR nameBuf[512] = {};
    ULONG nameLen = 0;
    mdTypeDef owner = mdTypeDefNil;
    DWORD attrs = 0;
    PCCOR_SIGNATURE sig = nullptr;
    ULONG sigSize = 0;
    ULONG codeRva = 0;
    DWORD implFlags = 0;
    hr = import->GetMethodProps(out.token, &owner, nameBuf, _countof(nameBuf), &nameLen, &attrs, &sig, &sigSize, &codeRva, &implFlags);
    if (FAILED(hr))
    {
        return hr;
    }

    out.methodName = FromMetadata(nameBuf);
    out.isStatic = (attrs & mdStatic) != 0;
    out.classDefinition = GetTypeDefinitionName(import.get(), owner);
    out.className = out.classId != 0 ? ClassNameImpl(info, out.classId, false) : out.classDefinition;

    std::vector<std::wstring> classParams = GenericParamNames(import.get(), owner);
    out.methodTypeParams = GenericParamNames(import.get(), out.token);

    SignatureReader reader(import.get(), sig, sigSize, classParams, out.methodTypeParams);
    BYTE callConv = reader.ReadByte();
    if (callConv & IMAGE_CEE_CS_CALLCONV_GENERIC)
    {
        reader.ReadCompressed();
    }
    ULONG paramCount = reader.ReadCompressed();
    if (!reader.ok() || paramCount > kMaxSignatureParams)
    {
        return E_FAIL;
    }

    out.returnType = reader.ReadType();
    std::vector<std::wstring> paramNames = ParamNames(import.get(), out.token, paramCount + 1);

    for (ULONG i = 0; i < paramCount && reader.ok(); ++i)
    {
        if (reader.PeekByte() == ELEMENT_TYPE_SENTINEL)
        {
            reader.ReadByte();
        }
        ArgumentInfo arg;
        arg.type = reader.ReadType();
        arg.name = paramNames[i + 1];
        out.args.push_back(std::move(arg));
    }

    return reader.ok() ? S_OK : E_FAIL;
}

std::wstring GetFunctionName(const FunctionInfo& fn)
{
    return fn.methodName;
}

std::wstring GetFunctionFullName(const FunctionInfo& fn)
{
    std::wstring name;
    if (!fn.className.empty())
    {
        name = fn.className + L".";
    }
    name += fn.methodName;
    name += Angle(fn.methodTypeArgs);
    return name;
}

std::wstring GetFunctionDefinition(const FunctionInfo& fn)
{
    std::wstring def;
    if (fn.isStatic)
    {
        def += L"static ";
    }
    def += fn.returnType + L" ";
    if (!fn.classDefinition.empty())
    {
        def += fn.classDefinition + L".";
    }
    def += fn.methodName + Angle(fn.methodTypeParams);
    def += L"(" + GetFunctionArgsString(fn) + L")";
    return def;
}

std::vector<ArgumentInfo> GetFunctionArgs(const FunctionInfo& fn)
{
    return fn.args;
}

std::wstring GetFunctionArgsString(const FunctionInfo& fn)
{
    std::wstring out;
    for (size_t i = 0; i < fn.args.size(); ++i)
    {
        if (i > 0)
        {
            out += L", ";
        }
        out += fn.args[i].type;
        if (!fn.args[i].name.empty())
        {
            out += L" " + fn.args[i].name;
        }
    }
    return out;
}
