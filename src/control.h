// 2차 동작: 네트워크 차단(윈도우 방화벽 규칙). 윈도우 공개 API(Windows Firewall)만 사용, 되돌릴 수 있음.
#pragma once
#include "common.h"
#include "proc.h"
#include <objbase.h>
#include <netfw.h>
#include <iphlpapi.h>
#include <vector>
#include <set>
#include <map>

// ── 네트워크: 특정 실행파일의 인바운드/아웃바운드를 방화벽에서 차단/해제 ──
inline const wchar_t* FwGroup() { return L"KGNF NetShutter"; }

inline INetFwRules* OpenFwRules(INetFwPolicy2** polOut) {
    INetFwPolicy2* pol = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwPolicy2), (void**)&pol))) return nullptr;
    INetFwRules* rules = nullptr;
    if (FAILED(pol->get_Rules(&rules))) { pol->Release(); return nullptr; }
    *polOut = pol;
    return rules;
}
inline bool FwBlockAdd(const std::wstring& path) {
    INetFwPolicy2* pol = nullptr;
    INetFwRules* rules = OpenFwRules(&pol);
    if (!rules) return false;
    bool ok = true;
    for (int dir = 0; dir < 2; dir++) {
        INetFwRule* r = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(INetFwRule), (void**)&r))) { ok = false; continue; }
        std::wstring name = std::wstring(L"NetShutter ") + (dir ? L"IN " : L"OUT ") + path;
        BSTR bn = SysAllocString(name.c_str()), bp = SysAllocString(path.c_str()), bg = SysAllocString(FwGroup());
        r->put_Name(bn);
        r->put_ApplicationName(bp);
        r->put_Grouping(bg);
        r->put_Direction(dir ? NET_FW_RULE_DIR_IN : NET_FW_RULE_DIR_OUT);
        r->put_Action(NET_FW_ACTION_BLOCK);
        r->put_Profiles(NET_FW_PROFILE2_ALL);
        r->put_Enabled(VARIANT_TRUE);
        if (FAILED(rules->Add(r))) ok = false;
        SysFreeString(bn); SysFreeString(bp); SysFreeString(bg);
        r->Release();
    }
    rules->Release();
    pol->Release();
    return ok;
}
// path 비어 있으면 NetShutter가 만든 규칙 전부 삭제 (종료 시 청소)
inline void FwBlockRemove(const std::wstring& path) {
    INetFwPolicy2* pol = nullptr;
    INetFwRules* rules = OpenFwRules(&pol);
    if (!rules) return;
    std::vector<std::wstring> kill;
    IUnknown* unk = nullptr;
    if (SUCCEEDED(rules->get__NewEnum(&unk))) {
        IEnumVARIANT* ev = nullptr;
        if (SUCCEEDED(unk->QueryInterface(IID_IEnumVARIANT, (void**)&ev))) {
            VARIANT var;
            VariantInit(&var);
            ULONG got = 0;
            while (ev->Next(1, &var, &got) == S_OK && got == 1) {
                INetFwRule* r = nullptr;
                if (var.vt == VT_DISPATCH && var.pdispVal &&
                    SUCCEEDED(var.pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)&r))) {
                    BSTR g = nullptr, n = nullptr, a = nullptr;
                    r->get_Grouping(&g);
                    r->get_Name(&n);
                    r->get_ApplicationName(&a);
                    if (g && wcscmp(g, FwGroup()) == 0 && n &&
                        (path.empty() || (a && Lower(a) == Lower(path))))
                        kill.push_back(n);
                    if (g) SysFreeString(g);
                    if (n) SysFreeString(n);
                    if (a) SysFreeString(a);
                    r->Release();
                }
                VariantClear(&var);
            }
            ev->Release();
        }
        unk->Release();
    }
    for (auto& n : kill) {
        BSTR b = SysAllocString(n.c_str());
        rules->Remove(b);
        SysFreeString(b);
    }
    rules->Release();
    pol->Release();
}
// ── 지금 외부(인터넷)와 통신 중인 프로세스 경로 수집 (연결 테이블 기반) ──
// ETW 귀속과 무관하게 소켓 owner PID로 잡으므로 크롬 원격·UDP·서비스도 정확히 잡힘.
inline bool IsLocalOrLan(DWORD addrBE) {
    BYTE a = addrBE & 0xFF, b = (addrBE >> 8) & 0xFF;
    if (a == 127) return true;               // localhost
    if (a == 10) return true;                // 10.0.0.0/8
    if (a == 192 && b == 168) return true;   // 192.168.0.0/16
    if (a == 169 && b == 254) return true;   // link-local
    if (a == 172 && b >= 16 && b <= 31) return true; // 172.16/12
    if (addrBE == 0) return true;            // 0.0.0.0
    return false;
}
inline std::set<std::wstring> ActiveExternalProcs(std::map<std::wstring, int>* connCount = nullptr) {
    std::set<std::wstring> paths;
    std::map<DWORD, std::wstring> cache;
    auto pathOf = [&](DWORD pid) -> std::wstring {
        auto it = cache.find(pid);
        if (it != cache.end()) return it->second;
        std::wstring p = PathOfPid(pid);
        cache[pid] = p;
        return p;
    };
    // TCP: 외부 IP로 ESTABLISHED인 것
    {
        DWORD sz = 0;
        GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        std::vector<BYTE> buf(sz + 8192);
        sz = (DWORD)buf.size();
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            auto t = (PMIB_TCPTABLE_OWNER_PID)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                auto& e = t->table[i];
                if (e.dwState != MIB_TCP_STATE_ESTAB) continue;
                if (IsLocalOrLan(e.dwRemoteAddr)) continue;
                std::wstring p = pathOf(e.dwOwningPid);
                if (p.empty() || p == L"system") continue;
                paths.insert(p);
                if (connCount) (*connCount)[p]++;
            }
        }
    }
    // UDP: 로컬 포트에 바인드된 소켓(원격 주소 없음) → owner PID로 포함 (WebRTC/크롬 원격 커버)
    {
        DWORD sz = 0;
        GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        std::vector<BYTE> buf(sz + 8192);
        sz = (DWORD)buf.size();
        if (GetExtendedUdpTable(buf.data(), &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            auto t = (PMIB_UDPTABLE_OWNER_PID)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; i++) {
                auto& e = t->table[i];
                USHORT port = ntohs((u_short)e.dwLocalPort);
                if (port == 0 || port == 53 || port == 5353 || port == 1900 || port == 137 || port == 138)
                    continue;  // DNS·mDNS·SSDP·NetBIOS 등 로컬 서비스 제외
                std::wstring p = pathOf(e.dwOwningPid);
                if (p.empty() || p == L"system") continue;
                paths.insert(p);
                if (connCount) (*connCount)[p]++;
            }
        }
    }
    return paths;
}

// 이미 맺어진 TCP(IPv4) 연결을 끊어 재연결이 새 규칙에 막히게 함 (즉시 효과)
inline void DropEstablishedTcp(const std::set<DWORD>& pids) {
    DWORD sz = 0;
    GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (!sz) return;
    std::vector<BYTE> buf(sz + 4096);
    sz = (DWORD)buf.size();
    if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return;
    auto t = (PMIB_TCPTABLE_OWNER_PID)buf.data();
    for (DWORD i = 0; i < t->dwNumEntries; i++) {
        auto& e = t->table[i];
        if (!pids.count(e.dwOwningPid) || e.dwState != MIB_TCP_STATE_ESTAB) continue;
        MIB_TCPROW row;
        row.dwState = MIB_TCP_STATE_DELETE_TCB;
        row.dwLocalAddr = e.dwLocalAddr;
        row.dwLocalPort = e.dwLocalPort;
        row.dwRemoteAddr = e.dwRemoteAddr;
        row.dwRemotePort = e.dwRemotePort;
        SetTcpEntry(&row);  // 요청 프로세스만 대상, 관리자 권한 필요
    }
}
