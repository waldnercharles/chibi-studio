/* The Zep C binding (zep_bridge.h): type conversions and forwarding only. */
#include "editor/text/zep_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <zep/commands.h>
#include <zep/display.h>
#include <zep/editor.h>
#include <zep/filesystem.h>
#include <zep/mode.h>
#include <zep/mode_standard.h>
#include <zep/mode_vim.h>
#include <zep/range_markers.h>
#include <zep/syntax.h>
#include <zep/theme.h>
#include <zep/window.h>

#define SAME(c, zep) static_assert((int)(c) == (int)(zep))
#define SAME_COLOR(c, zep) SAME(ZEP_COLOR_##c, Zep::ThemeColor::zep)

SAME_COLOR(NONE, None);
SAME_COLOR(TAB_BORDER, TabBorder);
SAME_COLOR(HIDDEN_TEXT, HiddenText);
SAME_COLOR(TEXT, Text);
SAME_COLOR(TEXT_DIM, TextDim);
SAME_COLOR(BACKGROUND, Background);
SAME_COLOR(TAB_INACTIVE, TabInactive);
SAME_COLOR(TAB_ACTIVE, TabActive);
SAME_COLOR(LINE_NUMBER_BACKGROUND, LineNumberBackground);
SAME_COLOR(LINE_NUMBER, LineNumber);
SAME_COLOR(LINE_NUMBER_ACTIVE, LineNumberActive);
SAME_COLOR(CURSOR_NORMAL, CursorNormal);
SAME_COLOR(CURSOR_INSERT, CursorInsert);
SAME_COLOR(LIGHT, Light);
SAME_COLOR(DARK, Dark);
SAME_COLOR(VISUAL_SELECT_BACKGROUND, VisualSelectBackground);
SAME_COLOR(CURSOR_LINE_BACKGROUND, CursorLineBackground);
SAME_COLOR(AIRLINE_BACKGROUND, AirlineBackground);
SAME_COLOR(MODE, Mode);
SAME_COLOR(NORMAL, Normal);
SAME_COLOR(KEYWORD, Keyword);
SAME_COLOR(IDENTIFIER, Identifier);
SAME_COLOR(NUMBER, Number);
SAME_COLOR(STRING, String);
SAME_COLOR(COMMENT, Comment);
SAME_COLOR(WHITESPACE, Whitespace);
SAME_COLOR(HIDDEN_CHAR, HiddenChar);
SAME_COLOR(PARENTHESIS, Parenthesis);
SAME_COLOR(ERROR, Error);
SAME_COLOR(WARNING, Warning);
SAME_COLOR(INFO, Info);
SAME_COLOR(WIDGET_BORDER, WidgetBorder);
SAME_COLOR(WIDGET_BACKGROUND, WidgetBackground);
SAME_COLOR(WIDGET_ACTIVE, WidgetActive);
SAME_COLOR(WIDGET_INACTIVE, WidgetInactive);
SAME_COLOR(FLASH, FlashColor);
SAME_COLOR(CUSTOM, Custom);

SAME(ZEP_KEY_RETURN, Zep::ExtKeys::RETURN);
SAME(ZEP_KEY_ESCAPE, Zep::ExtKeys::ESCAPE);
SAME(ZEP_KEY_BACKSPACE, Zep::ExtKeys::BACKSPACE);
SAME(ZEP_KEY_LEFT, Zep::ExtKeys::LEFT);
SAME(ZEP_KEY_RIGHT, Zep::ExtKeys::RIGHT);
SAME(ZEP_KEY_UP, Zep::ExtKeys::UP);
SAME(ZEP_KEY_DOWN, Zep::ExtKeys::DOWN);
SAME(ZEP_KEY_TAB, Zep::ExtKeys::TAB);
SAME(ZEP_KEY_DELETE, Zep::ExtKeys::DEL);
SAME(ZEP_KEY_HOME, Zep::ExtKeys::HOME);
SAME(ZEP_KEY_END, Zep::ExtKeys::END);
SAME(ZEP_KEY_PAGE_DOWN, Zep::ExtKeys::PAGEDOWN);
SAME(ZEP_KEY_PAGE_UP, Zep::ExtKeys::PAGEUP);
SAME(ZEP_KEY_F1, Zep::ExtKeys::F1);
SAME(ZEP_KEY_F12, Zep::ExtKeys::F12);

