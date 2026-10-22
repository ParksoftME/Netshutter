// 프로세스 경로 / 표시 이름 / 창 존재 여부
#pragma once
#include "common.h"
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <winternl.h>
#include <shellapi.h>
#include <vector>
#include <set>
#include <unordered_map>

struct ProcInfo {
    std::wstring path;   // 소문자 전체 경로 (앱 식별 키)
    DWORD session = 0;   // 0 = 서비스 세션
    std::wstring pkg;    // UWP 패키지 패밀리명 (있을 때만)
};

inline std::wstring PathOfPid(DWORD pid) {
    if (pid == 0) return L"";
    if (pid == 4) return L"system";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r = Lower(buf);
    CloseHandle(h);
    return r;
}
inline std::wstring PkgOfPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[256];
    UINT32 n = 256;
    std::wstring r;
    if (GetPackageFamilyName(h, &n, buf) == ERROR_SUCCESS) r = Lower(buf);
    CloseHandle(h);
    return r;
}

// 모든 프로세스의 pid -> 경로/세션 캐시 갱신 (이전 캐시 재사용)
inline void SnapshotProcs(std::unordered_map<DWORD, ProcInfo>& procs, bool wantPkg) {
    std::unordered_map<DWORD, ProcInfo> next;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe)) {
        DWORD pid = pe.th32ProcessID;
        ProcInfo pi;
        auto it = procs.find(pid);
        if (it != procs.end() && !it->second.path.empty() &&
            Lower(FileStem(it->second.path)) == Lower(FileStem(pe.szExeFile))) {
            pi = it->second;  // 같은 pid+exe면 재사용
        } else {
            pi.path = PathOfPid(pid);
            if (pi.path.empty()) pi.path = Lower(pe.szExeFile);
            ProcessIdToSessionId(pid, &pi.session);
        }
        if (wantPkg && pi.pkg.empty() && pi.session != 0) pi.pkg = PkgOfPid(pid);
        next[pid] = pi;
    }
    CloseHandle(s);
    procs.swap(next);
}

// 보이는(최상위, 비클로킹) 창을 가진 pid 집합
struct WinCtx { std::set<DWORD>* pids; HWND self; };
inline BOOL CALLBACK EnumVisibleProc(HWND w, LPARAM lp) {
    auto c = (WinCtx*)lp;
    if (w == c->self || !IsWindowVisible(w)) return TRUE;
    LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindow(w, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
    if (GetWindowTextLengthW(w) == 0) return TRUE;
    DWORD cloaked = 0;  // 가상 데스크톱 등으로 숨은 창
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    c->pids->insert(pid);
    return TRUE;
}
inline std::set<DWORD> VisibleWindowPids(HWND self) {
    std::set<DWORD> pids;
    WinCtx c{&pids, self};
    EnumWindows(EnumVisibleProc, (LPARAM)&c);
    return pids;
}

// python·node 같은 호스트 런처는 실행파일명이 다 같아 어느 스크립트인지 모름 → 명령줄에서 뽑음
inline bool IsHostProcess(const std::wstring& stem) {
    const wchar_t* h[] = {L"python", L"pythonw", L"py", L"node", L"java", L"javaw",
                          L"electron", L"ruby", L"perl", L"powershell", L"pwsh",
                          L"wscript", L"cscript", L"dotnet", L"deno", L"bun"};
    for (auto s : h) if (stem == s) return true;
    return false;
}
inline std::wstring CommandLineOfPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return L"";
    std::wstring out;
    auto NtQIP = (NTSTATUS(NTAPI*)(HANDLE, UINT, PVOID, ULONG, PULONG))
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (NtQIP) {
        PROCESS_BASIC_INFORMATION pbi{};
        ULONG rl = 0;
        if (NtQIP(h, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), &rl) == 0 && pbi.PebBaseAddress) {
            PVOID params = nullptr;  // PEB.ProcessParameters (x64: +0x20)
            ReadProcessMemory(h, (BYTE*)pbi.PebBaseAddress + 0x20, &params, sizeof(params), nullptr);
            if (params) {
                UNICODE_STRING us{};  // RTL_USER_PROCESS_PARAMETERS.CommandLine (x64: +0x70)
                ReadProcessMemory(h, (BYTE*)params + 0x70, &us, sizeof(us), nullptr);
                if (us.Length && us.Buffer && us.Length < 0x2000) {
                    std::wstring buf(us.Length / 2, 0);
                    if (ReadProcessMemory(h, us.Buffer, &buf[0], us.Length, nullptr)) out = buf;
                }
            }
        }
    }
    CloseHandle(h);
    return out;
}
// 명령줄에서 스크립트 파일명 추출 (.py/.js/.jar 우선, 없으면 첫 비옵션 인자)
inline std::wstring ScriptName(const std::wstring& cmd) {
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(cmd.c_str(), &n);
    if (!a) return L"";
    std::wstring best, fallback;
    for (int i = 1; i < n; i++) {
        std::wstring t = a[i];
        if (t.empty() || t[0] == L'-' || t[0] == L'/') continue;
        std::wstring lt = Lower(t);
        auto has = [&](const wchar_t* e) { return lt.find(e) != std::wstring::npos; };
        if (has(L".py") || has(L".js") || has(L".jar") || has(L".ps1") || has(L".rb") || has(L".pl")) { best = t; break; }
        if (fallback.empty()) fallback = t;
    }
    LocalFree(a);
    std::wstring r = best.empty() ? fallback : best;
    size_t s = r.find_last_of(L"\\/");
    if (s != std::wstring::npos) r = r.substr(s + 1);
    if (r.size() > 20) r = r.substr(0, 19) + L"…";
    return r;
}

// exe의 FileDescription을 사람이 읽을 이름으로 (없으면 파일명)
inline std::wstring DisplayName(const std::wstring& path) {
    if (path == L"system") return L"Windows";
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (sz) {
        std::vector<BYTE> v(sz);
        if (GetFileVersionInfoW(path.c_str(), 0, sz, v.data())) {
            struct LangCp { WORD l, c; } *tr = nullptr;
            UINT n = 0;
            if (VerQueryValueW(v.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &n) && n >= sizeof(LangCp)) {
                wchar_t q[128];
                swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].l, tr[0].c);
                wchar_t* d = nullptr;
                UINT dl = 0;
                if (VerQueryValueW(v.data(), q, (LPVOID*)&d, &dl) && d && dl > 1) {
                    std::wstring s = d;
                    while (!s.empty() && iswspace(s.back())) s.pop_back();
                    if (!s.empty() && s.size() <= 22) return s;
                }
            }
        }
    }
    std::wstring st = FileStem(path);
    if (!st.empty()) st[0] = towupper(st[0]);
    return st;
}
