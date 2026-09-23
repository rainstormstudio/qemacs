/*
 * MS Windows driver for QEmacs
 *
 * Copyright (c) 2002 Fabrice Bellard.
 * Copyright (c) 2002-2024 Charlie Gordon.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qe.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

extern int main1(int argc, char **argv);

#define PROG_NAME L"qemacs"

static HINSTANCE win_instance;
static int win_show_command;

typedef struct WinWindow {
    HWND w;
    HDC hdc;
    QEmacsState *qs;
    QEditScreen *screen;
    WCHAR high_surrogate;
    int suppress_char;
} WinWindow;

typedef struct QEEventQ {
    QEEvent ev;
    struct QEEventQ *next;
} QEEventQ;

static WinWindow win_ctx;
static QEEventQ *first_event, *last_event, *free_events;

/* CommandLineToArgvW follows the Windows quote/backslash rules, including
 * filenames containing spaces. QEmacs uses UTF-8 paths and command strings. */
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous,
                   LPSTR command_line, int show_command)
{
    LPWSTR *wide_argv;
    char **argv;
    int argc, i, status;

    (void)previous;
    (void)command_line;
    win_instance = instance;
    win_show_command = show_command;
    wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv)
        return 1;
    argv = qe_mallocz_array(char *, argc + 1);
    if (!argv) {
        LocalFree(wide_argv);
        return 1;
    }
    for (i = 0; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1,
                                      NULL, 0, NULL, NULL);
        if (!len || !(argv[i] = qe_malloc_array(char, len)))
            break;
        if (!WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1,
                                 argv[i], len, NULL, NULL))
            break;
    }
    LocalFree(wide_argv);
    status = i == argc ? main1(argc, argv) : 1;
    for (i = 0; i < argc; i++)
        qe_free(&argv[i]);
    qe_free(&argv);
    return status;
}

static int win_probe(void)
{
    return !force_tty;
}

static COLORREF win_color(QEColor color)
{
    return RGB(QERGB_RED(color), QERGB_GREEN(color), QERGB_BLUE(color));
}

static int win_state(void)
{
    int state = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        state |= KEY_STATE_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        state |= KEY_STATE_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) {
        /* AltGr is Right Alt + Control. It produces a character, not M-C-. */
        if (GetKeyState(VK_RMENU) & 0x8000)
            state &= ~KEY_STATE_CONTROL;
        else
            state |= KEY_STATE_META;
    }
    return state;
}

static void push_event(const QEEvent *ev)
{
    QEEventQ *e = free_events;
    if (e) {
        free_events = e->next;
    } else {
        e = qe_mallocz(QEEventQ);
        if (!e)
            return;
    }
    e->ev = *ev;
    e->next = NULL;
    if (last_event)
        last_event->next = e;
    else
        first_event = e;
    last_event = e;
}

static void push_key(int key, int state)
{
    QEEvent ev;
    qe_event_clear(&ev);
    ev.type = QE_KEY_EVENT;
    ev.key_event.shift = state;
    ev.key_event.key = get_modified_key(key, state);
    push_event(&ev);
}

static void win_character(unsigned int ch, int state)
{
    unsigned int high = win_ctx.high_surrogate;
    win_ctx.high_surrogate = 0;
    if (ch >= 0xD800 && ch <= 0xDBFF) {
        win_ctx.high_surrogate = ch;
        return;
    }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
        if (!high)
            return;
        ch = 0x10000 + ((high - 0xD800) << 10) + (ch - 0xDC00);
    }
    if (ch > 0x10FFFF)
        return;
    /* WM_CHAR has already translated Control-A..Z to control characters. */
    if (ch < 32 || ch == 127)
        state &= ~KEY_STATE_CONTROL;
    state &= ~KEY_STATE_SHIFT;
    push_key(ch, state);
}

