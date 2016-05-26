// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <initializer_list>
#include <list>
#include <memory>
#include <vector>

#include <boost/variant/recursive_wrapper.hpp>
#include <boost/variant/variant.hpp>

#include "common/common_types.h"

#include "core/arm/jit/common.h"

namespace ArmJit {

// ARM Jit Microinstruction Intermediate Representation
//
// This intermediate representation is an SSA IR. It is designed
// primarily for analysis, though it can be interpreted when lowered
// into a reduced form. Each IR node is a microinstruction of an
// idealised ARM CPU (MicroValue).
//
// A basic block is represented as a MicroBlock.

// Forward declarations

enum class MicroArmFlags;
class MicroBlock;
class MicroConstU32;
class MicroGetGPR;
class MicroInst;
enum class MicroOp;
struct MicroOpInfo;
class MicroSetGPR;
class MicroValue;

// MicroTerminal declarations

namespace MicroTerm {
    struct If;

    /// This terminal instruction calls the interpreter.
    struct Interpret {
        LocationDescriptor next;
    };

    /// This terminal instruction jumps to the basic block described by `next` if we have enough cycles remaining.
    struct LinkBlock {
        LocationDescriptor next;
    };

    /// This terminal instruction jumps to the basic block described by `next` unconditionally, regardless of cycles remaining.
    struct LinkBlockFast {
        LocationDescriptor next;
    };

    /// This terminal instruction returns control to the dispatcher.
    struct ReturnToDispatch {};

    /**
     * This terminal instruction checks the top of the return stack buffer. If RSB return fails, control is returned to the dispatcher.
     * This is an optimization for faster function calls; a backend may choose to implement this as ReturnToDispatch instead.
     */
    struct PopRSBHint {};

    using MicroTerminal = boost::variant<
        ReturnToDispatch,
        PopRSBHint,
        Interpret,
        LinkBlock,
        LinkBlockFast,
        boost::recursive_wrapper<If>
    >;

    /// This terminal instruction conditionally executes one terminal or another depending on the run-time state of the ARM flags.
    struct If {
        Cond if_;
        MicroTerminal then_;
        MicroTerminal else_;
    };
}

/// A MicroTerminal is the terminal instruction in a basic block.
using MicroTerm::MicroTerminal;

// Type declarations

enum class MicroArmFlags {
    N  = 1 << 0,
    Z  = 1 << 1,
    C  = 1 << 2,
    V  = 1 << 3,
    Q  = 1 << 4,
    GE = 1 << 5,

    None = 0,
    NZC = N | Z | C,
    NZCV = N | Z | C | V,
    Any = N | Z | C | V | Q | GE,
};

MicroArmFlags operator~(MicroArmFlags a);
MicroArmFlags operator|(MicroArmFlags a, MicroArmFlags b);
MicroArmFlags operator&(MicroArmFlags a, MicroArmFlags b);

enum class MicroType {
    Void,
    U32,
};

/// The operation type of a microinstruction. These are suboperations
/// of a ARM instruction.
enum class MicroOp {
    // Simple Values
    ConstU32,          // value := const
    GetGPR,            // value := R[reg]

    // Cleanup
    SetGPR,            // R[reg] := $0

    // Hints
    PushRSBHint,       // R[14] := $0, and pushes return info onto the return stack buffer [optimization].

    // ARM PC
    AluWritePC,        // R[15] := $0 & (APSR.T ? 0xFFFFFFFE : 0xFFFFFFFC) // ARMv6 behaviour
    LoadWritePC,       // R[15] := $0 & 0xFFFFFFFE, APSR.T := $0 & 0x1     // ARMv6 behaviour (UNPREDICTABLE if $0 & 0x3 == 0)

    // ARM ALU
    Add,               // value := $0 + $1, writes ASPR.NZCV
    AddWithCarry,      // value := $0 + $1 + APSR.C, writes ASPR.NZCV
    Sub,               // value := $0 - $1, writes ASPR.NZCV

    And,               // value := $0 & $1, writes ASPR.NZC
    Eor,               // value := $0 ^ $1, writes ASPR.NZC
    Not,               // value := ~$0

    LSL,               // value := $0 LSL $1, writes ASPR.C
    LSR,               // value := $0 LSR $1, writes ASPR.C
    ASR,               // value := $0 ASR $1, writes ASPR.C
    ROR,               // value := $0 ROR $1, writes ASPR.C
    RRX,               // value := $0 RRX

    CountLeadingZeros, // value := CLZ $0

    // ARM Synchronisation
    ClearExclusive,    // Clears exclusive access record

