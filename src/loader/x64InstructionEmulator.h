#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstdint>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);

// Issue #1 diagnostics (a boot crash seen on Intel PCs), off unless the variable is 1:
// - KYTY_FORCE_SSE4A_TRAP=1 (diagnostic builds only, CMake KYTY_SSE4A_TRAP_DIAG; shipping builds ignore it
//   and log that): the guest's EXTRQ/INSERTQ go through this emulator on hosts that run them natively
//   (AMD), as on Intel. The patcher marks each site by swapping its opcode byte for UD1 (0F B9,
//   from the immediate form 0F 78) or UD0 (0F FF, from the register form 0F 79). Both raise #UD on every
//   x86-64 CPU, and the rest of the instruction is kept, so the emulator still reads its operands.
// - KYTY_INSERTQ_CLEAR_HIGH=1: emulated INSERTQ zeroes the destination's upper quadword, as EXTRQ does.
[[nodiscard]] bool ForceSse4aTrap() noexcept;
inline constexpr uint8_t Sse4aTrapImmediateOpcode = 0xb9;
inline constexpr uint8_t Sse4aTrapRegisterOpcode  = 0xff;

// Reads `size` bytes at `address` from somewhere other than the faulting view; false when it
// cannot serve the read.
using LoadReader = bool (*)(uint64_t address, void* out, uint64_t size);

// Completes a faulting plain load (MOV/MOVZX/MOVSX(D) into a general register, or a whole-vector
// SSE/AVX load into XMM/YMM) with bytes from `reader`, then skips the instruction. Returns false
// without touching the context for anything else.
[[nodiscard]] bool TryEmulateLoad(void* native_context, LoadReader reader);

// Debugging: "<mnemonic> <prefix> mem=<bytes>" for the instruction at the context's RIP.
bool DescribeInstruction(void* native_context, char* out, uint32_t out_size);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
