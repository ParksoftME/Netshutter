// NetShutter 공통 유틸 — KGNF Games, MIT License
// 본인 PC에서 "지금 무엇이 밖으로 나가고 있나"를 보여주는 프라이버시 위젯.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <algorithm>

inline std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}
inline std::wstring FileStem(const std::wstring& path) {
    size_t a = path.find_last_of(L"\\/");
    std::wstring f = (a == std::wstring::npos) ? path : path.substr(a + 1);
    size_t d = f.find_last_of(L'.');
    if (d != std::wstring::npos) f = f.substr(0, d);
    return f;
}

// 간단한 설정 저장 (HKCU)
inline const wchar_t* RegPath() { return L"Software\\KGNF\\NetShutter"; }
inline DWORD CfgGet(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, RegPath(), name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return def;
    return v;
}
inline void CfgSet(const wchar_t* name, DWORD v) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegPath(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(h);
    }
}
