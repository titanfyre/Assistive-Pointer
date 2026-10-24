#include "Interceptor.h"
#include "Ui.h"
#include "Model.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <system_error>
#include <map>
namespace
{
constexpr UINT Updated = WM_APP + 2;
struct App
{
    HWND window = nullptr, start = nullptr, stop = nullptr, setup = nullptr, mouse = nullptr, gain = nullptr,
         axis = nullptr, threshold = nullptr, debounce = nullptr, bypass = nullptr;
    HANDLE stopEvent = nullptr;
    std::thread worker;
    std::atomic<bool> running{false}, passthrough{false};
    std::atomic<std::uint32_t> target{0};
    std::mutex mutex;
    Pointer::Settings settings;
    struct Mouse
    {
        std::uint32_t id;
        std::string token;
        std::wstring label;
    };
    std::vector<Mouse> mice;
    std::string selectedToken;
    std::uint64_t listVersion = 0, shownVersion = ~std::uint64_t{0};
    std::wstring status = L"Start, choose a mouse, then adjust its motion. Nothing is intercepted until Start.";
    std::uint64_t reports = 0;
    std::int32_t inX = 0, inY = 0, outX = 0, outY = 0;
    bool preview = false;
    ~App()
    {
        Stop();
        if (stopEvent)
            CloseHandle(stopEvent);
    }
    void Stop()
    {
        if (stopEvent)
            SetEvent(stopEvent);
        if (worker.joinable())
            worker.join();
        running = false;
    }
    void State(const std::wstring &text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        status = text;
    }
    void Start()
    {
        if (running || preview)
            return;
        if (!Ui::StartElevated(window))
            return;
        Stop();
        target = 0;
        passthrough = false;
        ResetEvent(stopEvent);
        {
            std::lock_guard<std::mutex> lock(mutex);
            reports = 0;
            mice.clear();
            selectedToken.clear();
            ++listVersion;
            status = L"Starting. All mice remain unchanged until you choose one below.";
        }
        SendMessageW(mouse, CB_RESETCONTENT, 0, 0);
        SendMessageW(mouse, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Choose a mouse - all inputs pass through"));
        SendMessageW(mouse, CB_SETCURSEL, 0, 0);
        running = true;
        try
        {
            worker = std::thread([this] { Run(); });
        }
        catch (const std::exception &e)
        {
            running = false;
            State(Ui::Wide(e.what()));
        }
    }
    void Run()
    {
        try
        {
            Pointer::State filter;
            std::string previousToken;
            bool previousBypass = false;
            std::uint64_t nextDiscovery = 0;
            while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
            {
                const auto now = GetTickCount64();
                if (now >= nextDiscovery)
                {
                    auto devices = Interceptor::Get_Devices();
                    std::vector<Mouse> fresh;
                    for (const auto &d : devices)
                        if (d.type == IC_TYPE_MOUSE && !d.token.empty() && fresh.size() < 64)
                        {
                            std::wstring label;
                            for (size_t i = 0; i < 128 && d.name[i]; ++i)
                                label += static_cast<wchar_t>(d.name[i]);
                            if (label.empty())
                                label = L"Mouse";
                            fresh.push_back({d.id, d.token, label + L" / source " + std::to_wstring(d.id)});
                        }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        const bool changed =
                            fresh.size() != mice.size() ||
                            !std::equal(fresh.begin(), fresh.end(), mice.begin(),
                                        [](const Mouse &x, const Mouse &y) {
                                            return x.id == y.id && x.token == y.token && x.label == y.label;
                                        });
                        if (changed)
                        {
                            mice = std::move(fresh);
                            ++listVersion;
                        }
                        if (target.load() && std::none_of(mice.begin(), mice.end(), [&](const Mouse &m) {
                                return m.id == target.load() && Pointer::Matches(selectedToken, m.token);
                            }))
                        {
                            target = 0;
                            selectedToken.clear();
                            filter.Clear();
                        }
                    }
                    nextDiscovery = now + 500;
                }
                auto batch = Interceptor::Get_Input_With_Timeout(100, stopEvent);
                std::uint32_t selected = 0;
                std::string token;
                const bool pass = passthrough.load();
                Pointer::Settings config;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    config = settings;
                    selected = target.load();
                    token = selectedToken;
                }
                if (previousToken != token || previousBypass != pass)
                {
                    filter.Clear();
                    previousToken = token;
                    previousBypass = pass;
                }
                std::uint64_t count = 0;
                std::int32_t ix = 0, iy = 0, ox = 0, oy = 0;
                for (auto &e : batch)
                {
                    if (auto p = e.Get<Interceptor::Mouse_Input>())
                    {
                        if (e.device != selected)
                            continue;
                        if (!e.device_info || !Pointer::Matches(token, e.device_info->token))
                        {
                            filter.Clear();
                            selected = 0;
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                if (selectedToken == token)
                                {
                                    target = 0;
                                    selectedToken.clear();
                                    ++listVersion;
                                }
                            }
                            continue;
                        }
                        ++count;
                        ix = p->stroke.x;
                        iy = p->stroke.y;
                        if (!pass)
                            filter.Apply(p->stroke.x, p->stroke.y, p->stroke.button_flags, p->stroke.flags,
                                         GetTickCount64(), config);
                        ox = p->stroke.x;
                        oy = p->stroke.y;
                    }
                }
                const auto sent = Interceptor::Send_Input(batch);
                if (sent.error || sent.accepted != batch.size())
                    throw std::system_error(static_cast<int>(sent.error ? sent.error : ERROR_WRITE_FAULT),
                                            std::system_category(), "Forwarding input failed");
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    reports += count;
                    if (count)
                    {
                        inX = ix;
                        inY = iy;
                        outX = ox;
                        outY = oy;
                    }
                    status =
                        pass ? L"Bypass active. Every input is forwarded unchanged."
                        : selected
                            ? L"Running. Selected relative mouse motion is transformed; other devices pass "
                              L"through."
                            : L"Running in passthrough. Choose a mouse source above; unplugging clears selection.";
                }
            }
            State(L"Stopped. Capture released; all input returns to normal passthrough.");
        }
        catch (const std::exception &e)
        {
            State(L"Stopped: " + Ui::Wide(e.what()) + L". Capture released; input returns to passthrough.");
        }
        try
        {
            Interceptor::Destroy_Context();
        }
        catch (...)
        {
        }
        running = false;
        PostMessageW(window, Updated, 0, 0);
    }
    void Change()
    {
        Pointer::Settings s;
        s.gain = static_cast<double>(SendMessageW(gain, TBM_GETPOS, 0, 0)) / 100.0;
        s.lock = static_cast<unsigned>(std::max<LRESULT>(0, SendMessageW(axis, CB_GETCURSEL, 0, 0)));
        s.threshold = static_cast<unsigned>(SendMessageW(threshold, TBM_GETPOS, 0, 0));
        s.debounce = static_cast<unsigned>(SendMessageW(debounce, TBM_GETPOS, 0, 0));
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings = s;
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void Layout()
    {
        RECT r{};
        GetClientRect(window, &r);
        const int w = Ui::Logical(window, r.right);
        Ui::Fonts(window);
        Ui::Move(start, 24, 96, 110, 40, TRUE);
        Ui::Move(stop, 144, 96, 110, 40, TRUE);
        Ui::Move(bypass, 264, 96, 158, 40, TRUE);
        Ui::Move(setup, w - 180, 28, 156, 38, TRUE);
        Ui::Move(mouse, 24, 158, w - 48, 300, TRUE);
        Ui::Move(gain, 42, 305, w / 2 - 80, 42, TRUE);
        Ui::Move(axis, w / 2 + 25, 305, w / 2 - 67, 200, TRUE);
        Ui::Move(threshold, 42, 435, w / 2 - 80, 42, TRUE);
        Ui::Move(debounce, w / 2 + 25, 435, w / 2 - 67, 42, TRUE);
    }
    void Paint(HDC dc, int w, int h)
    {
        Ui::Canvas scale(dc, window);
        w = Ui::Logical(window, w);
        h = Ui::Logical(window, h);
        Ui::Box(dc, RECT{0, 0, w, h}, Ui::Background, 0);
        Ui::TextAt(dc, 24, 24, L"Assistive Pointer", w - 230, Ui::Text, 30, FW_SEMIBOLD);
        Ui::TextAt(dc, 24, 66, L"Choose one mouse. Tune motion live. Stop restores normal input.", w - 50,
                   Ui::Muted, 17);
        Pointer::Settings snapshot;
        std::wstring note;
        std::uint64_t count;
        std::int32_t sampleIX, sampleIY, sampleOX, sampleOY;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = settings;
            note = status;
            count = reports;
            sampleIX = inX;
            sampleIY = inY;
            sampleOX = outX;
            sampleOY = outY;
        }

        Ui::Box(dc, RECT{24, 253, w / 2 - 10, 365});
        Ui::Box(dc, RECT{w / 2 + 10, 253, w - 24, 365});
        std::wostringstream value;
        value << std::fixed << std::setprecision(2) << L"Motion gain  " << snapshot.gain << L"x";
        Ui::TextAt(dc, 42, 270, value.str(), w / 2 - 80, Ui::Text, 20, FW_SEMIBOLD);
        Ui::TextAt(dc, w / 2 + 25, 270, L"Axis restriction", w / 2 - 70, Ui::Text, 20, FW_SEMIBOLD);
        Ui::Box(dc, RECT{24, 385, w / 2 - 10, 488});
        Ui::Box(dc, RECT{w / 2 + 10, 385, w - 24, 488});
        Ui::TextAt(dc, 42, 396, L"Motion aggregation  " + std::to_wstring(snapshot.threshold) + L" counts",
                   w / 2 - 70, Ui::Text, 18, FW_SEMIBOLD);
        Ui::TextAt(dc, w / 2 + 25, 396, L"Click debounce  " + std::to_wstring(snapshot.debounce) + L" ms",
                   w / 2 - 70, Ui::Text, 18, FW_SEMIBOLD);
        Ui::Box(dc, RECT{24, 507, w - 24, 595});
        Ui::TextAt(dc, 42, 520, L"SELECTED MOUSE  |  " + std::to_wstring(count) + L" reports", w - 80, Ui::Muted,
                   15);
        Ui::TextAt(dc, 42, 552,
                   L"IN    X " + std::to_wstring(sampleIX) + L" / Y " + std::to_wstring(sampleIY) +
                       L"             OUT    X " + std::to_wstring(sampleOX) + L" / Y " +
                       std::to_wstring(sampleOY),
                   w - 80, Ui::Accent, 22, FW_SEMIBOLD);
        Ui::TextAt(dc, 28, 617, note, w - 56, Ui::Accent, 17);
        Ui::TextAt(dc, 28, h - 96,
                   L"Aggregation holds small motion until the threshold is reached. It adds delay and can retain "
                   L"up to 7 counts per axis; default 0 disables it. Buttons and wheel are unchanged unless "
                   L"debounce is enabled. Absolute devices pass through.",
                   w - 56, Ui::Muted, 15);
        Ui::TextAt(dc, 28, h - 34, L"Emergency stop: Ctrl+Alt+F12. No automatic clicking or hidden recording.",
                   w - 56, Ui::Muted, 15);
    }
    void Refresh()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (shownVersion != listVersion)
        {
            SendMessageW(mouse, CB_RESETCONTENT, 0, 0);
            SendMessageW(mouse, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(L"Choose a mouse - all inputs pass through"));
            int index = 1, chosen = 0;
            for (const auto &item : mice)
            {
                SendMessageW(mouse, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
                if (Pointer::Matches(selectedToken, item.token))
                    chosen = index;
                ++index;
            }
            SendMessageW(mouse, CB_SETCURSEL, static_cast<WPARAM>(chosen), 0);
            shownVersion = listVersion;
        }
        InvalidateRect(window, nullptr, FALSE);
    }
};
int Tests()
{
    unsigned n = 0;
    auto check = [&](bool good) {
        ++n;
        if (!good)
            throw std::runtime_error("Pointer fixture failed: " + std::to_string(n));
    };
    check(Pointer::Matches("capture-a:mouse-1", "capture-a:mouse-1"));
    check(!Pointer::Matches("capture-a:mouse-1", "capture-b:mouse-1"));
    check(!Pointer::Matches("", ""));
    Pointer::State f;
    Pointer::Settings s;
    std::int32_t x = 4, y = -3;
    std::uint16_t b = 0xC01;
    f.Apply(x, y, b, 0, 1000, s);
    check(x == 4 && y == -3 && b == 0xC01);
    s.gain = 0.25;
    std::int64_t sum = 0;
    for (int i = 0; i < 1000; ++i)
    {
        x = 1;
        y = 0;
        b = 0;
        f.Apply(x, y, b, 0, static_cast<std::uint64_t>(i) + 2000, s);
        sum += x;
    }
    check(sum == 250);
    f.Clear();
    sum = 0;
    for (int i = 0; i < 1000; ++i)
    {
        x = -1;
        y = 0;
        f.Apply(x, y, b, 0, 1, s);
        sum += x;
    }
    check(sum == -250);
    s.lock = 1;
    x = 8;
    y = 8;
    f.Apply(x, y, b, 0, 1, s);
    check(y == 0);
    s.lock = 2;
    x = 8;
    y = 8;
    f.Apply(x, y, b, 0, 1, s);
    check(x == 0);
    s = {};
    s.threshold = 4;
    f.Clear();
    sum = 0;
    for (int i = 0; i < 20; ++i)
    {
        x = 1;
        y = 0;
        f.Apply(x, y, b, 0, 1, s);
        sum += x;
    }
    check(sum == 20 && f.heldX == 0);
    s.debounce = 100;
    b = 1;
    x = y = 0;
    f.Apply(x, y, b, 0, 1000, s);
    check(b == 1);
    b = 2;
    f.Apply(x, y, b, 0, 1010, s);
    check(b == 2);
    b = 0x401;
    f.Apply(x, y, b, 0, 1020, s);
    check(b == 0x400);
    b = 2;
    f.Apply(x, y, b, 0, 1030, s);
    check(b == 0);
    b = 1;
    f.Apply(x, y, b, 0, 1200, s);
    check(b == 1);
    b=1;f.Apply(x,y,b,0,1201,s);check(b==0);
    b=2;f.Apply(x,y,b,0,1202,s);check(b==2);
    b=1;f.Apply(x,y,b,0,1203,s);check(b==0);
    b=1;f.Apply(x,y,b,0,1500,s);check(b==0);
    b=2;f.Apply(x,y,b,0,1501,s);check(b==0);
    x = 100;
    y = 200;
    b = 1;
    s.gain = 4;
    f.Apply(x, y, b, 1, 1201, s);
    check(x == 100 && y == 200 && b == 1);
    f.Clear();
    x = INT32_MAX;
    y = INT32_MIN;
    b = 0;
    f.Apply(x, y, b, 0, 1202, s);
    check(x == INT32_MAX && y == INT32_MIN);
    s.gain = NAN;
    check(Pointer::State::Validate(s).gain == 1);
    for (int i = 0; i < 100000; ++i)
    {
        x = (i % 9) - 4;
        y = (i % 5) - 2;
        b = 0;
        f.Apply(x, y, b, 0, static_cast<std::uint64_t>(i) + 2000, s);
        check(std::abs(f.residualX) < 1 && std::abs(f.residualY) < 1 && std::abs(f.heldX) < 8 &&
              std::abs(f.heldY) < 8);
    }
    std::cout << "Assistive Pointer: " << n << " offline assertions passed. No driver opened.\n";
    return 0;
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto *a = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        a = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        a->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
    }
    if (!a)
        return DefWindowProcW(window, message, w, l);
    switch (message)
    {
    case WM_CREATE:
        a->start = Ui::Button(window, L"Start", 1);
        a->stop = Ui::Button(window, L"Stop", 2);
        a->bypass = Ui::Button(window, L"Toggle bypass", 3);
        a->setup = Ui::Button(window, L"Driver setup", 4);
        a->mouse = Ui::Control(window, L"COMBOBOX", L"", 5, CBS_DROPDOWNLIST | WS_VSCROLL);
        a->axis = Ui::Control(window, L"COMBOBOX", L"", 6, CBS_DROPDOWNLIST);
        for (auto text : {L"Free motion", L"Horizontal only", L"Vertical only"})
            SendMessageW(a->axis, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(a->axis, CB_SETCURSEL, 0, 0);
        SendMessageW(a->mouse, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Choose a mouse - all inputs pass through"));
        SendMessageW(a->mouse, CB_SETCURSEL, 0, 0);
        a->gain = Ui::Control(window, TRACKBAR_CLASSW, L"", 7, TBS_NOTICKS);
        a->threshold = Ui::Control(window, TRACKBAR_CLASSW, L"", 8, TBS_NOTICKS);
        a->debounce = Ui::Control(window, TRACKBAR_CLASSW, L"", 9, TBS_NOTICKS);
        SendMessageW(a->gain, TBM_SETRANGE, TRUE, MAKELPARAM(5, 400));
        SendMessageW(a->gain, TBM_SETPOS, TRUE, static_cast<LPARAM>(a->settings.gain * 100));
        SendMessageW(a->threshold, TBM_SETRANGE, TRUE, MAKELPARAM(0, 8));
        SendMessageW(a->debounce, TBM_SETRANGE, TRUE, MAKELPARAM(0, 250));
        for (auto h :
             {a->start, a->stop, a->setup, a->bypass, a->mouse, a->axis, a->gain, a->threshold, a->debounce})
            Ui::Hand(h);
        RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12);
        SetTimer(window, 1, 100, nullptr);
        a->Layout();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case 1:
            a->Start();
            break;
        case 2:
            a->Stop();
            a->State(L"Stopped. Normal input passthrough is restored.");
            break;
        case 3:
            a->passthrough = !a->passthrough.load();
            break;
        case 4:
            Ui::Setup(window);
            break;
        case 5:
            if (HIWORD(w) == CBN_SELCHANGE)
            {
                const auto index = SendMessageW(a->mouse, CB_GETCURSEL, 0, 0);
                std::lock_guard<std::mutex> lock(a->mutex);
                a->target = index > 0 && index <= static_cast<LRESULT>(a->mice.size())
                                ? a->mice[static_cast<size_t>(index) - 1].id
                                : 0;
                a->selectedToken = index > 0 && index <= static_cast<LRESULT>(a->mice.size())
                                       ? a->mice[static_cast<size_t>(index) - 1].token
                                       : std::string{};
            }
            break;
        case 6:
            if (HIWORD(w) == CBN_SELCHANGE)
                a->Change();
            break;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_HSCROLL:
        a->Change();
        return 0;
    case WM_HOTKEY:
        a->Stop();
        a->State(L"Emergency stop. Capture closed; all input passes through.");
        return 0;
    case WM_SIZE:
        a->Layout();
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {Ui::Pixel(window, 950), Ui::Pixel(window, 850)};
        return 0;
    case WM_TIMER:
    case Updated:
        a->Refresh();
        return 0;
    case WM_DRAWITEM:
        Ui::DrawButton(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(w), Ui::Text);
        SetBkColor(reinterpret_cast<HDC>(w), Ui::Card);
        {
            static auto b = CreateSolidBrush(Ui::Card);
            return reinterpret_cast<LRESULT>(b);
        }
    case WM_PRINTCLIENT: {
        RECT r{};
        GetClientRect(window, &r);
        a->Paint(reinterpret_cast<HDC>(w), r.right, r.bottom);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        RECT r{};
        GetClientRect(window, &r);
        auto memory = CreateCompatibleDC(dc);
        auto bmp = CreateCompatibleBitmap(dc, std::max<LONG>(1, r.right), std::max<LONG>(1, r.bottom));
        auto old = SelectObject(memory, bmp);
        a->Paint(memory, r.right, r.bottom);
        BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bmp);
        DeleteDC(memory);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        auto *r = reinterpret_cast<RECT *>(l);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_CLOSE:
        a->Stop();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(window, 1);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w, l);
    }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    if (Ui::Command(L"--self-test") || Ui::Command(L"--offline"))
    {
        try
        {
            return Tests();
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    App app;
    app.preview = Ui::Command(L"--preview") || Ui::Command(L"--render-preview");
    app.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!app.stopEvent)
        return 2;
    if (app.preview)
    {
        app.status = L"PREVIEW DATA. No driver opened and no inputs intercepted or sent.";
        app.settings.gain = 0.5;
        app.reports = 147;
        app.inX = 20;
        app.inY = -12;
        app.outX = 10;
        app.outY = -6;
    }
    else
    {
        try
        {
            const auto status = Interceptor::Get_Status();
            app.status = status.version == IC_VERSION
                             ? L"Driver responds. Start and select a specific mouse to enable adjustments."
                             : L"Driver ABI mismatch. Install the matching driver.";
        }
        catch (const std::exception &e)
        {
            app.status = L"Matching driver unavailable: " + Ui::Wide(e.what()) + L". Use Driver setup.";
        }
    }
    WNDCLASSEXW c{sizeof(c)};
    c.lpfnWndProc = Procedure;
    c.hInstance = instance;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.lpszClassName = L"TitanAssistivePointer";
    RegisterClassExW(&c);
    HWND window = CreateWindowExW(0, c.lpszClassName, L"Assistive Pointer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1080, 900, nullptr, nullptr, instance, &app);
    if (!window)
        return 3;
    if (Ui::Command(L"--render-preview"))
    {
        app.Refresh();
        const bool ok = Ui::Capture(window, Ui::ExeDirectory() / L"Preview.bmp");
        DestroyWindow(window);
        return ok ? 0 : 4;
    }
    ShowWindow(window, show);
    if (Ui::Command(L"--start") && !app.preview)
        PostMessageW(window, WM_COMMAND, 1, 0);
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return static_cast<int>(m.wParam);
}
