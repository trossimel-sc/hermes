/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT
#include "JitEmitter.h"

#include "JitHandlers.h"

#include "../RuntimeOffsets.h"
#include "hermes/BCGen/HBC/StackFrameLayout.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/Support/ErrorHandling.h"
#include "llvh/Support/SaveAndRestore.h"

#define DEBUG_TYPE "jit"

#if defined(HERMESVM_COMPRESSED_POINTERS) || defined(HERMESVM_BOXED_DOUBLES)
#error JIT does not support compressed pointers or boxed doubles yet
#endif

namespace hermes::vm::arm64 {
namespace {

// Ensure that HermesValue tags are handled correctly by updating this every
// time the HERMESVALUE_VERSION changes, and going through the JIT and updating
// any relevant code.
static_assert(
    HERMESVALUE_VERSION == 1,
    "HermesValue version mismatch, JIT may need to be updated");

void emit_sh_ljs_get_pointer(a64::Assembler &a, const a64::GpX inOut) {
  // See:
  // https://dinfuehr.github.io/blog/encoding-of-immediate-values-on-aarch64/
  static_assert(
      HERMESVALUE_VERSION == 1,
      "kHV_DataMask is 0x000...1111... and can be encoded as a logical immediate");
  a.and_(inOut, inOut, kHV_DataMask);
}

void emit_sh_ljs_object(a64::Assembler &a, const a64::GpX inOut) {
  static_assert(
      HERMESVALUE_VERSION == 1,
      "HVTag_Object << kHV_NumDataBits is 0x1111...0000... and can be encoded as a logical immediate");
  a.movk(inOut, (uint16_t)HVTag_Object, kHV_NumDataBits);
}

class OurErrorHandler : public asmjit::ErrorHandler {
  asmjit::Error &expectedError_;

 public:
  /// \param expectedError if we get an error matching this value, we ignore it.
  explicit OurErrorHandler(asmjit::Error &expectedError)
      : expectedError_(expectedError) {}

  void handleError(
      asmjit::Error err,
      const char *message,
      asmjit::BaseEmitter *origin) override {
    if (err == expectedError_) {
      LLVM_DEBUG(
          llvh::outs() << "Expected AsmJit error: " << err << ": "
                       << asmjit::DebugUtils::errorAsString(err) << ": "
                       << message << "\n");
      return;
    }

    llvh::errs() << "AsmJit error: " << err << ": "
                 << asmjit::DebugUtils::errorAsString(err) << ": " << message
                 << "\n";
    hermes_fatal("AsmJit error");
  }
};

class OurLogger : public asmjit::Logger {
  ASMJIT_API asmjit::Error _log(const char *data, size_t size) noexcept
      override {
    llvh::outs()
        << (size == SIZE_MAX ? llvh::StringRef(data)
                             : llvh::StringRef(data, size));
    return asmjit::kErrorOk;
  }
};

} // unnamed namespace

/// Return true if the specified 64-bit value can be efficiently loaded on
/// Arm64 with up to two integer instructions. In other words, it has at most
/// two non-zero 16-bit words.
static bool isCheapConst(uint64_t k) {
  unsigned count = 0;
  for (uint64_t mask = 0xFFFF; mask != 0; mask <<= 16) {
    if (k & mask)
      ++count;
  }
  return count <= 2;
}

#define EMIT_RUNTIME_CALL(em, type, func) \
  do {                                    \
    using _FnT = type;                    \
    _FnT _fn = func;                      \
    (void)_fn;                            \
    (em).call((void *)func, #func);       \
  } while (0)

Emitter::Emitter(
    asmjit::JitRuntime &jitRT,
    bool dumpJitCode,
    PropertyCacheEntry *readPropertyCache,
    PropertyCacheEntry *writePropertyCache,
    uint32_t numFrameRegs,
    unsigned numCount,
    unsigned npCount)
    : frameRegs_(numFrameRegs) {
  if (dumpJitCode)
    logger_ = std::unique_ptr<asmjit::Logger>(new OurLogger());
  if (logger_)
    logger_->setIndentation(asmjit::FormatIndentationGroup::kCode, 4);

  errorHandler_ = std::unique_ptr<asmjit::ErrorHandler>(
      new OurErrorHandler(expectedError_));

  code.init(jitRT.environment(), jitRT.cpuFeatures());
  code.setErrorHandler(errorHandler_.get());
  if (logger_)
    code.setLogger(logger_.get());
  code.attach(&a);

  roDataLabel_ = a.newNamedLabel("RO_DATA");
  returnLabel_ = a.newNamedLabel("leave");

  unsigned nextVec = kVecSaved.first;
  unsigned nextGp = kGPSaved.first;

  // Number registers: allocate in vector hw regs first.
  for (unsigned frIndex = 0; frIndex < numCount; ++frIndex) {
    HWReg hwReg;
    if (nextVec <= kVecSaved.second) {
      hwReg = HWReg::vecD(nextVec);
      comment("    ; alloc: d%u <= r%u", nextVec, frIndex);
      ++nextVec;
    } else if (nextGp <= kGPSaved.second) {
      hwReg = HWReg::gpX(nextGp);
      comment("    ; alloc: x%u <= r%u", nextGp, frIndex);
      ++nextGp;
    } else
      break;

    frameRegs_[frIndex].globalReg = hwReg;
    frameRegs_[frIndex].globalType = FRType::Number;
  }
  // Non-pointer regs: allocate in gp regs first.
  for (unsigned frIndex = 0; frIndex < npCount; ++frIndex) {
    HWReg hwReg;
    if (nextGp <= kGPSaved.second) {
      hwReg = HWReg::gpX(nextGp);
      comment("    ; alloc: x%u <= r%u", nextGp, frIndex);
      ++nextGp;
    } else if (nextVec <= kVecSaved.second) {
      hwReg = HWReg::vecD(nextVec);
      comment("    ; alloc: d%u <= r%u", nextVec, frIndex);
      ++nextVec;
    } else
      break;

    frameRegs_[frIndex].globalReg = hwReg;
    frameRegs_[frIndex].globalType = FRType::Unknown;
  }

  // Save read/write property cache addresses.
  roOfsReadPropertyCachePtr_ =
      uint64Const((uint64_t)readPropertyCache, "readPropertyCache");
  roOfsWritePropertyCachePtr_ =
      uint64Const((uint64_t)writePropertyCache, "writePropertyCache");

  frameSetup(numFrameRegs, nextGp - kGPSaved.first, nextVec - kVecSaved.first);
}

void Emitter::comment(const char *fmt, ...) {
  if (!logger_)
    return;
  va_list args;
  va_start(args, fmt);
  char buf[80];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  a.comment(buf);
}

JITCompiledFunctionPtr Emitter::addToRuntime(asmjit::JitRuntime &jr) {
  emitSlowPaths();
  emitThunks();
  emitROData();

  code.detach(&a);
  JITCompiledFunctionPtr fn;
  asmjit::Error err = jr.add(&fn, &code);
  if (err) {
    llvh::errs() << "AsmJit failed: " << asmjit::DebugUtils::errorAsString(err)
                 << "\n";
    hermes::hermes_fatal("AsmJit failed");
  }
  return fn;
}

void Emitter::newBasicBlock(const asmjit::Label &label) {
  syncAllTempExcept({});
  freeAllTempExcept({});

  // Clear all local types and regs when starting a new basic block.
  // TODO: there must be a faster way to do this when there are many regs.
  for (FRState &frState : frameRegs_) {
    frState.localType = frState.globalType;
    assert(!frState.localGpX);
    assert(!frState.localVecD);
  }

  a.bind(label);
}

void Emitter::frameSetup(
    unsigned numFrameRegs,
    unsigned gpSaveCount,
    unsigned vecSaveCount) {
  assert(
      gpSaveCount <= kGPSaved.second - kGPSaved.first + 1 &&
      "Too many callee saved GP regs");
  assert(
      vecSaveCount <= kVecSaved.second - kVecSaved.first + 1 &&
      "Too many callee saved Vec regs");

  static_assert(
      kGPSaved.first == 22, "Callee saved GP regs must start from x22");
  // Always save x22.
  if (gpSaveCount == 0)
    gpSaveCount = 1;
  // We always save x19, x20, x21.
  gpSaveCount += 3;

  gpSaveCount_ = gpSaveCount;
  vecSaveCount_ = vecSaveCount;

  //  0-3: SHLocals
  //  4: x22
  //  5: x21
  //  6: x20
  //  7: x19
  //  8: x29 <- new x29 points here
  //  9: x30
  a.sub(
      a64::sp,
      a64::sp,
      (4 + ((gpSaveCount + 1) & ~1) + ((vecSaveCount + 1) & ~1) + 2) * 8);

  unsigned stackOfs = 4 * 8;
  for (unsigned i = 0; i < gpSaveCount; i += 2, stackOfs += 16) {
    if (i + 1 < gpSaveCount)
      a.stp(a64::GpX(19 + i), a64::GpX(20 + i), a64::Mem(a64::sp, stackOfs));
    else
      a.str(a64::GpX(19 + i), a64::Mem(a64::sp, stackOfs));
  }
  for (unsigned i = 0; i < vecSaveCount; i += 2, stackOfs += 16) {
    if (i + 1 < vecSaveCount)
      a.stp(
          a64::VecD(kVecSaved.first + i),
          a64::VecD(kVecSaved.first + 1 + i),
          a64::Mem(a64::sp, stackOfs));
    else
      a.str(a64::VecD(kVecSaved.first + i), a64::Mem(a64::sp, stackOfs));
  }
  a.stp(a64::x29, a64::x30, a64::Mem(a64::sp, stackOfs));
  a.add(a64::x29, a64::sp, stackOfs);

  // ((uint64_t)HVTag_First << kHV_NumDataBits)
  static_assert(
      HERMESVALUE_VERSION == 1,
      "HVTag_First must be the first after double limit");
  comment("// xDoubleLim");
  a.mov(xDoubleLim, ((uint64_t)HVTag_First << kHV_NumDataBits));

  comment("// xRuntime");
  a.mov(xRuntime, a64::x0);

  //  _sh_check_native_stack_overflow(shr);
  EMIT_RUNTIME_CALL(
      *this, void (*)(SHRuntime *), _sh_check_native_stack_overflow);

  // Function<bench>(3 params, 13 registers):
  //  SHLegacyValue *frame = _sh_enter(shr, &locals.head, 13);
  comment("// _sh_enter");
  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, a64::sp);
  a.mov(a64::w2, numFrameRegs);
  EMIT_RUNTIME_CALL(
      *this, SHLegacyValue * (*)(SHRuntime *, SHLocals *, uint32_t), _sh_enter);
  comment("// xFrame");
  a.mov(xFrame, a64::x0);

  //  locals.head.count = 0;
  comment("// locals.head.count = 0");
  a.mov(a64::w1, 0);
  a.str(a64::w1, a64::Mem(a64::sp, offsetof(SHLocals, count)));
}

void Emitter::leave() {
  comment("// leaveFrame");
  a.bind(returnLabel_);
  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, a64::sp);
  a.mov(a64::x2, xFrame);
  EMIT_RUNTIME_CALL(
      *this, void (*)(SHRuntime *, SHLocals *, SHLegacyValue *), _sh_leave);