static void win_mouse(UINT msg, WPARAM flags, LPARAM position)
{
    QEEvent ev;
    POINT pt;
    int button = QE_BUTTON_NONE;
    int state = 0;

    qe_event_clear(&ev);
    pt.x = GET_X_LPARAM(position);
    pt.y = GET_Y_LPARAM(position);
    if (flags & MK_SHIFT)
        state |= KEY_STATE_SHIFT;
    if (flags & MK_CONTROL)
        state |= KEY_STATE_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)
        state |= KEY_STATE_META;
    if (msg == WM_MOUSEWHEEL) {
        ScreenToClient(win_ctx.w, &pt);
        ev.type = QE_BUTTON_PRESS_EVENT;
        button = GET_WHEEL_DELTA_WPARAM(flags) > 0 ? QE_WHEEL_UP : QE_WHEEL_DOWN;
    } else
    if (msg == WM_MOUSEMOVE) {
        ev.type = QE_MOTION_EVENT;
        if (flags & MK_LBUTTON) button |= QE_BUTTON_LEFT;
        if (flags & MK_MBUTTON) button |= QE_BUTTON_MIDDLE;
        if (flags & MK_RBUTTON) button |= QE_BUTTON_RIGHT;
    } else {
        ev.type = (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                   msg == WM_RBUTTONDOWN) ? QE_BUTTON_PRESS_EVENT : QE_BUTTON_RELEASE_EVENT;
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) button = QE_BUTTON_LEFT;
        if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) button = QE_BUTTON_MIDDLE;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) button = QE_BUTTON_RIGHT;
    }
    ev.button_event.shift = state;
    ev.button_event.x = pt.x;
    ev.button_event.y = pt.y;
    ev.button_event.button = button;
    push_event(&ev);
}

