// NetShutter — 어디로 새고 있나. 컴팩트 목록 위젯.
// KGNF Games · MIT License. 관리자 권한으로 실행(앱별 전송량·방화벽 차단에 필요).
#include "common.h"
#include "model.h"
#include "autostart.h"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <winver.h>
#include <vector>
#include <string>
#include <cmath>

using namespace Gdiplus;

// ── 전역 상태 ──
static Model g;
static HWND gWnd = nullptr;
static NOTIFYICONDATAW gTray{};
static ULONG_PTR gGdip = 0;
static float gDpi = 1.f;
static int gHoverRow = -1;      // 마우스가 올라간 줄
static int gHoverBtn = -1;      // -1=없음, 0=마이크, 1=네트워크
static bool gBlinkOn = false;   // 트레이 송출 깜빡임 위상
static bool gAlwaysTop = false; // 항상 위 (기본 끔)
static bool gHideCapture = false; // 방송 화면에서 숨기기 (기본 끔)
static bool gMenuOpen = false;  // 메뉴 떠 있는 동안 z-순서 고정
static bool gWinHover = false;  // 위젯에 마우스가 올라와 있음 (헤더 표시 + 톤 밝기)
static bool gDragging = false;
static DWORD gStartTick = 0;    // 시작 직후 잠깐 앞에 띄워 위치를 알게 함
static bool gCloseToTray = true; // X 누르면 트레이로(기본). 끄면 완전 종료
static const wchar_t* kSiteUrl = L"https://kgnfgames.com";  // 홈페이지·후원 (임시로 둘 다 회사 사이트)
static int gLang = 0;  // 0 = 한국어, 1 = English
static const wchar_t* T(const wchar_t* ko, const wchar_t* en) { return gLang == 0 ? ko : en; }

// 버전: exe에 들어 있는 버전 리소스(netmeter.rc의 PRODUCTVERSION)에서 읽어 "v주.부.수" 세 자리로.
// 릴리스 때 rc 값만 바꾸면 화면에도 그대로 나옴. 읽기 실패 시 빈 문자열.
static const std::wstring& AppVersion() {
    static std::wstring v;
    static bool done = false;
    if (done) return v;
    done = true;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /*RT_VERSION*/);
    HGLOBAL hg = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = hg ? LockResource(hg) : nullptr;
    if (!p) return v;
    std::vector<BYTE> copy((const BYTE*)p, (const BYTE*)p + SizeofResource(nullptr, r));  // VerQueryValue는 쓰기 가능한 사본 필요
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT n = 0;
    if (VerQueryValueW(copy.data(), L"\\", (void**)&fi, &n) && fi && n >= sizeof(*fi)) {
        wchar_t buf[32];
        swprintf(buf, 32, L"v%u.%u.%u", HIWORD(fi->dwProductVersionMS), LOWORD(fi->dwProductVersionMS),
                 HIWORD(fi->dwProductVersionLS));
        v = buf;
    }
    return v;
}
static POINT gDragStart{}, gWinStart{};
static DWORD gFxStart = 0;      // 위치 알림 효과 시작 시각 (0 = 효과 없음)

// 히트 영역 (줄/버튼 클릭 판정)
struct Hit { RectF rc; int row; int btn; std::wstring key; };
static std::vector<Hit> gHits;

// ── 디자인 토큰 (캔버스 시안과 동일 계열) ──
static const float ROWH = 22, PADW = 4, RAD = 9, HEADERH = 18;
static const float BODYW = 260, BODYH = 64;  // 고정 본문 크기 (헤더 자리는 그 위)