  // The return value has been stashed in x22 by ret(). Move it to the return
  // register.
  a.mov(a64::x0, a64::x22);

  unsigned stackOfs = 4 * 8;
  for (unsigned i = 0; i < gpSaveCount_; i += 2, stackOfs += 16) {
    if (i + 1 < gpSaveCount_)
      a.ldp(a64::GpX(19 + i), a64::GpX(20 + i), a64::Mem(a64::sp, stackOfs));
    else
      a.ldr(a64::GpX(19 + i), a64::Mem(a64::sp, stackOfs));
  }
  for (unsigned i = 0; i < vecSaveCount_; i += 2, stackOfs += 16) {
    if (i + 1 < vecSaveCount_)
      a.ldp(
          a64::VecD(kVecSaved.first + i),
          a64::VecD(kVecSaved.first + 1 + i),
          a64::Mem(a64::sp, stackOfs));
    else
      a.ldr(a64::VecD(kVecSaved.first + i), a64::Mem(a64::sp, stackOfs));
  }
  a.ldp(a64::x29, a64::x30, a64::Mem(a64::sp, stackOfs));

  a.add(
      a64::sp,
      a64::sp,
      (4 + ((gpSaveCount_ + 1) & ~1) + ((vecSaveCount_ + 1) & ~1) + 2) * 8);

  a.ret(a64::x30);
}

void Emitter::call(void *fn, const char *name) {
  //    comment("// call %s", name);
  a.bl(registerCall(fn, name));
}

void Emitter::loadFrameAddr(a64::GpX dst, FR frameReg) {
  // FIXME: check range of frameReg * 8
  if (frameReg == FR(0))
    a.mov(dst, xFrame);
  else
    a.add(dst, xFrame, frameReg.index() * sizeof(SHLegacyValue));
}

template <bool use>
void Emitter::movHWReg(HWReg dst, HWReg src) {
  if (dst != src) {
    if (dst.isVecD() && src.isVecD())
      a.fmov(dst.a64VecD(), src.a64VecD());
    else if (dst.isVecD())
      a.fmov(dst.a64VecD(), src.a64GpX());
    else if (src.isVecD())
      a.fmov(dst.a64GpX(), src.a64VecD());
    else
      a.mov(dst.a64GpX(), src.a64GpX());
  }
  if constexpr (use) {
    useReg(src);
    useReg(dst);
  }
}

void Emitter::_storeHWRegToFrame(FR fr, HWReg src) {
  _storeFrame(src, fr);
  frameRegs_[fr.index()].frameUpToDate = true;
}

void Emitter::movHWFromFR(HWReg hwRes, FR src) {
  FRState &frState = frameRegs_[src.index()];
  if (frState.localGpX)
    movHWReg<true>(hwRes, frState.localGpX);
  else if (frState.localVecD)
    movHWReg<true>(hwRes, frState.localVecD);
  else if (frState.globalReg && frState.globalRegUpToDate)
    movHWReg<true>(hwRes, frState.globalReg);
  else
    _loadFrame(useReg(hwRes), src);
}

void Emitter::movHWFromMem(HWReg hwRes, a64::Mem src) {
  if (hwRes.isVecD())
    a.ldr(hwRes.a64VecD(), src);
  else
    a.ldr(hwRes.a64GpX(), src);
}

void Emitter::movFRFromHW(FR dst, HWReg src, OptValue<FRType> type) {
  FRState &frState = frameRegs_[dst.index()];
  // If it is a local or global register, move the value into it and mark it as
  // updated.
  if (frState.localGpX) {
    movHWReg<false>(frState.localGpX, src);
    frUpdatedWithHWReg(dst, frState.localGpX, type);
  } else if (frState.localVecD) {
    movHWReg<false>(frState.localVecD, src);
    frUpdatedWithHWReg(dst, frState.localVecD, type);
  } else if (frState.globalReg) {
    movHWReg<false>(frState.globalReg, src);
    frUpdatedWithHWReg(dst, frState.globalReg, type);
  } else {
    // Otherwise store it directly to the frame.
    _storeHWRegToFrame(dst, src);
    if (type)
      frUpdateType(dst, *type);
    frState.frameUpToDate = true;
  }
}

template <class TAG>
HWReg Emitter::_allocTemp(TempRegAlloc &ra, llvh::Optional<HWReg> preferred) {
  llvh::Optional<unsigned> pr{};
  if (preferred)
    pr = preferred->indexInClass();
  if (auto optReg = ra.alloc(pr); optReg)
    return HWReg(*optReg, TAG{});
  // Spill one register.
  unsigned index = pr ? *pr : ra.leastRecentlyUsed();
  spillTempReg(HWReg(index, TAG{}));
  ra.free(index);
  // Allocate again. This must succeed.
  return HWReg(*ra.alloc(), TAG{});
}

