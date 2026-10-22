// 감지 결과 → 앱별 업로드 목록·출처·전송 판정. 네트워크 전송만 본다.
#pragma once
#include "common.h"
#include "proc.h"
#include "detect.h"
#include "control.h"
#include <deque>
#include <map>
#include <set>
#include <vector>
#include <algorithm>

static const int kHist = 12;  // 미니 차트 포인트 수(초)

struct App {
    std::wstring key;   // 소문자 전체 경로
    std::wstring name;  // 표시 이름
    std::deque<double> up, down;  // bytes/s 이력
    double curUp = 0, curDown = 0;
    bool heavy = false, blocked = false, listed = false, nameFixed = false;
    int heavyRun = 0, hold = 0;
};

// 알려진 방송·화상 앱: 고업로드를 "화면 송출"로 볼 근거. 그 외는 그냥 대용량 업로드.
inline bool IsStreamApp(const std::wstring& path) {
    std::wstring f = Lower(FileStem(path));
    const wchar_t* list[] = {L"obs", L"obs64", L"obs32", L"streamlabs", L"xsplit", L"prism",
                             L"zoom", L"teams", L"ms-teams", L"discord", L"webex", L"skype",
                             L"slack", L"gotomeeting", L"whereby"};
    for (auto s : list) if (f.find(s) != std::wstring::npos) return true;
    return false;
}

// 전송(화면·영상·파일) 판정: "최근 N초 평균 업로드"만 본다.
//  켜짐: 평균 ≥ kOn,  꺼짐: 평균 < kOff (kOff < kOn 이라 경계에서 깜빡이지 않음)
//  감도는 평균 내는 시간 N만 다름: 민감 2초, 보통 3초, 둔감 5초
//  보통·둔감은 켜질 때 하나 더: 최근 W초 중 K초 이상이 켜짐 기준(kOn) 이상이어야 함.
//   사진 한 장은 1~2초에 몰려 나가 평균은 높아도 이 조건을 못 채움. 화면 공유의 순간 끊김 1초는 허용.
//   보통 4초 중 3초(2초 이상 계속), 둔감 6초 중 5초(4초 이상 계속). 민감은 평균만(사진 한 장도 잡음).
//  위젯에 보이는 속도도 이 평균(avg)이라, 표시가 kOff보다 낮으면 항상 "외부 전송 없음".
// 기준값(실측): 평소(아무것도 안 할 때) 2초 평균 최대 28KB/s, 480kbps 화면 공유 평균 약 62KB/s.
//  켜짐 48KB/s(384kbps)는 그 사이, 꺼짐 24KB/s(192kbps)는 평소 중앙값(3~4KB/s)보다 충분히 위.
// 입력 eff는 업로드 - 다운로드×5% (큰 다운로드에 딸려 나가는 ACK 3.5~3.9%를 뺌).
struct UploadJudge {
    static constexpr double kOn = 48 * 1024;
    static constexpr double kOff = 24 * 1024;
    std::deque<double> hist;  // 최근 6초
    double avg = 0;           // 최근 N초 평균 (판정·표시 공통)
    bool on = false;
    static size_t Window(int sens) { return sens == 2 ? 2 : sens == 1 ? 3 : 5; }
    bool Feed(double eff, int sens) {
        hist.push_back(eff > 0 ? eff : 0);
        if (hist.size() > 6) hist.pop_front();
        size_t n = std::min(Window(sens), hist.size());
        double sum = 0;
        for (size_t i = hist.size() - n; i < hist.size(); i++) sum += hist[i];
        avg = sum / n;
        // 계속 올라가는 초 수: 보통 4초 중 3초, 둔감 6초 중 5초 (민감은 조건 없음)
        size_t w = sens == 1 ? 4 : 6, k = sens == 1 ? 3 : 5;
        size_t busy = 0;
        for (size_t i = hist.size() > w ? hist.size() - w : 0; i < hist.size(); i++) busy += hist[i] >= kOn;
        bool sustained = sens == 2 || busy >= k;
        if (!on && avg >= kOn && sustained) on = true;
        else if (on && avg < kOff) on = false;
        return on;
    }
};

