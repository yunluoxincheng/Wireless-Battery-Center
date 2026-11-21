#pragma once

#include <QObject>
#include <windows.h>

class QQuickItem;
class QWindow;

class WindowFrame final : public QObject {
    Q_OBJECT

public:
    explicit WindowFrame(QWindow& window);
    ~WindowFrame();
    WindowFrame(const WindowFrame&) = delete;
    WindowFrame& operator=(const WindowFrame&) = delete;

public slots:
    void toggleMaximized();
    void updateBackdrop();

private:
    static LRESULT CALLBACK subclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR id, DWORD_PTR data);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT hitTest(LPARAM point) const;
    void updateMaximizeButton(bool hovered, bool pressed);

    QWindow& m_window;
    HWND m_hwnd;
    QQuickItem* m_dragArea;
    QQuickItem* m_maximizeButton;
    bool m_maxHovered = false;
    bool m_maxPressed = false;
};