void Emitter::freeReg(HWReg hwReg) {
  if (!hwReg.isValid())
    return;

  FR fr = hwRegs_[hwReg.combinedIndex()].contains;
  hwRegs_[hwReg.combinedIndex()].contains = {};

  if (hwReg.isGpX()) {
    if (fr.isValid()) {
      comment("    ; free x%u (r%u)", hwReg.indexInClass(), fr.index());
      assert(frameRegs_[fr.index()].localGpX == hwReg);
      frameRegs_[fr.index()].localGpX = {};
    } else {
      comment("    ; free x%u", hwReg.indexInClass());
    }
    if (isTempGpX(hwReg))
      gpTemp_.free(hwReg.indexInClass());
  } else {
    if (fr.isValid()) {
      comment("    ; free d%u (r%u)", hwReg.indexInClass(), fr.index());
      assert(frameRegs_[fr.index()].localVecD == hwReg);
      frameRegs_[fr.index()].localVecD = {};
    } else {
      comment("    ; free d%u", hwReg.indexInClass());
    }
    if (isTempVecD(hwReg))
      vecTemp_.free(hwReg.indexInClass());
  }
}
void Emitter::syncAndFreeTempReg(HWReg hwReg) {
  if (!hwReg.isValid() || !isTemp(hwReg) ||
      !hwRegs_[hwReg.combinedIndex()].contains.isValid()) {
    return;
  }
  spillTempReg(hwReg);
  freeReg(hwReg);
}

// TODO: check wherger we should make this call require a temp reg.
HWReg Emitter::useReg(HWReg hwReg) {
  if (!hwReg.isValid())
    return hwReg;
  // Check whether it is a temporary.
  if (hwReg.isGpX()) {
    if (isTempGpX(hwReg))
      gpTemp_.use(hwReg.indexInClass());
  } else {
    if (isTempVecD(hwReg))
      vecTemp_.use(hwReg.indexInClass());
  }
  return hwReg;
}

void Emitter::spillTempReg(HWReg toSpill) {
  assert(isTemp(toSpill));

  HWRegState &hwState = hwRegs_[toSpill.combinedIndex()];
  FR fr = hwState.contains;
  hwState.contains = {};
  assert(fr.isValid() && "Allocated tmp register is unused");

  FRState &frState = frameRegs_[fr.index()];

  assert(frState.globalReg != toSpill && "global regs can't be temporary");
  if (frState.globalReg) {
    if (!frState.globalRegUpToDate) {
      movHWReg<false>(frState.globalReg, toSpill);
      frState.globalRegUpToDate = true;
    }
  } else {
    if (!frState.frameUpToDate) {
      _storeHWRegToFrame(fr, toSpill);
      frState.frameUpToDate = true;
    }
  }

  if (frState.localGpX == toSpill)
    frState.localGpX = {};
  else if (frState.localVecD == toSpill)
    frState.localVecD = {};
  else
    assert(false && "local reg not used by FR");
}

void Emitter::syncToMem(FR fr) {
  FRState &frState = frameRegs_[fr.index()];
  if (frState.frameUpToDate)
    return;

  HWReg hwReg = isFRInRegister(fr);
  assert(
      hwReg.isValid() && "FR is not synced to frame and is not in a register");

  // We have an invariant that the global reg cannot have an old value if the
  // frame has a new one.
  if (frState.globalReg && !frState.globalRegUpToDate) {
    assert(hwReg != frState.globalReg && "FR is in a global reg");
    movHWReg<false>(frState.globalReg, hwReg);
    frState.globalRegUpToDate = true;
  }
  _storeHWRegToFrame(fr, hwReg);
}

