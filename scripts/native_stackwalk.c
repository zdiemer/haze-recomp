/* Optional AMD64 diagnostic helper. Use outside FPS benchmark windows.
 * DbgHelp is initialized by the caller, which also owns suspend/resume.
 * SDK declarations keep STACKFRAME64/CONTEXT layouts out of Python.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <string.h>

__declspec(dllexport) unsigned haze_profile_stackwalk(
    HANDLE process, HANDLE thread, const CONTEXT* snapshot,
    DWORD64* output, unsigned capacity)
{
    if (!snapshot || !output || !capacity || capacity > 64) return 0;
    CONTEXT context;
    memcpy(&context, snapshot, sizeof(context));
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    DWORD64 previous_pc = context.Rip, previous_stack = context.Rsp;
    unsigned count = 1;
    output[0] = context.Rip;
    for (unsigned attempt = 0; count < capacity && attempt < 64; ++attempt) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (!frame.AddrPC.Offset) break;
        /* The first call may report the supplied frame before unwinding. */
        if (frame.AddrPC.Offset == previous_pc && frame.AddrStack.Offset == previous_stack) {
            if (!attempt) continue;
            break;
        }
        previous_pc = frame.AddrPC.Offset;
        previous_stack = frame.AddrStack.Offset;
        output[count++] = previous_pc;
    }
    return count;
}