// 셔터(조리개) 로고: assets/icon/netshutter.svg 를 64 단위 좌표 그대로 옮김
static void DrawLogo(Gdiplus::Graphics& gr, float x, float y, float s) {
    using namespace Gdiplus;
    const Color orange(255, 0xE8, 0x57, 0x2A), white(255, 255, 255, 255);
    GraphicsState st = gr.Save();
    gr.SetPixelOffsetMode(PixelOffsetModeHalf);  // SVG와 같은 픽셀 중심 기준
    gr.TranslateTransform(x, y);
    gr.ScaleTransform(s / 64.f, s / 64.f);

    GraphicsPath box;  // rect 64×64, rx 15
    box.AddArc(0.f, 0.f, 30.f, 30.f, 180, 90);
    box.AddArc(34.f, 0.f, 30.f, 30.f, 270, 90);
    box.AddArc(34.f, 34.f, 30.f, 30.f, 0, 90);
    box.AddArc(0.f, 34.f, 30.f, 30.f, 90, 90);
    box.CloseFigure();
    SolidBrush ob(orange), wb(white);
    gr.FillPath(&ob, &box);
    gr.FillEllipse(&wb, 32 - 15.5f, 32 - 15.5f, 31.f, 31.f);

    // 조리개 날: r18 원으로 클립, 중심 기준 15° 회전
    GraphicsPath clipL; clipL.AddEllipse(32 - 18.f, 32 - 18.f, 36.f, 36.f);
    gr.SetClip(&clipL);
    GraphicsState rs = gr.Save();
    gr.TranslateTransform(32, 32); gr.RotateTransform(15); gr.TranslateTransform(-32, -32);
    Pen blade(orange, 3.2f);
    blade.SetStartCap(LineCapRound); blade.SetEndCap(LineCapRound);
    static const float L[6][4] = {
        {50, 32, 34.6f, 35.1f}, {41, 47.6f, 30.6f, 35.8f}, {23, 47.6f, 28.1f, 32.7f},
        {14, 32, 29.4f, 28.9f}, {23, 16.4f, 33.4f, 28.2f}, {41, 16.4f, 35.9f, 31.3f}};
    for (auto& l : L) gr.DrawLine(&blade, l[0], l[1], l[2], l[3]);
    gr.Restore(rs);
    gr.ResetClip();

    Pen ring(white, 5.5f);
    gr.DrawEllipse(&ring, 32 - 20.f, 32 - 20.f, 40.f, 40.f);

    // 번개: r15.5 안에서 흰 테두리(6) → 주황 채움
    static const PointF B[] = {{34.5f, 19}, {25, 33.5f}, {31, 33.5f}, {28.5f, 45}, {39, 30}, {33, 30}};
    GraphicsPath bolt; bolt.AddPolygon(B, 6);
    GraphicsPath clipH; clipH.AddEllipse(32 - 15.5f, 32 - 15.5f, 31.f, 31.f);
    gr.SetClip(&clipH);
    Pen halo(white, 6.f); halo.SetLineJoin(LineJoinRound);
    gr.FillPath(&wb, &bolt);
    gr.DrawPath(&halo, &bolt);
    gr.ResetClip();
    gr.FillPath(&ob, &bolt);

    gr.Restore(st);
}
static Color Col(BYTE r, BYTE gg, BYTE b, BYTE a = 255) { return Color(a, r, gg, b); }
static const Color cBg = Col(28, 28, 30, 244), cBorder = Col(255, 255, 255, 24),
                   cFg = Col(242, 242, 242), cDim = Col(142, 142, 147), cOff = Col(96, 96, 100),
                   cRed = Col(255, 107, 74), cYel = Col(255, 212, 59), cBlu = Col(143, 211, 255),
                   cHover = Col(255, 255, 255, 20), cBtnBg = Col(255, 255, 255, 28);
// 상태 색: 배경은 항상 같은 어두운 색, 상태는 막대·아이콘·글자색으로만
static const Color cPanel = Col(24, 32, 27), cFrame = Col(40, 46, 43),
                   cGreen = Col(74, 200, 120), cGreenText = Col(120, 220, 150),
                   cOrange = Col(255, 150, 50), cOrangeText = Col(255, 190, 125),
                   cRedA = Col(235, 64, 52), cRedText = Col(255, 140, 128);

static int Sx(float v) { return (int)(v * gDpi + 0.5f); }

static std::wstring FmtRate(double bps) {
    double bits = bps * 8;
    wchar_t buf[32];
    if (bits >= 1e6) swprintf(buf, 32, L"%.1fM", bits / 1e6);
    else if (bits >= 1e3) swprintf(buf, 32, L"%.0fK", bits / 1e3);
    else swprintf(buf, 32, L"%.0f", bits);
    return buf;
}

