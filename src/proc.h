// 프로세스 경로 / 표시 이름 / 창 존재 여부
#pragma once
#include "common.h"
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <winternl.h>
#include <shellapi.h>
#include <vector>
#include <set>
#include <unordered_map>

struct ProcInfo {
    std::wstring path;   // 소문자 전체 경로 (앱 식별 키)
    DWORD session = 0;   // 0 = 서비스 세션
    DWORD parent = 0;    // 부모 pid (업로드 출처를 창 있는 앱으로 묶을 때)
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
// 모든 프로세스의 pid -> 경로/세션 캐시 갱신 (이전 캐시 재사용)
inline void SnapshotProcs(std::unordered_map<DWORD, ProcInfo>& procs) {
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
        pi.parent = pe.th32ParentProcessID;
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

// exe 버전 정보의 문자열 하나. 원하는 언어(0x0412 한국어 / 0x0409 영어) 항목이 있으면 그것, 없으면 첫 항목.
inline std::wstring VersionString(const std::wstring& path, const wchar_t* field, WORD lang) {
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return L"";
    std::vector<BYTE> v(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, v.data())) return L"";
    struct LangCp { WORD l, c; } *tr = nullptr;
    UINT n = 0;
    if (!VerQueryValueW(v.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &n) || n < sizeof(LangCp)) return L"";
    UINT cnt = n / sizeof(LangCp), pick = 0;
    for (UINT i = 0; i < cnt; i++) if (tr[i].l == lang) { pick = i; break; }
    wchar_t q[128];
    swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\%ls", tr[pick].l, tr[pick].c, field);
    wchar_t* d = nullptr;
    UINT dl = 0;
    if (!VerQueryValueW(v.data(), q, (LPVOID*)&d, &dl) || !d || dl <= 1) return L"";
    std::wstring s = d;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

// 업로드 출처 표시 이름: 제품 이름 → (윈도우 기본 프로그램이면) 파일 설명 → 실행파일 이름
inline std::wstring ProductLabel(const std::wstring& path, WORD lang) {
    bool full = path.find(L'\\') != std::wstring::npos;  // 파일 이름만 있으면 엉뚱한 파일을 찾지 않게 버전 정보는 안 읽음
    std::wstring s = full ? VersionString(path, L"ProductName", lang) : L"";
    if (full && (s.empty() || s.find(L"Operating System") != std::wstring::npos || s.find(L"운영 체제") != std::wstring::npos))
        s = VersionString(path, L"FileDescription", lang);
    if (s.empty()) {
        s = FileStem(path);
        if (!s.empty()) s[0] = towupper(s[0]);
    }
    if (s.size() > 28) s = s.substr(0, 27) + L"…";
    return s;
}

// 윈도우 핵심 프로세스(서비스 호스트, System 등): 윈도우 폴더의 서비스 세션 프로세스와 서비스 호스트류
inline bool IsWindowsCore(const std::wstring& path, DWORD session) {
    if (path == L"system") return true;
    if (path.find(L'\\') == std::wstring::npos) {  // 경로를 못 읽은 보호 프로세스: 이름으로
        const wchar_t* core[] = {L"svchost.exe", L"registry", L"lsass.exe", L"services.exe", L"csrss.exe",
                                 L"smss.exe", L"wininit.exe", L"memory compression", L"system"};
        for (auto c : core) if (path == c) return true;
        return false;
    }
    static std::wstring win;
    if (win.empty()) {
        wchar_t b[MAX_PATH];
        win = GetWindowsDirectoryW(b, MAX_PATH) ? Lower(b) + L"\\" : L"c:\\windows\\";
    }
    if (path.compare(0, win.size(), win) != 0) return false;
    if (session == 0) return true;
    std::wstring st = Lower(FileStem(path));
    const wchar_t* hosts[] = {L"svchost", L"taskhostw", L"backgroundtaskhost", L"runtimebroker", L"dllhost", L"wudfhost"};
    for (auto h : hosts) if (st == h) return true;
    return false;
}

// 부모를 거슬러 올라가다 멈출 곳: 셸·터미널·탐색기·서비스 관리자 (그 아래 프로그램은 각자 출처)
inline bool IsLauncherShell(const std::wstring& path) {
    std::wstring st = Lower(FileStem(path));
    const wchar_t* s[] = {L"explorer", L"cmd", L"powershell", L"pwsh", L"windowsterminal", L"conhost", L"openconsole",
                          L"bash", L"sh", L"mintty", L"wsl", L"wslhost", L"services", L"svchost", L"wininit",
                          L"userinit", L"sihost", L"taskeng", L"taskhostw", L"code", L"devenv", L"claude"};
    for (auto x : s) if (st == x) return true;
    return false;
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