SAME(ZEP_MOD_CTRL, Zep::ModifierKey::Ctrl);
SAME(ZEP_MOD_ALT, Zep::ModifierKey::Alt);
SAME(ZEP_MOD_SHIFT, Zep::ModifierKey::Shift);

SAME(ZEP_MODE_NONE, Zep::EditorMode::None);
SAME(ZEP_MODE_NORMAL, Zep::EditorMode::Normal);
SAME(ZEP_MODE_INSERT, Zep::EditorMode::Insert);
SAME(ZEP_MODE_VISUAL, Zep::EditorMode::Visual);
SAME(ZEP_MODE_EX, Zep::EditorMode::Ex);

SAME(ZEP_CHANGE_BEFORE, Zep::BufferMessageType::PreBufferChange);
SAME(ZEP_CHANGE_TEXT, Zep::BufferMessageType::TextChanged);
SAME(ZEP_CHANGE_DELETED, Zep::BufferMessageType::TextDeleted);
SAME(ZEP_CHANGE_ADDED, Zep::BufferMessageType::TextAdded);
SAME(ZEP_CHANGE_LOADED, Zep::BufferMessageType::Loaded);
SAME(ZEP_CHANGE_MARKERS, Zep::BufferMessageType::MarkersChanged);

SAME(ZEP_WINDOW_WHITESPACE, Zep::WindowFlags::ShowWhiteSpace);
SAME(ZEP_WINDOW_LINE_NUMBERS, Zep::WindowFlags::ShowLineNumbers);
SAME(ZEP_WINDOW_INDICATORS, Zep::WindowFlags::ShowIndicators);
SAME(ZEP_WINDOW_HIDE_SCROLLBAR, Zep::WindowFlags::HideScrollBar);
SAME(ZEP_WINDOW_WRAP, Zep::WindowFlags::WrapText);

SAME(ZEP_MARK_UNDERLINE, Zep::RangeMarkerDisplayType::Underline);
SAME(ZEP_MARK_BACKGROUND, Zep::RangeMarkerDisplayType::Background);
SAME(ZEP_MARK_TOOLTIP, Zep::RangeMarkerDisplayType::Tooltip);
SAME(ZEP_MARK_TOOLTIP_AT_LINE, Zep::RangeMarkerDisplayType::TooltipAtLine);
SAME(ZEP_MARK_CURSOR_TIP, Zep::RangeMarkerDisplayType::CursorTip);
SAME(ZEP_MARK_CURSOR_TIP_AT_LINE, Zep::RangeMarkerDisplayType::CursorTipAtLine);
SAME(ZEP_MARK_INDICATOR, Zep::RangeMarkerDisplayType::Indicator);

static_assert(sizeof(zep_rgba) == sizeof(Zep::NVec4f));
static_assert(offsetof(zep_rgba, r) == offsetof(Zep::NVec4f, x));
static_assert(offsetof(zep_rgba, a) == offsetof(Zep::NVec4f, w));

namespace {

namespace fs = std::filesystem;

zep_rgba rgba(const Zep::NVec4f &c)
{
    return { c.x, c.y, c.z, c.w };
}

size_t span(const uint8_t *begin, const uint8_t *end)
{
    return end ? (size_t)(end - begin) : std::strlen((const char *)begin);
}

class Font final : public Zep::ZepFont
{
public:
    Font(Zep::ZepDisplay &display, const zep_host *host)
      : Zep::ZepFont(display)
      , host(host)
    {
        m_pixelHeight = (int)Zep::DefaultTextSize;
    }

    void SetPixelHeight(int height) override
    {
        m_pixelHeight = height;
        InvalidateCharCache();
    }

    Zep::NVec2f GetTextSize(const uint8_t *begin, const uint8_t *end) const override
    {
        Zep::NVec2f size;
        host->text_size(host->user, (const char *)begin, span(begin, end), &size.x, &size.y);
        return size;
    }

private:
    const zep_host *host;
};

class Display final : public Zep::ZepDisplay
{
public:
    explicit Display(const zep_host *host)
      : host(host)
      , font(std::make_shared<Font>(*this, host))
    {
    }

