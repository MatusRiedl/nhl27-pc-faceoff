#ifndef KYTY_COMMON_PROCESS_EXIT_H_
#define KYTY_COMMON_PROCESS_EXIT_H_

namespace Common {

// Set once a DLL is loaded whose DLL_PROCESS_DETACH must not run when the process ends. NVIDIA Streamline
// (frame generation) is one: its detach releases DLSS-G while the Vulkan device and the guest threads are
// still alive and crashes inside nvngx_dlssg.dll. With the flag set, DbgExit ends the process with
// TerminateProcess (after the emergency flush) instead of ExitProcess, and the other exits route through it.
void SetTerminateOnExit();
bool TerminateOnExit();

} // namespace Common

#endif /* KYTY_COMMON_PROCESS_EXIT_H_ */
