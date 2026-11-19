#include "windowframe.h"

#include <QQuickItem>
#include <QVariant>
#include <QWindow>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

WindowFrame::WindowFrame(QWindow& window)
    : m_window(window),
      m_hwnd(reinterpret_cast<HWND>(window.winId())),
      m_dragArea(window.property("titleDragArea").value<QQuickItem*>()),
      m_maximizeButton(window.property("maximizeButton").value<QQuickItem*>()) {
    SetWindowSubclass(m_hwnd, subclass, 0, reinterpret_cast<DWORD_PTR>(this));

    // Qt lays out a frameless client surface. Windows still owns the window states and snapping.
    const auto style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_OVERLAPPEDWINDOW);

    // Windows 11 attributes are missing from some MinGW SDK headers.
    constexpr DWORD cornerPreference = 33;
    constexpr DWORD borderColor = 34;
    constexpr DWORD roundCorners = 2;
    constexpr DWORD noBorderColor = 0xFFFFFFFE;
    DwmSetWindowAttribute(m_hwnd, cornerPreference, &roundCorners, sizeof(roundCorners));
    DwmSetWindowAttribute(m_hwnd, borderColor, &noBorderColor, sizeof(noBorderColor));

    // Keep the system's resizable window styles and recalculate the client area.
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

WindowFrame::~WindowFrame() {
    RemoveWindowSubclass(m_hwnd, subclass, 0);
}

void WindowFrame::toggleMaximized() {
    PostMessageW(m_hwnd, WM_SYSCOMMAND, IsZoomed(m_hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
}

LRESULT CALLBACK WindowFrame::subclass(HWND, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    return reinterpret_cast<WindowFrame*>(data)->handleMessage(message, wParam, lParam);
}

LRESULT WindowFrame::hitTest(LPARAM coords) const {
    const POINT screen{GET_X_LPARAM(coords), GET_Y_LPARAM(coords)};
    if (!IsZoomed(m_hwnd)) {
        RECT frame{};
        GetWindowRect(m_hwnd, &frame);
        const UINT dpi = GetDpiForWindow(m_hwnd);
        const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int horizontal = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padding;
        const int vertical = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padding;
        const bool left = screen.x < frame.left + horizontal;
        const bool right = screen.x >= frame.right - horizontal;
        if (screen.y < frame.top + vertical) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (screen.y >= frame.bottom - vertical) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }

    POINT client = screen;
    ScreenToClient(m_hwnd, &client);
    const qreal scale = m_window.devicePixelRatio();
    const QPointF position(client.x / scale, client.y / scale);
    if (m_maximizeButton->contains(m_maximizeButton->mapFromScene(position))) return HTMAXBUTTON;
    if (m_dragArea->contains(m_dragArea->mapFromScene(position))) return HTCAPTION;
    return HTCLIENT;
}

void WindowFrame::updateMaximizeButton(bool hovered, bool pressed) {
    if (hovered != m_maxHovered) {
        m_maxHovered = hovered;
        m_maximizeButton->setProperty("nativeHovered", hovered);
    }
    if (pressed != m_maxPressed) {
        m_maxPressed = pressed;
        m_maximizeButton->setProperty("nativePressed", pressed);
    }
}

LRESULT WindowFrame::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_GETMINMAXINFO: {
            const auto result = DefSubclassProc(m_hwnd, message, wParam, lParam);
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                limits.ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                                        monitor.rcWork.top - monitor.rcMonitor.top};
                limits.ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                                    monitor.rcWork.bottom - monitor.rcWork.top};
            }
            return result;
        }
        case WM_NCCALCSIZE:
            if (wParam && IsZoomed(m_hwnd)) {
                auto& client = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0];
                MONITORINFO monitor{sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromRect(&client, MONITOR_DEFAULTTONEAREST), &monitor))
                    client = monitor.rcWork;
            }
            return 0;
        case WM_NCHITTEST:
            return hitTest(lParam);
        case WM_NCMOUSEMOVE:
            if (wParam == HTMAXBUTTON && !m_maxHovered) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE | TME_NONCLIENT, m_hwnd, 0};
                TrackMouseEvent(&tracking);
                updateMaximizeButton(true, false);
            } else if (wParam != HTMAXBUTTON) {
                updateMaximizeButton(false, false);
            }
            break;  // Let Windows handle the non-client hover, including Snap Layouts.
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTMAXBUTTON) {
                updateMaximizeButton(true, true);
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (wParam == HTMAXBUTTON) {
                const bool activate = m_maxPressed;
                updateMaximizeButton(false, false);
                if (activate) toggleMaximized();
                return 0;
            }
            break;
        case WM_NCMOUSELEAVE:
        case WM_CANCELMODE:
            updateMaximizeButton(false, false);
            break;
    }
    return DefSubclassProc(m_hwnd, message, wParam, lParam);
}
