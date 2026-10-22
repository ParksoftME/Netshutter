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
static POINT gDragStart{}, gWinStart{};

// 히트 영역 (줄/버튼 클릭 판정)
struct Hit { RectF rc; int row; int btn; std::wstring key; };
static std::vector<Hit> gHits;

// ── 디자인 토큰 (캔버스 시안과 동일 계열) ──
static const float ROWH = 22, PADW = 4, RAD = 9, HEADERH = 20;

// 셔터(조리개) 로고: 둥근 사각 + 중앙 조리개 날
static void DrawLogo(Gdiplus::Graphics& gr, float x, float y, float s, Gdiplus::Color accent) {
    using namespace Gdiplus;
    GraphicsPath box;
    float d = s * 0.52f;
    box.AddArc(x, y, d, d, 180, 90);
    box.AddArc(x + s - d, y, d, d, 270, 90);
    box.AddArc(x + s - d, y + s - d, d, d, 0, 90);
    box.AddArc(x, y + s - d, d, d, 90, 90);
    box.CloseFigure();
    SolidBrush b(accent);
    gr.FillPath(&b, &box);
    float cx = x + s / 2, cy = y + s / 2, r = s * 0.30f;
    Pen w(Color(255, 255, 255, 255), s * 0.08f);
    w.SetStartCap(LineCapRound); w.SetEndCap(LineCapRound);
    gr.DrawEllipse(&w, cx - r, cy - r, 2 * r, 2 * r);
    for (int i = 0; i < 6; i++) {
        float a = (i * 60 + 15) * 3.14159265f / 180.f;
        gr.DrawLine(&w, cx + cosf(a) * r * 0.15f, cy + sinf(a) * r * 0.15f,
                    cx + cosf(a) * r, cy + sinf(a) * r);
    }
}
static Color Col(BYTE r, BYTE gg, BYTE b, BYTE a = 255) { return Color(a, r, gg, b); }
static const Color cBg = Col(28, 28, 30, 244), cBorder = Col(255, 255, 255, 24),
                   cFg = Col(242, 242, 242), cDim = Col(142, 142, 147), cOff = Col(96, 96, 100),
                   cRed = Col(255, 107, 74), cYel = Col(255, 212, 59), cBlu = Col(143, 211, 255),
                   cHover = Col(255, 255, 255, 20), cBtnBg = Col(255, 255, 255, 28);

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
enum IconId { ICmic, ICmicOff, ICcam, ICup, ICnet, ICnetOff, IClock, ICtray };
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

// ── 창 크기 = 상태에 맞춤 ──
static void Relayout() {
    bool leak = g.AnyLeak();
    bool startup = gStartTick && (GetTickCount() - gStartTick < 6000);  // 시작 후 6초간 앞에
    int nLeak = (g.VideoLeak() ? 1 : 0) + (g.AudioLeak() ? 1 : 0);
    int body = leak ? nLeak * Sx(30) : Sx(40);  // 안전 상태는 크게
    int w = Sx(244);
    int h = Sx(PADW * 2) + body + (gWinHover ? Sx(HEADERH) : 0);
    HWND z = (gAlwaysTop || leak || startup) ? HWND_TOPMOST : HWND_NOTOPMOST;
    UINT zf = SWP_NOMOVE | SWP_NOACTIVATE;
    if (gMenuOpen) zf |= SWP_NOZORDER;  // 메뉴 중엔 위젯을 다시 위로 올리지 않음
    SetWindowPos(gWnd, gMenuOpen ? nullptr : z, 0, 0, w, h, zf);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, Sx(RAD) * 2, Sx(RAD) * 2);
    SetWindowRgn(gWnd, rgn, TRUE);  // 둥근 모서리
}

