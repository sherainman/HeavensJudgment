#include <Windows.h>

#include "HeavensJudgment.hpp"

BOOL APIENTRY DllMain(
    HMODULE module,
    DWORD reason,
    LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(
            nullptr,
            0,
            HJ::Initialize,
            module,
            0,
            nullptr
        );

        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}