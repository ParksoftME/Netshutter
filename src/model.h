// 감지 결과 → 화면에 보일 앱 목록으로 가공. 상태 판정(트레이 송출, 영상/대용량 분류) 포함.
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
    bool mic = false, cam = false, heavy = false;
    bool tray = false, blocked = false, router = false, listed = false, nameFixed = false;
    int heavyRun = 0, hold = 0;

    bool Sending() const { return mic || cam; }       // 확실한 사생활 송출
    bool Red() const { return mic || cam; }            // 마이크·카메라 = 빨강
};

// 오디오 중계 앱: 창 없이 마이크 상시 점유가 정상이라 트레이 경고 제외
inline bool IsAudioRouter(const std::wstring& path) {
    std::wstring f = Lower(FileStem(path));
    const wchar_t* list[] = {L"voicemeeter", L"nvidia broadcast", L"nvbroadcast",
                             L"wavelink", L"wave link", L"krisp", L"audiodg", L"rtx voice"};
    for (auto s : list) if (f.find(s) != std::wstring::npos) return true;
    return false;
}

// 알려진 방송·화상 앱: 고업로드를 "화면 송출"로 볼 근거. 그 외는 그냥 대용량 업로드.
inline bool IsStreamApp(const std::wstring& path) {
    std::wstring f = Lower(FileStem(path));
    const wchar_t* list[] = {L"obs", L"obs64", L"obs32", L"streamlabs", L"xsplit", L"prism",
                             L"zoom", L"teams", L"ms-teams", L"discord", L"webex", L"skype",
                             L"slack", L"gotomeeting", L"whereby"};
    for (auto s : list) if (f.find(s) != std::wstring::npos) return true;
    return false;
}

struct Model {
    std::map<std::wstring, App> apps;
    std::unordered_map<DWORD, ProcInfo> procs;
    EtwCounter etw;
    unsigned long long lastIn = 0, lastOut = 0;
    DWORD lastMs = 0;
    int tick = 0, lastActive = -1000;
    int micMuted = -1;
    // 메인 UI용 통합 상태: 유출 중이냐 / 외부 전송 없음이냐
    bool stMic = false, stCam = false, stUpBig = false;
    double stUp = 0, stDown = 0;
    std::deque<double> upHist;  // 최근 4초 실효 업로드(바이트/초)
    bool AudioLeak() const { return stMic; }                 // 마이크 사용 = 음성 송출 (확실)
    bool VideoLeak() const { return stCam || stUpBig; }      // 카메라(확실) 또는 영상급 업로드(화면 추정)
    bool AnyLeak() const { return AudioLeak() || VideoLeak(); }

