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
#ifndef LAYER2_ARM_H
#define LAYER2_ARM_H

#include "Patch/ARM/RelocatableInst_ARM.h"
#include "Patch/Types.h"
#include "Utility/LogSys.h"

namespace QBDI {

// Low level layer 2

llvm::MCInst ldri12(unsigned int reg, unsigned int base, rword offset);

llvm::MCInst stri12(unsigned int reg, unsigned int base, rword offset);

llvm::MCInst adr(unsigned int reg, rword offset);

llvm::MCInst mov(unsigned int dst, unsigned int src);

llvm::MCInst add(unsigned int dst, unsigned int src);

llvm::MCInst pop(unsigned int reg, int64_t cond);

llvm::MCInst push(unsigned int reg, int64_t cond);

llvm::MCInst b(rword offset);

// High level layer 2

RelocatableInst::SharedPtr Ldr(Reg reg, Reg base, Offset offset);

RelocatableInst::SharedPtr Ldr(Reg reg, Offset offset);

RelocatableInst::SharedPtr Ldr(Reg reg, Constant constant);

RelocatableInst::SharedPtr Str(Reg reg, Reg base, Offset offset);

RelocatableInst::SharedPtr Str(Reg reg, Offset offset);

RelocatableInst::SharedPtr Str(Reg reg, Constant constant);

RelocatableInst::SharedPtr Vstrs(unsigned int reg, unsigned int base,
                                 rword offset);

RelocatableInst::SharedPtr Vldrs(unsigned int reg, unsigned int base,
                                 rword offset);

RelocatableInst::SharedPtr Adr(Reg reg, rword offset);

RelocatableInst::SharedPtr Adr(Reg reg, Offset offset);

RelocatableInst::SharedPtr Msr(Reg reg);

RelocatableInst::SharedPtr Mrs(Reg reg);

RelocatableInst::SharedPtr Popr(Reg reg);

RelocatableInst::SharedPtr Pushr(Reg reg);
} // namespace QBDI

#endif
