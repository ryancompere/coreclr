// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "jitpch.h"
#include "hwintrinsicArm64.h"

#ifdef FEATURE_HW_INTRINSICS

namespace IsaFlag
{
enum Flag
{
#define HARDWARE_INTRINSIC_CLASS(flag, isa) isa = 1ULL << InstructionSet_##isa,
#include "hwintrinsiclistArm64.h"
    None     = 0,
    Base     = 1ULL << InstructionSet_Base,
    EveryISA = ~0ULL
};

Flag operator|(Flag a, Flag b)
{
    return Flag(uint64_t(a) | uint64_t(b));
}

Flag flag(InstructionSet isa)
{
    return Flag(1ULL << isa);
}
}

// clang-format off
static const HWIntrinsicInfo hwIntrinsicInfoArray[] = {
    // Add lookupHWIntrinsic special cases see lookupHWIntrinsic() below
    //     NI_ARM64_IsSupported_True is used to expand get_IsSupported to const true
    //     NI_ARM64_IsSupported_False is used to expand get_IsSupported to const false
    //     NI_ARM64_PlatformNotSupported to throw PlatformNotSupported exception for every intrinsic not supported on the running platform
    {NI_ARM64_IsSupported_True,     "get_IsSupported",                 IsaFlag::EveryISA, HWIntrinsicInfo::IsSupported, HWIntrinsicInfo::None, {}},
    {NI_ARM64_IsSupported_False,    "::NI_ARM64_IsSupported_False",    IsaFlag::EveryISA, HWIntrinsicInfo::IsSupported, HWIntrinsicInfo::None, {}},
    {NI_ARM64_PlatformNotSupported, "::NI_ARM64_PlatformNotSupported", IsaFlag::EveryISA, HWIntrinsicInfo::Unsupported, HWIntrinsicInfo::None, {}},
#define HARDWARE_INTRINSIC(id, isa, name, form, i0, i1, i2, flags) \
    {id,                            #name,                             IsaFlag::isa,      HWIntrinsicInfo::form,        HWIntrinsicInfo::flags, { i0, i1, i2 }},
#include "hwintrinsiclistArm64.h"
};
// clang-format on

extern const char* getHWIntrinsicName(NamedIntrinsic intrinsic)
{
    return hwIntrinsicInfoArray[intrinsic - NI_HW_INTRINSIC_START - 1].intrinsicName;
}

const HWIntrinsicInfo& Compiler::getHWIntrinsicInfo(NamedIntrinsic intrinsic)
{
    assert(intrinsic > NI_HW_INTRINSIC_START);
    assert(intrinsic < NI_HW_INTRINSIC_END);

    return hwIntrinsicInfoArray[intrinsic - NI_HW_INTRINSIC_START - 1];
}

//------------------------------------------------------------------------
// lookupHWIntrinsicISA: map class name to InstructionSet value
//
// Arguments:
//    className -- class name in System.Runtime.Intrinsics.Arm.Arm64
//
// Return Value:
//    Id for the ISA class if enabled.
//
InstructionSet Compiler::lookupHWIntrinsicISA(const char* className)
{
    if (className != nullptr)
    {
        if (strcmp(className, "Base") == 0)
            return InstructionSet_Base;
#define HARDWARE_INTRINSIC_CLASS(flag, isa)                                                                            \
    if (strcmp(className, #isa) == 0)                                                                                  \
        return InstructionSet_##isa;
#include "hwintrinsiclistArm64.h"
    }

    return InstructionSet_NONE;
}

//------------------------------------------------------------------------
// lookupHWIntrinsic: map intrinsic name to named intrinsic value
//
// Arguments:
//    methodName -- name of the intrinsic function.
//    isa        -- instruction set of the intrinsic.
//
// Return Value:
//    Id for the hardware intrinsic.
//
// TODO-Throughput: replace sequential search by hash lookup
NamedIntrinsic Compiler::lookupHWIntrinsic(const char* className, const char* methodName)
{
    InstructionSet isa    = lookupHWIntrinsicISA(className);
    NamedIntrinsic result = NI_Illegal;
    if (isa != InstructionSet_NONE)
    {
        IsaFlag::Flag isaFlag = IsaFlag::flag(isa);
        for (int i = 0; i < NI_HW_INTRINSIC_END - NI_HW_INTRINSIC_START; i++)
        {
            if ((isaFlag & hwIntrinsicInfoArray[i].isaflags) &&
                strcmp(methodName, hwIntrinsicInfoArray[i].intrinsicName) == 0)
            {
                if (compSupports(isa))
                {
                    // Intrinsic is supported on platform
                    result = hwIntrinsicInfoArray[i].intrinsicID;
                }
                else
                {
                    // When the intrinsic class is not supported
                    // Return NI_ARM64_PlatformNotSupported for all intrinsics
                    // Return NI_ARM64_IsSupported_False for the IsSupported property
                    result = (hwIntrinsicInfoArray[i].intrinsicID != NI_ARM64_IsSupported_True)
                                 ? NI_ARM64_PlatformNotSupported
                                 : NI_ARM64_IsSupported_False;
                }
                break;
            }
        }
    }
    return result;
}

//------------------------------------------------------------------------
// impCheckImmediate: check if immediate is const and in range for inlining
//
bool Compiler::impCheckImmediate(GenTree* immediateOp, unsigned int max)
{
    return immediateOp->IsCnsIntOrI() && (immediateOp->AsIntConCommon()->IconValue() < max);
}

//------------------------------------------------------------------------
// impHWIntrinsic: dispatch hardware intrinsics to their own implementation
// function
//
// Arguments:
//    intrinsic -- id of the intrinsic function.
//    method    -- method handle of the intrinsic function.
//    sig       -- signature of the intrinsic call
//
// Return Value:
//    the expanded intrinsic.
//
GenTree* Compiler::impHWIntrinsic(NamedIntrinsic        intrinsic,
                                  CORINFO_METHOD_HANDLE method,
                                  CORINFO_SIG_INFO*     sig,
                                  bool                  mustExpand)
{
    GenTree*             retNode       = nullptr;
    GenTree*             op1           = nullptr;
    GenTree*             op2           = nullptr;
    GenTree*             op3           = nullptr;
    CORINFO_CLASS_HANDLE simdClass     = nullptr;
    var_types            simdType      = TYP_UNKNOWN;
    var_types            simdBaseType  = TYP_UNKNOWN;
    unsigned             simdSizeBytes = 0;

    switch (getHWIntrinsicInfo(intrinsic).form)
    {
        case HWIntrinsicInfo::SimdBinaryOp:
        case HWIntrinsicInfo::SimdInsertOp:
        case HWIntrinsicInfo::SimdSelectOp:
        case HWIntrinsicInfo::SimdSetAllOp:
        case HWIntrinsicInfo::SimdUnaryOp:
            simdClass = sig->retTypeClass;
            break;
        case HWIntrinsicInfo::SimdExtractOp:
            info.compCompHnd->getArgType(sig, sig->args, &simdClass);
            break;
        default:
            break;
    }

    // Simd instantiation type check
    if (simdClass != nullptr)
    {
        simdBaseType = getBaseTypeAndSizeOfSIMDType(simdClass, &simdSizeBytes);

        if (simdBaseType == TYP_UNKNOWN)
        {
            return impUnsupportedHWIntrinsic(CORINFO_HELP_THROW_TYPE_NOT_SUPPORTED, method, sig, mustExpand);
        }
        simdType = getSIMDTypeForSize(simdSizeBytes);
    }

    switch (getHWIntrinsicInfo(intrinsic).form)
    {
        case HWIntrinsicInfo::IsSupported:
            return gtNewIconNode((intrinsic == NI_ARM64_IsSupported_True) ? 1 : 0);

        case HWIntrinsicInfo::Unsupported:
            return impUnsupportedHWIntrinsic(CORINFO_HELP_THROW_PLATFORM_NOT_SUPPORTED, method, sig, mustExpand);

        case HWIntrinsicInfo::SimdBinaryOp:
            // op1 is the first operand
            // op2 is the second operand
            op2 = impSIMDPopStack(simdType);
            op1 = impSIMDPopStack(simdType);

            return gtNewSimdHWIntrinsicNode(simdType, op1, op2, intrinsic, simdBaseType, simdSizeBytes);

        case HWIntrinsicInfo::SimdSelectOp:
            // op1 is the first operand
            // op2 is the second operand
            // op3 is the third operand
            op3 = impSIMDPopStack(simdType);
            op2 = impSIMDPopStack(simdType);
            op1 = impSIMDPopStack(simdType);

            return gtNewSimdHWIntrinsicNode(simdType, op1, op2, op3, intrinsic, simdBaseType, simdSizeBytes);

        case HWIntrinsicInfo::SimdSetAllOp:
            op1 = impPopStack().val;

            return gtNewSimdHWIntrinsicNode(simdType, op1, intrinsic, simdBaseType, simdSizeBytes);

        case HWIntrinsicInfo::SimdUnaryOp:
            op1 = impSIMDPopStack(simdType);

            return gtNewSimdHWIntrinsicNode(simdType, op1, intrinsic, simdBaseType, simdSizeBytes);

        case HWIntrinsicInfo::SimdExtractOp:
            if (!mustExpand && !impCheckImmediate(impStackTop(0).val, getSIMDVectorLength(simdSizeBytes, simdBaseType)))
            {
                // Immediate lane not constant or out of range
                return nullptr;
            }
            op2 = impPopStack().val;
            op1 = impSIMDPopStack(simdType);

            return gtNewScalarHWIntrinsicNode(JITtype2varType(sig->retType), op1, op2, intrinsic);

        case HWIntrinsicInfo::SimdInsertOp:
            if (!mustExpand && !impCheckImmediate(impStackTop(1).val, getSIMDVectorLength(simdSizeBytes, simdBaseType)))
            {
                // Immediate lane not constant or out of range
                return nullptr;
            }
            op3 = impPopStack().val;
            op2 = impPopStack().val;
            op1 = impSIMDPopStack(simdType);

            return gtNewSimdHWIntrinsicNode(simdType, op1, op2, op3, intrinsic, simdBaseType, simdSizeBytes);

        default:
            JITDUMP("Not implemented hardware intrinsic form");
            assert(!"Unimplemented SIMD Intrinsic form");

            break;
    }
    return retNode;
}

#endif // FEATURE_HW_INTRINSICS
