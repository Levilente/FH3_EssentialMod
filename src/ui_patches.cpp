// Runtime UI patches: the platform changes the game's own menu layout without anybody copying files around.
//
// The menus are XAML in game media\UI.zip. At startup the platform reads the original out of that zip, applies the
// edits below to the text, and writes the result under its own folder, <game>\mediaplatform\. media_overrides.cpp then
// mounts that folder over game:\media\ below the players' mediaoverride, so a player's own file still wins and
// nothing is ever added to mediaoverride. Delete mediaplatform and it's rebuilt next start.
//
// Each edit finds a tile by its x:Name and sets attributes on that one element. If the game's file isn't what an
// edit expects (an update, another build), the edit is skipped and logged, never guessed.
#include "platform.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fh3platform {
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- inflate (RFC 1951)

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t buf = 0;
    int cnt      = 0;
    bool bad     = false;
    int get(int n) {
        while (cnt < n) {
            if (p >= end) { bad = true; return 0; }
            buf |= static_cast<uint32_t>(*p++) << cnt;
            cnt += 8;
        }
        const int v = static_cast<int>(buf & ((1u << n) - 1));
        buf >>= n;
        cnt -= n;
        return v;
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

void build(Huffman& h, const uint8_t* length, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; ++i) ++h.count[length[i]];
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; ++i) offs[i + 1] = static_cast<uint16_t>(offs[i] + h.count[i]);
    for (int i = 0; i < n; ++i)
        if (length[i]) h.symbol[offs[length[i]]++] = static_cast<uint16_t>(i);
}

int decode(Bits& b, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= b.get(1);
        if (b.bad) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool inflate(const uint8_t* in, std::size_t size, std::vector<uint8_t>& out) {
    static const uint16_t lbase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint16_t lext[]  = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint16_t dext[]  = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    Bits b{in, in + size};
    int last = 0;
    do {
        last          = b.get(1);
        const int type = b.get(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0;
            b.cnt = 0;
            if (b.end - b.p < 4) return false;
            const unsigned len = b.p[0] | (b.p[1] << 8);
            b.p += 4;
            if (static_cast<std::size_t>(b.end - b.p) < len) return false;
            out.insert(out.end(), b.p, b.p + len);
            b.p += len;
            continue;
        }
        if (type == 3) return false;
        Huffman lencode, distcode;
        if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; ++i) l[i] = 8;
            for (; i < 256; ++i) l[i] = 9;
            for (; i < 280; ++i) l[i] = 7;
            for (; i < 288; ++i) l[i] = 8;
            build(lencode, l, 288);
            for (i = 0; i < 30; ++i) l[i] = 5;
            build(distcode, l, 30);
        } else {
            const int nlen = b.get(5) + 257, ndist = b.get(5) + 1, ncode = b.get(4) + 4;
            if (b.bad || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(b.get(3));
            Huffman codes;
            build(codes, lengths, 19);
            int index = 0;
            uint8_t all[320] = {};
            while (index < nlen + ndist) {
                const int sym = decode(b, codes);
                if (sym < 0) return false;
                if (sym < 16) {
                    all[index++] = static_cast<uint8_t>(sym);
                } else {
                    int len = 0, rep;
                    if (sym == 16) {
                        if (index == 0) return false;
                        len = all[index - 1];
                        rep = 3 + b.get(2);
                    } else if (sym == 17) {
                        rep = 3 + b.get(3);
                    } else {
                        rep = 11 + b.get(7);
                    }
                    if (index + rep > nlen + ndist) return false;
                    while (rep--) all[index++] = static_cast<uint8_t>(len);
                }
            }
            build(lencode, all, nlen);
            build(distcode, all + nlen, ndist);
        }
        for (;;) {
            int sym = decode(b, lencode);
            if (sym < 0) return false;
            if (sym < 256) {
                out.push_back(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                break;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                const int len  = lbase[sym] + b.get(lext[sym]);
                const int ds   = decode(b, distcode);
                if (ds < 0 || ds >= 30) return false;
                const std::size_t dist = dbase[ds] + b.get(dext[ds]);
                if (b.bad || dist > out.size()) return false;
                for (int i = 0; i < len; ++i) out.push_back(out[out.size() - dist]);
            }
        }
    } while (!last);
    return true;
}

// ---------------------------------------------------------------- reading one entry out of a zip

bool read_file(const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return u16(p) | (static_cast<uint32_t>(u16(p + 2)) << 16); }

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (std::tolower(static_cast<unsigned char>(x)) != std::tolower(static_cast<unsigned char>(y))) return false;
    }
    return true;
}