    void DrawLine(const Zep::NVec2f &a, const Zep::NVec2f &b, const Zep::NVec4f &color, float width) const override
    {
        host->draw_line(host->user, a.x, a.y, b.x, b.y, rgba(color), width);
    }

    void DrawChars(
        Zep::ZepFont &,
        const Zep::NVec2f &at,
        const Zep::NVec4f &color,
        const uint8_t *begin,
        const uint8_t *end
    ) const override
    {
        host->draw_text(host->user, at.x, at.y, rgba(color), (const char *)begin, span(begin, end));
    }

    void DrawRectFilled(const Zep::NRectf &r, const Zep::NVec4f &color) const override
    {
        host->draw_rect(
            host->user,
            r.topLeftPx.x,
            r.topLeftPx.y,
            r.bottomRightPx.x,
            r.bottomRightPx.y,
            rgba(color)
        );
    }

    void SetClipRect(const Zep::NRectf &r) override
    {
        host->clip(
            host->user,
            r.topLeftPx.x,
            r.topLeftPx.y,
            r.bottomRightPx.x,
            r.bottomRightPx.y
        );
    }

    Zep::ZepFont &GetFont(Zep::ZepTextType) override { return *font; }

private:
    const zep_host *host;
    std::shared_ptr<Font> font;
};

/* Reads and writes nothing: text arrives through zep_set_text and leaves
   through the host's command callback. */
class FileSystem final : public Zep::IZepFileSystem
{
public:
    std::string Read(const fs::path &) override { return {}; }
    bool Write(const fs::path &, const void *, size_t) override
    {
        return false;
    }
    fs::path GetConfigPath() const override { return {}; }
    fs::path GetSearchRoot(const fs::path &, bool &found) const override
    {
        found = false;
        return {};
    }
    const fs::path &GetWorkingDirectory() const override { return root; }
    void SetWorkingDirectory(const fs::path &) override {}
    bool MakeDirectories(const fs::path &) override { return false; }
    bool IsDirectory(const fs::path &) const override { return false; }
    bool IsReadOnly(const fs::path &) const override { return true; }
    bool Exists(const fs::path &) const override { return false; }
    void ScanDirectory(const fs::path &, std::function<bool(const fs::path &, bool &)>) const override
    {
    }
    bool Equivalent(const fs::path &a, const fs::path &b) const override
    {
        return a == b;
    }
    fs::path Canonical(const fs::path &path) const override { return path; }
    void SetFlags(uint32_t) override {}

private:
    fs::path root;
};

/* Colors each byte as the host says. */
class Syntax final : public Zep::ZepSyntax
{
public:
    Syntax(Zep::ZepBuffer &buffer, const zep_host *host)
      : Zep::ZepSyntax(buffer)
      , host(host)
    {
        m_adornments.clear();
    }

    Zep::SyntaxResult GetSyntaxAt(const Zep::GlyphIterator &at) const override
    {
        Zep::SyntaxResult result;
        zep_rgba c;
        if (host->syntax(host->user, at.Index(), &c)) {
            result.foreground = Zep::ThemeColor::Custom;
            result.customForegroundColor = { c.r, c.g, c.b, c.a };
        }
        return result;
    }

    void UpdateSyntax() override {}
    void Notify(std::shared_ptr<Zep::ZepMessage>) override {}

private:
    const zep_host *host;
};

/* A mode whose undo and redo go to the host first. */
template <class Mode>
class HostHistory final : public Mode
{
public:
    HostHistory(Zep::ZepEditor &editor, const zep_host *host)
      : Mode(editor)
      , host(host)
    {
    }

    void Undo() override
    {
        if (!host->history(host->user, false)) {
            Mode::Undo();
        }
    }

    void Redo() override
    {
        if (!host->history(host->user, true)) {
            Mode::Redo();
        }
    }

private:
    const zep_host *host;
};

/* Forwards the editor's messages about its buffer to the host. */
class Client final : public Zep::ZepComponent
{
public:
    Client(Zep::ZepEditor &editor, const zep_host *host)
      : Zep::ZepComponent(editor)
      , host(host)
    {
    }