struct Model {
    std::map<std::wstring, App> apps;
    std::unordered_map<DWORD, ProcInfo> procs;
    EtwCounter etw;
    unsigned long long lastIn = 0, lastOut = 0;
    DWORD lastMs = 0;
    int tick = 0, lastActive = -1000;
    // 메인 UI용 통합 상태: 화면·영상·파일 전송 중이냐 / 외부 전송 없음이냐
    bool stUpBig = false;
    double stUp = 0, stDown = 0;
    UploadJudge judge;
    int sens = 1;               // 전송 감지 감도: 0 둔감, 1 보통, 2 민감
    // 업로드 출처: 업로드 많은 순 최대 3개 (키, 평활 바이트/초). 키 = 실행파일 경로, L"*win" = 윈도우 서비스, L"" = 불분명
    std::vector<std::pair<std::wstring, double>> srcTop;
    std::wstring srcKey;  // 1위 (진단 로그용)
    std::map<std::wstring, double> srcEma;  // 출처별 평활 업로드(바이트/초)
    double prevTot = 0, prevAttr = 0, prevDown = 0;  // 직전 틱 (ETW 지연 보정용)
    int etwTicks = 0;                                // ETW가 켜진 뒤 지난 틱 수
    int etwRestarts = 0;                             // 바깥에서 멈춘 ETW 세션을 다시 켠 횟수 (진단 로그용)
    std::wstring srcDiag;                            // 진단 로그용 최근 출처 계산 요약
    bool AnyLeak() const { return stUpBig; }  // 영상급 업로드(화면·영상·파일 전송 추정)

    App& Get(const std::wstring& key) {
        auto& a = apps[key];
        if (a.key.empty()) {
            a.key = key;
            a.name = (key == L"__unknown__") ? L"알 수 없는 업로드" : DisplayName(key);
            a.up.assign(kHist, 0);
            a.down.assign(kHist, 0);
        }
        return a;
    }
    // 업로드한 pid의 출처: 작업표시줄에 창이 있는 앱 소속(보조·자식 프로세스 포함)이면 그 앱,
    // 윈도우 핵심 프로세스면 "*win", 그 외에는 실행파일 자신. 프로세스를 모르면 "" (불분명).
    std::wstring SourceKeyOf(DWORD pid, const std::set<DWORD>& winPids, const std::set<std::wstring>& winKeys) {
        auto it = procs.find(pid);
        if (it == procs.end() || pid == 0) return L"";
        if (IsWindowsCore(it->second.path, it->second.session)) return L"*win";
        if (it->second.path.find(L'\\') == std::wstring::npos) {
            // 경로를 못 읽은 프로세스(권한·보호): 같은 실행파일의 다른 프로세스에서 경로를 빌리고, 없으면 파일 이름
            for (auto& kv : procs)
                if (kv.second.path.find(L'\\') != std::wstring::npos &&
                    Lower(FileStem(kv.second.path)) == Lower(FileStem(it->second.path)))
                    return kv.second.path;
            return it->second.path;
        }
        // 부모를 따라 올라가며 창 있는 앱을 찾음. 창이 없으면(트레이 앱) 셸·탐색기 바로 아래의 최상위 프로세스.
        DWORD cur = pid;
        std::wstring root = it->second.path;
        for (int depth = 0; depth < 8; depth++) {
            auto c = procs.find(cur);
            if (c == procs.end()) break;
            if (winPids.count(cur) || winKeys.count(c->second.path)) return c->second.path;
            if (c->second.path.find(L'\\') != std::wstring::npos && !IsWindowsCore(c->second.path, c->second.session))
                root = c->second.path;
            DWORD par = c->second.parent;
            auto p = procs.find(par);
            if (p == procs.end() || par == cur || p->second.session != c->second.session ||
                IsLauncherShell(p->second.path)) break;
            cur = par;
        }
        if (root != it->second.path) return root;
        // 부모가 이미 끝난 보조 프로세스: 같은 설치 폴더(또는 그 상위)에서 창이 있거나 셸·탐색기가 직접 띄운
        // 본체 앱이 실행 중이면 그 앱 소속. "C:\Program Files\" 같은 공용 폴더(깊이 2 이하)는 묶지 않음.
        std::wstring dir = root.substr(0, root.find_last_of(L'\\') + 1), best;
        size_t bestLen = 0;
        for (auto& kv : procs) {
            const std::wstring& p = kv.second.path;
            if (p == root || kv.second.session != it->second.session || p.find(L'\\') == std::wstring::npos) continue;
            std::wstring d = p.substr(0, p.find_last_of(L'\\') + 1);
            if (std::count(d.begin(), d.end(), L'\\') < 3 || dir.compare(0, d.size(), d) != 0) continue;
            if (IsWindowsCore(p, kv.second.session) || IsLauncherShell(p)) continue;
            auto par = procs.find(kv.second.parent);
            bool main = winPids.count(kv.first) || par == procs.end() || IsLauncherShell(par->second.path);
            if (main && (best.empty() || d.size() < bestLen)) { best = p; bestLen = d.size(); }
        }
        return best.empty() ? root : best;
    }