void Emitter::syncAllTempExcept(FR exceptFR) {
  for (unsigned i = kGPTemp.first; i <= kGPTemp.second; ++i) {
    HWReg hwReg(i, HWReg::GpX{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid() || fr == exceptFR)
      continue;

    FRState &frState = frameRegs_[fr.index()];
    assert(frState.localGpX == hwReg && "tmpreg not bound to FR localreg");
    if (frState.globalReg) {
      if (!frState.globalRegUpToDate) {
        comment("    ; sync: x%u (r%u)", i, fr.index());
        movHWReg<false>(frState.globalReg, hwReg);
        frState.globalRegUpToDate = true;
      }
    } else {
      if (!frState.frameUpToDate) {
        comment("    ; sync: x%u (r%u)", i, fr.index());
        _storeHWRegToFrame(fr, hwReg);
      }
    }
  }

  for (unsigned i = kVecTemp.first; i <= kVecTemp.second; ++i) {
    HWReg hwReg(i, HWReg::VecD{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid() || fr == exceptFR)
      continue;

    FRState &frState = frameRegs_[fr.index()];
    assert(frState.localVecD == hwReg && "tmpreg not bound to FR localreg");
    // If there is a local GpX, it already synced the value.
    if (frState.localGpX)
      continue;
    if (frState.globalReg) {
      if (!frState.globalRegUpToDate) {
        comment("    ; sync d%u (r%u)", i, fr.index());
        movHWReg<false>(frState.globalReg, hwReg);
        frState.globalRegUpToDate = true;
      }
    } else {
      if (!frState.frameUpToDate) {
        comment("    ; sync d%u (r%u)", i, fr.index());
        _storeHWRegToFrame(fr, hwReg);
      }
    }
  }
}

void Emitter::freeAllTempExcept(FR exceptFR) {
  for (unsigned i = kGPTemp.first; i <= kGPTemp.second; ++i) {
    HWReg hwReg(i, HWReg::GpX{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid() || fr == exceptFR)
      continue;
    freeFRTemp(fr);
  }

  for (unsigned i = kVecTemp.first; i <= kVecTemp.second; ++i) {
    HWReg hwReg(i, HWReg::VecD{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid() || fr == exceptFR)
      continue;
    freeFRTemp(fr);
  }
}

void Emitter::freeFRTemp(FR fr) {
  auto &frState = frameRegs_[fr.index()];
  if (frState.localGpX) {
    assert(isTempGpX(frState.localGpX));
    comment(
        "    ; free x%u (r%u)", frState.localGpX.indexInClass(), fr.index());
    hwRegs_[frState.localGpX.combinedIndex()].contains = {};
    gpTemp_.free(frState.localGpX.indexInClass());
    frState.localGpX = {};
  }
  if (frState.localVecD) {
    assert(isTempVecD(frState.localVecD));
    comment(
        "    ; free d%u (r%u)", frState.localVecD.indexInClass(), fr.index());
    hwRegs_[frState.localVecD.combinedIndex()].contains = {};
    vecTemp_.free(frState.localVecD.indexInClass());
    frState.localVecD = {};
  }
}

void Emitter::assignAllocatedLocalHWReg(FR fr, HWReg hwReg) {
  hwRegs_[hwReg.combinedIndex()].contains = fr;
  if (hwReg.isGpX()) {
    comment("    ; alloc: x%u <- r%u", hwReg.indexInClass(), fr.index());
    frameRegs_[fr.index()].localGpX = hwReg;
  } else {
    comment("    ; alloc: d%u <- r%u", hwReg.indexInClass(), fr.index());
    frameRegs_[fr.index()].localVecD = hwReg;
  }
}

HWReg Emitter::isFRInRegister(FR fr) {
  auto &frState = frameRegs_[fr.index()];
  if (frState.localGpX)
    return useReg(frState.localGpX);
  if (frState.localVecD)
    return useReg(frState.localVecD);
  if (frState.globalReg)
    return frState.globalReg;
  return {};
}

HWReg Emitter::getOrAllocFRInVecD(FR fr, bool load) {
  auto &frState = frameRegs_[fr.index()];

  if (frState.localVecD)
    return useReg(frState.localVecD);

  // Do we have a global VecD allocated to this FR?
  if (frState.globalReg.isValidVecD()) {
    // If the caller requires that the latest value is present, but it isn't,
    // we need to put it there.
    if (load && !frState.globalRegUpToDate) {
      assert(
          frState.localGpX &&
          "If globalReg is not up to date, there must be a localReg");
      movHWReg<true>(frState.globalReg, frState.localGpX);
      frState.globalRegUpToDate = true;
    }

    return frState.globalReg;
  }

  // We have neither global nor local VecD, so we must allocate a new tmp reg.
  HWReg hwVecD = allocTempVecD();
  assignAllocatedLocalHWReg(fr, hwVecD);

  if (load) {
    if (frState.localGpX) {
      movHWReg<false>(hwVecD, frState.localGpX);
    } else if (frState.globalReg.isValidGpX()) {
      assert(
          frState.globalRegUpToDate &&
          "globalReg must be up to date if no local regs");
      movHWReg<false>(hwVecD, frState.globalReg);
    } else {
      _loadFrame(hwVecD, fr);
      frState.frameUpToDate = true;
    }
  }

  return hwVecD;
}

HWReg Emitter::getOrAllocFRInGpX(FR fr, bool load) {
  auto &frState = frameRegs_[fr.index()];

  if (frState.localGpX)
    return useReg(frState.localGpX);

  // Do we have a global GpX allocated to this FR?
  if (frState.globalReg.isValidGpX()) {
    // If the caller requires that the latest value is present, but it isn't,
    // we need to put it there.
    if (load && !frState.globalRegUpToDate) {
      assert(
          frState.localVecD &&
          "If globalReg is not up to date, there must be a localReg");
      movHWReg<true>(frState.globalReg, frState.localVecD);
      frState.globalRegUpToDate = true;
    }

    return frState.globalReg;
  }

  // We have neither global nor local GpX, so we must allocate a new tmp reg.
  HWReg hwGpX = allocTempGpX();
  assignAllocatedLocalHWReg(fr, hwGpX);

  if (load) {
    if (frState.localVecD) {
      movHWReg<false>(hwGpX, frState.localVecD);
    } else if (frState.globalReg.isValidVecD()) {
      assert(
          frState.globalRegUpToDate &&
          "globalReg must be up to date if no local regs");
      movHWReg<false>(hwGpX, frState.globalReg);
    } else {
      _loadFrame(hwGpX, fr);
      frState.frameUpToDate = true;
    }
  }

  return hwGpX;
}

HWReg Emitter::getOrAllocFRInAnyReg(
    FR fr,
    bool load,
    llvh::Optional<HWReg> preferred) {
  if (HWReg tmp = isFRInRegister(fr))
    return tmp;

  // We have neither global nor local reg, so we must allocate a new tmp reg.
  // We only allocate GpX though.
  if (preferred && !preferred->isGpX())
    preferred.reset();
  HWReg hwGpX = allocTempGpX();
  assignAllocatedLocalHWReg(fr, hwGpX);

  if (load) {
    _loadFrame(hwGpX, fr);
    frameRegs_[fr.index()].frameUpToDate = true;
  }

  return hwGpX;
}

void Emitter::frUpdatedWithHWReg(
    FR fr,
    HWReg hwReg,
    hermes::OptValue<FRType> localType) {
  FRState &frState = frameRegs_[fr.index()];

  frState.frameUpToDate = false;

  if (frState.globalReg == hwReg) {
    frState.globalRegUpToDate = true;

    if (frState.localGpX)
      freeReg(frState.localGpX);
    if (frState.localVecD)
      freeReg(frState.localVecD);
  } else {
    frState.globalRegUpToDate = false;
    if (hwReg == frState.localGpX) {
      freeReg(frState.localVecD);
    } else {
      assert(
          hwReg == frState.localVecD &&
          "Updated reg doesn't match any FRState register");
      freeReg(frState.localGpX);
    }
  }
  if (localType)
    frUpdateType(fr, *localType);
}

void Emitter::frUpdateType(FR fr, FRType type) {
  frameRegs_[fr.index()].localType = type;
}

void Emitter::ret(FR frValue) {
  if (HWReg hwReg = isFRInRegister(frValue))
    movHWReg<false>(HWReg::gpX(22), hwReg);
  else
    _loadFrame(HWReg::gpX(22), frValue);
  a.b(returnLabel_);
}

void Emitter::mov(FR frRes, FR frInput, bool logComment) {
  // Sometimes mov() is used by other instructions, so logging is optional.
  if (logComment)
    comment("// %s r%u, r%u", "mov", frRes.index(), frInput.index());
  if (frRes == frInput)
    return;

  HWReg hwInput = getOrAllocFRInAnyReg(frInput, true);
  HWReg hwDest = getOrAllocFRInAnyReg(frRes, false);
  movHWReg<false>(hwDest, hwInput);
  frUpdatedWithHWReg(frRes, hwDest, frameRegs_[frInput.index()].localType);
}

void Emitter::loadParam(FR frRes, uint32_t paramIndex) {
  comment("// LoadParam r%u, %u", frRes.index(), paramIndex);

  asmjit::Error err;
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwTmp = allocAndLogTempGpX();
  a64::GpW wTmp(hwTmp.indexInClass());

  a.ldur(
      wTmp,
      a64::Mem(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));

  {
    llvh::SaveAndRestore<asmjit::Error> sav(
        expectedError_, asmjit::kErrorInvalidImmediate);
    err = a.cmp(wTmp, paramIndex);
  }
  // Does paramIndex fit in the 12-bit unsigned immediate?
  if (err) {
    HWReg hwTmp2 = allocAndLogTempGpX();
    a64::GpW wTmp2(hwTmp2.indexInClass());
    loadBits64InGp(wTmp2, paramIndex, "paramIndex");
    a.cmp(wTmp, wTmp2);
    freeReg(hwTmp2);
  }
  a.b_lo(slowPathLab);

  freeReg(hwTmp);

  HWReg hwRes = getOrAllocFRInGpX(frRes, false);

  // FIXME: handle integer overflow better?
  int32_t ofs = ((int)StackFrameLayout::ThisArg - (int32_t)paramIndex) *
      (int)sizeof(SHLegacyValue);
  if (ofs >= 0)
    hermes_fatal("JIT integer overflow");
  {
    llvh::SaveAndRestore<asmjit::Error> sav(
        expectedError_, asmjit::kErrorInvalidDisplacement);
    err = a.ldur(hwRes.a64GpX(), a64::Mem(xFrame, ofs));
  }
  // Does the offset fit in the 9-bit signed offset?
  if (err) {
    ofs = -ofs;
    a64::GpX xRes = hwRes.a64GpX();
    if (ofs <= 4095) {
      a.sub(xRes, xFrame, ofs);
    } else {
      loadBits64InGp(xRes, ofs, nullptr);
      a.sub(xRes, xFrame, xRes);
    }
    a.ldr(xRes, a64::Mem(xRes));
  }

  a.bind(contLab);
  frUpdatedWithHWReg(frRes, hwRes);

  slowPaths_.push_back(
      {.slowPathLab = slowPathLab,
       .contLab = contLab,
       .name = "LoadParam",
       .frRes = frRes,
       .hwRes = hwRes,
       .emit = [](Emitter &em, SlowPath &sl) {
         em.comment("// Slow path: %s r%u", sl.name, sl.frRes.index());
         em.a.bind(sl.slowPathLab);
         em.loadBits64InGp(
             sl.hwRes.a64GpX(), _sh_ljs_undefined().raw, "undefined");
         em.a.b(sl.contLab);
       }});
}

void Emitter::loadConstDouble(FR frRes, double val, const char *name) {
  comment("// LoadConst%s r%u, %f", name, frRes.index(), val);
  HWReg hwRes{};

  // Check bitwise for zero because it may be -0.
  if (llvh::DoubleToBits(val) == llvh::DoubleToBits(0)) {
    // TODO: this check should be wider.
    hwRes = getOrAllocFRInVecD(frRes, false);
    a.movi(hwRes.a64VecD(), 0);
  } else if (a64::Utils::isFP64Imm8((double)val)) {
    hwRes = getOrAllocFRInVecD(frRes, false);
    a.fmov(hwRes.a64VecD(), (double)val);
  } else {
    uint64_t bits = llvh::DoubleToBits(val);
    if (isCheapConst(bits)) {
      hwRes = getOrAllocFRInGpX(frRes, false);
      a.mov(hwRes.a64GpX(), bits);
    } else {
      hwRes = getOrAllocFRInVecD(frRes, false);
      a.ldr(
          hwRes.a64VecD(),
          a64::Mem(roDataLabel_, uint64Const(bits, "fp64 const")));
    }
  }
  frUpdatedWithHWReg(frRes, hwRes, FRType::Number);
}

template <typename REG>
void Emitter::loadBits64InGp(
    const REG &dest,
    uint64_t bits,
    const char *constName) {
  if (isCheapConst(bits)) {
    a.mov(dest, bits);
  } else {
    a.ldr(dest, a64::Mem(roDataLabel_, uint64Const(bits, constName)));
  }
}

void Emitter::loadConstBits64(
    FR frRes,
    uint64_t bits,
    FRType type,
    const char *name) {
  comment(
      "// LoadConst%s r%u, %llu",
      name,
      frRes.index(),
      (unsigned long long)bits);
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);

  loadBits64InGp(hwRes.a64GpX(), bits, name);
  frUpdatedWithHWReg(frRes, hwRes, type);
}

void Emitter::loadConstString(
    FR frRes,
    RuntimeModule *runtimeModule,
    uint32_t stringID) {
  comment("// LoadConstString r%u, stringID %u", frRes.index(), stringID);

  syncAllTempExcept(frRes);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)runtimeModule, "RuntimeModule");
  a.mov(a64::w2, stringID);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHRuntimeModule *, uint32_t),
      _sh_ljs_get_bytecode_string);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<true>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::toNumber(FR frRes, FR frInput) {
  comment("// %s r%u, r%u", "toNumber", frRes.index(), frInput.index());
  if (isFRKnownNumber(frInput))
    return mov(frRes, frInput, false);

  HWReg hwRes, hwInput;
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();
  syncAllTempExcept(frRes != frInput ? frRes : FR());
  syncToMem(frInput);

  hwInput = getOrAllocFRInGpX(frInput, true);
  a.cmp(hwInput.a64GpX(), xDoubleLim);
  a.b_hs(slowPathLab);

  if (frRes != frInput) {
    hwRes = getOrAllocFRInVecD(frRes, false);
    movHWReg<false>(hwRes, hwInput);
  } else {
    hwRes = hwInput;
  }
  frUpdatedWithHWReg(frRes, hwRes, FRType::Number);

  freeAllTempExcept(frRes);
  a.bind(contLab);

  slowPaths_.push_back(
      {.slowPathLab = slowPathLab,
       .contLab = contLab,
       .name = "toNumber",
       .frRes = frRes,
       .frInput1 = frInput,
       .hwRes = hwRes,
       .slowCall = (void *)_sh_ljs_to_double_rjs,
       .slowCallName = "_sh_ljs_to_double_rjs",
       .emit = [](Emitter &em, SlowPath &sl) {
         em.comment(
             "// Slow path: %s r%u, r%u",
             sl.name,
             sl.frRes.index(),
             sl.frInput1.index());
         em.a.bind(sl.slowPathLab);
         em.a.mov(a64::x0, xRuntime);
         em.loadFrameAddr(a64::x1, sl.frInput1);
         em.call(sl.slowCall, sl.slowCallName);
         em.movHWReg<false>(sl.hwRes, HWReg::vecD(0));
         em.a.b(sl.contLab);
       }});
}

