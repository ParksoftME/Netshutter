// 감지: (1) 마이크·카메라 사용 앱 — 윈도우 프라이버시 기록(ConsentStore)을 읽음
//       (2) 앱별 송수신량 — Kernel-Network ETW 세션 (활동 중일 때만 켬)
// 둘 다 윈도우가 공개한 자체 기록/계측을 읽는 방식이고, 다른 앱 메모리는 건드리지 않음.
#pragma once
#include "common.h"
#include "proc.h"
#include <evntrace.h>
#include <evntcons.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <vector>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>

// ── 마이크·카메라: 작업표시줄 마이크 아이콘이 읽는 바로 그 레지스트리 ──
inline bool ConsentInUse(HKEY h, const wchar_t* sub) {
    ULONGLONG start = 0, stop = 1;
    DWORD sz = sizeof(start);
    if (RegGetValueW(h, sub, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &sz) != ERROR_SUCCESS) return false;
    sz = sizeof(stop);
    if (RegGetValueW(h, sub, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &sz) != ERROR_SUCCESS) return false;
    return start != 0 && stop == 0;  // 시작됨, 아직 안 멈춤 = 지금 사용 중
}
inline void ReadConsent(HKEY root, const wchar_t* cap, std::set<std::wstring>& paths, std::set<std::wstring>& pkgs) {
    std::wstring base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    base += cap;
    HKEY h;
    if (RegOpenKeyExW(root, base.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    wchar_t name[512];
    for (DWORD i = 0;; i++) {
        DWORD n = 512;
        if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (_wcsicmp(name, L"NonPackaged") == 0) {
            HKEY hn;
            if (RegOpenKeyExW(h, name, 0, KEY_READ, &hn) != ERROR_SUCCESS) continue;
            wchar_t sub[1024];
            for (DWORD j = 0;; j++) {
                DWORD m = 1024;
                if (RegEnumKeyExW(hn, j, sub, &m, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                if (ConsentInUse(hn, sub)) {
                    std::wstring p = sub;
                    std::replace(p.begin(), p.end(), L'#', L'\\');
                    paths.insert(Lower(p));
                }
            }
            RegCloseKey(hn);
        } else if (ConsentInUse(h, name)) {
            pkgs.insert(Lower(name));  // UWP 패키지는 패밀리명으로
        }
    }
    RegCloseKey(h);
}

// ── 전체 인터페이스 누적 바이트 (활동 게이트 + 폴백 표시용) ──
inline bool TotalOctets(unsigned long long& in, unsigned long long& out) {
    PMIB_IF_TABLE2 t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR) return false;
    in = out = 0;
    for (ULONG i = 0; i < t->NumEntries; i++) {
        auto& r = t->Table[i];
        if (!r.InterfaceAndOperStatusFlags.HardwareInterface) continue;
        if (r.OperStatus != IfOperStatusUp) continue;
        in += r.InOctets;
        out += r.OutOctets;
    }
    FreeMibTable(t);
    return true;
}

// ── 앱별 송수신: Microsoft-Windows-Kernel-Network 실시간 ETW ──
// 이벤트 페이로드 선두 = PID(UInt32), size(UInt32). 송신/수신 이벤트 ID로 구분.
inline const GUID& KernelNetGuid() {
    static const GUID g = {0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
    return g;
}
inline const wchar_t* EtwSessionName() { return L"KGNF-NetShutter"; }

struct EtwCounter {
    TRACEHANDLE session = 0;
    TRACEHANDLE trace = (TRACEHANDLE)INVALID_HANDLE_VALUE;
    std::thread worker;
    bool running = false;
    std::mutex mu;
    std::unordered_map<DWORD, std::pair<unsigned long long, unsigned long long>> acc;  // pid -> up,down

    static void FillProps(std::vector<BYTE>& buf) {
        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(EtwSessionName()) + 1) * sizeof(wchar_t) + 8;
        buf.assign(sz, 0);
        auto p = (EVENT_TRACE_PROPERTIES*)buf.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;  // QPC
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p->FlushTimer = 1;
        p->BufferSize = 32;
        p->MinimumBuffers = 4;
        p->MaximumBuffers = 16;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    }
    static void StopStale() {
        std::vector<BYTE> buf;
        FillProps(buf);
        ControlTraceW(0, EtwSessionName(), (EVENT_TRACE_PROPERTIES*)buf.data(), EVENT_TRACE_CONTROL_STOP);
    }
    static VOID WINAPI OnEvent(PEVENT_RECORD r) {
        if (!IsEqualGUID(r->EventHeader.ProviderId, KernelNetGuid())) return;
        if (r->UserDataLength < 8 || !r->UserData) return;
        USHORT id = r->EventHeader.EventDescriptor.Id;
        bool send = (id == 10 || id == 26 || id == 42 || id == 58);
        bool recv = (id == 11 || id == 27 || id == 43 || id == 59);
        if (!send && !recv) return;
        auto self = (EtwCounter*)r->UserContext;
        DWORD pid = *(const UINT32*)r->UserData;
        DWORD size = *((const UINT32*)r->UserData + 1);
        std::lock_guard<std::mutex> lk(self->mu);
        auto& a = self->acc[pid];
        if (send) a.first += size; else a.second += size;
    }
    bool Start() {
        if (running) return true;
        StopStale();
        std::vector<BYTE> buf;
        FillProps(buf);
        if (StartTraceW(&session, EtwSessionName(), (EVENT_TRACE_PROPERTIES*)buf.data()) != ERROR_SUCCESS) {
            session = 0;
            return false;
        }
        if (EnableTraceEx2(session, &KernelNetGuid(), EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                           TRACE_LEVEL_VERBOSE, 0x30 /*IPv4|IPv6*/, 0, 0, nullptr) != ERROR_SUCCESS) {
            FillProps(buf);
            ControlTraceW(session, EtwSessionName(), (EVENT_TRACE_PROPERTIES*)buf.data(), EVENT_TRACE_CONTROL_STOP);
            session = 0;
            return false;
        }
        EVENT_TRACE_LOGFILEW lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.LoggerName = (LPWSTR)EtwSessionName();
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &EtwCounter::OnEvent;
        lf.Context = this;
        trace = OpenTraceW(&lf);
        if (trace == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
            FillProps(buf);
            ControlTraceW(session, EtwSessionName(), (EVENT_TRACE_PROPERTIES*)buf.data(), EVENT_TRACE_CONTROL_STOP);
            session = 0;
            return false;
        }
        TRACEHANDLE t = trace;
        worker = std::thread([t]() mutable { ProcessTrace(&t, 1, nullptr, nullptr); });
        running = true;
        return true;
    }
    void Stop() {
        if (!running) return;
        std::vector<BYTE> buf;
        FillProps(buf);
        ControlTraceW(session, EtwSessionName(), (EVENT_TRACE_PROPERTIES*)buf.data(), EVENT_TRACE_CONTROL_STOP);
        CloseTrace(trace);
        if (worker.joinable()) worker.join();
        session = 0;
        trace = (TRACEHANDLE)INVALID_HANDLE_VALUE;
        running = false;
        std::lock_guard<std::mutex> lk(mu);
        acc.clear();
    }
    void Drain(std::unordered_map<DWORD, std::pair<unsigned long long, unsigned long long>>& out) {
        std::lock_guard<std::mutex> lk(mu);
        out.swap(acc);
        acc.clear();
    }
};