    // Memory
    Read32,            // value := Memory::Read32($0)
};

/// Information about an opcode.
struct MicroOpInfo final {
    MicroOp op;
    size_t NumArgs() const { return types.size(); }
    MicroType ret_type;
    MicroArmFlags read_flags;
    MicroArmFlags default_write_flags;
    std::vector<MicroType> types;
};
MicroOpInfo GetMicroOpInfo(MicroOp op);

/// Base-class for microinstructions to derive from.
class MicroValue : protected std::enable_shared_from_this<MicroValue> {
public:
    virtual ~MicroValue() = default;

    bool HasUses() const { return !uses.empty(); }
    bool HasOneUse() const { return uses.size() == 1; }
    bool HasManyUses() const { return uses.size() > 1; }

    /// Replace all uses of this MicroValue with `replacement`.
    void ReplaceUsesWith(std::shared_ptr<MicroValue> replacement);

    /// Get the microop this microinstruction represents.
    virtual MicroOp GetOp() const = 0;
    /// Get the type this instruction returns.
    virtual MicroType GetType() const = 0;
    /// Get the number of arguments this instruction has.
    virtual size_t NumArgs() const { return 0; }
    /// Gets the flags this instruction reads.
    virtual MicroArmFlags ReadFlags() const { return MicroArmFlags::None; }
    /// Gets the flags this instruction writes.
    virtual MicroArmFlags WriteFlags() const { return MicroArmFlags::None; }

protected:
    friend class MicroInst;
    friend class MicroSetGPR;

    void AddUse(std::shared_ptr<MicroValue> owner);
    void RemoveUse(std::shared_ptr<MicroValue> owner);
    virtual void ReplaceUseOfXWithY(std::shared_ptr<MicroValue> x, std::shared_ptr<MicroValue> y);

    struct Use {
        /// The instruction which is being used.
        std::weak_ptr<MicroValue> value;
        /// The instruction which is using `value`.
        std::weak_ptr<MicroValue> use_owner;
    };

    std::list<Use> uses;
};

/// Representation of a u32 const load instruction.
class MicroConstU32 final : public MicroValue {
public:
    explicit MicroConstU32(u32 value_) : value(value_) {}
    ~MicroConstU32() override = default;

    MicroOp GetOp() const override { return MicroOp::ConstU32; }
    MicroType GetType() const override { return MicroType::U32; }

    const u32 value;
};

/// Representation of a GPR load instruction.
class MicroGetGPR final : public MicroValue {
public:
    explicit MicroGetGPR(ArmReg reg_) : reg(reg_) {}
    ~MicroGetGPR() override = default;

    MicroOp GetOp() const override { return MicroOp::GetGPR; }
    MicroType GetType() const override { return MicroType::U32; }

    const ArmReg reg;
};

/// Representation of a GPR store instruction.
class MicroSetGPR final : public MicroValue {
public:
    MicroSetGPR(ArmReg reg_, std::shared_ptr<MicroValue> arg_) : reg(reg_) {
        SetArg(arg_);
    }
    ~MicroSetGPR() override = default;

    MicroOp GetOp() const override { return MicroOp::SetGPR; }
    MicroType GetType() const override { return MicroType::Void; }

    /// Set argument number `index` to `value`.
    void SetArg(std::shared_ptr<MicroValue> value);
    /// Get argument number `index`.
    std::shared_ptr<MicroValue> GetArg() const;

    ArmReg reg;

private:
    std::weak_ptr<MicroValue> arg;
};

/// A representation of a microinstruction. A single ARM/Thumb
/// instruction may be converted into zero or more microinstructions.
class MicroInst final : public MicroValue {
public:
    MicroInst(MicroOp op, std::initializer_list<std::shared_ptr<MicroValue>> values);
    ~MicroInst() override = default;

    MicroOp GetOp() const override { return op; }
    MicroType GetType() const override;

    /// Get number of arguments.
    size_t NumArgs() const override;
    /// Set argument number `index` to `value`.
    void SetArg(size_t index, std::shared_ptr<MicroValue> value);
    /// Get argument number `index`.
    std::shared_ptr<MicroValue> GetArg(size_t index) const;

    MicroArmFlags ReadFlags() const override;
    MicroArmFlags WriteFlags() const override;
    void SetWriteFlags(MicroArmFlags flags) { write_flags = flags; }

protected:
    void ReplaceUseOfXWithY(std::shared_ptr<MicroValue> x, std::shared_ptr<MicroValue> y) override;

private:
    MicroOp op;
    std::vector<std::weak_ptr<MicroValue>> args;
    MicroArmFlags write_flags;
};

class MicroBlock final {
public:
    explicit MicroBlock(const LocationDescriptor& location) : location(location) {}

    LocationDescriptor location;
    std::list<std::shared_ptr<MicroValue>> instructions;
    MicroTerminal terminal;
};

} // namespace ArmJit