static LRESULT CALLBACK qe_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    QEditScreen *s = win_ctx.screen;
    QEEvent ev;
    int key, state;

    switch (msg) {
    case WM_CREATE:
        win_ctx.w = hwnd;
        return 0;
    case WM_CLOSE:
        /* Use the normal editor exit path, including unsaved-buffer prompts. */
        push_key(KEY_QUIT, 0);
        push_key(KEY_EXIT, 0);
        return 0;
    case WM_DESTROY:
        win_ctx.w = NULL;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (s && wparam != SIZE_MINIMIZED && LOWORD(lparam) && HIWORD(lparam)) {
            s->width = LOWORD(lparam);
            s->height = HIWORD(lparam);
            qe_event_clear(&ev);
            ev.type = QE_EXPOSE_EVENT;
            push_event(&ev);
        }
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            /* Startup paints can precede qe_init's first do_refresh(). */
            if (s && s->qs && s->qs->first_window) {
                qe_event_clear(&ev);
                ev.type = QE_EXPOSE_EVENT;
                push_event(&ev);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        /* The full expose redraw paints the background and all text. */
        return 1;
    case WM_UNICHAR:
        if (wparam == UNICODE_NOCHAR)
            return TRUE;
        win_character((unsigned int)wparam, win_state());
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (win_ctx.suppress_char) {
            win_ctx.suppress_char = 0;
            return 0;
        }
        win_character((unsigned int)wparam, win_state());
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        state = win_state();
        key = -1;
        win_ctx.suppress_char = 0;
        if (msg == WM_SYSKEYDOWN && (wparam == VK_F4 || wparam == VK_SPACE))
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        switch (wparam) {
        case VK_BACK:   key = KEY_DEL; break;
        case VK_TAB:    key = KEY_TAB; break;
        case VK_RETURN: key = KEY_RET; break;
        case VK_ESCAPE: key = KEY_ESC; break;
        case VK_HOME:   key = KEY_HOME; break;
        case VK_END:    key = KEY_END; break;
        case VK_PRIOR:  key = KEY_PAGEUP; break;
        case VK_NEXT:   key = KEY_PAGEDOWN; break;
        case VK_LEFT:   key = KEY_LEFT; break;
        case VK_RIGHT:  key = KEY_RIGHT; break;
        case VK_UP:     key = KEY_UP; break;
        case VK_DOWN:   key = KEY_DOWN; break;
        case VK_INSERT: key = KEY_INSERT; break;
        case VK_DELETE: key = KEY_DELETE; break;
        case VK_F1: case VK_F2: case VK_F3: case VK_F4:
        case VK_F5: case VK_F6: case VK_F7: case VK_F8:
        case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        case VK_F13: case VK_F14: case VK_F15: case VK_F16:
        case VK_F17: case VK_F18: case VK_F19: case VK_F20:
            key = KEY_F1 + (int)wparam - VK_F1;
            break;
        case VK_SPACE:
            if (state & KEY_STATE_CONTROL) {
                key = ' ';
                state &= ~KEY_STATE_SHIFT;
            }
            break;
        default:
            /* Control-letter WM_CHAR is layout-dependent and can be absent
             * with Alt pressed. Handle it from the virtual key exactly once. */
            if ((state & KEY_STATE_CONTROL) && wparam >= 'A' && wparam <= 'Z') {
                key = 'a' + (int)wparam - 'A';
                state &= ~KEY_STATE_SHIFT;
            }
            break;
        }
        if (key >= 0) {
            win_ctx.suppress_char = 1;
            push_key(key, state);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
        win_mouse(msg, wparam, lparam);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

QEmacsState *win32_get_state(void)
{
    return win_ctx.qs;
}

int win32_pump_messages(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

int win32_get_event(QEditScreen *s, QEEvent *ev)
{
    QEEventQ *e = first_event;
    (void)s;
    if (!e)
        return 0;
    *ev = e->ev;
    first_event = e->next;
    if (!first_event)
        last_event = NULL;
    e->next = free_events;
    free_events = e;
    return 1;
}

static int win_init(QEditScreen *s, QEmacsState *qs, int w, int h)
{
    WNDCLASSW wc = { 0 };
    QEStyleDef style;
    TEXTMETRICW tm = { 0 };
    RECT rect;
    HFONT font, old_font;
    HDC dc;
    int width, height;

    s->qs = qs;
    s->priv_data = &win_ctx;
    s->media = CSS_MEDIA_SCREEN;
    s->charset = &charset_utf8;

    get_style(&style, 0, 0);
    dc = GetDC(NULL);
    if (!dc)
        return -1;
    font = CreateFontW(-MulDiv(max_int(style.font_size, 1), GetDeviceCaps(dc, LOGPIXELSY), 72),
                       0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font) {
        ReleaseDC(NULL, dc);
        return -1;
    }
    old_font = SelectObject(dc, font);
    if (!GetTextMetricsW(dc, &tm)) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        ReleaseDC(NULL, dc);
        return -1;
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    ReleaseDC(NULL, dc);

    width = w > 0 ? w : 80 * max_int(tm.tmAveCharWidth, 1);
    height = h > 0 ? h : 25 * max_int(tm.tmHeight, 1);
    s->width = width;
    s->height = height;
    s->clip_x1 = s->clip_y1 = 0;
    s->clip_x2 = width;
    s->clip_y2 = height;
    win_ctx.screen = s;
    win_ctx.qs = qs;

    wc.lpfnWndProc = qe_wnd_proc;
    wc.hInstance = win_instance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.lpszClassName = PROG_NAME;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        goto fail;
    SetRect(&rect, 0, 0, width, height);
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    win_ctx.w = CreateWindowW(PROG_NAME, PROG_NAME, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              NULL, NULL, win_instance, NULL);
    if (!win_ctx.w)
        goto fail;
    win_ctx.hdc = GetDC(win_ctx.w);
    if (!win_ctx.hdc) {
        DestroyWindow(win_ctx.w);
        goto fail;
    }
    SetBkMode(win_ctx.hdc, TRANSPARENT);
    ShowWindow(win_ctx.w, win_show_command);
    UpdateWindow(win_ctx.w);
    return 0;

fail:
    win_ctx.screen = NULL;
    win_ctx.qs = NULL;
    return -1;
}

static void win_close(QEditScreen *s)
{
    QEEvent ev;
    if (win_ctx.hdc) {
        ReleaseDC(win_ctx.w, win_ctx.hdc);
        win_ctx.hdc = NULL;
    }
    if (win_ctx.w)
        DestroyWindow(win_ctx.w);
    win_ctx.screen = NULL;
    win_ctx.qs = NULL;
    while (win32_get_event(s, &ev))
        continue;
    while (free_events) {
        QEEventQ *e = free_events;
        free_events = e->next;
        qe_free(&e);
    }
}

static void win_flush(QEditScreen *s)
{
    (void)s;
    GdiFlush();
}

static int win_is_user_input_pending(QEditScreen *s)
{
    MSG msg;
    (void)s;
    return first_event != NULL || PeekMessageW(&msg, NULL, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE)
        || PeekMessageW(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE);
}

static void win_fill_rectangle(QEditScreen *s, int x, int y, int w, int h, QEColor color)
{
    RECT rc;
    (void)s;
    if (w <= 0 || h <= 0)
        return;
    SetRect(&rc, x, y, x + w, y + h);
    SetDCBrushColor(win_ctx.hdc, win_color(color));
    FillRect(win_ctx.hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void win_xor_rectangle(QEditScreen *s, int x, int y, int w, int h, QEColor color)
{
    (void)s;
    (void)color;
    if (w > 0 && h > 0)
        PatBlt(win_ctx.hdc, x, y, w, h, DSTINVERT);
}

static QEFont *win_open_font(QEditScreen *s, int style, int size)
{
    QEFont *font;
    HFONT handle, old_font;
    TEXTMETRICW tm = { 0 };
    const WCHAR *family;
    int pitch;
    (void)s;

    if (style & QE_FONT_FALLBACK_MASK)
        return NULL;
    switch (style & QE_FONT_FAMILY_MASK) {
    case QE_FONT_FAMILY_SERIF: family = L"Times New Roman"; pitch = FF_ROMAN; break;
    case QE_FONT_FAMILY_SANS:  family = L"Segoe UI"; pitch = FF_SWISS; break;
    default: family = L"Consolas"; pitch = FIXED_PITCH | FF_MODERN; break;
    }
    handle = CreateFontW(-MulDiv(max_int(size, 1), GetDeviceCaps(win_ctx.hdc, LOGPIXELSY), 72),
                         0, 0, 0, (style & QE_FONT_STYLE_BOLD) ? FW_BOLD : FW_NORMAL,
                         !!(style & QE_FONT_STYLE_ITALIC),
                         !!(style & QE_FONT_STYLE_UNDERLINE),
                         !!(style & QE_FONT_STYLE_LINE_THROUGH), DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         pitch, family);
    if (!handle)
        return NULL;
    font = qe_mallocz(QEFont);
    if (!font) {
        DeleteObject(handle);
        return NULL;
    }
    old_font = SelectObject(win_ctx.hdc, handle);
    if (!GetTextMetricsW(win_ctx.hdc, &tm)) {
        SelectObject(win_ctx.hdc, old_font);
        DeleteObject(handle);
        qe_free(&font);
        return NULL;
    }
    SelectObject(win_ctx.hdc, old_font);
    font->ascent = tm.tmAscent;
    font->descent = tm.tmDescent;
    font->priv_data = handle;
    return font;
}

static void win_close_font(QEditScreen *s, QEFont **fontp)
{
    (void)s;
    DeleteObject((HFONT)(*fontp)->priv_data);
    qe_free(fontp);
}

/* Convert UTF-32 display runs to Win32's UTF-16 without losing supplementary
 * code points. The same representation is used for measuring and drawing. */
static WCHAR *win_utf16(const char32_t *str, int len, int *out_len,
                        WCHAR stack[256])
{
    WCHAR *buf;
    int i, j;
    if (len < 0 || len > (INT_MAX - 1) / 2)
        return NULL;
    buf = len <= 128 ? stack : qe_malloc_array(WCHAR, 2 * len);
    if (!buf)
        return NULL;
    for (i = j = 0; i < len; i++) {
        unsigned int ch = str[i];
        if (ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF))
            ch = 0xFFFD;
        if (ch >= 0x10000) {
            ch -= 0x10000;
            buf[j++] = 0xD800 + (ch >> 10);
            buf[j++] = 0xDC00 + (ch & 0x3FF);
        } else {
            buf[j++] = ch;
        }
    }
    *out_len = j;
    return buf;
}

static void win_text_metrics(QEditScreen *s, QEFont *font, QECharMetrics *metrics,
                             const char32_t *str, int len)
{
    HFONT old_font;
    WCHAR *buf;
    WCHAR stack[256];
    SIZE extent = { 0 };
    int units;
    (void)s;
    metrics->font_ascent = font->ascent;
    metrics->font_descent = font->descent;
    metrics->width = 0;
    if (len <= 0 || !(buf = win_utf16(str, len, &units, stack)))
        return;
    old_font = SelectObject(win_ctx.hdc, (HFONT)font->priv_data);
    if (GetTextExtentPoint32W(win_ctx.hdc, buf, units, &extent))
        metrics->width = extent.cx;
    SelectObject(win_ctx.hdc, old_font);
    if (buf != stack)
        qe_free(&buf);
}

static void win_draw_text(QEditScreen *s, QEFont *font, int x, int y,
                          const char32_t *str, int len, QEColor color)
{
    HFONT old_font;
    WCHAR *buf;
    WCHAR stack[256];
    int units;
    (void)s;
    if (len <= 0 || !(buf = win_utf16(str, len, &units, stack)))
        return;
    old_font = SelectObject(win_ctx.hdc, (HFONT)font->priv_data);
    SetTextColor(win_ctx.hdc, win_color(color));
    ExtTextOutW(win_ctx.hdc, x, y - font->ascent, 0, NULL, buf, units, NULL);
    if (font->style & (QE_FONT_STYLE_OVERLINE | QE_FONT_STYLE_BOX)) {
        SIZE extent;
        HBRUSH brush;
        if (GetTextExtentPoint32W(win_ctx.hdc, buf, units, &extent)
        &&  (brush = CreateSolidBrush(win_color(color))) != NULL) {
            RECT rc = { x, y - font->ascent - 1, x + extent.cx, y - font->ascent };
            FillRect(win_ctx.hdc, &rc, brush);
            if (font->style & QE_FONT_STYLE_BOX) {
                SetRect(&rc, x - 1, y - font->ascent - 1, x, y + font->descent + 1);
                FillRect(win_ctx.hdc, &rc, brush);
                OffsetRect(&rc, extent.cx + 1, 0);
                FillRect(win_ctx.hdc, &rc, brush);
                SetRect(&rc, x, y + font->descent, x + extent.cx, y + font->descent + 1);
                FillRect(win_ctx.hdc, &rc, brush);
            }
            DeleteObject(brush);
        }
    }
    SelectObject(win_ctx.hdc, old_font);
    if (buf != stack)
        qe_free(&buf);
}

static void win_set_clip(QEditScreen *s, int x, int y, int w, int h)
{
    (void)s;
    SelectClipRgn(win_ctx.hdc, NULL);
    IntersectClipRect(win_ctx.hdc, x, y, x + max_int(w, 0), y + max_int(h, 0));
}

static QEDisplay win32_dpy = {
    "win32", 1, 1,
    win_probe,
    win_init,
    win_close,
    win_flush,
    win_is_user_input_pending,
    win_fill_rectangle,
    win_xor_rectangle,
    win_open_font,
    win_close_font,
    win_text_metrics,
    win_draw_text,
    win_set_clip,
    NULL, /* dpy_selection_activate */
    NULL, /* dpy_selection_request */
    NULL, /* dpy_invalidate */
    NULL, /* dpy_cursor_at */
    NULL, /* dpy_bmp_alloc */
    NULL, /* dpy_bmp_free */
    NULL, /* dpy_bmp_draw */
    NULL, /* dpy_bmp_lock */
    NULL, /* dpy_bmp_unlock */
    NULL, /* dpy_draw_picture */
    NULL, /* dpy_full_screen */
    NULL, /* dpy_describe */
    NULL, /* dpy_sound_bell */
    NULL, /* dpy_suspend */
    qe_dpy_error, /* dpy_error */
    NULL, /* next */
};

static int win32_init(QEmacsState *qs)
{
    return qe_register_display(qs, &win32_dpy);
}

qe_module_init(win32_init);
