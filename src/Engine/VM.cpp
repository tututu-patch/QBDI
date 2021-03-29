/*
 * This file is part of QBDI.
 *
 * Copyright 2017 - 2021 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "QBDI/VM.h"
#include "QBDI/Errors.h"
#include "QBDI/Memory.hpp"
#include "QBDI/Range.h"

#include "Engine/Engine.h"
#include "ExecBlock/ExecBlock.h"
#include "Patch/InstInfo.h"
#include "Patch/InstrRule.h"
#include "Patch/InstrRules.h"
#include "Patch/MemoryAccess.h"
#include "Patch/PatchCondition.h"
#include "Patch/PatchGenerator.h"
#include "Utility/LogSys.h"

// Mask to identify Virtual Callback events
#define EVENTID_VIRTCB_MASK (1UL << 31)

namespace QBDI {

struct MemCBInfo {
  MemoryAccessType type;
  Range<rword> range;
  InstCallback cbk;
  void *data;
};

struct InstrCBInfo {
  Range<rword> range;
  InstrRuleCallbackC cbk;
  AnalysisType type;
  void *data;
};

static VMAction memReadGate(VMInstanceRef vm, GPRState *gprState,
                            FPRState *fprState, void *data) {
  std::vector<std::pair<uint32_t, MemCBInfo>> *memCBInfos =
      static_cast<std::vector<std::pair<uint32_t, MemCBInfo>> *>(data);
  std::vector<MemoryAccess> memAccesses = vm->getInstMemoryAccess();
  RangeSet<rword> readRange;
  for (const MemoryAccess &memAccess : memAccesses) {
    if (memAccess.type & MEMORY_READ) {
      Range<rword> accessRange(memAccess.accessAddress,
                               memAccess.accessAddress + memAccess.size);
      readRange.add(accessRange);
    }
  }

  VMAction action = VMAction::CONTINUE;
  for (size_t i = 0; i < memCBInfos->size(); i++) {
    // Check access type and range
    if ((*memCBInfos)[i].second.type == MEMORY_READ &&
        readRange.overlaps((*memCBInfos)[i].second.range)) {
      // Forward to virtual callback
      VMAction ret = (*memCBInfos)[i].second.cbk(vm, gprState, fprState,
                                                 (*memCBInfos)[i].second.data);
      // Always keep the most extreme action as the return
      if (ret > action) {
        action = ret;
      }
    }
  }
  return action;
}

static VMAction memWriteGate(VMInstanceRef vm, GPRState *gprState,
                             FPRState *fprState, void *data) {
  std::vector<std::pair<uint32_t, MemCBInfo>> *memCBInfos =
      static_cast<std::vector<std::pair<uint32_t, MemCBInfo>> *>(data);
  std::vector<MemoryAccess> memAccesses = vm->getInstMemoryAccess();
  RangeSet<rword> readRange;
  RangeSet<rword> writeRange;
  for (const MemoryAccess &memAccess : memAccesses) {
    Range<rword> accessRange(memAccess.accessAddress,
                             memAccess.accessAddress + memAccess.size);
    if (memAccess.type & MEMORY_READ) {
      readRange.add(accessRange);
    }
    if (memAccess.type & MEMORY_WRITE) {
      writeRange.add(accessRange);
    }
  }

  VMAction action = VMAction::CONTINUE;
  for (size_t i = 0; i < memCBInfos->size(); i++) {
    // Check accessCB
    // 1. has MEMORY_WRITE and write range overlaps
    // 2. is MEMORY_READ_WRITE and read range overlaps
    // note: the case with MEMORY_READ only is managed by memReadGate
    if ((((*memCBInfos)[i].second.type & MEMORY_WRITE) &&
         writeRange.overlaps((*memCBInfos)[i].second.range)) ||
        ((*memCBInfos)[i].second.type == MEMORY_READ_WRITE &&
         readRange.overlaps((*memCBInfos)[i].second.range))) {
      // Forward to virtual callback
      VMAction ret = (*memCBInfos)[i].second.cbk(vm, gprState, fprState,
                                                 (*memCBInfos)[i].second.data);
      // Always keep the most extreme action as the return
      if (ret > action) {
        action = ret;
      }
    }
  }
  return action;
}

static std::vector<InstrRuleDataCBK>
InstrCBGateC(VMInstanceRef vm, const InstAnalysis *inst, void *_data) {
  InstrCBInfo *data = static_cast<InstrCBInfo *>(_data);
  std::vector<InstrRuleDataCBK> vec{};
  data->cbk(vm, inst, &vec, data->data);
  return vec;
}

static VMAction stopCallback(VMInstanceRef vm, GPRState *gprState,
                             FPRState *fprState, void *data) {
  return VMAction::STOP;
}

VM::VM(const std::string &cpu, const std::vector<std::string> &mattrs,
       Options opts)
    : memoryLoggingLevel(0), memCBID(0),
      memReadGateCBID(VMError::INVALID_EVENTID),
      memWriteGateCBID(VMError::INVALID_EVENTID) {
#if defined(_QBDI_ASAN_ENABLED_)
  opts |= Options::OPT_DISABLE_FPR;
#endif
  engine = std::make_unique<Engine>(cpu, mattrs, opts, this);
  memCBInfos = std::make_unique<std::vector<std::pair<uint32_t, MemCBInfo>>>();
  instrCBInfos = std::make_unique<
      std::vector<std::pair<uint32_t, std::unique_ptr<InstrCBInfo>>>>();
}

VM::~VM() = default;

VM::VM(VM &&vm)
    : engine(std::move(vm.engine)), memoryLoggingLevel(vm.memoryLoggingLevel),
      memCBInfos(std::move(vm.memCBInfos)), memCBID(vm.memCBID),
      memReadGateCBID(vm.memReadGateCBID),
      memWriteGateCBID(vm.memWriteGateCBID),
      instrCBInfos(std::move(vm.instrCBInfos)) {

  engine->changeVMInstanceRef(this);
}

VM &VM::operator=(VM &&vm) {
  engine = std::move(vm.engine);
  memoryLoggingLevel = vm.memoryLoggingLevel;
  memCBInfos = std::move(vm.memCBInfos);
  memCBID = vm.memCBID;
  memReadGateCBID = vm.memReadGateCBID;
  memWriteGateCBID = vm.memWriteGateCBID;
  instrCBInfos = std::move(vm.instrCBInfos);

  engine->changeVMInstanceRef(this);

  return *this;
}

VM::VM(const VM &vm)
    : engine(std::make_unique<Engine>(*vm.engine)),
      memoryLoggingLevel(vm.memoryLoggingLevel),
      memCBInfos(std::make_unique<std::vector<std::pair<uint32_t, MemCBInfo>>>(
          *vm.memCBInfos)),
      memCBID(vm.memCBID), memReadGateCBID(vm.memReadGateCBID),
      memWriteGateCBID(vm.memWriteGateCBID) {

  engine->changeVMInstanceRef(this);
  instrCBInfos = std::make_unique<
      std::vector<std::pair<uint32_t, std::unique_ptr<InstrCBInfo>>>>();
  for (const auto &p : *vm.instrCBInfos) {
    engine->deleteInstrumentation(p.first);
    addInstrRuleRange(p.second->range.start(), p.second->range.end(),
                      p.second->cbk, p.second->type, p.second->data);
  }
}

VM &VM::operator=(const VM &vm) {
  *engine = *vm.engine;
  *memCBInfos = *vm.memCBInfos;

  memoryLoggingLevel = vm.memoryLoggingLevel;
  memCBID = vm.memCBID;
  memReadGateCBID = vm.memReadGateCBID;
  memWriteGateCBID = vm.memWriteGateCBID;

  instrCBInfos = std::make_unique<
      std::vector<std::pair<uint32_t, std::unique_ptr<InstrCBInfo>>>>();
  for (const auto &p : *vm.instrCBInfos) {
    engine->deleteInstrumentation(p.first);
    addInstrRuleRange(p.second->range.start(), p.second->range.end(),
                      p.second->cbk, p.second->type, p.second->data);
  }

  engine->changeVMInstanceRef(this);

  return *this;
}

GPRState *VM::getGPRState() const { return engine->getGPRState(); }

FPRState *VM::getFPRState() const { return engine->getFPRState(); }

void VM::setGPRState(const GPRState *gprState) {
  QBDI_REQUIRE_ACTION(gprState != nullptr, return );
  engine->setGPRState(gprState);
}

void VM::setFPRState(const FPRState *fprState) {
  QBDI_REQUIRE_ACTION(fprState != nullptr, return );
  engine->setFPRState(fprState);
}

Options VM::getOptions() const { return engine->getOptions(); }

void VM::setOptions(Options options) {
#if defined(_QBDI_ASAN_ENABLED_)
  options |= Options::OPT_DISABLE_FPR;
#endif
  engine->setOptions(options);
}

void VM::addInstrumentedRange(rword start, rword end) {
  QBDI_REQUIRE_ACTION(start < end, return );
  engine->addInstrumentedRange(start, end);
}

bool VM::addInstrumentedModule(const std::string &name) {
  return engine->addInstrumentedModule(name);
}

bool VM::addInstrumentedModuleFromAddr(rword addr) {
  return engine->addInstrumentedModuleFromAddr(addr);
}

bool VM::instrumentAllExecutableMaps() {
  return engine->instrumentAllExecutableMaps();
}

void VM::removeInstrumentedRange(rword start, rword end) {
  QBDI_REQUIRE_ACTION(start < end, return );
  engine->removeInstrumentedRange(start, end);
}

void VM::removeAllInstrumentedRanges() {
  engine->removeAllInstrumentedRanges();
}

bool VM::removeInstrumentedModule(const std::string &name) {
  return engine->removeInstrumentedModule(name);
}

bool VM::removeInstrumentedModuleFromAddr(rword addr) {
  return engine->removeInstrumentedModuleFromAddr(addr);
}

bool VM::run(rword start, rword stop) {
  uint32_t stopCB =
      addCodeAddrCB(stop, InstPosition::PREINST, stopCallback, nullptr);
  bool ret = engine->run(start, stop);
  deleteInstrumentation(stopCB);
  return ret;
}

#define FAKE_RET_ADDR 42

bool VM::callA(rword *retval, rword function, uint32_t argNum,
               const rword *args) {
  GPRState *state = nullptr;

  state = getGPRState();
  QBDI_REQUIRE_ACTION(state != nullptr, abort());

  // a stack pointer must be set in state
  if (QBDI_GPR_GET(state, REG_SP) == 0) {
    return false;
  }
  // push arguments in current context
  simulateCallA(state, FAKE_RET_ADDR, argNum, args);
  // call function
  bool res = run(function, FAKE_RET_ADDR);
  // get return value from current state
  if (retval != nullptr) {
    *retval = QBDI_GPR_GET(state, REG_RETURN);
  }
  return res;
}

bool VM::call(rword *retval, rword function, const std::vector<rword> &args) {
  return this->callA(retval, function, args.size(), args.data());
}

bool VM::callV(rword *retval, rword function, uint32_t argNum, va_list ap) {
  std::vector<rword> args(argNum);
  for (uint32_t i = 0; i < argNum; i++) {
    args[i] = va_arg(ap, rword);
  }

  bool res = this->callA(retval, function, argNum, args.data());

  return res;
}

uint32_t VM::addInstrRule(InstrRuleCallback cbk, AnalysisType type,
                          void *data) {
  RangeSet<rword> r;
  r.add(Range<rword>(0, (rword)-1));
  return engine->addInstrRule(InstrRuleUser::unique(cbk, type, data, this, r));
}

uint32_t VM::addInstrRule(InstrRuleCallbackC cbk, AnalysisType type,
                          void *data) {
  InstrCBInfo *_data =
      new InstrCBInfo{Range<rword>(0, (rword)-1), cbk, type, data};
  uint32_t id = addInstrRule(InstrCBGateC, type, _data);
  instrCBInfos->emplace_back(id, _data);
  return id;
}

uint32_t VM::addInstrRuleRange(rword start, rword end, InstrRuleCallback cbk,
                               AnalysisType type, void *data) {
  RangeSet<rword> r;
  r.add(Range<rword>(start, end));
  return engine->addInstrRule(InstrRuleUser::unique(cbk, type, data, this, r));
}

uint32_t VM::addInstrRuleRange(rword start, rword end, InstrRuleCallbackC cbk,
                               AnalysisType type, void *data) {
  InstrCBInfo *_data =
      new InstrCBInfo{Range<rword>(start, end), cbk, type, data};
  uint32_t id = addInstrRuleRange(start, end, InstrCBGateC, type, _data);
  instrCBInfos->emplace_back(id, _data);
  return id;
}

uint32_t VM::addInstrRuleRangeSet(RangeSet<rword> range, InstrRuleCallback cbk,
                                  AnalysisType type, void *data) {
  return engine->addInstrRule(
      InstrRuleUser::unique(cbk, type, data, this, range));
}

uint32_t VM::addMnemonicCB(const char *mnemonic, InstPosition pos,
                           InstCallback cbk, void *data) {
  QBDI_REQUIRE_ACTION(mnemonic != nullptr, return VMError::INVALID_EVENTID);
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  return engine->addInstrRule(
      InstrRuleBasic::unique(MnemonicIs::unique(mnemonic),
                             getCallbackGenerator(cbk, data), pos, true));
}

uint32_t VM::addCodeCB(InstPosition pos, InstCallback cbk, void *data) {
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  return engine->addInstrRule(InstrRuleBasic::unique(
      True::unique(), getCallbackGenerator(cbk, data), pos, true));
}

uint32_t VM::addCodeAddrCB(rword address, InstPosition pos, InstCallback cbk,
                           void *data) {
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  return engine->addInstrRule(InstrRuleBasic::unique(
      AddressIs::unique(address), getCallbackGenerator(cbk, data), pos, true));
}

uint32_t VM::addCodeRangeCB(rword start, rword end, InstPosition pos,
                            InstCallback cbk, void *data) {
  QBDI_REQUIRE_ACTION(start < end, return VMError::INVALID_EVENTID);
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  return engine->addInstrRule(
      InstrRuleBasic::unique(InstructionInRange::unique(start, end),
                             getCallbackGenerator(cbk, data), pos, true));
}

uint32_t VM::addMemAccessCB(MemoryAccessType type, InstCallback cbk,
                            void *data) {
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  recordMemoryAccess(type);
  switch (type) {
    case MEMORY_READ:
      return engine->addInstrRule(InstrRuleBasic::unique(
          DoesReadAccess::unique(), getCallbackGenerator(cbk, data),
          InstPosition::PREINST, true));
    case MEMORY_WRITE:
      return engine->addInstrRule(InstrRuleBasic::unique(
          DoesWriteAccess::unique(), getCallbackGenerator(cbk, data),
          InstPosition::POSTINST, true));
    case MEMORY_READ_WRITE:
      return engine->addInstrRule(InstrRuleBasic::unique(
          Or::unique(conv_unique<PatchCondition>(DoesReadAccess::unique(),
                                                 DoesWriteAccess::unique())),
          getCallbackGenerator(cbk, data), InstPosition::POSTINST, true));
    default:
      return VMError::INVALID_EVENTID;
  }
}

uint32_t VM::addMemAddrCB(rword address, MemoryAccessType type,
                          InstCallback cbk, void *data) {
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  return addMemRangeCB(address, address + 1, type, cbk, data);
}

uint32_t VM::addMemRangeCB(rword start, rword end, MemoryAccessType type,
                           InstCallback cbk, void *data) {
  QBDI_REQUIRE_ACTION(start < end, return VMError::INVALID_EVENTID);
  QBDI_REQUIRE_ACTION(type & MEMORY_READ_WRITE,
                      return VMError::INVALID_EVENTID);
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  if ((type == MEMORY_READ) && memReadGateCBID == VMError::INVALID_EVENTID) {
    memReadGateCBID =
        addMemAccessCB(MEMORY_READ, memReadGate, memCBInfos.get());
  }
  if ((type & MEMORY_WRITE) && memWriteGateCBID == VMError::INVALID_EVENTID) {
    memWriteGateCBID =
        addMemAccessCB(MEMORY_READ_WRITE, memWriteGate, memCBInfos.get());
  }
  uint32_t id = memCBID++;
  QBDI_REQUIRE_ACTION(id < EVENTID_VIRTCB_MASK,
                      return VMError::INVALID_EVENTID);
  memCBInfos->emplace_back(id, MemCBInfo{type, {start, end}, cbk, data});
  return id | EVENTID_VIRTCB_MASK;
}

uint32_t VM::addVMEventCB(VMEvent mask, VMCallback cbk, void *data) {
  QBDI_REQUIRE_ACTION(mask != 0, return VMError::INVALID_EVENTID);
  QBDI_REQUIRE_ACTION(cbk != nullptr, return VMError::INVALID_EVENTID);
  return engine->addVMEventCB(mask, cbk, data);
}

bool VM::deleteInstrumentation(uint32_t id) {
  if (id & EVENTID_VIRTCB_MASK) {
    id &= ~EVENTID_VIRTCB_MASK;
    for (size_t i = 0; i < memCBInfos->size(); i++) {
      if ((*memCBInfos)[i].first == id) {
        memCBInfos->erase(memCBInfos->begin() + i);
        return true;
      }
    }
    return false;
  } else {
    instrCBInfos->erase(
        std::remove_if(
            instrCBInfos->begin(), instrCBInfos->end(),
            [id](const std::pair<uint32_t, std::unique_ptr<InstrCBInfo>> &x) {
              return x.first == id;
            }),
        instrCBInfos->end());
    return engine->deleteInstrumentation(id);
  }
}

void VM::deleteAllInstrumentations() {
  engine->deleteAllInstrumentations();
  memReadGateCBID = VMError::INVALID_EVENTID;
  memWriteGateCBID = VMError::INVALID_EVENTID;
  memCBInfos->clear();
  instrCBInfos->clear();
  memoryLoggingLevel = 0;
}

const InstAnalysis *VM::getInstAnalysis(AnalysisType type) const {
  const ExecBlock *curExecBlock = engine->getCurExecBlock();
  QBDI_REQUIRE_ACTION(curExecBlock != nullptr, return nullptr);
  uint16_t curInstID = curExecBlock->getCurrentInstID();
  return curExecBlock->getInstAnalysis(curInstID, type);
}

const InstAnalysis *VM::getCachedInstAnalysis(rword address,
                                              AnalysisType type) const {
  return engine->getInstAnalysis(address, type);
}

bool VM::recordMemoryAccess(MemoryAccessType type) {
  if constexpr (not(is_x86_64 or is_x86))
    return false;

  if (type & MEMORY_READ && !(memoryLoggingLevel & MEMORY_READ)) {
    memoryLoggingLevel |= MEMORY_READ;
    for (auto &r : getInstrRuleMemAccessRead()) {
      engine->addInstrRule(std::move(r));
    }
  }
  if (type & MEMORY_WRITE && !(memoryLoggingLevel & MEMORY_WRITE)) {
    memoryLoggingLevel |= MEMORY_WRITE;
    for (auto &r : getInstrRuleMemAccessWrite()) {
      engine->addInstrRule(std::move(r));
    }
  }
  return true;
}

std::vector<MemoryAccess> VM::getInstMemoryAccess() const {
  const ExecBlock *curExecBlock = engine->getCurExecBlock();
  if (curExecBlock == nullptr) {
    return {};
  }
  uint16_t instID = curExecBlock->getCurrentInstID();
  std::vector<MemoryAccess> memAccess;
  analyseMemoryAccess(*curExecBlock, instID, !engine->isPreInst(), memAccess);
  return memAccess;
}

std::vector<MemoryAccess> VM::getBBMemoryAccess() const {
  const ExecBlock *curExecBlock = engine->getCurExecBlock();
  if (curExecBlock == nullptr) {
    return {};
  }
  uint16_t bbID = curExecBlock->getCurrentSeqID();
  uint16_t instID = curExecBlock->getCurrentInstID();
  std::vector<MemoryAccess> memAccess;
  QBDI_DEBUG(
      "Search MemoryAccess for Basic Block {:x} stopping at Instruction {:x}",
      bbID, instID);

  uint16_t endInstID = curExecBlock->getSeqEnd(bbID);
  for (uint16_t itInstID = curExecBlock->getSeqStart(bbID);
       itInstID <= std::min(endInstID, instID); itInstID++) {

    analyseMemoryAccess(*curExecBlock, itInstID,
                        itInstID != instID || !engine->isPreInst(), memAccess);
  }
  return memAccess;
}

bool VM::precacheBasicBlock(rword pc) { return engine->precacheBasicBlock(pc); }

void VM::clearAllCache() { engine->clearAllCache(); }

void VM::clearCache(rword start, rword end) { engine->clearCache(start, end); }

} // namespace QBDI
