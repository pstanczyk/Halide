#include <iostream>
#include <sstream>

#include "CodeGen_ARM.h"
#include "IROperator.h"
#include "IRMatch.h"
#include "IREquality.h"
#include "Debug.h"
#include "Util.h"
#include "Simplify.h"
#include "IntegerDivisionTable.h"
#include "LLVM_Headers.h"

// Native client llvm relies on global flags to control sandboxing on
// arm, because they expect you to be coming from the command line.
#if WITH_NATIVE_CLIENT
#include <llvm/Support/CommandLine.h>
namespace llvm {
extern cl::opt<bool> FlagSfiData,
    FlagSfiLoad,
    FlagSfiStore,
    FlagSfiStack,
    FlagSfiBranch,
    FlagSfiDisableCP,
    FlagSfiZeroMask;
}
extern llvm::cl::opt<bool> ReserveR9;
#endif

namespace Halide {
namespace Internal {

using std::vector;
using std::string;
using std::ostringstream;
using std::pair;
using std::make_pair;

using namespace llvm;

namespace {
// cast operators
Expr _i64(Expr e) {
    return cast(Int(64, e.type().width), e);
}

Expr _u64(Expr e) {
    return cast(UInt(64, e.type().width), e);
}
Expr _i32(Expr e) {
    return cast(Int(32, e.type().width), e);
}

Expr _u32(Expr e) {
    return cast(UInt(32, e.type().width), e);
}

Expr _i16(Expr e) {
    return cast(Int(16, e.type().width), e);
}

Expr _u16(Expr e) {
    return cast(UInt(16, e.type().width), e);
}

Expr _i8(Expr e) {
    return cast(Int(8, e.type().width), e);
}

Expr _u8(Expr e) {
    return cast(UInt(8, e.type().width), e);
}

/*
Expr _f32(Expr e) {
    return cast(Float(32, e.type().width), e);
}

Expr _f64(Expr e) {
    return cast(Float(64, e.type().width), e);
}
*/

// saturating cast operators
Expr _i8q(Expr e) {
    return cast(Int(8, e.type().width), clamp(e, -128, 127));
}

Expr _u8q(Expr e) {
    if (e.type().is_uint()) {
        return cast(UInt(8, e.type().width), min(e, 255));
    } else {
        return cast(UInt(8, e.type().width), clamp(e, 0, 255));
    }
}

Expr _i16q(Expr e) {
    return cast(Int(16, e.type().width), clamp(e, -32768, 32767));
}

Expr _u16q(Expr e) {
    if (e.type().is_uint()) {
        return cast(UInt(16, e.type().width), min(e, 65535));
    } else {
        return cast(UInt(16, e.type().width), clamp(e, 0, 65535));
    }
}

Expr _i32q(Expr e) {
    return cast(Int(32, e.type().width), clamp(e, Int(32).min(), Int(32).max()));
}

Expr _u32q(Expr e) {
    if (e.type().is_uint()) {
        return cast(UInt(32, e.type().width), min(e, UInt(32).max()));
    } else {
        return cast(UInt(32, e.type().width), clamp(e, 0, UInt(32).max()));
    }
}
}

CodeGen_ARM::CodeGen_ARM(Target t) : CodeGen_Posix(),
                                     target(t) {
    #if !(WITH_ARM)
    assert(false && "arm not enabled for this build of Halide.");
    #endif

    if (t.bits == 32) {
        assert(llvm_ARM_enabled && "llvm build not configured with ARM target enabled.");
    } else {
        assert(llvm_AArch64_enabled && "llvm build not configured with AArch64 target enabled.");
    }

    #if !(WITH_NATIVE_CLIENT)
    assert(t.os != Target::NaCl && "llvm build not configured with native client enabled.");
    #endif

    // These patterns went away in llvm commit r189481, which is
    // unfortunate, because they don't always get generated
    // automatically. It also means that vshiftn catches these
    // patterns instead of letting them fall through to the natural
    // bitcode that triggers llvm's recognition.
    #if LLVM_VERSION < 34
    casts.push_back(Pattern("vaddhn.v8i8", _i8((wild_i16x8 + wild_i16x8)/256)));
    casts.push_back(Pattern("vaddhn.v4i16", _i16((wild_i32x4 + wild_i32x4)/65536)));
    casts.push_back(Pattern("vaddhn.v8i8", _u8((wild_u16x8 + wild_u16x8)/256)));
    casts.push_back(Pattern("vaddhn.v4i16", _u16((wild_u32x4 + wild_u32x4)/65536)));
    casts.push_back(Pattern("vsubhn.v8i8", _i8((wild_i16x8 - wild_i16x8)/256)));
    casts.push_back(Pattern("vsubhn.v4i16", _i16((wild_i32x4 - wild_i32x4)/65536)));
    casts.push_back(Pattern("vsubhn.v8i8", _u8((wild_u16x8 - wild_u16x8)/256)));
    casts.push_back(Pattern("vsubhn.v4i16", _u16((wild_u32x4 - wild_u32x4)/65536)));
    #endif

    // Generate the cast patterns that can take vector or scalar
    // types.  We need to iterate over all 64 and 128 bit integer
    // types relevant for neon.
    Type types[] = {Int(8, 8), Int(8, 16), UInt(8, 8), UInt(8, 16),
                    Int(16, 4), Int(16, 8), UInt(16, 4), UInt(16, 8),
                    Int(32, 2), Int(32, 4), UInt(32, 2), UInt(32, 4)};
    for (int i = 0; i < 12; i++) {
        Type t = types[i];
        std::ostringstream oss;
        oss << (t.is_int() ? 's' : 'u') << ".v" << t.width << "i" << t.bits;
        string t_str = oss.str();

        // Wider versions of the type
        Type w = t;
        w.bits *= 2;
        Type ws = Int(t.bits*2, t.width);

        // Vector wildcard for this type
        Expr vector = Variable::make(t, "*");
        Expr w_vector = cast(w, vector);
        Expr ws_vector = cast(ws, vector);

        // Scalar wildcard for this type
        Expr scalar = Variable::make(t.element_of(), "*");
        Expr w_scalar = cast(w, scalar);
        Expr ws_scalar = cast(ws, scalar);

        // Bounds of the type stored in the wider vector type
        Expr tmin = simplify(cast(w, t.imin()));
        Expr tmax = simplify(cast(w, t.imax()));
        Expr tsmin = simplify(cast(ws, t.imin()));
        Expr tsmax = simplify(cast(ws, t.imax()));

        // Can't fit uint32 max into an intimm
        if (t.element_of() == UInt(32)) {
            tmax = simplify(cast(w, t.max()));
            tsmax = simplify(cast(ws, t.max()));
        }

        // Rounding-up averaging
        casts.push_back(Pattern("vrhadd" + t_str, cast(t, (w_vector + w_vector + 1)/2)));
        casts.push_back(Pattern("vrhadd" + t_str, cast(t, (w_vector + (w_scalar + 1))/2)));
        casts.push_back(Pattern("vrhadd" + t_str, cast(t, ((w_scalar + 1) + w_vector)/2)));
        casts.push_back(Pattern("vrhadd" + t_str, cast(t, ((w_scalar + w_vector) + 1)/2)));
        casts.push_back(Pattern("vrhadd" + t_str, cast(t, ((w_vector + w_scalar) + 1)/2)));

        // Rounding down averaging
        casts.push_back(Pattern("vhadd" + t_str, cast(t, (w_vector + w_vector)/2)));
        casts.push_back(Pattern("vhadd" + t_str, cast(t, (w_vector + w_scalar)/2)));
        casts.push_back(Pattern("vhadd" + t_str, cast(t, (w_scalar + w_vector)/2)));

        // Halving subtract
        casts.push_back(Pattern("vhsub" + t_str, cast(t, (w_vector - w_vector)/2)));
        casts.push_back(Pattern("vhsub" + t_str, cast(t, (w_vector - w_scalar)/2)));
        casts.push_back(Pattern("vhsub" + t_str, cast(t, (w_scalar - w_vector)/2)));

        // Saturating add
        casts.push_back(Pattern("vqadd" + t_str, cast(t, clamp(w_vector + w_vector, tmin, tmax))));
        casts.push_back(Pattern("vqadd" + t_str, cast(t, clamp(w_vector + w_scalar, tmin, tmax))));
        casts.push_back(Pattern("vqadd" + t_str, cast(t, clamp(w_scalar + w_vector, tmin, tmax))));

        // In the unsigned case, the saturation below in unnecessary
        if (t.is_uint()) {
            casts.push_back(Pattern("vqadd" + t_str, cast(t, min(w_vector + w_vector, tmax))));
            casts.push_back(Pattern("vqadd" + t_str, cast(t, min(w_vector + w_scalar, tmax))));
            casts.push_back(Pattern("vqadd" + t_str, cast(t, min(w_scalar + w_vector, tmax))));
        }

        // Saturating subtract
        // N.B. Saturating subtracts always widen to a signed type
        casts.push_back(Pattern("vqsub" + t_str, cast(t, clamp(ws_vector - ws_vector, tsmin, tsmax))));
        casts.push_back(Pattern("vqsub" + t_str, cast(t, clamp(ws_vector - ws_scalar, tsmin, tsmax))));
        casts.push_back(Pattern("vqsub" + t_str, cast(t, clamp(ws_scalar - ws_vector, tsmin, tsmax))));

        // In the unsigned case, we may detect that the top of the clamp is unnecessary
        if (t.is_uint()) {
            casts.push_back(Pattern("vqsub" + t_str, cast(t, max(ws_vector - ws_vector, 0))));
            casts.push_back(Pattern("vqsub" + t_str, cast(t, max(ws_scalar - ws_vector, 0))));
            casts.push_back(Pattern("vqsub" + t_str, cast(t, max(ws_vector - ws_scalar, 0))));
        }
    }

    casts.push_back(Pattern("vshiftn.v8i8", _i8(wild_i16x8/wild_i16x8), Pattern::RightShift));
    casts.push_back(Pattern("vshiftn.v4i16", _i16(wild_i32x4/wild_i32x4), Pattern::RightShift));
    casts.push_back(Pattern("vshiftn.v2i32", _i32(wild_i64x2/wild_i64x2), Pattern::RightShift));
    casts.push_back(Pattern("vshiftn.v8i8", _u8(wild_u16x8/wild_u16x8), Pattern::RightShift));
    casts.push_back(Pattern("vshiftn.v4i16", _u16(wild_u32x4/wild_u32x4), Pattern::RightShift));
    casts.push_back(Pattern("vshiftn.v2i32", _u32(wild_u64x2/wild_u64x2), Pattern::RightShift));

    casts.push_back(Pattern("vqshiftns.v8i8", _i8q(wild_i16x8/wild_i16x8), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftns.v4i16", _i16q(wild_i32x4/wild_i32x4), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftns.v2i32", _i32q(wild_i64x2/wild_i64x2), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftnu.v8i8", _u8q(wild_u16x8/wild_u16x8), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftnu.v4i16", _u16q(wild_u32x4/wild_u32x4), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftnu.v2i32", _u32q(wild_u64x2/wild_u64x2), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftnsu.v8i8", _u8q(wild_i16x8/wild_i16x8), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftnsu.v4i16", _u16q(wild_i32x4/wild_i32x4), Pattern::RightShift));
    casts.push_back(Pattern("vqshiftnsu.v2i32", _u32q(wild_i64x2/wild_i64x2), Pattern::RightShift));

    casts.push_back(Pattern("vqshifts.v8i8", _i8q(_i16(wild_i8x8)*wild_i16x8), Pattern::LeftShift));
    casts.push_back(Pattern("vqshifts.v4i16", _i16q(_i32(wild_i16x4)*wild_i32x4), Pattern::LeftShift));
    casts.push_back(Pattern("vqshifts.v2i32", _i32q(_i64(wild_i32x2)*wild_i64x2), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftu.v8i8", _u8q(_u16(wild_u8x8)*wild_u16x8), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftu.v4i16", _u16q(_u32(wild_u16x4)*wild_u32x4), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftu.v2i32", _u32q(_u64(wild_u32x2)*wild_u64x2), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftsu.v8i8", _u8q(_i16(wild_i8x8)*wild_i16x8), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftsu.v4i16", _u16q(_i32(wild_i16x4)*wild_i32x4), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftsu.v2i32", _u32q(_i64(wild_i32x2)*wild_i64x2), Pattern::LeftShift));
    casts.push_back(Pattern("vqshifts.v16i8", _i8q(_i16(wild_i8x16)*wild_i16x16), Pattern::LeftShift));
    casts.push_back(Pattern("vqshifts.v8i16", _i16q(_i32(wild_i16x8)*wild_i32x8), Pattern::LeftShift));
    casts.push_back(Pattern("vqshifts.v4i32", _i32q(_i64(wild_i32x4)*wild_i64x4), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftu.v16i8", _u8q(_u16(wild_u8x16)*wild_u16x16), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftu.v8i16", _u16q(_u32(wild_u16x8)*wild_u32x8), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftu.v4i32", _u32q(_u64(wild_u32x4)*wild_u64x4), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftsu.v16i8", _u8q(_i16(wild_i8x16)*wild_i16x16), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftsu.v8i16", _u16q(_i32(wild_i16x8)*wild_i32x8), Pattern::LeftShift));
    casts.push_back(Pattern("vqshiftsu.v4i32", _u32q(_i64(wild_i32x4)*wild_i64x4), Pattern::LeftShift));

    casts.push_back(Pattern("vqmovns.v8i8", _i8q(wild_i16x8)));
    casts.push_back(Pattern("vqmovns.v4i16", _i16q(wild_i32x4)));
    casts.push_back(Pattern("vqmovns.v2i32", _i32q(wild_i64x2)));
    casts.push_back(Pattern("vqmovnu.v8i8", _u8q(wild_u16x8)));
    casts.push_back(Pattern("vqmovnu.v4i16", _u16q(wild_u32x4)));
    casts.push_back(Pattern("vqmovnu.v2i32", _u32q(wild_u64x2)));
    casts.push_back(Pattern("vqmovnsu.v8i8", _u8q(wild_i16x8)));
    casts.push_back(Pattern("vqmovnsu.v4i16", _u16q(wild_i32x4)));
    casts.push_back(Pattern("vqmovnsu.v2i32", _u32q(wild_i64x2)));

    // Widening left shifts
    left_shifts.push_back(Pattern("vshiftls.v8i16", _i16(wild_i8x8)*wild_i16x8, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftls.v4i32", _i32(wild_i16x4)*wild_i32x4, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftls.v2i64", _i64(wild_i32x2)*wild_i64x2, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftlu.v8i16", _u16(wild_u8x8)*wild_u16x8, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftlu.v4i32", _u32(wild_u16x4)*wild_u32x4, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftlu.v2i64", _u64(wild_u32x2)*wild_u64x2, Pattern::LeftShift));

    // Non-widening left shifts
    left_shifts.push_back(Pattern("vshifts.v16i8", wild_i8x16*wild_i8x16, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshifts.v8i16", wild_i16x8*wild_i16x8, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshifts.v4i32", wild_i32x4*wild_i32x4, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshifts.v2i64", wild_i64x2*wild_i64x2, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v16i8", wild_u8x16*wild_u8x16, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v8i16", wild_u16x8*wild_u16x8, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v4i32", wild_u32x4*wild_u32x4, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v2i64", wild_u64x2*wild_u64x2, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshifts.v8i8",  wild_i8x8*wild_i8x8, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshifts.v4i16", wild_i16x4*wild_i16x4, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshifts.v2i32", wild_i32x2*wild_i32x2, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v8i8",  wild_u8x8*wild_u8x8, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v4i16", wild_u16x4*wild_u16x4, Pattern::LeftShift));
    left_shifts.push_back(Pattern("vshiftu.v2i32", wild_u32x2*wild_u32x2, Pattern::LeftShift));

    averagings.push_back(Pattern("vhadds.v8i8", (wild_i8x8 + wild_i8x8)));
    averagings.push_back(Pattern("vhaddu.v8i8", (wild_u8x8 + wild_u8x8)));
    averagings.push_back(Pattern("vhadds.v4i16", (wild_i16x4 + wild_i16x4)));
    averagings.push_back(Pattern("vhaddu.v4i16", (wild_u16x4 + wild_u16x4)));
    averagings.push_back(Pattern("vhadds.v2i32", (wild_i32x2 + wild_i32x2)));
    averagings.push_back(Pattern("vhaddu.v2i32", (wild_u32x2 + wild_u32x2)));
    averagings.push_back(Pattern("vhadds.v16i8", (wild_i8x16 + wild_i8x16)));
    averagings.push_back(Pattern("vhaddu.v16i8", (wild_u8x16 + wild_u8x16)));
    averagings.push_back(Pattern("vhadds.v8i16", (wild_i16x8 + wild_i16x8)));
    averagings.push_back(Pattern("vhaddu.v8i16", (wild_u16x8 + wild_u16x8)));
    averagings.push_back(Pattern("vhadds.v4i32", (wild_i32x4 + wild_i32x4)));
    averagings.push_back(Pattern("vhaddu.v4i32", (wild_u32x4 + wild_u32x4)));
    averagings.push_back(Pattern("vhsubs.v8i8", (wild_i8x8 - wild_i8x8)));
    averagings.push_back(Pattern("vhsubu.v8i8", (wild_u8x8 - wild_u8x8)));
    averagings.push_back(Pattern("vhsubs.v4i16", (wild_i16x4 - wild_i16x4)));
    averagings.push_back(Pattern("vhsubu.v4i16", (wild_u16x4 - wild_u16x4)));
    averagings.push_back(Pattern("vhsubs.v2i32", (wild_i32x2 - wild_i32x2)));
    averagings.push_back(Pattern("vhsubu.v2i32", (wild_u32x2 - wild_u32x2)));
    averagings.push_back(Pattern("vhsubs.v16i8", (wild_i8x16 - wild_i8x16)));
    averagings.push_back(Pattern("vhsubu.v16i8", (wild_u8x16 - wild_u8x16)));
    averagings.push_back(Pattern("vhsubs.v8i16", (wild_i16x8 - wild_i16x8)));
    averagings.push_back(Pattern("vhsubu.v8i16", (wild_u16x8 - wild_u16x8)));
    averagings.push_back(Pattern("vhsubs.v4i32", (wild_i32x4 - wild_i32x4)));
    averagings.push_back(Pattern("vhsubu.v4i32", (wild_u32x4 - wild_u32x4)));

    negations.push_back(Pattern("vqneg.v8i8", -max(wild_i8x8, -127)));
    negations.push_back(Pattern("vqneg.v16i8", -max(wild_i8x16, -127)));
    negations.push_back(Pattern("vqneg.v4i16", -max(wild_i16x4, -32767)));
    negations.push_back(Pattern("vqneg.v8i16", -max(wild_i16x8, -32767)));
    negations.push_back(Pattern("vqneg.v2i32", -max(wild_i32x2, -(0x7fffffff))));
    negations.push_back(Pattern("vqneg.v4i32", -max(wild_i32x4, -(0x7fffffff))));

}


void CodeGen_ARM::compile(Stmt stmt, string name,
                          const vector<Argument> &args,
                          const vector<Buffer> &images_to_embed) {

    init_module();

    module = get_initial_module_for_target(target, context);

    // Fix the target triple.
    if (target.bits == 64) {

    }

    debug(1) << "Target triple of initial module: " << module->getTargetTriple() << "\n";
    llvm::Triple triple;
    if (target.bits == 32) {
        triple.setArch(llvm::Triple::arm);
    } else {
        assert(target.bits == 64);
        #if (WITH_ARM64)
        triple.setArch(llvm::Triple::aarch64);
        #else
        assert(false && "AArch64 llvm target not enabled in this build of Halide");
        #endif
        std::cerr << "WARNING: 64-bit arm builds are completely untested\n";
    }

    if (target.os == Target::Android) {
        assert(target.bits == 32 && "Not sure what llvm target triple to use for 64-bit arm android");
        triple.setOS(llvm::Triple::Linux);
        triple.setEnvironment(llvm::Triple::EABI);
    } else if (target.os == Target::IOS) {
        triple.setOS(llvm::Triple::IOS);
        triple.setVendor(llvm::Triple::Apple);
    } else if (target.os == Target::NaCl) {
        assert(target.bits == 32 && "Not sure what llvm target triple to use for 64-bit arm nacl");
        #if WITH_NATIVE_CLIENT
        triple.setOS(llvm::Triple::NaCl);
        triple.setEnvironment(llvm::Triple::EABI);
        // The ARM Nacl backend relies on global switches being set to do
        // the sandboxing, so set them here.
        llvm::FlagSfiData = true;
        llvm::FlagSfiLoad = true;
        llvm::FlagSfiStore = true;
        llvm::FlagSfiStack = true;
        llvm::FlagSfiBranch = true;
        llvm::FlagSfiDisableCP = true;
        llvm::FlagSfiZeroMask = false;
        ReserveR9 = true;
        #else
        assert(false && "This version of Halide was compiled without nacl support");
        #endif
    } else if (target.os == Target::Linux) {
        triple.setOS(llvm::Triple::Linux);
        triple.setEnvironment(llvm::Triple::GNUEABIHF);
    } else {
        assert(false && "No arm support for this OS");
    }
    module->setTargetTriple(triple.str());
    debug(1) << "Target triple of initial module: " << module->getTargetTriple() << "\n";

    // Pass to the generic codegen
    CodeGen::compile(stmt, name, args, images_to_embed);

    // Optimize
    CodeGen::optimize_module();
}


Value *CodeGen_ARM::call_intrin(Type result_type, const string &name, vector<Expr> args) {
    vector<Value *> arg_values(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        arg_values[i] = codegen(args[i]);
    }

    return call_intrin(llvm_type_of(result_type), name, arg_values);
}

Value *CodeGen_ARM::call_intrin(llvm::Type *result_type,
                                const string &name,
                                vector<Value *> arg_values) {
    vector<llvm::Type *> arg_types(arg_values.size());
    for (size_t i = 0; i < arg_values.size(); i++) {
        arg_types[i] = arg_values[i]->getType();
    }

    llvm::Function *fn = module->getFunction("llvm.arm.neon." + name);

    if (!fn) {
        FunctionType *func_t = FunctionType::get(result_type, arg_types, false);
        fn = llvm::Function::Create(func_t,
                                    llvm::Function::ExternalLinkage,
                                    "llvm.arm.neon." + name, module);
        fn->setCallingConv(CallingConv::C);

        if (starts_with(name, "vld")) {
            fn->setOnlyReadsMemory();
            fn->setDoesNotCapture(1);
        } else {
            fn->setDoesNotAccessMemory();
        }
        fn->setDoesNotThrow();

    }

    debug(4) << "Creating call to " << name << "\n";

    return builder->CreateCall(fn, arg_values, name);

}

Instruction *CodeGen_ARM::call_void_intrin(const string &name, vector<Expr> args) {
    vector<Value *> arg_values(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        arg_values[i] = codegen(args[i]);
    }

    return call_void_intrin(name, arg_values);
}


Instruction *CodeGen_ARM::call_void_intrin(const string &name, vector<Value *> arg_values) {
    vector<llvm::Type *> arg_types(arg_values.size());
    for (size_t i = 0; i < arg_values.size(); i++) {
        arg_types[i] = arg_values[i]->getType();
    }

    llvm::Function *fn = module->getFunction("llvm.arm.neon." + name);

    if (!fn) {
        FunctionType *func_t = FunctionType::get(void_t, arg_types, false);
        fn = llvm::Function::Create(func_t,
                                    llvm::Function::ExternalLinkage,
                                    "llvm.arm.neon." + name, module);
        fn->setCallingConv(CallingConv::C);

        if (starts_with(name, "vst")) {
            fn->setDoesNotCapture(1);
        }
        fn->setDoesNotThrow();
    }

    debug(4) << "Creating call to " << name << "\n";
    return builder->CreateCall(fn, arg_values);
}

void CodeGen_ARM::visit(const Cast *op) {
    vector<Expr> matches;

    for (size_t i = 0; i < casts.size() ; i++) {
        const Pattern &pattern = casts[i];
        //debug(4) << "Trying pattern: " << patterns[i].intrin << " " << patterns[i].pattern << "\n";
        if (expr_match(pattern.pattern, op, matches)) {
            // Broadcast any scalar args
            for (size_t i = 0; i < matches.size(); i++) {
                if (op->type.is_vector() && matches[i].type().is_scalar()) {
                    matches[i] = Broadcast::make(matches[i], op->type.width);
                }
            }

            //debug(4) << "Match!\n";
            if (pattern.type == Pattern::Simple) {
                value = call_intrin(pattern.pattern.type(), pattern.intrin, matches);
                return;
            } else { // must be a shift
                Expr constant = matches[1];
                int shift_amount;
                bool power_of_two = is_const_power_of_two(constant, &shift_amount);
                if (power_of_two && shift_amount < matches[0].type().bits) {
                    if (pattern.type == Pattern::RightShift) {
                        shift_amount = -shift_amount;
                    } else {
                        assert(pattern.type == Pattern::LeftShift);
                    }
                    Value *shift = ConstantInt::get(llvm_type_of(matches[0].type()),
                                                    shift_amount);
                    value = call_intrin(llvm_type_of(pattern.pattern.type()),
                                        pattern.intrin,
                                        vec(codegen(matches[0]), shift));
                    return;
                }
            }
        }
    }


    CodeGen::visit(op);

}

void CodeGen_ARM::visit(const Mul *op) {
    // We only have peephole optimizations for int vectors for now
    if (op->type.is_scalar() || op->type.is_float()) {
        CodeGen::visit(op);
        return;
    }

    // Vector multiplies by 3, 5, 7, 9 should do shift-and-add or
    // shift-and-sub instead to reduce register pressure (the
    // shift is an immediate)
    if (is_const(op->b, 3)) {
        value = codegen(op->a*2 + op->a);
        return;
    } else if (is_const(op->b, 5)) {
        value = codegen(op->a*4 + op->a);
        return;
    } else if (is_const(op->b, 7)) {
        value = codegen(op->a*8 - op->a);
        return;
    } else if (is_const(op->b, 9)) {
        value = codegen(op->a*8 + op->a);
        return;
    }

    vector<Expr> matches;

    int shift_amount = 0;
    bool power_of_two = is_const_power_of_two(op->b, &shift_amount);
    if (power_of_two) {
        for (size_t i = 0; i < left_shifts.size(); i++) {
            const Pattern &pattern = left_shifts[i];
            assert(pattern.type == Pattern::LeftShift);
            if (expr_match(pattern.pattern, op, matches)) {
                llvm::Type *t_arg = llvm_type_of(matches[0].type());
                llvm::Type *t_result = llvm_type_of(pattern.pattern.type());
                Value *shift = ConstantInt::get(t_arg, shift_amount);
                value = call_intrin(t_result, pattern.intrin, vec(codegen(matches[0]), shift));
                return;
            }
        }
    }

    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const Div *op) {

    if (is_two(op->b) && (op->a.as<Add>() || op->a.as<Sub>())) {
        vector<Expr> matches;
        for (size_t i = 0; i < averagings.size(); i++) {
            if (expr_match(averagings[i].pattern, op->a, matches)) {
                value = call_intrin(matches[0].type(), averagings[i].intrin, matches);
                return;
            }
        }
    }

    // Detect if it's a small int division
    const Broadcast *broadcast = op->b.as<Broadcast>();
    const Cast *cast_b = broadcast ? broadcast->value.as<Cast>() : NULL;
    const IntImm *int_imm = cast_b ? cast_b->value.as<IntImm>() : NULL;
    if (broadcast && !int_imm) int_imm = broadcast->value.as<IntImm>();
    if (!int_imm) int_imm = op->b.as<IntImm>();
    int const_divisor = int_imm ? int_imm->value : 0;

    // Check if the divisor is a power of two
    int shift_amount;
    bool power_of_two = is_const_power_of_two(op->b, &shift_amount);

    vector<Expr> matches;
    if (op->type == Float(32, 4) && is_one(op->a)) {
        // Reciprocal and reciprocal square root
        if (expr_match(Call::make(Float(32, 4), "sqrt_f32", vec(wild_f32x4), Call::Extern), op->b, matches)) {
            value = call_intrin(Float(32, 4), "vrsqrte.v4f32", matches);
        } else {
            value = call_intrin(Float(32, 4), "vrecpe.v4f32", vec(op->b));
        }
    } else if (op->type == Float(32, 2) && is_one(op->a)) {
        // Reciprocal and reciprocal square root
        if (expr_match(Call::make(Float(32, 2), "sqrt_f32", vec(wild_f32x2), Call::Extern), op->b, matches)) {
            value = call_intrin(Float(32, 2), "vrsqrte.v2f32", matches);
        } else {
            value = call_intrin(Float(32, 2), "vrecpe.v2f32", vec(op->b));
        }
    } else if (power_of_two && op->type.is_int()) {
        Value *numerator = codegen(op->a);
        Constant *shift = ConstantInt::get(llvm_type_of(op->type), shift_amount);
        value = builder->CreateAShr(numerator, shift);
    } else if (power_of_two && op->type.is_uint()) {
        Value *numerator = codegen(op->a);
        Constant *shift = ConstantInt::get(llvm_type_of(op->type), shift_amount);
        value = builder->CreateLShr(numerator, shift);
    } else if (op->type.is_int() &&
               (op->type.bits == 32 || op->type.bits == 16 || op->type.bits == 8) &&
               const_divisor > 1 &&
               ((op->type.bits > 8 && const_divisor < 256) || const_divisor < 128)) {

        int64_t multiplier, shift;
        if (op->type.bits == 32) {
            multiplier = IntegerDivision::table_s32[const_divisor][2];
            shift      = IntegerDivision::table_s32[const_divisor][3];
        } else if (op->type.bits == 16) {
            multiplier = IntegerDivision::table_s16[const_divisor][2];
            shift      = IntegerDivision::table_s16[const_divisor][3];
        } else {
            // 8 bit
            multiplier = IntegerDivision::table_s8[const_divisor][2];
            shift      = IntegerDivision::table_s8[const_divisor][3];
        }

        Value *val = codegen(op->a);

        // Make an all-ones mask if the numerator is negative
        Value *sign = builder->CreateAShr(val, codegen(make_const(op->type, op->type.bits-1)));
        // Flip the numerator bits if the mask is high
        Value *flipped = builder->CreateXor(sign, val);
        // Grab the multiplier
        Value *mult = codegen(make_const(op->type, (int)multiplier));
        // Widening multiply
        llvm::Type *narrower = llvm_type_of(op->type);
        llvm::Type *wider = llvm_type_of(Int(op->type.bits*2, op->type.width));
        // flipped's high bit is zero, so it's ok to zero-extend it
        Value *flipped_wide = builder->CreateIntCast(flipped, wider, false);
        Value *mult_wide = builder->CreateIntCast(mult, wider, false);
        Value *wide_val = builder->CreateMul(flipped_wide, mult_wide);
        // Do the shift (add 8 or 16 to narrow back down)
        if (op->type == Int(32, 2) && shift == 0) {
            Constant *shift_amount = ConstantInt::get(wider, -32);
            val = call_intrin(narrower, "vshiftn.v2i32", vec<Value *>(wide_val, shift_amount));
        } else if (op->type == Int(16, 4) && shift == 0) {
            Constant *shift_amount = ConstantInt::get(wider, -16);
            val = call_intrin(narrower, "vshiftn.v4i16", vec<Value *>(wide_val, shift_amount));
        } else if (op->type == Int(8, 8) && shift == 0) {
            Constant *shift_amount = ConstantInt::get(wider, -8);
            val = call_intrin(narrower, "vshiftn.v8i8", vec<Value *>(wide_val, shift_amount));
        } else {
            Constant *shift_amount = ConstantInt::get(wider, (shift + op->type.bits));
            val = builder->CreateLShr(wide_val, shift_amount);
            val = builder->CreateIntCast(val, narrower, true);
        }
        // Maybe flip the bits again
        value = builder->CreateXor(val, sign);

    } else if (op->type.is_uint() &&
               (op->type.bits == 32 || op->type.bits == 16 || op->type.bits == 8) &&
               const_divisor > 1 && const_divisor < 256) {

        int64_t method, multiplier, shift;
        if (op->type.bits == 32) {
            method     = IntegerDivision::table_u32[const_divisor][1];
            multiplier = IntegerDivision::table_u32[const_divisor][2];
            shift      = IntegerDivision::table_u32[const_divisor][3];
        } else if (op->type.bits == 16) {
            method     = IntegerDivision::table_u16[const_divisor][1];
            multiplier = IntegerDivision::table_u16[const_divisor][2];
            shift      = IntegerDivision::table_u16[const_divisor][3];
        } else {
            method     = IntegerDivision::table_u8[const_divisor][1];
            multiplier = IntegerDivision::table_u8[const_divisor][2];
            shift      = IntegerDivision::table_u8[const_divisor][3];
        }

        assert(method != 0 &&
               "method 0 division is for powers of two and should have been handled elsewhere");

        Value *num = codegen(op->a);

        // Widen
        llvm::Type *narrower = llvm_type_of(op->type);
        llvm::Type *wider = llvm_type_of(UInt(op->type.bits*2, op->type.width));
        Value *mult = ConstantInt::get(narrower, multiplier);
        mult = builder->CreateIntCast(mult, wider, false);
        Value *val = builder->CreateIntCast(num, wider, false);

        // Multiply
        val = builder->CreateMul(val, mult);

        // Narrow
        if (op->type == UInt(32, 2) && shift == 0) {
            Constant *shift_amount = ConstantInt::get(wider, -32);
            val = call_intrin(narrower, "vshiftn.v2i32", vec<Value *>(val, shift_amount));
        } else if (op->type == UInt(16, 4) && shift == 0) {
            Constant *shift_amount = ConstantInt::get(wider, -16);
            val = call_intrin(narrower, "vshiftn.v4i16", vec<Value *>(val, shift_amount));
        } else if (op->type == UInt(8, 8) && shift == 0) {
            Constant *shift_amount = ConstantInt::get(wider, -8);
            val = call_intrin(narrower, "vshiftn.v8i8", vec<Value *>(val, shift_amount));
        } else {
            int shift_bits = op->type.bits;
            // For method 1, we can do the final shift here too.
            if (method == 1) {
                shift_bits += (int)shift;
            }
            Constant *shift_amount = ConstantInt::get(wider, shift_bits);
            val = builder->CreateLShr(val, shift_amount);
            val = builder->CreateIntCast(val, narrower, false);
        }

        // Average with original numerator
        if (method == 2) {
            if (op->type == Int(32, 2)) {
                val = call_intrin(narrower, "vhaddu.v2i32", vec(val, num));
            } else if (op->type == Int(16, 4)) {
                val = call_intrin(narrower, "vhaddu.v4i16", vec(val, num));
            } else if (op->type == Int(8, 8)) {
                val = call_intrin(narrower, "vhaddu.v8i8", vec(val, num));
            } else {
                // num > val, so the following works without widening:
                // val += (num - val)/2
                Value *diff = builder->CreateSub(num, val);
                diff = builder->CreateLShr(diff, ConstantInt::get(diff->getType(), 1));
                val = builder->CreateAdd(val, diff);
            }

            // Do the final shift
            if (shift) {
                val = builder->CreateLShr(val, ConstantInt::get(narrower, shift));
            }
        }

        value = val;

    } else {

        CodeGen::visit(op);
    }
}

void CodeGen_ARM::visit(const Add *op) {
    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const Sub *op) {

    vector<Expr> matches;
    for (size_t i = 0; i < negations.size(); i++) {
        if (expr_match(negations[i].pattern, op, matches)) {
            value = call_intrin(matches[0].type(), negations[i].intrin, matches);
            return;
        }
    }

    // llvm will generate floating point negate instructions if we ask for (-0.0f)-x
    if (op->type.is_float() && is_zero(op->a)) {
        Constant *a;
        if (op->type.bits == 32) {
            a = ConstantFP::getNegativeZero(f32);
        } else if (op->type.bits == 64) {
            a = ConstantFP::getNegativeZero(f64);
        } else {
            a = NULL;
            assert(false && "Unknown bit width for floating point type");
        }

        Value *b = codegen(op->b);

        if (op->type.width > 1) {
            a = ConstantVector::getSplat(op->type.width, a);
        }
        value = builder->CreateFSub(a, b);
        return;
    }

    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const Min *op) {

    if (op->type == Float(32)) {
        // Use a 2-wide vector instead
        Value *undef = UndefValue::get(f32x2);
        Constant *zero = ConstantInt::get(i32, 0);
        Value *a_wide = builder->CreateInsertElement(undef, codegen(op->a), zero);
        Value *b_wide = builder->CreateInsertElement(undef, codegen(op->b), zero);
        Value *wide_result = call_intrin(f32x2, "vmins.v2f32", vec(a_wide, b_wide));
        value = builder->CreateExtractElement(wide_result, zero);
        return;
    }

    struct {
        Type t;
        const char *op;
    } patterns[] = {
        {UInt(8, 8), "vminu.v8i8"},
        {UInt(8, 16), "vminu.v16i8"},
        {UInt(16, 4), "vminu.v4i16"},
        {UInt(16, 8), "vminu.v8i16"},
        {UInt(32, 2), "vminu.v2i32"},
        {UInt(32, 4), "vminu.v4i32"},
        {Int(8, 8), "vmins.v8i8"},
        {Int(8, 16), "vmins.v16i8"},
        {Int(16, 4), "vmins.v4i16"},
        {Int(16, 8), "vmins.v8i16"},
        {Int(32, 2), "vmins.v2i32"},
        {Int(32, 4), "vmins.v4i32"},
        {Float(32, 2), "vmins.v2f32"},
        {Float(32, 4), "vmins.v4f32"}
    };

    for (size_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
        if (op->type == patterns[i].t) {
            value = call_intrin(op->type, patterns[i].op, vec(op->a, op->b));
            return;
        }
    }

    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const Max *op) {

    if (op->type == Float(32)) {
        // Use a 2-wide vector instead
        Value *undef = UndefValue::get(f32x2);
        Constant *zero = ConstantInt::get(i32, 0);
        Value *a_wide = builder->CreateInsertElement(undef, codegen(op->a), zero);
        Value *b_wide = builder->CreateInsertElement(undef, codegen(op->b), zero);
        Value *wide_result = call_intrin(f32x2, "vmaxs.v2f32", vec(a_wide, b_wide));
        value = builder->CreateExtractElement(wide_result, zero);
        return;
    }

    struct {
        Type t;
        const char *op;
    } patterns[] = {
        {UInt(8, 8), "vmaxu.v8i8"},
        {UInt(8, 16), "vmaxu.v16i8"},
        {UInt(16, 4), "vmaxu.v4i16"},
        {UInt(16, 8), "vmaxu.v8i16"},
        {UInt(32, 2), "vmaxu.v2i32"},
        {UInt(32, 4), "vmaxu.v4i32"},
        {Int(8, 8), "vmaxs.v8i8"},
        {Int(8, 16), "vmaxs.v16i8"},
        {Int(16, 4), "vmaxs.v4i16"},
        {Int(16, 8), "vmaxs.v8i16"},
        {Int(32, 2), "vmaxs.v2i32"},
        {Int(32, 4), "vmaxs.v4i32"},
        {Float(32, 2), "vmaxs.v2f32"},
        {Float(32, 4), "vmaxs.v4f32"}
    };

    for (size_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
        if (op->type == patterns[i].t) {
            value = call_intrin(op->type, patterns[i].op, vec(op->a, op->b));
            return;
        }
    }

    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const LT *op) {
    const Call *a = op->a.as<Call>(), *b = op->b.as<Call>();

    // Check if they're hidden behind a broadcast
    const Broadcast *ba = op->a.as<Broadcast>(), *bb = op->b.as<Broadcast>();
    if (ba) a = ba->value.as<Call>();
    if (bb) b = bb->value.as<Call>();

    if (a && b) {
        Expr va = a->args[0], vb = b->args[0];
        if (va.type().width != op->type.width) {
            va = Broadcast::make(va, op->type.width);
        }
        if (vb.type().width != op->type.width) {
            vb = Broadcast::make(vb, op->type.width);
        }

        Constant *zero = ConstantVector::getSplat(op->type.width, ConstantInt::get(i32, 0));
        if (va.type() == Float(32, 4) &&
            a->name == Call::abs &&
            b->name == Call::abs) {
            value = call_intrin(Int(32, 4), "vacgtq", vec(vb, va));
            value = builder->CreateICmpNE(value, zero);
        } else if (va.type() == Float(32, 2) &&
            a->name == Call::abs &&
            b->name == Call::abs) {
            value = call_intrin(Int(32, 2), "vacgtd", vec(vb, va));
            value = builder->CreateICmpNE(value, zero);
        } else {
            CodeGen::visit(op);
        }
    } else {
        CodeGen::visit(op);
    }

}

void CodeGen_ARM::visit(const LE *op) {
    const Call *a = op->a.as<Call>(), *b = op->b.as<Call>();

    // Check if they're hidden behind a broadcast
    const Broadcast *ba = op->a.as<Broadcast>(), *bb = op->b.as<Broadcast>();
    if (ba) a = ba->value.as<Call>();
    if (bb) b = bb->value.as<Call>();

    if (a && b) {
        Expr va = a->args[0], vb = b->args[0];
        if (va.type().width != op->type.width) {
            va = Broadcast::make(va, op->type.width);
        }
        if (vb.type().width != op->type.width) {
            vb = Broadcast::make(vb, op->type.width);
        }

        Constant *zero = ConstantVector::getSplat(op->type.width, ConstantInt::get(i32, 0));
        if (va.type() == Float(32, 4) &&
            a->name == Call::abs &&
            b->name == Call::abs) {
            value = call_intrin(Int(32, 4), "vacgeq", vec(vb, va));
            value = builder->CreateICmpNE(value, zero);
        } else if (va.type() == Float(32, 2) &&
            a->name == Call::abs &&
            b->name == Call::abs) {
            value = call_intrin(Int(32, 2), "vacged", vec(vb, va));
            value = builder->CreateICmpNE(value, zero);
        } else {
            CodeGen::visit(op);
        }
    } else {
        CodeGen::visit(op);
    }

}

namespace {

// Try to losslessly narrow an integer expression to half the bit-width
Expr try_narrow(Expr a) {
    if (const Cast *c = a.as<Cast>()) {
        Type old_type = c->value.type();
        Type new_type = a.type();
        old_type.bits *= 2;
        if (old_type == new_type) {
            return c->value;
        } else {
            return Expr();
        }
    }

    if (const Broadcast *b = a.as<Broadcast>()) {
        Expr n = try_narrow(b->value);
        if (n.defined()) {
            return Broadcast::make(n, b->width);
        } else {
            return Expr();
        }
    }

    if (const IntImm *i = a.as<IntImm>()) {
        if (i->value <= Int(16).imax() &&
            i->value >= Int(16).imin()) {
            return cast(Int(16), a);
        }
    }

    return Expr();
}
}

void CodeGen_ARM::visit(const Select *op) {

    // Absolute difference patterns:
    // select(a < b, b - a, a - b)
    const LT *cmp = op->condition.as<LT>();
    const Sub *a = op->true_value.as<Sub>();
    const Sub *b = op->false_value.as<Sub>();
    Type t = op->type;

    int vec_bits = t.bits * t.width;

    if (cmp && a && b &&
        equal(a->a, b->b) &&
        equal(a->b, b->a) &&
        equal(cmp->a, a->b) &&
        equal(cmp->b, a->a) &&
        (!t.is_float()) &&
        (t.bits == 8 || t.bits == 16 || t.bits == 32 || t.bits == 64) &&
        (vec_bits == 64 || vec_bits == 128)) {

        ostringstream ss;

        // If cmp->a and cmp->b are both widening casts of a narrower
        // int, we can use vadbl instead of vabd. llvm reaches vabdl
        // by expecting you to widen the result of a narrower vabd.
        Expr na = try_narrow(cmp->a);
        Expr nb = try_narrow(cmp->b);
        if (na.defined() && nb.defined() && vec_bits == 128) {
            ss << "vabd" << (t.is_int() ? "s" : "u") << ".v" << t.width << "i" << t.bits/2;
            value = call_intrin(na.type(), ss.str(), vec(na, nb));
            value = builder->CreateIntCast(value, llvm_type_of(t), false);
        } else if (t.bits <= 32) {
            ss << "vabd" << (t.is_int() ? "s" : "u") << ".v" << t.width << "i" << t.bits;
            value = call_intrin(t, ss.str(), vec(cmp->a, cmp->b));
        } else {
            CodeGen::visit(op);
        }

        return;
    }

    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const Store *op) {

    // A dense store of an interleaving can be done using a vst2 intrinsic
    const Ramp *ramp = op->index.as<Ramp>();

    // We only deal with ramps here
    if (!ramp) {
        CodeGen::visit(op);
        return;
    }

    // First dig through let expressions
    Expr rhs = op->value;
    vector<pair<string, Expr> > lets;
    while (const Let *let = rhs.as<Let>()) {
        rhs = let->body;
        lets.push_back(make_pair(let->name, let->value));
    }
    const Call *call = rhs.as<Call>();

    if (is_one(ramp->stride) &&
        call && call->call_type == Call::Intrinsic &&
        call->name == Call::interleave_vectors) {
        assert(call->args.size() == 2 && "Wrong number of args to interleave vectors");
        vector<Value *> args(call->args.size() + 2);

        Type t = call->args[0].type();
        int alignment = t.bytes();

        Value *ptr = codegen_buffer_pointer(op->name, call->type.element_of(), ramp->base);
        ptr = builder->CreatePointerCast(ptr, i8->getPointerTo());

        // Codegen the lets
        for (size_t i = 0; i < lets.size(); i++) {
            sym_push(lets[i].first, codegen(lets[i].second));
        }

        args[0] = ptr; // The pointer
        args[1] = codegen(call->args[0]);
        args[2] = codegen(call->args[1]);
        args[3] = ConstantInt::get(i32, alignment);

        Instruction *store = NULL;
        if (t == Int(8, 8) || t == UInt(8, 8)) {
            store = call_void_intrin("vst2.v8i8", args);
        } else if (t == Int(8, 16) || t == UInt(8, 16)) {
            store = call_void_intrin("vst2.v16i8", args);
        } else if (t == Int(16, 4) || t == UInt(16, 4)) {
            store = call_void_intrin("vst2.v4i16", args);
        } else if (t == Int(16, 8) || t == UInt(16, 8)) {
            store = call_void_intrin("vst2.v8i16", args);
        } else if (t == Int(32, 2) || t == UInt(32, 2)) {
            store = call_void_intrin("vst2.v2i32", args);
        } else if (t == Int(32, 4) || t == UInt(32, 4)) {
            store = call_void_intrin("vst2.v4i32", args);
        } else if (t == Float(32, 2)) {
            store = call_void_intrin("vst2.v2f32", args);
        } else if (t == Float(32, 4)) {
            store = call_void_intrin("vst2.v4f32", args);
        } else {
            CodeGen::visit(op);
        }
        if (store) {
            add_tbaa_metadata(store, op->name);
        }

        for (size_t i = 0; i < lets.size(); i++) {
            sym_pop(lets[i].first);
        }

        return;
    }

    // If the stride is one or minus one, we can deal with that using vanilla codegen
    const IntImm *stride = ramp->stride.as<IntImm>();
    if (stride && (stride->value == 1 || stride->value == -1)) {
        CodeGen::visit(op);
        return;
    }

    // We have builtins for strided stores with fixed but unknown stride, but they use inline assembly
    if (target.os != Target::NaCl) {
        ostringstream builtin;
        builtin << "strided_store_"
                << (op->value.type().is_float() ? 'f' : 'i')
                << op->value.type().bits
                << 'x' << op->value.type().width;

        llvm::Function *fn = module->getFunction(builtin.str());
        if (fn) {
            Value *base = codegen_buffer_pointer(op->name, op->value.type().element_of(), ramp->base);
            Value *stride = codegen(ramp->stride * op->value.type().bytes());
            Value *val = codegen(op->value);
            debug(4) << "Creating call to " << builtin.str() << "\n";
            Instruction *store = builder->CreateCall(fn, vec(base, stride, val));
            (void)store;
            add_tbaa_metadata(store, op->name);
            return;
        }
    }

    CodeGen::visit(op);

}

void CodeGen_ARM::visit(const Load *op) {
    const Ramp *ramp = op->index.as<Ramp>();

    // We only deal with ramps here
    if (!ramp) {
        CodeGen::visit(op);
        return;
    }

    const IntImm *stride = ramp ? ramp->stride.as<IntImm>() : NULL;

    // If the stride is one or minus one, we can deal with that using vanilla codegen
    if (stride && (stride->value == 1 || stride->value == -1)) {
        CodeGen::visit(op);
        return;
    }

    // Strided loads with known stride
    if (stride && stride->value >= 2 && stride->value <= 4) {
        // Check alignment on the base.
        Expr base = ramp->base;
        int offset = 0;
        ModulusRemainder mod_rem = modulus_remainder(ramp->base);


        if ((mod_rem.modulus % stride->value) == 0) {
            offset = mod_rem.remainder % stride->value;
            base = simplify(base - offset);
            mod_rem.remainder -= offset;
        }

        const Add *add = base.as<Add>();
        const IntImm *add_b = add ? add->b.as<IntImm>() : NULL;
        if ((mod_rem.modulus == 1) && add_b) {
            offset = add_b->value % stride->value;
            if (offset < 0) offset += stride->value;
            base = simplify(base - offset);
        }

        int alignment = op->type.bytes();
        //alignment *= gcd(gcd(mod_rem.modulus, mod_rem.remainder), 32);
        Value *align = ConstantInt::get(i32, alignment);

        Value *ptr = codegen_buffer_pointer(op->name, op->type.element_of(), base);
        ptr = builder->CreatePointerCast(ptr, i8->getPointerTo());

        vector<llvm::Type *> type_vec(stride->value);
        llvm::Type *elem_type = llvm_type_of(op->type);
        for (int i = 0; i < stride->value; i++) {
            type_vec[i] = elem_type;
        }
        llvm::StructType *result_type = StructType::get(*context, type_vec);

        ostringstream prefix;
        prefix << "vld" << stride->value << ".";
        string pre = prefix.str();

        Value *group = NULL;
        if (op->type == Int(8, 8) || op->type == UInt(8, 8)) {
            group = call_intrin(result_type, pre+"v8i8", vec(ptr, align));
        } else if (op->type == Int(16, 4) || op->type == UInt(16, 4)) {
            group = call_intrin(result_type, pre+"v4i16", vec(ptr, align));
        } else if (op->type == Int(32, 2) || op->type == UInt(32, 2)) {
            group = call_intrin(result_type, pre+"v2i32", vec(ptr, align));
        } else if (op->type == Float(32, 2)) {
            group = call_intrin(result_type, pre+"v2f32", vec(ptr, align));
        } else if (op->type == Int(8, 16) || op->type == UInt(8, 16)) {
            group = call_intrin(result_type, pre+"v16i8", vec(ptr, align));
        } else if (op->type == Int(16, 8) || op->type == UInt(16, 8)) {
            group = call_intrin(result_type, pre+"v8i16", vec(ptr, align));
        } else if (op->type == Int(32, 4) || op->type == UInt(32, 4)) {
            group = call_intrin(result_type, pre+"v4i32", vec(ptr, align));
        } else if (op->type == Float(32, 4)) {
            group = call_intrin(result_type, pre+"v4f32", vec(ptr, align));
        }

        if (group) {
            add_tbaa_metadata(dyn_cast<Instruction>(group), op->name);
            debug(4) << "Extracting element " << offset << " from resulting struct\n";
            value = builder->CreateExtractValue(group, vec((unsigned int)offset));
            return;
        }
    }

    // We have builtins for strided loads with fixed but unknown stride, but they use inline assembly.
    if (target.os != Target::NaCl) {
        ostringstream builtin;
        builtin << "strided_load_"
                << (op->type.is_float() ? 'f' : 'i')
                << op->type.bits
                << 'x' << op->type.width;

        llvm::Function *fn = module->getFunction(builtin.str());
        if (fn) {
            Value *base = codegen_buffer_pointer(op->name, op->type.element_of(), ramp->base);
            Value *stride = codegen(ramp->stride * op->type.bytes());
            debug(4) << "Creating call to " << builtin.str() << "\n";
            Instruction *load = builder->CreateCall(fn, vec(base, stride), builtin.str());
            add_tbaa_metadata(load, op->name);
            value = load;
            return;
        }
    }

    CodeGen::visit(op);
}

void CodeGen_ARM::visit(const Call *op) {
    if (op->call_type == Call::Intrinsic &&
        op->name == Call::profiling_timer) {
        // Android devices generally have read-cycle-counter
        // disabled in user mode; fall back to calling
        // halide_current_time_ns().
        Expr e = Call::make(UInt(64), "halide_current_time_ns", std::vector<Expr>(), Call::Extern);
        e.accept(this);
        return;
    }
    CodeGen::visit(op);
}

string CodeGen_ARM::mcpu() const {
    if (target.bits == 32) {
        return "cortex-a9";
    } else {
        return "generic";
    }
}

string CodeGen_ARM::mattrs() const {
    return "+neon";
}

bool CodeGen_ARM::use_soft_float_abi() const {
    return (target.os == Target::Android) || (target.os == Target::IOS);
}

}}