void Emitter::getGlobalObject(FR frRes) {
  comment("// GetGlobalObject r%u", frRes.index());
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(hwRes, a64::Mem(xRuntime, RuntimeOffsets::globalObject));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::declareGlobalVar(SHSymbolID symID) {
  comment("// DeclareGlobalVar %u", symID);

  syncAllTempExcept({});
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::w1, symID);
  EMIT_RUNTIME_CALL(
      *this, void (*)(SHRuntime *, SHSymbolID), _sh_ljs_declare_global_var);
}

void Emitter::createTopLevelEnvironment(FR frRes, uint32_t size) {
  comment("// CreateTopLevelEnvironment r%u, %u", frRes.index(), size);

  syncAllTempExcept(frRes);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, 0);
  a.mov(a64::w2, size);

  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, const SHLegacyValue *, uint32_t),
      _sh_ljs_create_environment);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::getParentEnvironment(FR frRes, uint32_t level) {
  comment("// GetParentEnvironment r%u, %u", frRes.index(), level);

  HWReg hwTmp1 = allocTempGpX();
  a64::GpX xTmp1 = hwTmp1.a64GpX();

  // Get current closure.
  a.ldur(
      xTmp1,
      a64::Mem(
          xFrame,
          (int)StackFrameLayout::CalleeClosureOrCB *
              (int)sizeof(SHLegacyValue)));
  // get pointer.
  emit_sh_ljs_get_pointer(a, xTmp1);
  // xTmp1 = closure->environment
  a.ldr(xTmp1, a64::Mem(xTmp1, offsetof(SHCallable, environment)));
  for (; level; --level) {
    // xTmp1 = env->parent.
    a.ldr(xTmp1, a64::Mem(xTmp1, offsetof(SHEnvironment, parentEnvironment)));
  }
  // encode object.
  emit_sh_ljs_object(a, xTmp1);

  freeReg(hwTmp1);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, hwTmp1);
  movHWReg<false>(hwRes, hwTmp1);
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::loadFromEnvironment(FR frRes, FR frEnv, uint32_t slot) {
  comment(
      "// LoadFromEnvironment r%u, r%u, %u",
      frRes.index(),
      frEnv.index(),
      slot);

  // TODO: register allocation could be smarter if frRes !=- frEnv.

  HWReg hwTmp1 = allocTempGpX();
  a64::GpX xTmp1 = hwTmp1.a64GpX();

  movHWFromFR(hwTmp1, frEnv);
  // get pointer.
  emit_sh_ljs_get_pointer(a, xTmp1);

  a.ldr(
      xTmp1,
      a64::Mem(
          xTmp1,
          offsetof(SHEnvironment, slots) + sizeof(SHLegacyValue) * slot));

  freeReg(hwTmp1);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, hwTmp1);
  movHWReg<false>(hwRes, hwTmp1);
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::storeToEnvironment(bool np, FR frEnv, uint32_t slot, FR frValue) {
  // TODO: this should really be inlined!
  comment(
      "// StoreNPToEnvironment r%u, %u, r%u",
      frEnv.index(),
      slot,
      frValue.index());

  // Here we apply a technique that may be subtle. We have various FRs that we
  // want to load into parameter registers (x0, x1, etc) by value. Some of these
  // FRs may live in the parameter registers we want to use, but some may not.
  // So, first we make sure that the FRs that live in x0, x1, etc., are synced
  // to their primary location and the temps x0, x1, etc., are freed.
  //
  // Then we make sure that all FRs are synced to memory or callee-saved reg,
  // because we will be making a call. But we are *not* freeing the temp regs
  // yet, because we want to be able to use them to populate the values of the
  // parameters before the call.
  //
  // Only in the end do we free all temps, to reflect the state of the world
  // after the call.

  // Make sure x0, x1, x2, x3 are unused.
  syncAndFreeTempReg(HWReg::gpX(0));
  syncAndFreeTempReg(HWReg::gpX(1));
  syncAndFreeTempReg(HWReg::gpX(2));
  syncAndFreeTempReg(HWReg::gpX(3));

  // Make sure all FRs can be accessed. Some of them might be in temp regs.
  syncAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  movHWFromFR(HWReg::gpX(1), frEnv);
  movHWFromFR(HWReg::gpX(2), frValue);
  a.mov(a64::w3, slot);
  if (np) {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(SHRuntime *, SHLegacyValue, SHLegacyValue, uint32_t),
        _sh_ljs_store_np_to_env);
  } else {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(SHRuntime *, SHLegacyValue, SHLegacyValue, uint32_t),
        _sh_ljs_store_to_env);
  }

  // No temp registers available anymore.
  freeAllTempExcept({});
}

void Emitter::createClosure(
    FR frRes,
    FR frEnv,
    RuntimeModule *runtimeModule,
    uint32_t functionID) {
  comment(
      "// CreateClosure r%u, r%u, %u",
      frRes.index(),
      frEnv.index(),
      functionID);
  syncAllTempExcept(frRes != frEnv ? frRes : FR());
  syncToMem(frEnv);

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frEnv);
  loadBits64InGp(a64::x2, (uint64_t)runtimeModule, "RuntimeModule");
  loadBits64InGp(a64::w3, functionID, nullptr);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(
          SHRuntime *, const SHLegacyValue *, SHRuntimeModule *, uint32_t),
      _sh_ljs_create_bytecode_closure);

  freeAllTempExcept({});
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::putByValImpl(
    FR frTarget,
    FR frKey,
    FR frValue,
    const char *name,
    void (*shImpl)(
        SHRuntime *shr,
        SHLegacyValue *target,
        SHLegacyValue *key,
        SHLegacyValue *value),
    const char *shImplName) {
  comment(
      "// %s r%u, r%u, r%u",
      name,
      frTarget.index(),
      frKey.index(),
      frValue.index());

  syncAllTempExcept({});
  syncToMem(frTarget);
  syncToMem(frKey);
  syncToMem(frValue);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);
  call((void *)shImpl, shImplName);
}