    void Notify(std::shared_ptr<Zep::ZepMessage> message) override
    {
        switch (message->messageId) {
            case Zep::Msg::Buffer: {
                auto change = std::static_pointer_cast<Zep::BufferMessage>(message);
                host->changed(
                    host->user,
                    (zep_change)change->type,
                    change->startLocation.Index(),
                    change->endLocation.Index()
                );
                break;
            }
            case Zep::Msg::HandleCommand:
                message->handled = host->command(host->user, message->str.c_str());
                break;
            case Zep::Msg::SetClipBoard:
                host->set_clipboard(host->user, message->str.c_str());
                message->handled = true;
                break;
            case Zep::Msg::GetClipBoard: {
                char *text = host->get_clipboard(host->user);
                message->handled = text != nullptr;
                message->str = text ? text : "";
                std::free(text);
                break;
            }
            default: break;
        }
    }

private:
    const zep_host *host;
};

} // namespace

struct zep_editor
{
    Zep::ZepEditor editor;
    Client client;
    Zep::ZepBuffer *buffer;

    explicit zep_editor(const zep_host *host)
      : editor(new Display(host), fs::path(), Zep::ZepEditorFlags::DisableThreads, new FileSystem)
      , client(editor, host)
      , buffer(editor.InitWithText("", ""))
    {
        buffer->SetSyntax(std::make_shared<Syntax>(*buffer, host));
        /* The host's modes replace Zep's, each while the other is current:
           switching modes reads the current one's window. */
        editor.RegisterGlobalMode(
            std::make_shared<HostHistory<Zep::ZepMode_Standard>>(editor, host)
        );
        editor.SetGlobalMode(Zep::ZepMode_Standard::StaticName());
        editor.RegisterGlobalMode(std::make_shared<HostHistory<Zep::ZepMode_Vim>>(editor, host));
    }

    Zep::ZepMode &mode() const { return *buffer->GetMode(); }
    Zep::ZepWindow &window() const { return *editor.GetActiveWindow(); }
    Zep::GlyphIterator at(long byte) const
    {
        return Zep::GlyphIterator(buffer, (unsigned long)byte);
    }
};

zep_editor *zep_create(const zep_host *host)
{
    return new zep_editor(host);
}

void zep_destroy(zep_editor *z)
{
    delete z;
}

void zep_set_text(zep_editor *z, const char *text)
{
    z->buffer->SetText(text);
}

size_t zep_text(const zep_editor *z, char *buffer, size_t size)
{
    /* The buffer ends in a NUL of its own. */
    std::string text = z->buffer->GetWorkingBuffer().string();
    size_t length = std::strlen(text.c_str());
    if (size) {
        std::snprintf(buffer, size, "%s", text.c_str());
    }
    return length;
}

void zep_clear_history(zep_editor *z)
{
    z->buffer->GetUndoStack() = {};
    z->buffer->GetRedoStack() = {};
}

void zep_begin_undo_group(zep_editor *z)
{
    z->mode().AddCommand(std::make_shared<Zep::ZepCommand_GroupMarker>(*z->buffer));
}

void zep_insert(zep_editor *z, long at, const char *text)
{
    z->mode().AddCommand(
        std::make_shared<Zep::ZepCommand_Insert>(*z->buffer, z->at(at), text)
    );
}

void zep_delete(zep_editor *z, long start, long end)
{
    z->mode().AddCommand(
        std::make_shared<Zep::ZepCommand_DeleteRange>(*z->buffer, z->at(start), z->at(end))
    );
}

void zep_replace(zep_editor *z, long start, long end, const char *text)
{
    z->mode().AddCommand(
        std::make_shared<Zep::ZepCommand_ReplaceRange>(
            *z->buffer,
            Zep::ReplaceRangeMode::Replace,
            z->at(start),
            z->at(end),
            text
        )
    );
}

void zep_undo(zep_editor *z)
{
    z->mode().Zep::ZepMode::Undo();
}

void zep_redo(zep_editor *z)
{
    z->mode().Zep::ZepMode::Redo();
}

long zep_cursor(const zep_editor *z)
{
    return z->window().GetBufferCursor().Index();
}

void zep_set_cursor(zep_editor *z, long byte)
{
    z->window().SetBufferCursor(z->at(byte));
}

void zep_cursor_rect(zep_editor *z, float *x, float *y, float *width, float *height)
{
    Zep::NVec2f at, size;
    z->window().GetCursorInfo(at, size);
    *x = at.x;
    *y = at.y;
    *width = size.x;
    *height = size.y;
}