    std::vector<App*> Rows() {
        std::vector<App*> v;
        for (auto& kv : apps) if (kv.second.listed) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](App* a, App* b) {
            return std::max(a->curUp, a->curDown) > std::max(b->curUp, b->curDown);
        });
        if (v.size() > 8) v.resize(8);
        return v;
    }

    void Tick(HWND self) {
        tick++;
        DWORD now = GetTickCount();
        double dt = lastMs ? (now - lastMs) / 1000.0 : 1.0;
        if (dt <= 0.05) dt = 1.0;
        lastMs = now;

        unsigned long long in = 0, out = 0;
        double totUp = 0, totDown = 0;
        if (TotalOctets(in, out)) {
            if (lastIn) { totUp = (out - lastOut) / dt; totDown = (in - lastIn) / dt; }
            lastIn = in; lastOut = out;
        }

        bool anyBlocked = false, anyListed = false;
        for (auto& kv : apps) { anyBlocked |= kv.second.blocked; anyListed |= kv.second.listed; }

        // 활동 게이트: 조용하면 ETW를 꺼서 유휴 시 부하 0
        bool active = totUp >= 6 * 1024 || totDown >= 200 * 1024 || anyBlocked;
        if (active) lastActive = tick;
        // ETW 세션이 바깥에서 멈춰졌으면(다른 프로그램이 같은 이름 세션을 정지, logman stop 등) 받는 스레드가 끝나 있음
        // → 정리하고 아래에서 다시 시작. 안 그러면 앱별 집계가 끊긴 채 출처가 계속 "불분명"이 됨.
        if (etw.running && etw.workerDone) { etw.Stop(); etwRestarts++; }
        if (active && !etw.running) etw.Start();
        else if (!active && etw.running && tick - lastActive > 60) etw.Stop();

        std::unordered_map<DWORD, std::pair<unsigned long long, unsigned long long>> raw;
        std::unordered_map<DWORD, unsigned long long> ports;
        etw.Drain(raw, ports);
        unsigned long long unresolvedPort = 0;
        ResolvePorts(ports, raw, unresolvedPort);  // PID 없는 송신 → 소켓 주인

        if (!raw.empty() || anyListed) SnapshotProcs(procs);

        // pid 트래픽을 실행파일 경로별로 합산
        std::map<std::wstring, std::pair<double, double>> rate;
        for (auto& kv : raw) {
            auto it = procs.find(kv.first);
            std::wstring key = (it != procs.end()) ? it->second.path : PathOfPid(kv.first);
            if (key.empty()) continue;
            auto& r = rate[key];
            r.first += kv.second.first / dt;
            r.second += kv.second.second / dt;
        }

        for (auto& kv : rate)
            if (kv.second.first >= 1536 || kv.second.second >= 64 * 1024) Get(kv.first);

        // 창 있는 앱(작업표시줄) 목록: 업로드 출처를 앱으로 묶을 때만 필요 → 업로드 이벤트가 있을 때만 읽음
        std::set<DWORD> winPids;
        std::set<std::wstring> winKeys;
        if (!raw.empty()) {
            winPids = VisibleWindowPids(self);
            for (DWORD p : winPids) {
                auto it = procs.find(p);
                if (it != procs.end()) winKeys.insert(it->second.path);
            }
        }

        // 전체 업로드 중 앱별로 귀속 안 된 분(UDP·서비스 프로세스 등 ETW가 PID를 못 붙인 트래픽)
        // → "알 수 없는 업로드"로 표시. 크롬 원격·WebRTC처럼 앱 특정이 안 되는 경우를 덮음.
        {
            double sumUp = 0;
            for (auto& kv : rate) sumUp += kv.second.first;
            double unattr = totUp - sumUp;
            if (unattr >= 64 * 1024) { rate[L"__unknown__"].first = unattr; Get(L"__unknown__"); }
        }

        // 업로드 출처: ETW로 실제 송신이 확인된 프로세스만 출처별로 묶음. 전체 업로드 중 어느 프로세스에도
        // 귀속되지 않은 몫(헤더·ACK 몫 제외)은 "불분명". 순간 출렁임에 이름이 바뀌지 않게 몇 초 평활.
        // ETW 이벤트는 인터페이스 카운터보다 최대 1초 늦게 오므로, 귀속 안 된 몫은 최근 2초 합으로 따짐
        // (짧은 업로드가 첫 1초 동안 "불분명"으로 잡히지 않게).
        {
            std::map<std::wstring, double> cur;
            std::vector<std::pair<double, DWORD>> byPid;
            double attributed = 0;
            for (auto& kv : raw) {
                double up = kv.second.first / dt;
                if (up <= 0) continue;
                attributed += up;
                cur[SourceKeyOf(kv.first, winPids, winKeys)] += up;
                byPid.push_back({up, kv.first});
            }
            double rest2 = (totUp + prevTot) - (attributed + prevAttr) * 1.06 - 0.05 * (totDown + prevDown);
            double rest = etw.running && etwTicks >= 2 ? std::max(0.0, std::min(rest2 / 2, totUp))
                                                       : totUp - attributed * 1.06 - 0.05 * totDown;
            prevTot = totUp; prevAttr = attributed; prevDown = totDown;
            etwTicks = etw.running ? etwTicks + 1 : 0;
            if (rest > 0) cur[L""] += rest;
            // 진단용 요약 (로그에만 씀)
            std::sort(byPid.rbegin(), byPid.rend());
            double portAll = 0;
            for (auto& kv : ports) portAll += kv.second;
            wchar_t d[800];
            swprintf(d, 800, L"tot=%.0fKB/s attributed=%.0f noPid=%.0f(unresolved %.0f) rest=%.0f etw=%d err=%lu events=%llu",
                     totUp / 1024, attributed / 1024, portAll / dt / 1024, unresolvedPort / dt / 1024, rest / 1024,
                     etw.running, etw.lastErr, etw.sendEvents);
            srcDiag = d;
            for (size_t i = 0; i < byPid.size() && i < 3; i++) {
                auto pi = procs.find(byPid[i].second);
                swprintf(d, 800, L" | pid %lu %.0fKB/s %ls -> %ls", byPid[i].second, byPid[i].first / 1024,
                         pi != procs.end() ? pi->second.path.c_str() : L"?",
                         SourceKeyOf(byPid[i].second, winPids, winKeys).c_str());
                srcDiag += d;
            }
            for (auto& kv : srcEma) kv.second *= 0.4;
            for (auto& kv : cur) srcEma[kv.first] += 0.6 * kv.second;
            double top = 0;
            srcKey.clear();
            for (auto it = srcEma.begin(); it != srcEma.end();) {
                if (it->second < 512) { it = srcEma.erase(it); continue; }
                if (it->second > top) { top = it->second; srcKey = it->first; }
                ++it;
            }
            if (top < 8 * 1024) srcKey.clear();  // 확인된 업로드가 거의 없으면 이름을 띄우지 않음
            srcTop.clear();
            for (auto& kv : srcEma)
                if (kv.second >= 8 * 1024) srcTop.push_back(kv);  // 확인된 업로드만
            std::sort(srcTop.begin(), srcTop.end(), [](auto& a, auto& b) { return a.second > b.second; });
            if (srcTop.size() > 3) srcTop.resize(3);
        }

        for (auto it = apps.begin(); it != apps.end();) {
            App& a = it->second;
            auto r = rate.find(a.key);
            a.curUp = r != rate.end() ? r->second.first : 0;
            a.curDown = r != rate.end() ? r->second.second : 0;
            a.up.pop_front(); a.up.push_back(a.curUp);
            a.down.pop_front(); a.down.push_back(a.curDown);

            // 네트워크만으론 화면/파일을 구분 못 하므로 업/다운로드는 "업로드 중(네트워크)"으로만 보고 송출로 단정하지 않음.
            bool heavyNow = a.curUp >= 6 * 1024 || a.curDown >= 1024 * 1024;
            a.heavyRun = heavyNow ? a.heavyRun + 1 : 0;
            a.heavy = a.heavyRun >= 3 || (a.heavy && heavyNow);

            // 실제로 네트워크를 쓸 때만 활성. 짧은 유예(버스트로 순간 0 되는 것 방지)
            bool trig = a.heavy;
            if (trig) a.hold = 3; else if (a.hold > 0) a.hold--;
            // 차단한 앱은 활동이 없어도 목록 유지(다시 켜기 위해)
            a.listed = trig || a.hold > 0 || a.blocked;

            // python·node 등 호스트 런처는 명령줄에서 스크립트명을 붙여 정체를 드러냄
            if (a.listed && !a.nameFixed) {
                if (IsHostProcess(Lower(FileStem(a.key)))) {
                    for (auto& kv : procs)
                        if (kv.second.path == a.key) {
                            std::wstring s = ScriptName(CommandLineOfPid(kv.first));
                            if (!s.empty()) a.name = DisplayName(a.key) + L" · " + s;
                            break;
                        }
                }
                a.nameFixed = true;
            }

            if (!a.listed) it = apps.erase(it); else ++it;  // 비활성이고 차단도 아니면 즉시 제거
        }

        // 메인 UI용 통합 상태 갱신
        stDown = totDown;
        // 전송 판정: 앱 귀속과 무관하게 PC 전체 업로드로 본다(서비스·UDP 트래픽 포함).
        // 큰 다운로드에 딸려 나가는 ACK(실측 다운로드의 3.5~3.9%)는 5%를 빼서 제외.
        stUpBig = judge.Feed(totUp - 0.05 * totDown, sens);
        stUp = judge.avg;  // 위젯 표시 속도 = 판정에 쓰는 평균
        etw.Flush();  // 이번 초의 이벤트를 다음 틱 전에 받도록
    }

    void ToggleBlock(App& a) {
        if (a.blocked) {
            FwBlockRemove(a.key);
            a.blocked = false;
            return;
        }
        if (a.key == L"system" || a.key.find(L'\\') == std::wstring::npos) return;
        if (FwBlockAdd(a.key)) {
            a.blocked = true;
            std::set<DWORD> pids;
            for (auto& kv : procs) if (kv.second.path == a.key) pids.insert(kv.first);
            DropEstablishedTcp(pids);
        }
    }
};