void Emitter::getByIdImpl(
    FR frRes,
    SHSymbolID symID,
    FR frSource,
    uint8_t cacheIdx,
    const char *name,
    SHLegacyValue (*shImpl)(
        SHRuntime *shr,
        const SHLegacyValue *source,
        SHSymbolID symID,
        SHPropertyCacheEntry *propCacheEntry),
    const char *shImplName) {
  comment(
      "// %s r%u, r%u, cache %u, symID %u",
      name,
      frRes.index(),
      frSource.index(),
      cacheIdx,
      symID);

  syncAllTempExcept(frRes != frSource ? frRes : FR{});
  syncToMem(frSource);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  a.mov(a64::w2, symID);
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x3, 0);
  } else {
    a.ldr(a64::x3, a64::Mem(roDataLabel_, roOfsReadPropertyCachePtr_));
    if (cacheIdx != 0)
      a.add(a64::x3, a64::x3, sizeof(SHPropertyCacheEntry) * cacheIdx);
  }
  call((void *)shImpl, shImplName);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
  freeAllTempExcept(frRes);
}

void Emitter::getByVal(FR frRes, FR frSource, FR frKey) {
  comment(
      "// getByVal r%u, r%u, r%u",
      frRes.index(),
      frSource.index(),
      frKey.index());

  syncAllTempExcept(frRes != frSource && frRes != frKey ? frRes : FR());
  syncToMem(frSource);
  syncToMem(frKey);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  loadFrameAddr(a64::x2, frKey);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_get_by_val_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::getByIndex(FR frRes, FR frSource, uint8_t key) {
  comment("// getByIdx r%u, r%u, %u", frRes.index(), frSource.index(), key);

  syncAllTempExcept(frRes != frSource ? frRes : FR());
  syncToMem(frSource);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  a.mov(a64::w2, key);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, uint8_t),
      _sh_ljs_get_by_index_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::putByIdImpl(
    FR frTarget,
    SHSymbolID symID,
    FR frValue,
    uint8_t cacheIdx,
    const char *name,
    void (*shImpl)(
        SHRuntime *shr,
        SHLegacyValue *target,
        SHSymbolID symID,
        SHLegacyValue *value,
        SHPropertyCacheEntry *propCacheEntry),
    const char *shImplName) {
  comment(
      "// %s r%u, r%u, cache %u, symID %u",
      name,
      frTarget.index(),
      frValue.index(),
      cacheIdx,
      symID);

  syncAllTempExcept({});
  syncToMem(frTarget);
  syncToMem(frValue);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  a.mov(a64::w2, symID);
  loadFrameAddr(a64::x3, frValue);
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x4, 0);
  } else {
    a.ldr(a64::x4, a64::Mem(roDataLabel_, roOfsWritePropertyCachePtr_));
    if (cacheIdx != 0)
      a.add(a64::x4, a64::x4, sizeof(SHPropertyCacheEntry) * cacheIdx);
  }
  call((void *)shImpl, shImplName);
}

asmjit::Label Emitter::newPrefLabel(const char *pref, size_t index) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%s%lu", pref, index);
  return a.newNamedLabel(buf);
}

void Emitter::isIn(FR frRes, FR frLeft, FR frRight) {
  comment(
      "// isIn r%u, r%u, r%u", frRes.index(), frLeft.index(), frRight.index());

  syncAllTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
  syncToMem(frLeft);
  syncToMem(frRight);
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frLeft);
  loadFrameAddr(a64::x2, frRight);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_is_in_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

int32_t Emitter::reserveData(
    int32_t dsize,
    size_t align,
    asmjit::TypeId typeId,
    int32_t itemCount,
    const char *comment) {
  // Align the new data.
  size_t oldSize = roData_.size();
  size_t dataOfs = (roData_.size() + align - 1) & ~(align - 1);
  if (dataOfs >= INT32_MAX)
    hermes::hermes_fatal("JIT RO data overflow");
  // Grow to include the data.
  roData_.resize(dataOfs + dsize);

  // If logging is enabled, generate data descriptors.
  if (logger_) {
    // Optional padding descriptor.
    if (dataOfs != oldSize) {
      int32_t gap = (int32_t)(dataOfs - oldSize);
      roDataDesc_.push_back(
          {.size = gap, .typeId = asmjit::TypeId::kUInt8, .itemCount = gap});
    }

    roDataDesc_.push_back(
        {.size = dsize,
         .typeId = typeId,
         .itemCount = itemCount,
         .comment = comment});
  }

  return (int32_t)dataOfs;
}

/// Register a 64-bit constant in RO DATA and return its offset.
int32_t Emitter::uint64Const(uint64_t bits, const char *comment) {
  auto [it, inserted] = fp64ConstMap_.try_emplace(bits, 0);
  if (inserted) {
    int32_t dataOfs = reserveData(
        sizeof(double), sizeof(double), asmjit::TypeId::kFloat64, 1, comment);
    memcpy(roData_.data() + dataOfs, &bits, sizeof(double));
    it->second = dataOfs;
  }
  return it->second;
}

asmjit::Label Emitter::registerCall(void *fn, const char *name) {
  auto [it, inserted] = thunkMap_.try_emplace(fn, 0);
  // Is this a new thunk?
  if (inserted) {
    it->second = thunks_.size();
    int32_t dataOfs =
        reserveData(sizeof(fn), sizeof(fn), asmjit::TypeId::kUInt64, 1, name);
    memcpy(roData_.data() + dataOfs, &fn, sizeof(fn));
    thunks_.emplace_back(name ? a.newNamedLabel(name) : a.newLabel(), dataOfs);
  }

  return thunks_[it->second].first;
}

void Emitter::emitSlowPaths() {
  while (!slowPaths_.empty()) {
    SlowPath &sp = slowPaths_.front();
    sp.emit(*this, sp);
    slowPaths_.pop_front();
  }
}

void Emitter::emitThunks() {
  comment("// Thunks");
  for (const auto &th : thunks_) {
    a.bind(th.first);
    a.ldr(a64::x16, a64::Mem(roDataLabel_, th.second));
    a.br(a64::x16);
  }
}

void Emitter::emitROData() {
  a.bind(roDataLabel_);
  if (!logger_) {
    a.embed(roData_.data(), roData_.size());
  } else {
    int32_t ofs = 0;
    for (const auto &desc : roDataDesc_) {
      if (desc.comment)
        comment("// %s", desc.comment);
      a.embedDataArray(desc.typeId, roData_.data() + ofs, desc.itemCount);
      ofs += desc.size;
    }
  }
}