bool zip_entry(const std::vector<uint8_t>& z, const std::string& name, std::string& text) {
    if (z.size() < 22) return false;
    std::size_t eocd = std::string::npos;
    for (std::size_t i = z.size() - 22;; --i) {
        if (u32(&z[i]) == 0x06054B50u) { eocd = i; break; }
        if (i == 0 || z.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) return false;
    const std::size_t entries = u16(&z[eocd + 10]);
    std::size_t at            = u32(&z[eocd + 16]);
    for (std::size_t n = 0; n < entries; ++n) {
        if (at + 46 > z.size() || u32(&z[at]) != 0x02014B50u) return false;
        const unsigned method = u16(&z[at + 10]);
        const std::size_t csize = u32(&z[at + 20]), usize = u32(&z[at + 24]);
        const std::size_t nlen = u16(&z[at + 28]), elen = u16(&z[at + 30]), clen = u16(&z[at + 32]);
        const std::size_t local = u32(&z[at + 42]);
        if (at + 46 + nlen > z.size()) return false;
        const std::string entry(reinterpret_cast<const char*>(&z[at + 46]), nlen);
        at += 46 + nlen + elen + clen;
        if (!iequals(entry, name)) continue;
        if (local + 30 > z.size() || u32(&z[local]) != 0x04034B50u) return false;
        const std::size_t data = local + 30 + u16(&z[local + 26]) + u16(&z[local + 28]);
        if (data + csize > z.size()) return false;
        std::vector<uint8_t> raw;
        if (method == 0) raw.assign(z.begin() + data, z.begin() + data + csize);
        else if (method == 8) {
            raw.reserve(usize);
            if (!inflate(&z[data], csize, raw)) return false;
        } else return false;
        if (raw.size() != usize) return false;
        text.assign(raw.begin(), raw.end());
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the edits

using Attrs = std::initializer_list<std::pair<const char*, const char*>>;

// Sets attributes on the element that carries x:Name="<name>". Fails (leaving the text alone) if the tile or one of
// its attributes isn't there.
bool patch_element(std::string& xaml, const std::string& key, Attrs attrs) {
    const std::size_t at  = xaml.find(key);
    if (at == std::string::npos) return false;
    const std::size_t open  = xaml.rfind("<local:", at);
    const std::size_t close = xaml.find("/>", at);
    if (open == std::string::npos || close == std::string::npos) return false;
    std::string element = xaml.substr(open, close - open);
    for (const auto& a : attrs) {
        const std::string attr = std::string(" ") + a.first + "=\"";
        const std::size_t pos  = element.find(attr);
        if (pos == std::string::npos) return false;
        const std::size_t v    = pos + attr.size();
        const std::size_t q    = element.find('"', v);
        if (q == std::string::npos) return false;
        element.replace(v, q - v, a.second);
    }
    xaml.replace(open, close - open, element);
    return true;
}

bool patch_tile(std::string& xaml, const char* name, Attrs attrs) {
    return patch_element(xaml, std::string("x:Name=\"") + name + "\"", attrs);
}

constexpr const char* kTileState = "local:TileMenuNavigationManager.TileState";
constexpr const char* kGridX     = "local:TileMenuNavigationManager.GridX";
constexpr const char* kGridY     = "local:TileMenuNavigationManager.GridY";
constexpr const char* kStyle2x1  = "{StaticResource A_Style_Button_Tile_2x1}";
constexpr const char* kSmallTile = "{StaticResource A_ControlTemplate_SmallHorizontalIconTile}";

// The Home tab: the Online Adventure / solo tile (bottom left) is halved, and the game's own Exit to Desktop tile
// (kept in the file for the PC version) goes under it, enabled.
std::string pause_menu(std::string xaml, std::string& report) {
    const auto step = [&](const char* what, bool ok) { report += std::string(ok ? " +" : " -") + what; };
    step("online-tile", patch_tile(xaml, "PART_Button_HorizonOnline", {{"Style", kStyle2x1}, {"Template", kSmallTile}}));
    step("solo-tile", patch_tile(xaml, "PART_Button_HorizonSolo", {{"Style", kStyle2x1}, {"Template", kSmallTile}}));
    step("exit-tile", patch_tile(xaml, "PART_Button_ExitToDesktop", {{kTileState, "Enabled"}, {kGridX, "0"}, {kGridY, "3"}}));
    // The Horizon Group tile (needs the online services) becomes the platform's MODS tile: enabled, and named.
    // Pressing it is taken over in mods_page.cpp; what it opens is set by the state-flow patch below.
    step("mods-tile", patch_tile(xaml, "PART_Button_GameParty", {{kTileState, "Enabled"}}));
    step("mods-title", patch_element(xaml, "x:Key=\"A_MenuDataItem_GameParty\"", {{"Heading", "MODS"}}));
    return xaml;
}

// Pause > "online_party_list" (what the Horizon Group tile fires) opens, instead of the
// party list: the HUD options page, which settings.cpp turns into Installed mods.
std::string pause_flow(std::string xml, std::string& report) {
    const char* const old_node =
        "<node id=\"online_party_list\" state=\"ui_screens.show_online_party_list\">";
    const std::size_t at = xml.find(old_node);
    if (at == std::string::npos) return report += " -mods-page", xml;
    // The HUD options page (its back already goes to the pause menu); settings.cpp points the page it loads at
    // the Mods page's own XAML.
    xml.replace(at, std::strlen(old_node), "<node id=\"online_party_list\" state=\"ui_screens.show_hud_options\">");
    report += " +mods-page";
    // Exit to Desktop, after Yes: the placeholder page as a "saving" screen (A_PlaceholderTextScene below shows a
    // spinner for this text), the game's own blocking save, then the same page again, waiting for A on a loop, so
    // the player is held there; the platform ends the game when the save is done (mods_page.cpp).
    // The transition is one the pause page already knows (TransitionId objects are made at start-up, and the state
    // flow resolves names against them when it loads): Online Adventure's custom road trip, which needs the online
    // services, so it can't happen here otherwise.
    const char* const anchor = "<exit id=\"online_public_custom_roadtrip\" target=\"online_public_custom_matchmaking\" />";
    const std::size_t ex2 = xml.find(anchor);
    if (ex2 == std::string::npos) return report += " -exit-save", xml;
    xml.replace(ex2, std::strlen(anchor), "<exit id=\"online_public_custom_roadtrip\" target=\"fh3mp_exit_save\" />");
    const std::size_t node = xml.find("<node id=\"online_party_list\"");
    xml.insert(node,
               "<node id=\"fh3mp_exit_save\" state=\"global.add_sticky\" name=\"PlaceholderTextSceneText\" type=\"string\" data=\"FH3MP_SAVING\">\n"
               "      <exit target=\"fh3mp_exit_page\" />\n    </node>\n\n"
               "    <node id=\"fh3mp_exit_page\" state=\"ui_screens.show_placeholder_text_scene_and_continue\">\n"
               "      <exit target=\"fh3mp_exit_saving\" />\n    </node>\n\n"
               "    <node id=\"fh3mp_exit_saving\" state=\"global.save_profile\" force=\"true\">\n"
               "      <exit target=\"fh3mp_exit_hold\" />\n    </node>\n\n"
               "    <node id=\"fh3mp_exit_hold\" state=\"ui_screens.show_placeholder_text_scene\" text=\"FH3MP_SAVING\" wait_for_button_press=\"true\">\n"
               "      <exit target=\"fh3mp_exit_hold\" />\n    </node>\n\n    ");
    report += " +exit-save";
    return xml;
}

// A string table (.str): a 0x18-byte header {0x00020300, 0xC, offset of the name table, that - 0x18, size of the
// texts, count}, then count {hash, byte offset} pairs, then the texts (UTF-16, each ending in 0), then the name
// table. Replaces one text, moving the ones after it.
bool replace_text(std::string& str, const std::u16string& from, const std::u16string& to) {
    auto u32 = [&](std::size_t at) {
        uint32_t v;
        std::memcpy(&v, str.data() + at, 4);
        return v;
    };
    auto put = [&](std::size_t at, uint32_t v) { std::memcpy(&str[at], &v, 4); };
    if (str.size() < 0x18 || u32(0) != 0x20300) return false;
    const uint32_t count = u32(0x14);
    const std::size_t data = 0x18 + std::size_t(count) * 8;
    if (data > str.size()) return false;
    const std::string a(reinterpret_cast<const char*>(from.data()), from.size() * 2);
    const std::string b(reinterpret_cast<const char*>(to.data()), to.size() * 2);
    const std::size_t at = str.find(a + std::string(2, '\0'), data);
    if (at == std::string::npos || at >= data + u32(0x10)) return false;
    const uint32_t rel = static_cast<uint32_t>(at - data);
    bool starts = false;
    for (uint32_t i = 0; i < count; ++i) starts |= u32(0x18 + 8 * i + 4) == rel;
    if (!starts) return false;
    const int32_t delta = static_cast<int32_t>(b.size()) - static_cast<int32_t>(a.size());
    str.replace(at, a.size(), b);
    for (uint32_t i = 0; i < count; ++i)
        if (const uint32_t off = u32(0x18 + 8 * i + 4); off > rel) put(0x18 + 8 * i + 4, off + delta);
    put(0x08, u32(0x08) + delta);
    put(0x0C, u32(0x0C) + delta);
    put(0x10, u32(0x10) + delta);
    return true;
}

// The Exit to Desktop confirmation reads Main.IDS_ExitGame_Desc; English says "Are you certain...".
std::string main_strings(std::string str, std::string& report) {
    const bool ok = replace_text(str, u"Are you certain you want to exit the game?", u"Are you sure you want to exit the game?");
    report += ok ? " +exit-question" : " -exit-question";
    return str;
}

// The placeholder page, used for three things, told apart by its text (the sticky PlaceholderTextSceneText, set by
// the state flow): FH3MP_MODS is the Mods page, FH3MP_SAVING the Exit to Desktop saving screen (the game's own
// "SAVING... PLEASE WAIT" texts over a blur, the wait spinner bottom right, no button prompts), anything else is
// shown as it is. The Mods page's rows are written in by write_mods_page() once the mods have loaded.
std::string g_mods_rows = R"(<local:FZTextBlock Style="{StaticResource A_Style_FZTextBlock_PopupText}" Text="Loading mods..." />)";
std::string g_mods_header;

std::string placeholder_xaml() {
    auto shown_for = [](const char* text, bool shown) {
        return std::string(R"(
            <Grid.Style>
                <Style TargetType="Grid">
                    <Setter Property="Visibility" Value=")") + (shown ? "Collapsed" : "Visible") + R"(" />
                    <Style.Triggers>
                        <DataTrigger Binding="{Binding Text}" Value=")" + text + R"(">
                            <Setter Property="Visibility" Value=")" + (shown ? "Visible" : "Collapsed") + R"(" />
                        </DataTrigger>
                    </Style.Triggers>
                </Style>
            </Grid.Style>)";
    };
    std::string x = R"(<Page xmlns:local="clr-namespace:ForzaUI"
      xmlns:System="clr-namespace:System;assembly=mscorlib"
      xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      x:Class="ForzaUI.Scenes.Anthem.A_PlaceholderTextScene"
      Style="{DynamicResource A_Style_Page_Generic}"
      DataContext="{Binding RelativeSource={RelativeSource Self}}">
    <Grid>
        <Grid>)";
    // anything else: the text itself
    x += R"(
            <Grid.Style>
                <Style TargetType="Grid">
                    <Setter Property="Visibility" Value="Visible" />
                    <Style.Triggers>
                        <DataTrigger Binding="{Binding Text}" Value="FH3MP_SAVING">
                            <Setter Property="Visibility" Value="Collapsed" />
                        </DataTrigger>
                        <DataTrigger Binding="{Binding Text}" Value="FH3MP_MODS">
                            <Setter Property="Visibility" Value="Collapsed" />
                        </DataTrigger>
                    </Style.Triggers>
                </Style>
            </Grid.Style>
            <local:FZTextBlock Style="{StaticResource A_Style_FZTextBlock_AnthemA}"
                               Color="White" FontSize="100"
                               Text="{Binding Text}" VerticalAlignment="Center" HorizontalAlignment="Center" />
        </Grid>
        <Grid>)" + shown_for("FH3MP_SAVING", true) + R"(
            <local:FZMaterial Style="{StaticResource A_Style_PauseMenuBlur}" />
            <StackPanel VerticalAlignment="Center" HorizontalAlignment="Center">
                <local:FZTextBlock Style="{StaticResource A_Style_FZTextBlock_AnthemA}"
                                   Color="White" FontSize="80" HorizontalAlignment="Center"
                                   Text="{local:Loc Main.IDS_Saving_Title}" />
                <local:FZTextBlock Style="{StaticResource A_Style_FZTextBlock_PopupText}"
                                   HorizontalAlignment="Center" Margin="0,20,0,0"
                                   Text="{local:Loc Main.IDS_Saving_Desc_PC}" />
            </StackPanel>
            <ProgressBar IsIndeterminate="True"
                         Style="{StaticResource A_Style_ProgressBar_WaitSpinner}"
                         Width="56" Height="56"
                         HorizontalAlignment="Right" VerticalAlignment="Bottom"
                         Margin="0,0,90,70" />
        </Grid>
        <Grid>)" + shown_for("FH3MP_MODS", true) + R"(
            <local:FZMaterial Style="{StaticResource A_Style_PauseMenuBlur}" />
            <ContentControl x:Name="BottomBar" Style="{StaticResource A_Style_ButtonBar}">
                <ContentControl local:ControllerButtonPanel.BDescription="{local:Loc HelpButtons.IDS_Back}"
                                Style="{StaticResource A_Style_ContentControl_DefaultButtonBar}" />
            </ContentControl>
            <Border Style="{StaticResource A_Style_Border_ScenePadding}">
                <Grid Style="{StaticResource A_Style_FrameworkElement_ScreenTilt}" HorizontalAlignment="Center" VerticalAlignment="Center">
                    <StackPanel MinWidth="1100">
                        <ContentControl Content="MODS"
                                        Style="{StaticResource A_Style_ContentControl_ScreenTitleBlock}"
                                        HorizontalAlignment="Left" VerticalAlignment="Top" Margin="0,0,0,13"
                                        local:BindingProxy.A="/ForzaUI;component/Textures/Anthem/Icons/Titles/Leaderboards.png" />
)" + g_mods_header + R"(
                        <StackPanel>
)" + g_mods_rows + R"(
                        </StackPanel>
                    </StackPanel>
                </Grid>
            </Border>
        </Grid>
    </Grid>
</Page>
)";
    return x;
}