void zep_select(zep_editor *z, long anchor, long cursor)
{
    z->mode().SelectRange(z->at(anchor), z->at(cursor));
}

bool zep_selection(const zep_editor *z, long *start, long *end)
{
    if (z->mode().GetEditorMode() != Zep::EditorMode::Visual) {
        return false;
    }
    Zep::GlyphRange range = z->mode().GetInclusiveVisualRange();
    if (!range.first.Valid()) {
        return false;
    }
    *start = range.first.Index();
    *end = range.second.Peek(1).Index();
    return true;
}

void zep_set_vim(zep_editor *z, bool vim)
{
    z->editor.SetGlobalMode(
        vim ? Zep::ZepMode_Vim::StaticName() : Zep::ZepMode_Standard::StaticName()
    );
}

zep_mode zep_current_mode(const zep_editor *z)
{
    return (zep_mode)z->mode().GetEditorMode();
}

void zep_set_command_text(zep_editor *z, const char *text)
{
    z->editor.SetCommandText(text);
}

void zep_press_key(zep_editor *z, uint32_t key, unsigned modifiers)
{
    z->mode().AddKeyPress(key, modifiers);
}

void zep_mouse_move(zep_editor *z, float x, float y)
{
    z->editor.OnMouseMove({ x, y });
}

void zep_mouse_down(zep_editor *z, float x, float y, int button)
{
    z->editor.OnMouseDown({ x, y }, (Zep::ZepMouseButton)button);
}

void zep_mouse_up(zep_editor *z, float x, float y, int button)
{
    z->editor.OnMouseUp({ x, y }, (Zep::ZepMouseButton)button);
}

void zep_mouse_wheel(zep_editor *z, float x, float y, float distance)
{
    z->editor.OnMouseWheel({ x, y }, distance);
}

void zep_set_region(zep_editor *z, float x0, float y0, float x1, float y1)
{
    z->editor.SetDisplayRegion({ x0, y0 }, { x1, y1 });
}

void zep_set_pixel_scale(zep_editor *z, float scale)
{
    z->editor.GetDisplay().SetPixelScale(Zep::NVec2f(scale));
}

void zep_set_font_height(zep_editor *z, int pixels)
{
    z->editor.GetDisplay().GetFont(Zep::ZepTextType::Text).SetPixelHeight(pixels);
    z->editor.GetDisplay().SetLayoutDirty();
}

void zep_set_color(zep_editor *z, zep_color role, zep_rgba color)
{
    z->editor.GetTheme()
        .SetColor((Zep::ThemeColor)role, { color.r, color.g, color.b, color.a });
}

void zep_set_window_flags(zep_editor *z, unsigned flags)
{
    z->window().SetWindowFlags(flags);
}

void zep_set_focused(zep_editor *z, bool focused)
{
    z->editor.isFocused = focused;
}

void zep_render(zep_editor *z)
{
    z->editor.Display();
}

long zep_hover_byte(const zep_editor *z)
{
    Zep::GlyphIterator at = z->window().GetMouseBufferLocation();
    return at.Valid() ? at.Index() : -1;
}

void zep_set_text_offset_x(zep_editor *z, float offset)
{
    z->window().SetTextOffsetX(offset);
}

void zep_text_region_x(const zep_editor *z, float *left, float *right)
{
    Zep::NVec2f x = z->window().GetTextRegionX();
    *left = x.x;
    *right = x.y;
}

void zep_hide_idle_command_line(zep_editor *z, bool hide)
{
    z->editor.GetConfig().autoHideCommandRegion = hide;
    z->editor.UpdateSize();
}

void zep_add_marker(zep_editor *z, long start, long end, unsigned display, zep_color color, const char *message)
{
    auto marker = std::make_shared<Zep::RangeMarker>(*z->buffer);
    marker->SetRange({ start, end });
    marker->displayType = display;
    marker->SetColors(Zep::ThemeColor::Background, Zep::ThemeColor::Text, (Zep::ThemeColor)color);
    marker->SetDescription(message);
    z->buffer->AddRangeMarker(marker);
}

void zep_clear_markers(zep_editor *z)
{
    z->buffer->ClearRangeMarkers(Zep::RangeMarkerType::Mark);
}
