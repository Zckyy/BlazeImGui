// =============================================================================
// blaze/icons.h - Icon glyphs from the Windows system icon font.
//
// BlazeImGui merges "Segoe Fluent Icons" (Windows 11, SegoeIcons.ttf) or
// "Segoe MDL2 Assets" (Windows 10, segmdl2.ttf) into every UI font, so no icon
// assets need to be shipped. Both fonts share the codepoints used below.
//
// Usage:   ImGui::Text("%s Settings", blaze::icons::Settings);
//          blaze::RegisterPanel({ .id="player", .icon=blaze::icons::Person, ... });
//
// Adding an icon: find the codepoint in Microsoft's "Segoe Fluent Icons font"
// docs and add a line `inline constexpr Icon Name{0xE000};`.
// If the icon font fails to load, icons render as empty strings (never "?").
// =============================================================================
#pragma once

namespace blaze::icons {

// UTF-8 encoded Private-Use-Area codepoint (U+E000..U+F8FF are all 3-byte).
struct Icon {
    char s[4];
    constexpr explicit Icon(unsigned cp)
        : s{ char(0xE0 | (cp >> 12)), char(0x80 | ((cp >> 6) & 0x3F)), char(0x80 | (cp & 0x3F)), 0 } {}
    constexpr operator const char*() const { return s; }
    constexpr const char* c_str() const { return s; }
};

// ---- Navigation / chrome ----------------------------------------------------
inline constexpr Icon Home{0xE80F};
inline constexpr Icon Settings{0xE713};
inline constexpr Icon Close{0xE8BB};
inline constexpr Icon ChevronRight{0xE76C};
inline constexpr Icon ChevronDown{0xE70D};
inline constexpr Icon Search{0xE721};
inline constexpr Icon Pin{0xE718};
inline constexpr Icon More{0xE712};
inline constexpr Icon Info{0xE946};
inline constexpr Icon Help{0xE897};

// ---- Theme -------------------------------------------------------------------
inline constexpr Icon Sun{0xE706};
inline constexpr Icon Moon{0xE708};
inline constexpr Icon Color{0xE790};
inline constexpr Icon Brush{0xE771};
inline constexpr Icon View{0xE890};
inline constexpr Icon Hide{0xED1A};

// ---- Performance -----------------------------------------------------------
inline constexpr Icon Speed{0xEC4A};
inline constexpr Icon Diagnostic{0xE9D9};
inline constexpr Icon Chart{0xE9D2};
inline constexpr Icon Clock{0xE917};
inline constexpr Icon Processing{0xE9F5};
inline constexpr Icon Memory{0xEEA0};
inline constexpr Icon Monitor{0xE7F4};
inline constexpr Icon Flash{0xE945};

// ---- Debug -------------------------------------------------------------------
inline constexpr Icon Bug{0xEBE8};
inline constexpr Icon Code{0xE943};
inline constexpr Icon Console{0xE756};
inline constexpr Icon List{0xEA37};
inline constexpr Icon Filter{0xE71C};
inline constexpr Icon Warning{0xE7BA};
inline constexpr Icon Error{0xEA39};
inline constexpr Icon Checkmark{0xE73E};
inline constexpr Icon Play{0xE768};
inline constexpr Icon Pause{0xE769};
inline constexpr Icon Stop{0xE71A};
inline constexpr Icon Refresh{0xE72C};
inline constexpr Icon Trash{0xE74D};
inline constexpr Icon Copy{0xE8C8};
inline constexpr Icon Lightbulb{0xEA80};
inline constexpr Icon Globe{0xE774};
inline constexpr Icon Person{0xE77B};
inline constexpr Icon Target{0xE81D}; // "Location"
inline constexpr Icon Camera{0xE722};
inline constexpr Icon Game{0xE7FC};
inline constexpr Icon Keyboard{0xE765};

// ---- Files / config ------------------------------------------------------------
inline constexpr Icon Save{0xE74E};
inline constexpr Icon SaveAs{0xE792};
inline constexpr Icon Folder{0xE8B7};
inline constexpr Icon OpenFolder{0xED25};
inline constexpr Icon Document{0xE8A5};
inline constexpr Icon Add{0xE710};
inline constexpr Icon Edit{0xE70F};
inline constexpr Icon Upload{0xE898};
inline constexpr Icon Download{0xE896};
inline constexpr Icon Undo{0xE7A7};

} // namespace blaze::icons