std::string placeholder_scene(std::string, std::string& report) {
    report += " +mods-page +saving-screen";
    return placeholder_xaml();
}

struct Patch {
    const char* zip;    // in the game's media folder
    const char* entry;  // inside the zip; its path under media\ is <zip name without .zip>\<entry>
    std::string (*apply)(std::string, std::string&);
};

const Patch kPatches[] = {
    {"UI.zip", "Scenes/Anthem/A_PauseMenu.xaml", pause_menu},
    {"stateflow.zip", "pause.xml", pause_flow},
    {"UI.zip", "Scenes/Anthem/A_PlaceholderTextScene.xaml", placeholder_scene},
    {"Stripped/StringTables/EN.zip", "Main.str", main_strings},
    {"Stripped/StringTables/GB.zip", "Main.str", main_strings},
};

} // namespace

// Builds <game>\mediaplatform\ and returns it, or an empty path if there is nothing to mount.
// The Mods page's contents, once the mods have loaded (header and rows are XAML fragments, made by platform.cpp).
void write_mods_page(const std::string& header, const std::string& rows) {
    g_mods_header = header;
    g_mods_rows   = rows;
    if (game_mount().empty()) return;
    const fs::path target = game_mount() / kOverlayFolder / L"UI" / L"Scenes" / L"Anthem" / L"A_PlaceholderTextScene.xaml";
    std::error_code ec;
    if (!fs::exists(target, ec)) return; // the overlay isn't in use
    const std::string x = placeholder_xaml();
    std::ofstream f(target, std::ios::binary | std::ios::trunc);
    f.write(x.data(), static_cast<std::streamsize>(x.size()));
    plog(f ? "Mods page: written" : "Mods page: couldn't write " + target.string());
}

