/* Real Win16 GDI regression: retained window pixels must survive moves and
 * resizes, without an extra InvalidateRect that would hide broken blits. */
#include <windows.h>

static int step, failures;
static HFILE report;
static const COLORREF colors[4] = {RGB(128,0,0), RGB(0,128,0),
                                  RGB(0,0,128), RGB(128,0,128)};

static void log_line(LPCSTR text) {
    _lwrite(report, text, lstrlen(text));
}

static void check_pixels(HWND window) {
    RECT r;
    HDC dc = GetDC(window);
    int x, y;
    GetClientRect(window, &r);
    for (y = 8; y < r.bottom; y += 16)
        for (x = 8; x < r.right; x += 16)
            if (GetPixel(dc, x, y) != colors[(y / 16) & 3]) {
                failures++;
                ReleaseDC(window, dc);
                log_line("FAIL retained client pixels\r\n");
                return;
            }
    ReleaseDC(window, dc);
    log_line("PASS retained client pixels\r\n");
}

LRESULT FAR PASCAL WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    PAINTSTRUCT paint;
    RECT r, band;
    HDC dc;
    HBRUSH brush;
    int y;
    switch (message) {
    case WM_CREATE:
        report = _lcreat("C:\\DRAWTEST.LOG", 0);
        log_line("WIN16 GDI retained-pixel regression\r\n");
        SetTimer(window, 1, 1200, NULL);
        return 0;
    case WM_PAINT:
        dc = BeginPaint(window, &paint);
        GetClientRect(window, &r);
        for (y = 0; y < r.bottom; y += 16) {
            SetRect(&band, 0, y, r.right, y + 16);
            brush = CreateSolidBrush(colors[(y / 16) & 3]);
            FillRect(dc, &band, brush);
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    case WM_TIMER:
        if (step <= 4) check_pixels(window);
        switch (step++) {
        case 0: MoveWindow(window, 110, 90, 470, 310, TRUE); break;
        case 1: MoveWindow(window, 40, 40, 310, 240, TRUE); break;
        case 2: MoveWindow(window, 190, 150, 400, 310, TRUE); break;
        case 3: ShowWindow(window, SW_MINIMIZE); ShowWindow(window, SW_RESTORE); break;
        case 4:
            SetWindowText(window, failures ? "DRAWTEST FAIL" : "DRAWTEST PASS");
            log_line(failures ? "RESULT FAIL\r\n" : "RESULT PASS\r\n");
            break;
        case 5: DestroyWindow(window); break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        log_line("COMPLETE\r\n");
        _lclose(report);
        PostQuitMessage(failures ? 1 : 0);
        return 0;
    }
    return DefWindowProc(window, message, wp, lp);
}

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR args, int show) {
    WNDCLASS cls;
    MSG msg;
    HWND window;
    (void)args;
    if (!previous) {
        cls.style = 0;
        cls.lpfnWndProc = WindowProc;
        cls.cbClsExtra = cls.cbWndExtra = 0;
        cls.hInstance = instance;
        cls.hIcon = NULL;
        cls.hCursor = LoadCursor(NULL, IDC_ARROW);
        cls.hbrBackground = NULL;
        cls.lpszMenuName = NULL;
        cls.lpszClassName = "CiukiDrawTest";
        if (!RegisterClass(&cls)) return 2;
    }
    window = CreateWindow("CiukiDrawTest", "CiukiOS window redraw test",
        WS_OVERLAPPEDWINDOW, 40, 40, 380, 280, NULL, NULL, instance, NULL);
    if (!window) return 3;
    ShowWindow(window, show);
    UpdateWindow(window);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}