void Emitter::call(FR frRes, FR frCallee, uint32_t argc) {
  comment("// Call r%u, r%u, %u", frRes.index(), frCallee.index(), argc);
  uint32_t nRegs = frameRegs_.size();
  syncAllTempExcept(FR());

  FR calleeFrameArg{nRegs + hbc::StackFrameLayout::CalleeClosureOrCB};

  // Store the callee to the right location in the frame, if it isn't already
  // there.
  if (frCallee != calleeFrameArg) {
    // Free any temp register before we mov into it so movFRFromHW stores
    // directly to the frame.
    freeFRTemp(calleeFrameArg);
    auto calleeReg = getOrAllocFRInAnyReg(frCallee, true);
    movFRFromHW(
        calleeFrameArg, calleeReg, frameRegs_[frCallee.index()].localType);
  }

  // Store undefined as the new target.
  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  loadConstBits64(
      ntFrameArg, _sh_ljs_undefined().raw, FRType::Unknown, "undefined");

  // Ensure that all the outgoing values are stored into the frame registers for
  // the call.
  syncToMem(calleeFrameArg);
  syncToMem(ntFrameArg);

  for (uint32_t i = 0; i < argc; ++i)
    syncToMem(FR{nRegs + hbc::StackFrameLayout::ThisArg - i});

  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, xFrame);
  a.mov(a64::w2, argc - 1);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, uint32_t),
      _sh_ljs_call);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::callN(FR frRes, FR frCallee, llvh::ArrayRef<FR> args) {
  comment(
      "// Call%zu r%u, r%u, ...args",
      args.size(),
      frRes.index(),
      frCallee.index());
  uint32_t nRegs = frameRegs_.size();

  FR calleeFrameArg{nRegs + hbc::StackFrameLayout::CalleeClosureOrCB};
  // Store the callee to the right location in the frame.
  if (frCallee != calleeFrameArg) {
    // Free any temp register before we mov into it so movFRFromHW stores
    // directly to the frame.
    freeFRTemp(calleeFrameArg);
    auto calleeReg = getOrAllocFRInAnyReg(frCallee, true);
    movFRFromHW(
        calleeFrameArg, calleeReg, frameRegs_[frCallee.index()].localType);
  }
  syncToMem(calleeFrameArg);

  for (uint32_t i = 0; i < args.size(); ++i) {
    auto argLoc = FR{nRegs + hbc::StackFrameLayout::ThisArg - i};

    if (args[i] != argLoc) {
      // Free any temp register before we mov into it so movFRFromHW stores
      // directly to the frame.
      freeFRTemp(argLoc);
      auto argReg = getOrAllocFRInAnyReg(args[i], true);
      movFRFromHW(argLoc, argReg, frameRegs_[args[i].index()].localType);
    }
    syncToMem(argLoc);
  }

  // Get a register for the new target.
  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  loadConstBits64(
      ntFrameArg, _sh_ljs_undefined().raw, FRType::Unknown, "undefined");
  syncToMem(ntFrameArg);

  // For now we sync all registers, since we skip writing to the frame in some
  // cases above, but in principle, we could track frRes specially.
  syncAllTempExcept(FR());
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, xFrame);
  a.mov(a64::w2, args.size() - 1);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, uint32_t),
      _sh_ljs_call);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::callBuiltin(FR frRes, uint32_t builtinIndex, uint32_t argc) {
  comment(
      "// CallBuiltin r%u, %s, %u",
      frRes.index(),
      getBuiltinMethodName(builtinIndex),
      argc);
  uint32_t nRegs = frameRegs_.size();

  // CallBuiltin internally sets "this", so we don't sync it to memory.
  for (uint32_t i = 1; i < argc; ++i)
    syncToMem(FR{nRegs + hbc::StackFrameLayout::ThisArg - i});

  syncAllTempExcept({});
  freeAllTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, xFrame);
  // The bytecode arg count includes "this", but the SH one does not, so
  // subtract 1.
  a.mov(a64::w2, argc - 1);
  a.mov(a64::w3, builtinIndex);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, uint32_t, uint32_t),
      _sh_ljs_call_builtin);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::callWithNewTarget(
    FR frRes,
    FR frCallee,
    FR frNewTarget,
    uint32_t argc) {
  comment(
      "// CallWithNewTarget r%u, r%u, r%u, %u",
      frRes.index(),
      frCallee.index(),
      frNewTarget.index(),
      argc);
  uint32_t nRegs = frameRegs_.size();

  auto calleeFrameArg = FR{nRegs + hbc::StackFrameLayout::CalleeClosureOrCB};
  // Store the callee to the right location in the frame.
  if (calleeFrameArg != frCallee) {
    // Free any temp register before we mov into it so movFRFromHW stores
    // directly to the frame.
    freeFRTemp(calleeFrameArg);
    auto calleeReg = getOrAllocFRInAnyReg(frCallee, true);
    movFRFromHW(
        calleeFrameArg, calleeReg, frameRegs_[frCallee.index()].localType);
  }

  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  // Store the new target to the right location in the frame.
  if (ntFrameArg != frNewTarget) {
    // Free the register before we mov into it so we store directly to the
    // frame.
    freeFRTemp(ntFrameArg);
    auto newTargetReg = getOrAllocFRInAnyReg(frNewTarget, true);
    movFRFromHW(
        ntFrameArg, newTargetReg, frameRegs_[frNewTarget.index()].localType);
  }

  // Sync the set up call stack to the frame memory.
  for (uint32_t i = 0; i < argc; ++i)
    syncToMem(FR{nRegs + hbc::StackFrameLayout::ThisArg - i});

  syncToMem(calleeFrameArg);
  syncToMem(ntFrameArg);

  syncAllTempExcept({});
  freeAllTempExcept(frRes);

  a.mov(a64::x0, xRuntime);
  a.mov(a64::x1, xFrame);
  a.mov(a64::w2, argc - 1);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, SHLegacyValue *, uint32_t),
      _sh_ljs_call);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::getBuiltinClosure(FR frRes, uint32_t builtinIndex) {
  comment(
      "// GetBuiltinClosure r%u, %s",
      frRes.index(),
      getBuiltinMethodName(builtinIndex));
  syncAllTempExcept(frRes);
  freeAllTempExcept(frRes);

  a.mov(a64::x0, xRuntime);
  a.mov(a64::w1, builtinIndex);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue(*)(SHRuntime *, uint32_t),
      _sh_ljs_get_builtin_closure);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWReg<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHWReg(frRes, hwRes);
}

void Emitter::arithUnop(
    bool forceNumber,
    FR frRes,
    FR frInput,
    const char *name,
    void (*fast)(
        a64::Assembler &a,
        const a64::VecD &dst,
        const a64::VecD &src,
        const a64::VecD &tmp),
    void *slowCall,
    const char *slowCallName) {
  comment("// %s r%u, r%u", name, frRes.index(), frInput.index());

  HWReg hwRes, hwInput;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool inputIsNum;

  if (forceNumber) {
    frameRegs_[frInput.index()].localType = FRType::Number;
    inputIsNum = true;
  } else {
    inputIsNum = isFRKnownNumber(frInput);
  }

  if (!inputIsNum) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncAllTempExcept(frRes != frInput ? frRes : FR());
    syncToMem(frInput);
  }

  if (inputIsNum) {
    hwInput = getOrAllocFRInVecD(frInput, true);
  } else {
    hwInput = getOrAllocFRInGpX(frInput, true);
    a.cmp(hwInput.a64GpX(), xDoubleLim);
    a.b_hs(slowPathLab);
    hwInput = getOrAllocFRInVecD(frInput, true);
  }

  hwRes = getOrAllocFRInVecD(frRes, false);
  HWReg hwTmp = hwRes != hwInput ? hwRes : allocTempVecD();
  fast(a, hwRes.a64VecD(), hwInput.a64VecD(), hwTmp.a64VecD());
  if (hwRes == hwInput)
    freeReg(hwTmp);

  frUpdatedWithHWReg(
      frRes, hwRes, inputIsNum ? OptValue(FRType::Number) : OptValue<FRType>());

  if (inputIsNum)
    return;

  freeAllTempExcept(frRes);
  a.bind(contLab);

  slowPaths_.push_back(
      {.slowPathLab = slowPathLab,
       .contLab = contLab,
       .name = name,
       .frRes = frRes,
       .frInput1 = frInput,
       .hwRes = hwRes,
       .slowCall = slowCall,
       .slowCallName = slowCallName,
       .emit = [](Emitter &em, SlowPath &sl) {
         em.comment(
             "// Slow path: %s r%u, r%u",
             sl.name,
             sl.frRes.index(),
             sl.frInput1.index());
         em.a.bind(sl.slowPathLab);
         em.a.mov(a64::x0, xRuntime);
         em.loadFrameAddr(a64::x1, sl.frInput1);
         em.call(sl.slowCall, sl.slowCallName);
         em.movHWReg<false>(sl.hwRes, HWReg::gpX(0));
         em.a.b(sl.contLab);
       }});
}