static void RoundRect(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

// 12×12 그리드 아이콘을 (x,y) size px 로 그림
enum IconId { ICmic, ICmicOff, ICcam, ICup, ICnet, ICnetOff, IClock, ICtray, ICscreenUp };
static void Icon(Graphics& gr, IconId id, float x, float y, float size, Color c, float sw = 1.4f) {
    float k = size / 12.f;
    Pen pen(c, sw * k);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    auto L = [&](float x1, float y1, float x2, float y2) { gr.DrawLine(&pen, x + x1 * k, y + y1 * k, x + x2 * k, y + y2 * k); };
    auto Arc = [&](float ax, float ay, float aw, float ah, float s, float e) { gr.DrawArc(&pen, x + ax * k, y + ay * k, aw * k, ah * k, s, e); };
    auto RR = [&](float ax, float ay, float aw, float ah) {
        GraphicsPath p; RoundRect(p, RectF(x + ax * k, y + ay * k, aw * k, ah * k), 1.0f * k); gr.DrawPath(&pen, &p);
    };
    switch (id) {
    case ICmic: case ICmicOff:
        RR(4.2f, 1, 3.6f, 6); Arc(2.5f, 3.3f, 7, 5, 20, 140); L(6, 9.3f, 6, 11);
        if (id == ICmicOff) L(1.5f, 1.5f, 10.5f, 10.5f);
        break;
    case ICcam:  // 카메라: 몸체 + 렌즈
        RR(1, 2.5f, 10, 7);
        gr.DrawEllipse(&pen, x + 4 * k, y + 4 * k, 4 * k, 4 * k);
        break;
    case ICup:   // 업로드: 위 화살표
        L(6, 10.5f, 6, 2); L(2.5f, 5.5f, 6, 2); L(6, 2, 9.5f, 5.5f);
        break;
    case ICnet: case ICnetOff:
        L(3.5f, 10, 3.5f, 2); L(1.5f, 4, 3.5f, 2); L(3.5f, 2, 5.5f, 4);
        L(8.5f, 2, 8.5f, 10); L(6.5f, 8, 8.5f, 10); L(8.5f, 10, 10.5f, 8);
        if (id == ICnetOff) L(1, 11, 11, 1);
        break;
    case IClock: RR(2.5f, 5.5f, 7, 5); Arc(4, 2, 4, 5, 180, 180); break;
    case ICtray: RR(1.5f, 2.5f, 9, 7); L(1.5f, 7.5f, 4, 7.5f); L(5, 9, 7, 9); L(8, 7.5f, 10.5f, 7.5f); break;
    case ICscreenUp:  // 모니터 + 위 화살표
        RR(0.8f, 1.2f, 10.4f, 7.3f); L(4, 11, 8, 11); L(6, 8.5f, 6, 11);
        L(6, 6.8f, 6, 3.2f); L(4.4f, 4.8f, 6, 3.2f); L(6, 3.2f, 7.6f, 4.8f);
        break;
    }
}

static void DrawSpark(Graphics& gr, RectF box, const std::deque<double>& vals, Color c) {
    double mx = 1;
    for (double v : vals) mx = v > mx ? v : mx;
    int n = (int)vals.size();
    if (n < 2) return;
    std::vector<PointF> pts(n);
    for (int i = 0; i < n; i++) {
        float fx = box.X + box.Width * i / (n - 1);
        float fy = box.Y + box.Height * (1.f - (float)(vals[i] / mx));
        pts[i] = PointF(fx, fy);
    }
    Pen pen(Color(c.GetA() * 9 / 10, c.GetR(), c.GetG(), c.GetB()), 1.2f * gDpi);
    pen.SetLineJoin(LineJoinRound);
    gr.DrawLines(&pen, pts.data(), n);
}

// ── 창 크기 고정: 위 헤더 자리 + 본문. 상태가 바뀌어도 크기·위치는 그대로 ──
// 헤더 자리는 평소 창 리전에서 빼서 완전 투명 + 클릭 통과, 호버 때만 리전에 포함.
static int WinW() { return Sx(BODYW); }
static int WinH() { return Sx(HEADERH + BODYH); }
static void Relayout() {
    bool leak = g.AnyLeak();
    bool startup = gStartTick && (GetTickCount() - gStartTick < 6000);  // 시작 후 6초간 앞에
    HWND z = (gAlwaysTop || leak || startup) ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (!gMenuOpen)  // 메뉴 중엔 위젯을 다시 위로 올리지 않음
        SetWindowPos(gWnd, z, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    static int rgnHover = -1;  // 리전은 호버 상태가 바뀔 때만 다시 만듦
    if (rgnHover == (int)gWinHover) return;
    rgnHover = gWinHover;
    int w = WinW(), h = WinH(), top = gWinHover ? 0 : Sx(HEADERH);
    HRGN rgn = CreateRoundRectRgn(0, top, w + 1, h + 1, Sx(RAD) * 2, Sx(RAD) * 2);
    SetWindowRgn(gWnd, rgn, TRUE);  // 둥근 모서리
}

// ── 위치 보정: 연결된 모니터 작업 영역 안에 절반 이상 보여야 유효 ──
static bool OnScreen(const RECT& r) {
    HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!m) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(m, &mi)) return false;
    RECT i;
    if (!IntersectRect(&i, &r, &mi.rcWork)) return false;
    return (i.right - i.left) * 2 >= (r.right - r.left) && (i.bottom - i.top) * 2 >= (r.bottom - r.top);
}
static POINT DefaultPos() {  // 주 모니터 우하단
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    return { wa.right - WinW() - Sx(12), wa.bottom - WinH() - Sx(12) };
}
static void EnsureOnScreen() {  // 모니터 분리·해상도 변경 후에도 화면 밖에 남지 않게
    RECT r; GetWindowRect(gWnd, &r);
    if (OnScreen(r)) return;
    POINT p = DefaultPos();
    SetWindowPos(gWnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ── 위치 알림 효과 (트레이 클릭): 테두리 번쩍임 + 바깥 글로우, 약 1초 ──
// 글로우는 위젯 바로 아래에 깔리는 임시 투명 창(클릭 통과)에 그리고, 끝나면 창·타이머·비트맵 모두 정리.
static const DWORD kFxMs = 1000;
static const UINT kFxTimer = 3;
static HWND gGlow = nullptr;
static HDC gGlowDC = nullptr;
static HBITMAP gGlowBmp = nullptr, gGlowOld = nullptr;
static void* gGlowBits = nullptr;
static int gGlowW = 0, gGlowH = 0;
static float FxT() { return gFxStart ? (GetTickCount() - gFxStart) / (float)kFxMs : 1.f; }
static float LocateFxBorder() {  // 3번 번쩍이며 잦아듦 (0~1)
    float t = FxT();
    if (t >= 1.f) return 0.f;
    float s = fabsf(sinf(3.f * 3.14159265f * t));
    return powf(s, 0.8f) * (1.f - 0.35f * t);
}
static int GlowMargin() { return Sx(26); }
static void EndLocateFx() {
    if (gWnd) KillTimer(gWnd, kFxTimer);
    gFxStart = 0;
    if (gGlow) { DestroyWindow(gGlow); gGlow = nullptr; }
    if (gGlowDC) { SelectObject(gGlowDC, gGlowOld); DeleteDC(gGlowDC); gGlowDC = nullptr; }
    if (gGlowBmp) { DeleteObject(gGlowBmp); gGlowBmp = nullptr; gGlowBits = nullptr; }
    if (gWnd) InvalidateRect(gWnd, nullptr, FALSE);
}
static void LocateFxFrame() {
    float t = FxT();
    if (t >= 1.f) { EndLocateFx(); return; }
    InvalidateRect(gWnd, nullptr, FALSE);  // 테두리
    if (!gGlow) return;
    // 글로우: 위젯의 보이는 모양(평소 본문, 호버 시 전체) 바깥으로 퍼졌다가 사라짐
    int m = GlowMargin();
    float u = std::min(1.f, t / 0.55f), grow = 1.f - (1.f - u) * (1.f - u) * (1.f - u);
    float spread = m * (0.3f + 0.7f * grow);
    float a = (t < 0.12f ? t / 0.12f : std::max(0.f, 1.f - (t - 0.12f) / 0.8f)) * (0.55f + 0.45f * LocateFxBorder());
    float top = gWinHover ? 0.f : (float)Sx(HEADERH);
    RectF vis((float)m, m + top, (float)WinW(), WinH() - top);
    {
        Bitmap bm(gGlowW, gGlowH, gGlowW * 4, PixelFormat32bppPARGB, (BYTE*)gGlowBits);
        Graphics gr(&bm);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.Clear(Color(0, 0, 0, 0));
        const int N = 10;
        for (int k = N; k >= 1; k--) {  // 겹친 둥근 사각형 = 부드러운 감쇠
            float e = spread * k / N;
            GraphicsPath p;
            RoundRect(p, RectF(vis.X - e, vis.Y - e, vis.Width + e * 2, vis.Height + e * 2), Sx(RAD) + e);
            SolidBrush b(Color((BYTE)(255 * a * 0.16f), 0xE8, 0x57, 0x2A));
            gr.FillPath(&b, &p);
        }
    }
    RECT wr; GetWindowRect(gWnd, &wr);
    POINT dst{wr.left - m, wr.top - m}, src{0, 0};
    SIZE sz{gGlowW, gGlowH};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(gGlow, nullptr, &dst, &sz, gGlowDC, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(gGlow, gWnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // 위젯 바로 아래
}
static void StartLocateFx() {
    gFxStart = GetTickCount();
    if (!gGlow) {
        static bool reg = false;
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(gWnd, GWLP_HINSTANCE);
        if (!reg) {
            WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = hi; wc.lpszClassName = L"KGNF_NetShutter_Glow";
            reg = RegisterClassW(&wc) != 0;
        }
        int m = GlowMargin();
        gGlowW = WinW() + m * 2; gGlowH = WinH() + m * 2;
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), gGlowW, -gGlowH, 1, 32, BI_RGB};
        gGlowDC = CreateCompatibleDC(nullptr);
        gGlowBmp = CreateDIBSection(gGlowDC, &bi, DIB_RGB_COLORS, &gGlowBits, nullptr, 0);
        if (gGlowBmp) {
            gGlowOld = (HBITMAP)SelectObject(gGlowDC, gGlowBmp);
            gGlow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    L"KGNF_NetShutter_Glow", L"", WS_POPUP, 0, 0, gGlowW, gGlowH,
                                    nullptr, nullptr, hi, nullptr);
        }
        if (gGlow) {
            if (gHideCapture) SetWindowDisplayAffinity(gGlow, WDA_EXCLUDEFROMCAPTURE);
            LocateFxFrame();
            ShowWindow(gGlow, SW_SHOWNOACTIVATE);
        }
    }
    SetTimer(gWnd, kFxTimer, 15, nullptr);
    LocateFxFrame();
}

// ── 렌더 ──
static std::wstring FmtBytesRate(double bps) {  // 바이트/초 → "1.8 MB/s"
    wchar_t buf[32];
    if (bps >= 1024.0 * 1024) swprintf(buf, 32, L"%.1f MB/s", bps / (1024.0 * 1024));
    else if (bps >= 1024) swprintf(buf, 32, L"%.0f KB/s", bps / 1024);
    else swprintf(buf, 32, L"%.0f B/s", bps);
    return buf;
}
static std::wstring AppNames(bool mic) {  // 마이크/카메라 사용 중인 앱 이름들
    std::wstring s;
    for (auto& kv : g.apps) {
        const App& a = kv.second;
        if (!(mic ? a.mic : a.cam)) continue;
        if (!s.empty()) s += L", ";
        s += a.name;
    }
    return s.empty() ? std::wstring(T(L"알 수 없는 앱", L"Unknown app")) : s;
}

static void Paint() {
    RECT rc; GetClientRect(gWnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    HDC screen = GetDC(gWnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    {
        Graphics gr(mem);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        gr.Clear(Color(255, 20, 20, 22));  // 창 영역은 둥근 리전으로 잘림

        float top = (float)Sx(HEADERH);  // 본문 시작
        RectF body(0.5f, top + 0.5f, W - 1.f, H - top - 1.f);
        SolidBrush bg(cPanel);

        // 호버: 헤더 자리까지 프레임이 씌워짐 (본문은 제자리)
        if (gWinHover) {
            GraphicsPath whole;
            RoundRect(whole, RectF(0.5f, 0.5f, W - 1.f, H - 1.f), (float)Sx(RAD));
            SolidBrush fb(cFrame);
            gr.FillPath(&fb, &whole);
        }
        GraphicsPath frame;
        RoundRect(frame, body, (float)Sx(RAD));
        gr.FillPath(&bg, &frame);
        Pen bord(gWinHover ? Col(255, 255, 255, 90) : cBorder, 1.0f);
        gr.DrawPath(&bord, &frame);
        if (float fi = LocateFxBorder(); fi > 0.01f) {  // 위치 알림: 주황 테두리 번쩍임
            GraphicsPath inner;
            float in = Sx(1.5f);
            RoundRect(inner, RectF(body.X + in, body.Y + in, body.Width - in * 2, body.Height - in * 2), (float)Sx(RAD) - in);
            Pen fp(Col(0xE8, 0x57, 0x2A, (BYTE)(255 * fi)), Sx(2.5f));
            gr.DrawPath(&fp, &inner);
        }
        if (gWinHover) {
            GraphicsPath whole;
            RoundRect(whole, RectF(0.5f, 0.5f, W - 1.f, H - 1.f), (float)Sx(RAD));
            Pen wp(Col(255, 255, 255, 60), 1.0f);
            gr.DrawPath(&wp, &whole);
        }

        FontFamily ff(L"Segoe UI");
        Font fName(&ff, 8.5f * gDpi, FontStyleBold, UnitPixel);
        Font fTitle(&ff, 11.f * gDpi, FontStyleBold, UnitPixel);
        Font fRow(&ff, 9.5f * gDpi, FontStyleBold, UnitPixel);
        Font fSub(&ff, 8.f * gDpi, FontStyleRegular, UnitPixel);
        Font fBtn(&ff, 8.f * gDpi, FontStyleBold, UnitPixel);
        StringFormat sfL; sfL.SetAlignment(StringAlignmentNear); sfL.SetLineAlignment(StringAlignmentCenter);
        sfL.SetFormatFlags(StringFormatFlagsNoWrap); sfL.SetTrimming(StringTrimmingEllipsisCharacter);
        StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);

        gHits.clear();

        // 헤더: 호버 시에만. 로고 · NetShutter · 메뉴 · X. 버튼 밖은 드래그 핸들.
        if (gWinHover) {
            float hh = (float)Sx(HEADERH);
            DrawLogo(gr, Sx(7), (hh - Sx(11)) / 2.f, Sx(11));
            SolidBrush tb(cFg);
            gr.DrawString(L"NetShutter", -1, &fName, RectF((float)Sx(23), 0, (float)Sx(120), hh), &sfL, &tb);
            if (!AppVersion().empty()) {  // 이름 바로 옆 작은 회색 버전
                RectF nb; gr.MeasureString(L"NetShutter", -1, &fName, PointF(0, 0), &nb);
                SolidBrush vb(cDim);
                gr.DrawString(AppVersion().c_str(), -1, &fSub, RectF(Sx(23) + nb.Width, 0, (float)Sx(60), hh), &sfL, &vb);
            }
            float cy = hh / 2.f, r2 = Sx(3.5f);
            // X (트레이로 숨김 또는 종료)
            float bx = (float)(W - Sx(13));
            Pen xp(gHoverBtn == 3 ? cFg : cDim, 1.3f * gDpi);
            xp.SetStartCap(LineCapRound); xp.SetEndCap(LineCapRound);
            gr.DrawLine(&xp, bx - r2, cy - r2, bx + r2, cy + r2);
            gr.DrawLine(&xp, bx + r2, cy - r2, bx - r2, cy + r2);
            gHits.push_back({RectF(bx - Sx(9), 0, (float)Sx(18), hh), -1, 3, L""});
            // 메뉴 (우클릭 메뉴와 같음)
            float mx = (float)(W - Sx(31));
            SolidBrush mb(gHoverBtn == 4 ? cFg : cDim);
            for (int i = -1; i <= 1; i++) {
                float d = Sx(1.0f) * 2.f;
                gr.FillEllipse(&mb, mx + i * Sx(4.5f) - d / 2, cy - d / 2, d, d);
            }
            gHits.push_back({RectF(mx - Sx(9), 0, (float)Sx(18), hh), -1, 4, L""});
        }

        // 상태 줄: 왼쪽 색 막대 + 아이콘 + 제목/부제 + 오른쪽 버튼
        float px = (float)Sx(10);
        auto bar = [&](float y, float h, Color c) {
            GraphicsPath p; RoundRect(p, RectF(px, y, (float)Sx(3), h), (float)Sx(1.5f));
            SolidBrush b(c); gr.FillPath(&b, &p);
        };
        auto button = [&](float cy, const wchar_t* label, Color c, int code, bool off) {
            float bw = Sx(48), bh = Sx(19), bx = W - Sx(10) - bw;
            RectF br(bx, cy - bh / 2, bw, bh);
            GraphicsPath p; RoundRect(p, br, (float)Sx(4));
            Color fill = off ? Col(90, 90, 96) : c;
            if (gHoverBtn == code) fill = Col(fill.GetR(), fill.GetG(), fill.GetB(), 220);
            SolidBrush bb(fill); gr.FillPath(&bb, &p);
            SolidBrush bt(Col(255, 255, 255));
            gr.DrawString(label, -1, &fBtn, br, &sfC, &bt);
            gHits.push_back({br, -1, code, L""});
        };
        // 제목 옆에 작게 붙는 보조 글 (두 줄 모드)
        auto titleWithAside = [&](float y, float h, const std::wstring& title, const std::wstring& aside,
                                  Color tc, float right) {
            float tx = px + Sx(30);
            RectF bound;
            gr.MeasureString(title.c_str(), -1, &fRow, PointF(0, 0), &bound);
            float tw = std::min(bound.Width, right - tx);
            SolidBrush tb(tc);
            gr.DrawString(title.c_str(), -1, &fRow, RectF(tx, y, tw + 1, h), &sfL, &tb);
            if (!aside.empty() && tx + tw + Sx(4) < right) {
                SolidBrush sb(cDim);
                gr.DrawString(aside.c_str(), -1, &fSub, RectF(tx + tw + Sx(2), y, right - tx - tw - Sx(2), h), &sfL, &sb);
            }
        };

        bool video = g.VideoLeak(), audio = g.AudioLeak();
        float right = (float)(W - Sx(64));  // 버튼 앞까지
        float rightNoBtn = (float)(W - Sx(12));  // 버튼 없는 줄(전송)은 끝까지
        if (!video && !audio) {
            // ── 외부 전송 없음: 녹색 체크 (사실만 표시, "안전" 단정 안 함) ──
            float cy = top + Sx(BODYH) / 2.f;
            bar(top + Sx(12), (float)Sx(BODYH - 24), cGreen);
            float ix = px + Sx(9);
            SolidBrush gb(cGreen);
            gr.FillEllipse(&gb, ix, cy - Sx(9), (float)Sx(18), (float)Sx(18));
            Pen ck(Col(20, 34, 24), 2.0f * gDpi);
            ck.SetStartCap(LineCapRound); ck.SetEndCap(LineCapRound); ck.SetLineJoin(LineJoinRound);
            float ex = ix + Sx(9);
            gr.DrawLine(&ck, ex - Sx(4), cy, ex - Sx(1), cy + Sx(3));
            gr.DrawLine(&ck, ex - Sx(1), cy + Sx(3), ex + Sx(4.5f), cy - Sx(3.5f));
            SolidBrush tg(cGreenText), sub(cDim);
            gr.DrawString(T(L"외부 전송 없음", L"No outgoing transfer"), -1, &fTitle,
                          RectF(px + Sx(36), cy - Sx(19), (float)(W - Sx(56)), (float)Sx(20)), &sfL, &tg);
            gr.DrawString(T(L"화면·영상·파일 전송 없음 · 마이크 꺼짐", L"No screen, video or file upload · Mic off"), -1, &fSub,
                          RectF(px + Sx(36), cy + Sx(1), (float)(W - Sx(56)), (float)Sx(16)), &sfL, &sub);
        } else if (video != audio) {
            // ── 한 가지: 큰 한 줄 + 부제 ──
            float cy = top + Sx(BODYH) / 2.f;
            Color c = video ? cOrange : cRedA, tc = video ? cOrangeText : cRedText;
            bar(top + Sx(12), (float)Sx(BODYH - 24), c);
            std::wstring title, sub;
            if (audio) {
                Icon(gr, ICmic, px + Sx(9), cy - Sx(9), Sx(18), c, 1.5f);
                title = T(L"마이크가 켜져 있어요", L"Microphone is on");
                sub = AppNames(true);
            } else if (g.stCam) {
                Icon(gr, ICcam, px + Sx(9), cy - Sx(9), Sx(18), c, 1.5f);
                title = std::wstring(T(L"카메라 켜짐 · ", L"Camera on · ")) + AppNames(false);
                if (g.stUpBig) sub = std::wstring(T(L"업로드 ", L"Upload ")) + FmtBytesRate(g.stUp);
            } else {
                Icon(gr, ICscreenUp, px + Sx(9), cy - Sx(9), Sx(18), c, 1.5f);
                title = T(L"화면·영상·파일 전송 중", L"Screen/video/file upload");
                sub = std::wstring(T(L"업로드 ", L"Upload ")) + FmtBytesRate(g.stUp);
            }
            SolidBrush tb(tc), sb(cDim);
            bool upOnly = video && !g.stCam;  // 전송 줄: 업로드를 멈출 수단이 없으므로 버튼 없음
            float tx = px + Sx(36), tw = (upOnly ? rightNoBtn : right) - tx;
            if (sub.empty()) {
                gr.DrawString(title.c_str(), -1, &fRow, RectF(tx, cy - Sx(10), tw, (float)Sx(20)), &sfL, &tb);
            } else {
                gr.DrawString(title.c_str(), -1, &fRow, RectF(tx, cy - Sx(18), tw, (float)Sx(18)), &sfL, &tb);
                gr.DrawString(sub.c_str(), -1, &fSub, RectF(tx, cy + Sx(1), tw, (float)Sx(16)), &sfL, &sb);
            }
            if (audio) button(cy, g.micMuted == 1 ? T(L"해제", L"Unmute") : T(L"음소거", L"Mute"), c, 11, g.micMuted == 1);
            else if (!upOnly) { bool off = CameraIsBlocked(); button(cy, off ? T(L"해제", L"On") : T(L"끄기", L"Off"), c, 10, off); }
        } else {
            // ── 둘 다: 본문 안에서 두 줄, 줄마다 버튼 ──
            float rh = Sx(BODYH) / 2.f;
            for (int row = 0; row < 2; row++) {
                float y = top + row * rh, cy = y + rh / 2.f;
                bool isVideo = row == 0;
                Color c = isVideo ? cOrange : cRedA, tc = isVideo ? cOrangeText : cRedText;
                bar(y + Sx(6), rh - Sx(12), c);
                std::wstring title, aside;
                IconId ic;
                if (!isVideo) { ic = ICmic; title = T(L"마이크가 켜져 있어요", L"Microphone is on"); aside = AppNames(true); }
                else if (g.stCam) { ic = ICcam; title = std::wstring(T(L"카메라 켜짐 · ", L"Camera on · ")) + AppNames(false); }
                else { ic = ICscreenUp; title = T(L"화면·영상·파일 전송 중", L"Screen/video/file upload"); aside = FmtBytesRate(g.stUp); }
                Icon(gr, ic, px + Sx(10), cy - Sx(7), Sx(14), c, 1.6f);
                bool upRow = isVideo && !g.stCam;  // 전송 줄은 버튼 없음
                titleWithAside(y, rh, title, aside, tc, upRow ? rightNoBtn : right);
                if (isVideo && !upRow) { bool off = CameraIsBlocked(); button(cy, off ? T(L"해제", L"On") : T(L"끄기", L"Off"), c, 10, off); }
                else if (!isVideo) button(cy, g.micMuted == 1 ? T(L"해제", L"Unmute") : T(L"음소거", L"Mute"), c, 11, g.micMuted == 1);
            }
        }
    }
    HDC wdc = GetDC(gWnd);
    BitBlt(wdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    ReleaseDC(gWnd, wdc);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(gWnd, screen);
}

// ── 우클릭 메뉴 ──
static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"NetShutter " + AppVersion()).c_str());  // 누를 수 없는 버전 줄
    AppendMenuW(m, MF_STRING, 7, L"NetShutter — kgnfgames.com");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (gAlwaysTop ? MF_CHECKED : 0), 1, T(L"항상 위에 표시", L"Always on top"));
    AppendMenuW(m, MF_STRING | (gHideCapture ? MF_CHECKED : 0), 2, T(L"화면 공유·녹화에서 숨기기", L"Hide from screen share"));
    AppendMenuW(m, MF_STRING | (AutoStartOn() ? MF_CHECKED : 0), 4, T(L"윈도우 시작 시 실행", L"Run at startup"));
    AppendMenuW(m, MF_STRING | (gCloseToTray ? MF_CHECKED : 0), 8, T(L"닫기(X) 시 트레이로", L"Close (X) to tray"));
    AppendMenuW(m, MF_STRING, 6, gLang == 0 ? L"English" : L"한국어");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, T(L"후원하기", L"Support us"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 9, T(L"종료", L"Exit"));
    POINT pt; GetCursorPos(&pt);
    // 메뉴 떠 있는 동안 위젯을 맨 위에서 내리고, 타이머가 다시 올리지 못하게 고정
    gMenuOpen = true;
    SetWindowPos(gWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(gWnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, gWnd, nullptr);
    DestroyMenu(m);
    gMenuOpen = false;
    switch (cmd) {
    case 1: gAlwaysTop = !gAlwaysTop; CfgSet(L"alwaysTop", gAlwaysTop); break;
    case 2:
        gHideCapture = !gHideCapture;
        CfgSet(L"hideCapture", gHideCapture);
        SetWindowDisplayAffinity(gWnd, gHideCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
        break;
    case 4: AutoStartSet(!AutoStartOn()); break;
    case 5: ShellExecuteW(nullptr, L"open", kSiteUrl, nullptr, nullptr, SW_SHOW); break;
    case 6: gLang = gLang == 0 ? 1 : 0; CfgSet(L"lang", gLang); InvalidateRect(gWnd, nullptr, FALSE); break;
    case 7: ShellExecuteW(nullptr, L"open", kSiteUrl, nullptr, nullptr, SW_SHOW); break;
    case 8: gCloseToTray = !gCloseToTray; CfgSet(L"closeToTray", gCloseToTray); break;
    case 9: DestroyWindow(gWnd); break;
    }
    Relayout();
}

static int HitTest(int mx, int my, int& row) {
    for (auto it = gHits.rbegin(); it != gHits.rend(); ++it) {
        if (mx >= it->rc.X && mx <= it->rc.X + it->rc.Width && my >= it->rc.Y && my <= it->rc.Y + it->rc.Height) {
            row = it->row;
            if (it->btn >= 0) return it->btn;
        }
    }
    // 줄만 맞은 경우
    for (auto it = gHits.rbegin(); it != gHits.rend(); ++it)
        if (it->btn == -1 && mx >= it->rc.X && mx <= it->rc.X + it->rc.Width && my >= it->rc.Y && my <= it->rc.Y + it->rc.Height) {
            row = it->row; return -1;
        }
    row = -1; return -1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        SetTimer(h, 1, 1000, nullptr);        // 1초 폴링
        SetTimer(h, 2, 500, nullptr);         // 깜빡임
        // 방송에 숨기기 옵션이 켜져 있을 때만 캡처 제외
        if (gHideCapture) SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) { g.Tick(h); Relayout(); InvalidateRect(h, nullptr, FALSE); }
        else if (wp == kFxTimer) LocateFxFrame();
        else if (wp == 2) {
            if (g.AnyLeak()) { gBlinkOn = !gBlinkOn; InvalidateRect(h, nullptr, FALSE); }
            else if (gBlinkOn) { gBlinkOn = false; InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); Paint(); EndPaint(h, &ps); return 0; }
    case WM_MOUSEMOVE: {
        if (gDragging) {
            POINT pt; GetCursorPos(&pt);
            SetWindowPos(h, nullptr, gWinStart.x + (pt.x - gDragStart.x), gWinStart.y + (pt.y - gDragStart.y),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (!gWinHover) {  // 창에 처음 들어옴 → 헤더 펼치고 톤 밝게
            gWinHover = true;
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
            Relayout();
        }
        int row, btn = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), row);
        if (row != gHoverRow || btn != gHoverBtn) { gHoverRow = row; gHoverBtn = btn; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        gHoverRow = -1; gHoverBtn = -1;
        if (gWinHover && !gDragging) {  // 창에서 벗어남 → 헤더 접고 톤 가라앉음
            gWinHover = false;
            SetLayeredWindowAttributes(h, 0, 235, LWA_ALPHA);
            Relayout();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int row, btn = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), row);
        if (btn == 3) {  // X = 설정에 따라 트레이로 숨김 또는 완전 종료
            if (gCloseToTray) { gWinHover = false; ShowWindow(h, SW_HIDE); Relayout(); }
            else DestroyWindow(h);
        } else if (btn == 4) {  // 헤더 메뉴 = 우클릭 메뉴
            ShowMenu();
        } else if (btn == 10) {  // 영상 끄기 = 카메라 하드웨어 차단 토글 (인터넷 무관)
            CameraSetBlocked(!CameraIsBlocked());
            InvalidateRect(h, nullptr, FALSE);
        } else if (btn == 11) {  // 음성 끄기 = 시스템 마이크 음소거 토글
            MicSetMute(g.micMuted == 1 ? FALSE : TRUE);
            g.micMuted = MicMuteState();
            InvalidateRect(h, nullptr, FALSE);
        } else if (!CfgGet(L"locked", 0)) {  // 드래그 이동
            gDragging = true;
            SetCapture(h);
            GetCursorPos(&gDragStart);
            RECT wr; GetWindowRect(h, &wr); gWinStart.x = wr.left; gWinStart.y = wr.top;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (gDragging) {
            gDragging = false;
            ReleaseCapture();
            RECT wr; GetWindowRect(h, &wr);
            CfgSet(L"posX", (DWORD)wr.left); CfgSet(L"posY", (DWORD)wr.top);
        }
        return 0;
    case WM_RBUTTONUP: ShowMenu(); return 0;
    case WM_DISPLAYCHANGE: EnsureOnScreen(); return 0;
    case WM_APP + 1:  // 트레이 아이콘
        if (LOWORD(lp) == WM_LBUTTONUP) {  // 항상 보이게 + 맨 앞으로 (숨기기는 X 버튼만)
            EnsureOnScreen();
            ShowWindow(h, SW_SHOW);
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(h);
            StartLocateFx();  // 클릭할 때마다 위치 알림 (연타 시 처음부터 다시)
        }
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_DESTROY:
        EndLocateFx();
        FwBlockRemove(L"");          // 남은 차단 규칙 청소
        g.etw.Stop();
        Shell_NotifyIconW(NIM_DELETE, &gTray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ── 관리자 권한 확인/승격 ──
static bool IsAdmin() {
    BOOL admin = FALSE;
    PSID grp = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(nullptr, grp, &admin);
        FreeSid(grp);
    }
    return admin;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // 단일 인스턴스
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"KGNF_NetShutter_Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"KGNF_NetShutter", nullptr);
        if (prev) { ShowWindow(prev, SW_SHOW); SetForegroundWindow(prev); }
        return 0;
    }
    // 관리자 아니면 승격 재실행
    if (!IsAdmin()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SHELLEXECUTEINFOW si{sizeof(si)};
        si.lpVerb = L"runas"; si.lpFile = exe; si.nShow = SW_NORMAL;
        if (ShellExecuteExW(&si)) return 0;  // 승격본이 뜸
        // 사용자가 UAC 거부 → 제한 모드로 계속 (앱별 특정·차단은 비활성)
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi;
    GdiplusStartup(&gGdip, &gsi, nullptr);

    HDC dc = GetDC(nullptr);
    gDpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.f;
    ReleaseDC(nullptr, dc);
    gDpi *= 1.5f;  // 전체 크게 (가독성)
    gAlwaysTop = CfgGet(L"alwaysTop", 0) != 0;
    gHideCapture = CfgGet(L"hideCapture", 0) != 0;
    // 언어: 저장값 없으면 시스템 UI 언어가 한국어일 때만 한국어, 그 외 영어
    {
        DWORD def = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN) ? 0 : 1;
        gLang = (int)CfgGet(L"lang", def);
    }
    gCloseToTray = CfgGet(L"closeToTray", 1) != 0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KGNF_NetShutter";
    RegisterClassW(&wc);

    int x = (int)CfgGet(L"posX", 0xFFFFFFFF);
    int y = (int)CfgGet(L"posY", 0xFFFFFFFF);
    RECT saved{x, y, x + WinW(), y + WinH()};
    if (x == (int)0xFFFFFFFF || !OnScreen(saved)) { POINT p = DefaultPos(); x = p.x; y = p.y; }

    gWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                           L"KGNF_NetShutter", L"NetShutter", WS_POPUP,
                           x, y, WinW(), WinH(), nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(gWnd, 0, 240, LWA_ALPHA);  // 전체 반투명
    gStartTick = GetTickCount();
    ShowWindow(gWnd, SW_SHOW);
    SetForegroundWindow(gWnd);  // 시작 시 위치가 보이게 앞으로

    // 트레이 아이콘
    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = gWnd;
    gTray.uID = 1;
    gTray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gTray.uCallbackMessage = WM_APP + 1;
    gTray.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wcscpy(gTray.szTip, T(L"NetShutter — 유출 감시", L"NetShutter — what's leaving your PC"));
    Shell_NotifyIconW(NIM_ADD, &gTray);

    g.Tick(gWnd);
    Relayout();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(gGdip);
    CoUninitialize();
    ReleaseMutex(mtx);
    return 0;
}
