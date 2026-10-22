// 윈도우 시작 시 자동 실행 — 작업 스케줄러(최고 권한)로 UAC 창 없이 로그온 시 실행.
#pragma once
#include "common.h"
#include <windows.h>
#include <vector>

inline DWORD RunHidden(const std::wstring& cmd) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}
inline bool AutoStartOn() { return RunHidden(L"schtasks.exe /Query /TN \"KGNF NetShutter\"") == 0; }
inline void AutoStartSet(bool on) {
    if (!on) { RunHidden(L"schtasks.exe /Delete /F /TN \"KGNF NetShutter\""); return; }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t user[256] = L"", dom[256] = L"";
    DWORD n = 256; GetEnvironmentVariableW(L"USERNAME", user, n);
    n = 256; GetEnvironmentVariableW(L"USERDOMAIN", dom, n);
    std::wstring who = std::wstring(dom) + L"\\" + user;
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + who + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"A\"><UserId>" + who + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings>\r\n"
        L"<Actions Context=\"A\"><Exec><Command>" + std::wstring(exe) + L"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring f = std::wstring(tmp) + L"kgnf_netmeter.xml";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WORD bom = 0xFEFF;
    WriteFile(h, &bom, 2, &w, nullptr);
    WriteFile(h, xml.c_str(), (DWORD)(xml.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
    RunHidden(L"schtasks.exe /Create /F /TN \"KGNF NetShutter\" /XML \"" + f + L"\"");
    DeleteFileW(f.c_str());
}