void Emitter::arithBinOp(
    bool forceNumber,
    FR frRes,
    FR frLeft,
    FR frRight,
    const char *name,
    void (*fast)(
        a64::Assembler &a,
        const a64::VecD &res,
        const a64::VecD &dl,
        const a64::VecD &dr),
    void *slowCall,
    const char *slowCallName) {
  comment(
      "// %s r%u, r%u, r%u",
      name,
      frRes.index(),
      frLeft.index(),
      frRight.index());
  HWReg hwRes, hwLeft, hwRight;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool leftIsNum, rightIsNum, slow;

  if (forceNumber) {
    frameRegs_[frLeft.index()].localType = FRType::Number;
    frameRegs_[frRight.index()].localType = FRType::Number;
    leftIsNum = rightIsNum = true;
    slow = false;
  } else {
    leftIsNum = isFRKnownNumber(frLeft);
    rightIsNum = isFRKnownNumber(frRight);
    slow = !(rightIsNum && leftIsNum);
  }

  if (slow) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncAllTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
    syncToMem(frLeft);
    syncToMem(frRight);
  }

  if (leftIsNum) {
    hwLeft = getOrAllocFRInVecD(frLeft, true);
  } else {
    hwLeft = getOrAllocFRInGpX(frLeft, true);
    a.cmp(hwLeft.a64GpX(), xDoubleLim);
    a.b_hs(slowPathLab);
  }
  if (rightIsNum) {
    hwRight = getOrAllocFRInVecD(frRight, true);
  } else {
    hwRight = getOrAllocFRInGpX(frRight, true);
    a.cmp(hwRight.a64GpX(), xDoubleLim);
    a.b_hs(slowPathLab);
  }

  if (!leftIsNum)
    hwLeft = getOrAllocFRInVecD(frLeft, true);
  if (!rightIsNum)
    hwRight = getOrAllocFRInVecD(frRight, true);

  hwRes = getOrAllocFRInVecD(frRes, false);
  fast(a, hwRes.a64VecD(), hwLeft.a64VecD(), hwRight.a64VecD());

  frUpdatedWithHWReg(
      frRes, hwRes, !slow ? OptValue(FRType::Number) : OptValue<FRType>());

  if (!slow)
    return;

  freeAllTempExcept(frRes);
  a.bind(contLab);

  slowPaths_.push_back(
      {.slowPathLab = slowPathLab,
       .contLab = contLab,
       .name = name,
       .frRes = frRes,
       .frInput1 = frLeft,
       .frInput2 = frRight,
       .hwRes = hwRes,
       .slowCall = slowCall,
       .slowCallName = slowCallName,
       .emit = [](Emitter &em, SlowPath &sl) {
         em.comment(
             "// %s r%u, r%u, r%u",
             sl.name,
             sl.frRes.index(),
             sl.frInput1.index(),
             sl.frInput2.index());
         em.a.bind(sl.slowPathLab);
         em.a.mov(a64::x0, xRuntime);
         em.loadFrameAddr(a64::x1, sl.frInput1);
         em.loadFrameAddr(a64::x2, sl.frInput2);
         em.call(sl.slowCall, sl.slowCallName);
         em.movHWReg<false>(sl.hwRes, HWReg::gpX(0));
         em.a.b(sl.contLab);
       }});
}

void Emitter::jmpTrueFalse(
    bool onTrue,
    const asmjit::Label &target,
    FR frInput) {
  comment("// Jmp%s r%u", onTrue ? "True" : "False", frInput.index());

  // Do this always, since this could be the end of the BB.
  syncAllTempExcept(FR());

  if (isFRKnownType(frInput, FRType::Number)) {
    HWReg hwInput = getOrAllocFRInVecD(frInput, true);
    a.fcmp(hwInput.a64VecD(), 0.0);
    if (onTrue) {
      // Branch on < 0 and > 0. All that remains is 0 and NaN.
      a.b_mi(target);
      a.b_gt(target);
    } else {
      asmjit::Label label = a.newLabel();
      a.b_mi(label);
      a.b_gt(label);
      a.b(target);
      a.bind(label);
    }
  } else if (isFRKnownType(frInput, FRType::Bool)) {
    HWReg hwInput = getOrAllocFRInGpX(frInput, true);
    a64::GpX xInput = hwInput.a64GpX();

    static_assert(
        HERMESVALUE_VERSION == 1,
        "bool is encoded as 32-bit value in the low bits");
    if (onTrue)
      a.cbnz(xInput.w(), target);
    else
      a.cbz(xInput.w(), target);
  } else {
    // TODO: we should inline all of it.
    syncAllTempExcept({});
    movHWFromFR(HWReg::gpX(0), frInput);
    EMIT_RUNTIME_CALL(*this, bool (*)(SHLegacyValue), _sh_ljs_to_boolean);
    if (onTrue)
      a.cbnz(a64::w0, target);
    else
      a.cbz(a64::w0, target);
    freeAllTempExcept(FR());
  }
}

void Emitter::jCond(
    bool forceNumber,
    bool invert,
    const asmjit::Label &target,
    FR frLeft,
    FR frRight,
    const char *name,
    void(fast)(a64::Assembler &a, const asmjit::Label &target),
    void *slowCall,
    const char *slowCallName) {
  comment(
      "// j_%s_%s Lx, r%u, r%u",
      invert ? "not" : "",
      name,
      frLeft.index(),
      frRight.index());
  HWReg hwLeft, hwRight;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool leftIsNum, rightIsNum, slow;

  if (forceNumber) {
    frameRegs_[frLeft.index()].localType = FRType::Number;
    frameRegs_[frRight.index()].localType = FRType::Number;
    leftIsNum = rightIsNum = true;
    slow = false;
  } else {
    leftIsNum = isFRKnownNumber(frLeft);
    rightIsNum = isFRKnownNumber(frRight);
    slow = !(rightIsNum && leftIsNum);
  }

  if (slow) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
  }
  // Do this always, since this could be the end of the BB.
  syncAllTempExcept(FR());

  if (leftIsNum) {
    hwLeft = getOrAllocFRInVecD(frLeft, true);
  } else {
    hwLeft = getOrAllocFRInGpX(frLeft, true);
    a.cmp(hwLeft.a64GpX(), xDoubleLim);
    a.b_hs(slowPathLab);
  }
  if (rightIsNum) {
    hwRight = getOrAllocFRInVecD(frRight, true);
  } else {
    hwRight = getOrAllocFRInGpX(frRight, true);
    a.cmp(hwRight.a64GpX(), xDoubleLim);
    a.b_hs(slowPathLab);
  }

  if (!leftIsNum)
    hwLeft = getOrAllocFRInVecD(frLeft, true);
  if (!rightIsNum)
    hwRight = getOrAllocFRInVecD(frRight, true);

  a.fcmp(hwLeft.a64VecD(), hwRight.a64VecD());
  if (!invert) {
    fast(a, target);
  } else {
    if (!contLab.isValid())
      contLab = a.newLabel();
    fast(a, contLab);
    a.b(target);
  }
  if (contLab.isValid())
    a.bind(contLab);

  if (!slow)
    return;

  // Do this always, since this is the end of the BB.
  freeAllTempExcept(FR());

  slowPaths_.push_back(
      {.slowPathLab = slowPathLab,
       .contLab = contLab,
       .target = target,
       .name = name,
       .frInput1 = frLeft,
       .frInput2 = frRight,
       .invert = invert,
       .slowCall = slowCall,
       .slowCallName = slowCallName,
       .emit = [](Emitter &em, SlowPath &sl) {
         em.comment(
             "// Slow path: j_%s%s Lx, r%u, r%u",
             sl.invert ? "not_" : "",
             sl.name,
             sl.frInput1.index(),
             sl.frInput2.index());
         em.a.bind(sl.slowPathLab);
         em.a.mov(a64::x0, xRuntime);
         em.loadFrameAddr(a64::x1, sl.frInput1);
         em.loadFrameAddr(a64::x2, sl.frInput2);
         em.call(sl.slowCall, sl.slowCallName);
         if (!sl.invert)
           em.a.cbnz(a64::w0, sl.target);
         else
           em.a.cbz(a64::w0, sl.target);
         em.a.b(sl.contLab);
       }});
}

} // namespace hermes::vm::arm64
#endif // HERMESVM_JIT