    App& Get(const std::wstring& key) {
        auto& a = apps[key];
        if (a.key.empty()) {
            a.key = key;
            a.name = (key == L"__unknown__") ? L"알 수 없는 업로드" : DisplayName(key);
            a.router = IsAudioRouter(key);
            a.up.assign(kHist, 0);
            a.down.assign(kHist, 0);
        }
        return a;
    }
    std::vector<App*> Rows() {
        std::vector<App*> v;
        for (auto& kv : apps) if (kv.second.listed) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](App* a, App* b) {
            if (a->tray != b->tray) return a->tray;       // 트레이 송출 최상단
            if (a->Red() != b->Red()) return a->Red();    // 송출성 먼저
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

        std::set<std::wstring> micP, micK, camP, camK;
        ReadConsent(HKEY_CURRENT_USER, L"microphone", micP, micK);
        ReadConsent(HKEY_LOCAL_MACHINE, L"microphone", micP, micK);
        MicCapturePaths(micP);  // 기록이 안 남는 빌드 대비: 실제 녹음 세션
        ReadConsent(HKEY_CURRENT_USER, L"webcam", camP, camK);
        ReadConsent(HKEY_LOCAL_MACHINE, L"webcam", camP, camK);
        micMuted = MicMuteState();

        bool anyBlocked = false, anyListed = false;
        for (auto& kv : apps) { anyBlocked |= kv.second.blocked; anyListed |= kv.second.listed; }

        // 활동 게이트: 조용하면 ETW를 꺼서 유휴 시 부하 0
        bool active = totUp >= 6 * 1024 || totDown >= 200 * 1024 ||
                      !micP.empty() || !micK.empty() || !camP.empty() || !camK.empty() || anyBlocked;
        if (active) lastActive = tick;
        if (active && !etw.running) etw.Start();
        else if (!active && etw.running && tick - lastActive > 60) etw.Stop();

        std::unordered_map<DWORD, std::pair<unsigned long long, unsigned long long>> raw;
        etw.Drain(raw);

        bool wantPkg = !micK.empty() || !camK.empty();
        if (!raw.empty() || wantPkg || !micP.empty() || !camP.empty() || anyListed)
            SnapshotProcs(procs, wantPkg);

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

        // 마이크·카메라 키 집합 (패키지는 실행 중 프로세스에서 경로로 환원)
        std::set<std::wstring> micKeys(micP), camKeys(camP);
        if (wantPkg) {
            for (auto& kv : procs) {
                if (kv.second.pkg.empty()) continue;
                if (micK.count(kv.second.pkg)) micKeys.insert(kv.second.path);
                if (camK.count(kv.second.pkg)) camKeys.insert(kv.second.path);
            }
        }
        for (auto& k : micKeys) Get(k);
        for (auto& k : camKeys) Get(k);
        for (auto& kv : rate)
            if (kv.second.first >= 1536 || kv.second.second >= 64 * 1024) Get(kv.first);

        auto winPids = VisibleWindowPids(self);
        std::set<std::wstring> winKeys, sess0, live;
        for (DWORD p : winPids) {
            auto it = procs.find(p);
            if (it != procs.end()) winKeys.insert(it->second.path);
        }
        for (auto& kv : procs) {
            live.insert(kv.second.path);
            if (kv.second.session == 0) sess0.insert(kv.second.path);
        }

        // 전체 업로드 중 앱별로 귀속 안 된 분(UDP·서비스 프로세스 등 ETW가 PID를 못 붙인 트래픽)
        // → "알 수 없는 업로드"로 표시. 크롬 원격·WebRTC처럼 앱 특정이 안 되는 경우를 덮음.
        {
            double sumUp = 0;
            for (auto& kv : rate) sumUp += kv.second.first;
            double unattr = totUp - sumUp;
            if (unattr >= 64 * 1024) { rate[L"__unknown__"].first = unattr; Get(L"__unknown__"); }
        }

        for (auto it = apps.begin(); it != apps.end();) {
            App& a = it->second;
            auto r = rate.find(a.key);
            a.curUp = r != rate.end() ? r->second.first : 0;
            a.curDown = r != rate.end() ? r->second.second : 0;
            a.up.pop_front(); a.up.push_back(a.curUp);
            a.down.pop_front(); a.down.push_back(a.curDown);
            a.mic = micKeys.count(a.key) > 0;
            a.cam = camKeys.count(a.key) > 0;

            // 확실한 신호는 마이크·카메라뿐. 네트워크만으론 화면/파일을 구분 못 하므로
            // 그 외 업/다운로드는 전부 "업로드 중(네트워크)"으로만 표시하고 송출로 단정하지 않음.
            bool heavyNow = a.curUp >= 6 * 1024 || a.curDown >= 1024 * 1024;
            a.heavyRun = heavyNow ? a.heavyRun + 1 : 0;
            a.heavy = (a.heavyRun >= 3 || (a.heavy && heavyNow)) && !a.mic && !a.cam;

            // 실제로 마이크/카메라/네트워크를 쓸 때만 활성. 짧은 유예(버스트로 순간 0 되는 것 방지)
            bool trig = a.mic || a.cam || a.heavy;
            if (trig) a.hold = 3; else if (a.hold > 0) a.hold--;
            // 차단한 앱은 활동이 없어도 목록 유지(다시 켜기 위해)
            a.listed = trig || a.hold > 0 || a.blocked;

            // 트레이 송출: 송출 중인데 보이는 창도, 서비스 세션도 아님
            a.tray = a.Sending() && !a.router && live.count(a.key) &&
                     !winKeys.count(a.key) && !sess0.count(a.key) && a.key != L"system";

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
        stMic = !micKeys.empty();
        stCam = !camKeys.empty();
        stUp = totUp; stDown = totDown;
        // 전송 판정: 앱 귀속과 무관하게 PC 전체 업로드로 본다(서비스·UDP 트래픽 포함).
        // 큰 다운로드에 딸려 나가는 ACK(실측 다운로드의 3.5~3.9%)는 5%를 빼서 제외.
        // 화면 공유는 비트레이트가 출렁이므로 "최근 4초 중 3초 이상 활동 + 평균"으로 켜고,
        // 3초 연속 조용해야 끔(짧은 튐에 켜지지 않고, 순간 하락에 깜빡이지 않게).
        const double kActive = 20 * 1024, kEnterAvg = 32 * 1024;  // 160kbps / 256kbps
        double eff = totUp - 0.05 * totDown;
        upHist.push_back(eff > 0 ? eff : 0);
        if (upHist.size() > 4) upHist.pop_front();
        int upSecs = 0, quietTail = 0;
        double sum = 0;
        for (double v : upHist) { sum += v; if (v >= kActive) upSecs++; }
        for (auto it = upHist.rbegin(); it != upHist.rend() && *it < kActive; ++it) quietTail++;
        if (!stUpBig) stUpBig = upHist.size() >= 3 && upSecs >= 3 && sum / upHist.size() >= kEnterAvg;
        else if (quietTail >= 3) stUpBig = false;
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