std::filesystem::path build_ui_overlay() {
    // (kOverlayFolder is declared in platform.hpp)
    const fs::path media = game_mount().empty() ? fs::path() : game_mount() / L"media";
    std::error_code ec;
    if (media.empty()) {
        plog("ui patches: the game folder wasn't found, nothing to patch");
        return {};
    }
    // Azimuth only takes content paths under game:\ with no ".." in them (it stops the game otherwise), so the
    // overlay can't live in fh3-mods; it's the platform's own folder next to media\ and mediaoverride\.
    const fs::path root = game_mount() / kOverlayFolder;
    fs::remove_all(azimuth_dir() / L"fh3-mods" / L"overlay", ec); // where the first build put it
    bool any = false;
    std::vector<fs::path> kept;
    for (const Patch& p : kPatches) {
        std::vector<uint8_t> zip;
        std::string text;
        const fs::path target = root / fs::path(std::string(p.zip)).replace_extension() / fs::path(std::string(p.entry));
        if (!read_file(media / p.zip, zip) || !zip_entry(zip, p.entry, text)) {
            plog(std::string("ui patches: couldn't read ") + p.entry + " from " + p.zip + "; left alone");
            fs::remove(target, ec);
            continue;
        }
        std::string report;
        const std::string patched = p.apply(text, report);
        if (report.find(" -") != std::string::npos || patched == text) {
            // All or nothing: a half-applied layout is worse than the game's own.
            plog(std::string("ui patches: ") + p.entry + ":" + report + " (not all edits fit this file; the game's own is used)");
            fs::remove(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        std::string existing;
        {
            std::ifstream f(target, std::ios::binary);
            if (f) existing.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (existing != patched) {
            std::ofstream f(target, std::ios::binary | std::ios::trunc);
            f.write(patched.data(), static_cast<std::streamsize>(patched.size()));
            if (!f) {
                plog(std::string("ui patches: couldn't write ") + target.string());
                continue;
            }
        }
        plog(std::string("ui patches: ") + p.entry + ":" + report + " -> " + target.string());
        kept.push_back(target.lexically_normal());
        any = true;
    }
    // Files an earlier version of the platform put here and this one doesn't make any more.
    std::vector<fs::path> stale;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && std::find(kept.begin(), kept.end(), it->path().lexically_normal()) == kept.end())
            stale.push_back(it->path());
    for (const fs::path& f : stale) {
        fs::remove(f, ec);
        plog("ui patches: removed " + f.string() + " (no longer made)");
    }
    return any ? root : fs::path();
}

} // namespace fh3platform
