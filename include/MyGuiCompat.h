#pragma once
// -----------------------------------------------------------------------------
// MiniMap - compatibility layer over the MyGUI SDK headers.
//
// The MyGUI headers that ship with the SDK (Include/mygui) do NOT reproduce the
// memory layout of the MyGUI classes inside the game. Measured on the shipped
// Kenshi_x64.exe (the game's own code adjusts `this` before every call):
//
//     base class          engine    SDK header
//     ICroppedRectangle   0x008     0x008   (ok)
//     WidgetInput         0x080     0x070   (does not match)
//     SkinItem            0x288     0x1E0   (does not match)
//
// The two mismatches matter for every *inherited non-virtual* member function:
// the compiler computes `this = (char*)widget + <SDK offset>`, so
//   * setNeedMouseFocus() wrote one byte 16 bytes early, i.e. into the widget's
//     user-data block instead of its input block. That corrupts a std::map and
//     crashes the game later (this was the crash after a marker was dropped);
//   * getSubWidgetMain() read 0xA8 bytes early and returned null, which is why
//     the player arrow never rotated.
//
// Every call that has to cross one of those bases goes through the helpers
// below: they rebuild the pointer using the *engine's* offset and then call the
// function through a pointer whose static type is already the base class, so the
// compiler adds no adjustment of its own.
//
// Members declared in the class we call them on (Widget::setCoord,
// ImageBox::setItemName, TextBox::setCaption, ...) need none of this: the object
// pointer is passed through unchanged.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <new>
#include <string>

#include <mygui/MyGUI_ICroppedRectangle.h>
#include <mygui/MyGUI_ImageBox.h>
#include <mygui/MyGUI_ISubWidgetRect.h>
#include <mygui/MyGUI_SkinItem.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_UString.h>
#include <mygui/MyGUI_Widget.h>
#include <mygui/MyGUI_WidgetInput.h>

namespace MyGuiCompat
{
    // Offsets of the base-class sub-objects inside the *engine's* MyGUI::Widget.
    const std::size_t kCroppedRectangleOffset = 0x008;
    const std::size_t kWidgetInputOffset      = 0x080;
    const std::size_t kSkinItemOffset         = 0x288;

    inline char* Raw(MyGUI::Widget* widget)
    {
        return reinterpret_cast<char*>(widget);
    }

    inline MyGUI::ICroppedRectangle* Cropped(MyGUI::Widget* widget)
    {
        return reinterpret_cast<MyGUI::ICroppedRectangle*>(Raw(widget) + kCroppedRectangleOffset);
    }

    inline MyGUI::WidgetInput* Input(MyGUI::Widget* widget)
    {
        return reinterpret_cast<MyGUI::WidgetInput*>(Raw(widget) + kWidgetInputOffset);
    }

    inline MyGUI::SkinItem* SkinItem(MyGUI::Widget* widget)
    {
        return reinterpret_cast<MyGUI::SkinItem*>(Raw(widget) + kSkinItemOffset);
    }

    // widget->setNeedMouseFocus(value), with the engine's base offset.
    inline void SetNeedMouseFocus(MyGUI::Widget* widget, bool value)
    {
        if (widget)
            Input(widget)->setNeedMouseFocus(value);
    }

    // widget->getSubWidgetMain(), with the engine's base offset. Returns the
    // main texture sub-widget (a MyGUI::RotatingSkin for skin "RotatingSkin").
    inline MyGUI::ISubWidgetRect* SubWidgetMain(MyGUI::Widget* widget)
    {
        return widget ? SkinItem(widget)->getSubWidgetMain() : nullptr;
    }

    // Geometry accessors live in ICroppedRectangle; the offset happens to match,
    // but they are routed through here as well so there is a single place that
    // knows about the engine's layout.
    inline int AbsoluteLeft(MyGUI::Widget* widget)
    {
        return widget ? Cropped(widget)->getAbsoluteLeft() : 0;
    }

    inline int AbsoluteTop(MyGUI::Widget* widget)
    {
        return widget ? Cropped(widget)->getAbsoluteTop() : 0;
    }

    inline int Width(MyGUI::Widget* widget)
    {
        return widget ? Cropped(widget)->getWidth() : 0;
    }

    inline int Height(MyGUI::Widget* widget)
    {
        return widget ? Cropped(widget)->getHeight() : 0;
    }

    // MyGUI::UString is only created and destroyed inside the engine DLL, but its
    // *size* is decided by the header we compile against. An instance of ours
    // would therefore be smaller than the engine's and the engine's constructor
    // would write past the end of it (stack corruption). Instances are built in
    // an oversized, aligned buffer instead, using the engine's own ctor / dtor.
    class UStringHolder
    {
    public:
        explicit UStringHolder(const std::string& utf8)
        {
            new (m_buffer) MyGUI::UString(utf8);
        }

        ~UStringHolder()
        {
            Get().~UString();
        }

        UStringHolder(const UStringHolder&) = delete;
        UStringHolder& operator=(const UStringHolder&) = delete;

        MyGUI::UString& Get()
        {
            return *reinterpret_cast<MyGUI::UString*>(m_buffer);
        }

    private:
        // A MyGUI::UString is a std::string plus a buffer type and two words;
        // 256 bytes is far more than enough and costs nothing on the stack.
        alignas(16) unsigned char m_buffer[256];
    };

    // First machine word of an object = its vtable pointer. Two objects of the
    // same MyGUI class (created by the same engine DLL) share it, which is a
    // safe way to ask "is this widget really a TextBox?" without the type-name
    // API that the trimmed SDK headers do not declare.
    inline const void* VTable(const void* object)
    {
        return object ? *reinterpret_cast<const void* const*>(object) : nullptr;
    }
}
