#pragma once

// Reading FH2's string tables (media\Stripped\StringTables\<lang>.zip) and
// writing edited copies for Forza Horizon 2 Extra Options' mediaoverride
// folder.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fh3platform {

// One file out of a zip (stored or deflated), or empty if it isn't there.
std::vector<char> unzip_file(const std::filesystem::path& zip, std::string_view name);

// Rebuilds an "LSB2" string table with each string passed through `edit`
// (key, current text) -> new text. Empty result if `table` isn't LSB2.
// `changed` (optional) counts strings that came out different.
std::vector<char> edit_lsb2(const std::vector<char>& table,
                            const std::function<std::wstring(uint16_t key, const std::wstring& text)>& edit,
                            int* changed = nullptr);

// <game Mount folder>, from Azimuth's data\azimuth.ini ([title-1a96545b]
// folder=...), falling back to FH2\Mount. Empty if not found.
std::filesystem::path find_game_mount(const std::filesystem::path& azimuth_dir,
                                      const std::filesystem::path& command_line_folder = {});

// Writes `file` from every language zip, edited, to
// <mount>\mediaoverride\Stripped\StringTables\<lang>\<file>. With `remove`
// set, deletes those overrides instead. Returns how many were written or
// removed.
int override_string_table(const std::filesystem::path& mount, std::string_view file,
                          const std::function<std::wstring(uint16_t, const std::wstring&)>& edit,
                          bool remove = false);

// FH3's database-backed tables (Upgrades.str, UpgradeTypes.str, ...) are keyed by 32-bit hashes (the database's text columns
// hold "_&<table hash><key>"). Layout (all little-endian):
//   header   u32 0x20300, u32 12, u32 A, u32 A-24, u32 C, u32 N
//   table    N x { u32 key, u32 byte offset into the strings }
//   strings  C bytes of NUL-terminated UTF-16 (A = 0x18 + 8N + C)
//   names    u32 8N+L, u32 L, u32 N, N x { u32 key, u32 offset into the names }, L bytes of NUL-terminated UTF-16 "IDS_..." names
// Returns the table with `adds` (key -> text) appended; keys that are already there are left alone. Empty if `table` isn't
// that format.
std::vector<char> add_hashed(const std::vector<char>& table, const std::vector<std::pair<uint32_t, std::wstring>>& adds);

// Writes `file` from every language zip with `adds` appended (see add_hashed) to the mediaoverride folder; with `remove`,
// deletes those overrides. Returns how many were written or removed.
int override_hashed_table(const std::filesystem::path& mount, std::string_view file,
                          const std::vector<std::pair<uint32_t, std::wstring>>& adds, bool remove = false);

} // namespace fh3platform
