#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using std::max;
using std::min;
#include <gdiplus.h>

namespace fs = std::filesystem;
using namespace Gdiplus;

namespace {

constexpr UINT WM_LOG = WM_APP + 1;
constexpr UINT WM_DONE = WM_APP + 2;
constexpr wchar_t kAppId[] = L"shell:AppsFolder\\Claude_pzs8sxrjxfjjc!Claude";
constexpr float kWidth = 600.f;
constexpr float kHeight = 530.f;

struct Target {
    const wchar_t* name;
    const wchar_t* detail;
    bool cliConfig;
    const wchar_t* path;
    int icon;
};

enum Icon { IconFile, IconFolder, IconGrid, IconGear };

const Target kTargets[] = {
    {L"~/.claude.json", L"(machineID, userID, anonymousId)", true, L"%USERPROFILE%\\.claude.json", IconFile},
    {L"~/.claude/.claude.json", L"(nested IDs)", true, L"%USERPROFILE%\\.claude\\.claude.json", IconFolder},
    {L"ant-did", L"(app device ID)", false, L"%APPDATA%\\Claude\\ant-did", IconGrid},
    {L"ant-device-registry.json", L"", false, L"%APPDATA%\\Claude\\ant-device-registry.json", IconFile},
    {L"remote-control-state.json", L"", false, L"%APPDATA%\\Claude\\remote-control-state.json", IconFile},
    // Only ID keys are stripped; the OAuth token cache stays so the account session survives.
    // claude-code-sessions/ (chat history) is intentionally never touched.
    {L"config.json", L"(pairedDeviceId only, login kept)", true, L"%APPDATA%\\Claude\\config.json", IconGear},
};
constexpr int kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

enum HitId { HitNone = -1, HitRow0 = 0, HitRun = 100, HitOnly = 101 };

struct HitArea {
    RectF rect;
    int id;
};

const RectF kList{21.f, 126.f, 558.f, 279.f};
constexpr float kRowHeight = 279.f / kTargetCount;
const RectF kRunButton{21.f, 423.f, 332.f, 59.f};
const RectF kOnlyButton{367.f, 423.f, 212.f, 59.f};

bool g_checked[kTargetCount];
int g_hover = HitNone;
int g_pressed = HitNone;
bool g_busy = false;
bool g_tracking = false;
float g_scale = 1.f;

std::wstring expand(const wchar_t* raw) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(raw, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::wstring(buf) : std::wstring(raw);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void post(HWND wnd, const std::wstring& line) {
    PostMessageW(wnd, WM_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
}

size_t skipSpace(const std::string& s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

size_t skipString(const std::string& s, size_t i) {
    ++i;
    while (i < s.size() && s[i] != '"') i += (s[i] == '\\') ? 2 : 1;
    return i + 1;
}

size_t skipValue(const std::string& s, size_t i) {
    if (i >= s.size()) return i;
    if (s[i] == '"') return skipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') {
                i = skipString(s, i);
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            if (c == '}' || c == ']') --depth;
            ++i;
            if (depth == 0) break;
        }
        return i;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

bool isIdKey(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    return key == "machineid" || key == "userid" || key == "anonymousid" || key == "paireddeviceid";
}

int stripIdKeys(std::string& text) {
    size_t open = text.find('{');
    if (open == std::string::npos) return -1;

    struct Member {
        size_t begin;
        size_t end;
        bool drop;
    };
    std::vector<Member> members;
    size_t i = skipSpace(text, open + 1);
    size_t close = std::string::npos;

    while (i < text.size()) {
        if (text[i] == '}') {
            close = i;
            break;
        }
        if (text[i] == ',') {
            i = skipSpace(text, i + 1);
            continue;
        }
        if (text[i] != '"') return -1;
        size_t keyBegin = i;
        size_t keyEnd = skipString(text, i);
        std::string key = text.substr(keyBegin + 1, keyEnd - keyBegin - 2);
        i = skipSpace(text, keyEnd);
        if (i >= text.size() || text[i] != ':') return -1;
        i = skipSpace(text, i + 1);
        size_t valueEnd = skipValue(text, i);
        members.push_back({keyBegin, valueEnd, isIdKey(key)});
        i = skipSpace(text, valueEnd);
    }
    if (close == std::string::npos) return -1;

    int dropped = static_cast<int>(std::count_if(members.begin(), members.end(), [](const Member& m) { return m.drop; }));
    if (dropped == 0) return 0;

    size_t lineStart = text.rfind('\n', members.front().begin);
    std::string indent = (lineStart == std::string::npos) ? "" : text.substr(lineStart + 1, members.front().begin - lineStart - 1);

    std::string out = text.substr(0, open + 1);
    bool first = true;
    for (const auto& m : members) {
        if (m.drop) continue;
        out += first ? "\n" : ",\n";
        out += indent + text.substr(m.begin, m.end - m.begin);
        first = false;
    }
    out += "\n" + text.substr(close);
    text = out;
    return dropped;
}

void killClaude(HWND wnd) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int killed = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring name = pe.szExeFile;
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        if (name != L"claude.exe") continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (h) {
            if (TerminateProcess(h, 0)) ++killed;
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    post(wnd, L"Closed Claude processes: " + std::to_wstring(killed));
    Sleep(1500);
}

void cleanCli(HWND wnd, const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        post(wnd, L"  not found, skipped");
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    std::string text = ss.str();
    int dropped = stripIdKeys(text);
    if (dropped < 0) {
        post(wnd, L"  unreadable JSON, skipped");
        return;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    post(wnd, L"  removed ID keys: " + std::to_wstring(dropped));
}

void cleanPath(HWND wnd, const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        post(wnd, L"  not found, skipped");
        return;
    }
    auto removed = fs::remove_all(path, ec);
    if (ec) post(wnd, L"  error: " + widen(ec.message()));
    else post(wnd, L"  deleted (" + std::to_wstring(removed) + L" items)");
}

struct Job {
    HWND wnd;
    bool launch;
    bool selected[kTargetCount];
};

DWORD WINAPI worker(LPVOID param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    killClaude(job->wnd);
    for (int i = 0; i < kTargetCount; ++i) {
        if (!job->selected[i]) continue;
        post(job->wnd, std::wstring(L"> ") + kTargets[i].name);
        fs::path p = expand(kTargets[i].path);
        if (kTargets[i].cliConfig) cleanCli(job->wnd, p);
        else cleanPath(job->wnd, p);
    }
    if (job->launch) {
        post(job->wnd, L"Launching Claude...");
        ShellExecuteW(nullptr, L"open", kAppId, nullptr, nullptr, SW_SHOWNORMAL);
    }
    post(job->wnd, L"Done.");
    PostMessageW(job->wnd, WM_DONE, 0, 0);
    return 0;
}

void start(HWND wnd, bool launch) {
    if (g_busy) return;
    auto* job = new Job{wnd, launch, {}};
    for (int i = 0; i < kTargetCount; ++i) job->selected[i] = g_checked[i];
    g_busy = true;
    InvalidateRect(wnd, nullptr, FALSE);
    HANDLE h = CreateThread(nullptr, 0, worker, job, 0, nullptr);
    if (h) CloseHandle(h);
}

RectF rowRect(int i) {
    return RectF(kList.X, kList.Y + kRowHeight * i, kList.Width, kRowHeight);
}

std::vector<HitArea> hitAreas() {
    std::vector<HitArea> a;
    for (int i = 0; i < kTargetCount; ++i) a.push_back({rowRect(i), HitRow0 + i});
    a.push_back({kRunButton, HitRun});
    a.push_back({kOnlyButton, HitOnly});
    return a;
}

int hitTest(float x, float y) {
    for (const auto& h : hitAreas()) {
        if (h.rect.Contains(x, y)) return h.id;
    }
    return HitNone;
}

void roundedPath(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void fillRounded(Graphics& g, RectF r, float rad, const Brush& b) {
    GraphicsPath p;
    roundedPath(p, r, rad);
    g.FillPath(&b, &p);
}

void strokeRounded(Graphics& g, RectF r, float rad, const Pen& pen) {
    GraphicsPath p;
    roundedPath(p, r, rad);
    g.DrawPath(&pen, &p);
}

void drawText(Graphics& g, const std::wstring& text, float x, float cy, float size, const wchar_t* family, Color color,
              StringAlignment align = StringAlignmentNear, float width = 600.f) {
    FontFamily ff(family);
    Font font(&ff, size, FontStyleRegular, UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt(StringFormatFlagsNoWrap);
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    RectF box(x, cy - size, width, size * 2);
    g.DrawString(text.c_str(), -1, &font, box, &fmt, &brush);
}

float measureText(Graphics& g, const std::wstring& text, float size, const wchar_t* family) {
    FontFamily ff(family);
    Font font(&ff, size, FontStyleRegular, UnitPixel);
    StringFormat fmt(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF out;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &fmt, &out);
    return out.Width;
}

void drawStarburst(Graphics& g, float cx, float cy, float r, Color color, float width) {
    static const float lengths[] = {1.f, 0.74f, 0.92f, 0.66f, 0.98f, 0.8f, 0.9f, 0.7f, 1.f, 0.78f, 0.94f, 0.68f, 0.86f, 0.72f};
    static const float angles[] = {0.f, 24.f, 49.f, 77.f, 100.f, 126.f, 152.f, 175.f, 203.f, 228.f, 254.f, 280.f, 308.f, 334.f};
    Pen pen(color, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    for (int i = 0; i < 14; ++i) {
        float a = angles[i] * 3.14159265f / 180.f;
        g.DrawLine(&pen, cx + std::cos(a) * r * 0.1f, cy + std::sin(a) * r * 0.1f, cx + std::cos(a) * r * lengths[i],
                   cy + std::sin(a) * r * lengths[i]);
    }
}

void drawChevron(Graphics& g, float cx, float cy, Color color) {
    Pen pen(color, 2.4f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    PointF pts[3] = {{cx - 3.5f, cy - 7.f}, {cx + 3.5f, cy}, {cx - 3.5f, cy + 7.f}};
    g.DrawLines(&pen, pts, 3);
}

void drawTrash(Graphics& g, float cx, float cy, Color color) {
    Pen pen(color, 2.f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - 9, cy - 7, cx + 9, cy - 7);
    g.DrawLine(&pen, cx - 3.5f, cy - 7, cx - 3.5f, cy - 10.5f);
    g.DrawLine(&pen, cx + 3.5f, cy - 7, cx + 3.5f, cy - 10.5f);
    g.DrawLine(&pen, cx - 3.5f, cy - 10.5f, cx + 3.5f, cy - 10.5f);
    GraphicsPath body;
    body.AddLine(cx - 7.5f, cy - 7, cx - 6.5f, cy + 9);
    body.AddLine(cx - 6.5f, cy + 9, cx + 6.5f, cy + 9);
    body.AddLine(cx + 6.5f, cy + 9, cx + 7.5f, cy - 7);
    g.DrawPath(&pen, &body);
    g.DrawLine(&pen, cx - 2.5f, cy - 3, cx - 2.5f, cy + 5);
    g.DrawLine(&pen, cx + 2.5f, cy - 3, cx + 2.5f, cy + 5);
}

void drawRowIcon(Graphics& g, int icon, float cx, float cy, Color color) {
    Pen pen(color, 2.f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    if (icon == IconFile) {
        GraphicsPath p;
        p.AddLine(cx - 7, cy - 10, cx + 2, cy - 10);
        p.AddLine(cx + 2, cy - 10, cx + 8, cy - 4);
        p.AddLine(cx + 8, cy - 4, cx + 8, cy + 10);
        p.AddLine(cx + 8, cy + 10, cx - 7, cy + 10);
        p.CloseFigure();
        g.DrawPath(&pen, &p);
        g.DrawLine(&pen, cx + 2, cy - 10, cx + 2, cy - 4);
        g.DrawLine(&pen, cx + 2, cy - 4, cx + 8, cy - 4);
    } else if (icon == IconFolder) {
        GraphicsPath p;
        roundedPath(p, RectF(cx - 11, cy - 7, 22, 17), 3.f);
        g.DrawPath(&pen, &p);
        g.DrawLine(&pen, cx - 11, cy - 7, cx - 11, cy - 10);
        g.DrawLine(&pen, cx - 11, cy - 10, cx - 3, cy - 10);
        g.DrawLine(&pen, cx - 3, cy - 10, cx - 1, cy - 7);
    } else if (icon == IconGrid) {
        for (int i = 0; i < 4; ++i) {
            float x = cx - 10 + (i % 2) * 12;
            float y = cy - 10 + (i / 2) * 12;
            strokeRounded(g, RectF(x, y, 8, 8), 2.f, pen);
        }
    } else {
        g.DrawEllipse(&pen, cx - 4.5f, cy - 4.5f, 9.f, 9.f);
        for (int i = 0; i < 8; ++i) {
            float a = static_cast<float>(i) * 3.14159265f / 4.f;
            g.DrawLine(&pen, cx + std::cos(a) * 7.5f, cy + std::sin(a) * 7.5f, cx + std::cos(a) * 10.5f, cy + std::sin(a) * 10.5f);
        }
        g.DrawEllipse(&pen, cx - 8.f, cy - 8.f, 16.f, 16.f);
    }
}

void drawCheckbox(Graphics& g, RectF r, bool on, bool hot) {
    if (on) {
        LinearGradientBrush b(r, Color(255, 255, 255, 255), Color(255, 205, 205, 212), LinearGradientModeVertical);
        fillRounded(g, r, 7.f, b);
        Pen pen(Color(255, 22, 22, 26), 2.6f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        PointF pts[3] = {{r.X + r.Width * 0.27f, r.Y + r.Height * 0.52f},
                         {r.X + r.Width * 0.44f, r.Y + r.Height * 0.69f},
                         {r.X + r.Width * 0.74f, r.Y + r.Height * 0.31f}};
        g.DrawLines(&pen, pts, 3);
    } else {
        SolidBrush b(Color(hot ? 40 : 18, 255, 255, 255));
        fillRounded(g, r, 7.f, b);
        Pen pen(Color(120, 255, 255, 255), 1.5f);
        strokeRounded(g, r, 7.f, pen);
    }
}

void drawButton(Graphics& g, RectF r, bool primary, int id, const wchar_t* label) {
    bool hot = g_hover == id;
    bool down = g_pressed == id && hot;
    float lift = down ? 0.f : (hot ? -1.f : 0.f);
    r.Y += lift;
    BYTE dim = g_busy ? 110 : 255;

    if (primary) {
        LinearGradientBrush b(r, Color(dim, down ? 215 : 255, down ? 215 : 255, down ? 220 : 255),
                              Color(dim, down ? 160 : 190, down ? 160 : 190, down ? 168 : 198), LinearGradientModeVertical);
        fillRounded(g, r, 18.f, b);
        Pen edge(Color(static_cast<BYTE>(dim * 0.9f), 255, 255, 255), 1.f);
        strokeRounded(g, r, 18.f, edge);
    } else {
        SolidBrush b(Color(static_cast<BYTE>((down ? 14 : (hot ? 34 : 22)) * (g_busy ? 0.6f : 1.f)), 255, 255, 255));
        fillRounded(g, r, 18.f, b);
        LinearGradientBrush e(r, Color(120, 255, 255, 255), Color(30, 255, 255, 255), LinearGradientModeVertical);
        Pen edge(&e, 1.2f);
        strokeRounded(g, r, 18.f, edge);
    }

    Color ink = primary ? Color(dim, 18, 18, 22) : Color(g_busy ? 120 : 235, 255, 255, 255);
    float cy = r.Y + r.Height / 2;
    if (primary) {
        drawText(g, label, r.X + 28.f, cy, 17.f, L"Segoe UI Semibold", ink);
        drawChevron(g, r.GetRight() - 28.f, cy, ink);
    } else {
        drawTrash(g, r.X + 36.f, cy, ink);
        drawText(g, label, r.X + 66.f, cy, 17.f, L"Segoe UI Semibold", ink);
        drawChevron(g, r.GetRight() - 24.f, cy, ink);
    }
}

void drawLogo(Graphics& g, RectF r, float rad, float rays, float width) {
    SolidBrush b(Color(255, 38, 38, 42));
    fillRounded(g, r, rad, b);
    Pen edge(Color(70, 255, 255, 255), 1.f);
    strokeRounded(g, r, rad, edge);
    drawStarburst(g, r.X + r.Width / 2, r.Y + r.Height / 2, rays, Color(255, 255, 255, 255), width);
}

void render(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    LinearGradientBrush sheen(RectF(0.f, 0.f, kWidth, kHeight), Color(26, 255, 255, 255), Color(6, 255, 255, 255), LinearGradientModeVertical);
    g.FillRectangle(&sheen, RectF(0.f, 0.f, kWidth, kHeight));

    drawLogo(g, RectF(34.f, 26.f, 82.f, 82.f), 21.f, 30.f, 4.6f);
    drawText(g, L"Safe Claude", 141.f, 58.f, 38.f, L"Segoe UI Semibold", Color(255, 255, 255, 255));
    drawText(g, L"Resets local device identifiers and starts Claude fresh.", 142.f, 92.f, 15.5f, L"Segoe UI",
             Color(170, 255, 255, 255));

    SolidBrush listFill(Color(16, 255, 255, 255));
    fillRounded(g, kList, 18.f, listFill);
    GraphicsPath listClip;
    roundedPath(listClip, kList, 18.f);
    for (int i = 0; i < kTargetCount; ++i) {
        RectF row = rowRect(i);
        bool hot = g_hover == HitRow0 + i;
        if (hot) {
            GraphicsState st = g.Save();
            g.SetClip(&listClip);
            SolidBrush h(Color(18, 255, 255, 255));
            g.FillRectangle(&h, row);
            g.Restore(st);
        }
        if (i > 0) {
            Pen line(Color(22, 255, 255, 255), 1.f);
            g.DrawLine(&line, row.X + 1.f, row.Y, row.GetRight() - 1.f, row.Y);
        }
        float cy = row.Y + row.Height / 2;
        drawCheckbox(g, RectF(40.f, cy - 12.f, 24.f, 24.f), g_checked[i], hot);
        std::wstring name = kTargets[i].name;
        drawText(g, name, 79.f, cy, 16.f, L"Segoe UI", Color(245, 255, 255, 255));
        if (kTargets[i].detail[0]) {
            float w = measureText(g, name, 16.f, L"Segoe UI");
            drawText(g, kTargets[i].detail, 79.f + w + 5.f, cy + 0.5f, 13.5f, L"Segoe UI", Color(135, 255, 255, 255));
        }
        drawRowIcon(g, kTargets[i].icon, 549.f, cy, Color(170, 255, 255, 255));
    }
    LinearGradientBrush listEdge(kList, Color(95, 255, 255, 255), Color(28, 255, 255, 255), LinearGradientModeVertical);
    Pen listPen(&listEdge, 1.2f);
    strokeRounded(g, kList, 18.f, listPen);

    drawButton(g, kRunButton, true, HitRun, g_busy ? L"Working..." : L"Clean and launch Claude");
    drawButton(g, kOnlyButton, false, HitOnly, L"Clean only");

    drawText(g, L"by @hiopat   @wucaz   @kierradev", 0.f, 508.f, 12.5f, L"Segoe UI", Color(110, 255, 255, 255), StringAlignmentCenter, kWidth);
}

void paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    RECT rc;
    GetClientRect(wnd, &rc);
    int w = rc.right;
    int h = rc.bottom;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
        {
            Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
            Graphics g(&bmp);
            g.ScaleTransform(g_scale, g_scale);
            render(g);
        }
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, dib);
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(dib);
    }
    EndPaint(wnd, &ps);
}

void applyGlass(HWND wnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark));
    int corner = 2;
    DwmSetWindowAttribute(wnd, 33, &corner, sizeof(corner));
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(wnd, &m);

    struct Accent {
        int state;
        int flags;
        unsigned gradient;
        int animation;
    };
    struct CompositionData {
        int attribute;
        void* data;
        ULONG size;
    };
    using SetAttr = BOOL(WINAPI*)(HWND, CompositionData*);
    auto setAttr = reinterpret_cast<SetAttr>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!setAttr) return;
    Accent accent{4, 0, 0x66101012, 0};
    CompositionData data{19, &accent, sizeof(accent)};
    setAttr(wnd, &data);
}

void mouseTrack(HWND wnd) {
    if (g_tracking) return;
    TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, wnd, 0};
    TrackMouseEvent(&t);
    g_tracking = true;
}

LRESULT CALLBACK wndProc(HWND wnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE:
            applyGlass(wnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(wnd);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && g_hover != HitNone && !(g_busy && (g_hover == HitRun || g_hover == HitOnly))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_MOUSEMOVE: {
            mouseTrack(wnd);
            int id = hitTest(GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale);
            if (id != g_hover) {
                g_hover = id;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = HitNone;
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            g_pressed = hitTest(GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale);
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            int id = hitTest(GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale);
            int pressed = g_pressed;
            g_pressed = HitNone;
            if (id != HitNone && id == pressed) {
                if (id >= HitRow0 && id < HitRow0 + kTargetCount) g_checked[id] = !g_checked[id];
                else if (id == HitRun) start(wnd, true);
                else if (id == HitOnly) start(wnd, false);
            }
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        case WM_LOG: {
            delete reinterpret_cast<std::wstring*>(l);
            return 0;
        }
        case WM_DONE:
            g_busy = false;
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd, msg, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    GdiplusStartupInput gsi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gsi, nullptr);

    HDC screen = GetDC(nullptr);
    g_scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.f;
    ReleaseDC(nullptr, screen);
    std::fill(std::begin(g_checked), std::end(g_checked), true);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"SafeClaudeWindow";
    RegisterClassExW(&wc);

    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT frame{0, 0, static_cast<LONG>(kWidth * g_scale), static_cast<LONG>(kHeight * g_scale)};
    AdjustWindowRect(&frame, style, FALSE);
    int w = frame.right - frame.left;
    int h = frame.bottom - frame.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Safe Claude", style, x, y, w, h,
                               nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(token);
    return static_cast<int>(msg.wParam);
}