// ── 렌더 ──
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

        bool leak = g.AnyLeak();
        GraphicsPath frame;
        RoundRect(frame, RectF(0.5f, 0.5f, W - 1.f, H - 1.f), (float)Sx(RAD));
        // 상태별 배경: 안전=어두운 녹색 기운, 송출 중=붉은 기운
        SolidBrush bg(leak ? Col(46, 24, 22) : Col(24, 32, 27));
        gr.FillPath(&bg, &frame);
        Color borderCol = leak ? Col(226, 64, 28, gBlinkOn ? 235 : 110)
                                : (gWinHover ? Col(255, 255, 255, 48) : cBorder);
        Pen bord(borderCol, leak ? 2.0f : 1.0f); gr.DrawPath(&bord, &frame);

        FontFamily ff(L"Segoe UI");
        Font fName(&ff, 9.5f * gDpi, FontStyleBold, UnitPixel);
        Font fRate(&ff, 8.5f * gDpi, FontStyleBold, UnitPixel);
        StringFormat sfL; sfL.SetAlignment(StringAlignmentNear); sfL.SetLineAlignment(StringAlignmentCenter);
        StringFormat sfR; sfR.SetAlignment(StringAlignmentFar); sfR.SetLineAlignment(StringAlignmentCenter);
        StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);

        gHits.clear();

        // 헤더: 호버 시에만. 로고 + 이름. 이 영역이 드래그 핸들 역할.
        float baseY = Sx(PADW);
        if (gWinHover) {
            float hy = Sx(PADW);
            DrawLogo(gr, Sx(7), hy + Sx(2), Sx(14), cRed);
            SolidBrush tb(cFg);
            RectF tr(Sx(7) + Sx(18), hy, (float)W, (float)Sx(HEADERH));
            gr.DrawString(L"NetShutter", -1, &fName, tr, &sfL, &tb);
            // 우측 X 버튼 (트레이로 숨김)
            float bx = (float)(W - Sx(7) - Sx(10)), by = hy + Sx(HEADERH) / 2.f;
            bool xh = (gHoverBtn == 3);
            Pen xp(xh ? cFg : cDim, 1.4f * gDpi);
            xp.SetStartCap(LineCapRound); xp.SetEndCap(LineCapRound);
            float r2 = Sx(4);
            gr.DrawLine(&xp, bx - r2, by - r2, bx + r2, by + r2);
            gr.DrawLine(&xp, bx + r2, by - r2, bx - r2, by + r2);
            gHits.push_back({RectF(bx - Sx(9), hy, (float)Sx(18), (float)Sx(HEADERH)), -1, 3, L""});
            // 얇은 구분선
            Pen sep(Col(255, 255, 255, 28), 1);
            gr.DrawLine(&sep, (float)Sx(8), hy + Sx(HEADERH) - 1, (float)(W - Sx(8)), hy + Sx(HEADERH) - 1);
            baseY += Sx(HEADERH);
        }

        float px = Sx(13);
        if (!leak) {
            // ── 안전: 녹색, 크게 ──
            float cy = baseY + Sx(20);
            SolidBrush gb(Col(74, 200, 120));
            gr.FillEllipse(&gb, px, cy - Sx(10), (float)Sx(20), (float)Sx(20));
            Pen ck(Col(20, 34, 24), 2.2f * gDpi);
            ck.SetStartCap(LineCapRound); ck.SetEndCap(LineCapRound); ck.SetLineJoin(LineJoinRound);
            float ex = px + Sx(10);
            gr.DrawLine(&ck, ex - Sx(4), cy, ex - Sx(1), cy + Sx(3.5f));
            gr.DrawLine(&ck, ex - Sx(1), cy + Sx(3.5f), ex + Sx(5), cy - Sx(4));
            Font fBig(&ff, 11.5f * gDpi, FontStyleBold, UnitPixel);
            SolidBrush tg(Col(120, 220, 150));
            gr.DrawString(T(L"안전합니다", L"You're safe"), -1, &fBig,
                          RectF(px + Sx(28), baseY + Sx(2), (float)W, (float)Sx(22)), &sfL, &tg);
            SolidBrush sub(cDim);
            gr.DrawString(T(L"아무것도 송출되지 않음", L"Nothing is being sent"), -1, &fRate,
                          RectF(px + Sx(28), baseY + Sx(22), (float)W, (float)Sx(14)), &sfL, &sub);
        } else {
            // ── 송출 중: 빨강 경고 + 끄기 ──
            Font fMsg(&ff, 9.5f * gDpi, FontStyleBold, UnitPixel);
            float ry = baseY;
            auto leakRow = [&](bool show, IconId ic, const wchar_t* msg, int btnCode, bool off) {
                if (!show) return;
                float cy = ry + Sx(15);
                Icon(gr, ic, px, cy - Sx(6), Sx(12), cRed, 1.6f);
                SolidBrush tr(Col(255, 150, 130));
                gr.DrawString(msg, -1, &fMsg, RectF(px + Sx(19), ry, (float)(W - Sx(62)), (float)Sx(30)), &sfL, &tr);
                float bw = Sx(38), bh = Sx(17), bx = W - Sx(9) - bw;
                RectF br(bx, cy - bh / 2, bw, bh);
                GraphicsPath p; RoundRect(p, br, Sx(4));
                SolidBrush bb(off ? Col(90, 90, 96) : Col(226, 64, 28)); gr.FillPath(&bb, &p);
                SolidBrush bt(Col(255, 255, 255));
                gr.DrawString(off ? T(L"해제", L"On") : T(L"끄기", L"Off"), -1, &fRate, br, &sfC, &bt);
                gHits.push_back({br, -1, btnCode, L""});
                ry += Sx(30);
            };
            leakRow(g.VideoLeak(), ICcam, T(L"화면·영상이 송출되고 있어요", L"Video is being sent out"), 10, CameraIsBlocked());
            leakRow(g.AudioLeak(), ICmic, T(L"음성이 송출되고 있어요", L"Audio is being sent out"), 11, g.micMuted == 1);
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
            if (gCloseToTray) { gWinHover = false; ShowWindow(h, SW_HIDE); }
            else DestroyWindow(h);
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
    case WM_APP + 1:  // 트레이 아이콘
        if (LOWORD(lp) == WM_LBUTTONUP) { ShowWindow(h, IsWindowVisible(h) ? SW_HIDE : SW_SHOW); }
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_DESTROY:
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
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x == (int)0xFFFFFFFF) { x = wa.right - Sx(230); y = wa.bottom - Sx(80); }

    gWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                           L"KGNF_NetShutter", L"NetShutter", WS_POPUP,
                           x, y, Sx(210), Sx(26), nullptr, nullptr, hInst, nullptr);
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
    gTray.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
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
