// Skips the Turn 10 / Microsoft Studios / Playground Games intro video at start-up (FH3).
//
// The start-up sequence is built by game+0x1728DC0 as a table of 0x110-byte entries; entry 4 is
// media\UI\videos\T10_MS_Combined.bk2 with its length written as a constant: `mov rax, 21.0` (48 B8 + the double)
// at game+0x1729326, stored as the entry's duration. The platform changes that constant to 0 in memory (the exe
// on disk is left alone) as soon as the game is mapped, before the sequence is built.
#include "platform.hpp"

#include <windows.h>

#include <cstring>
#include <thread>

namespace fh3platform {
namespace {

constexpr uint32_t RVA_INTRO_LENGTH = 0x1729326;
const unsigned char kExpected[10]   = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0x35, 0x40}; // mov rax, 21.0

void patch() {
    int mismatches = 0;
    for (int i = 0; i < 120000; ++i) { // up to two minutes
        char* base = static_cast<char*>(find_game());
        unsigned char now[10];
        if (base && safe_copy(now, base + RVA_INTRO_LENGTH, sizeof now)) {
            if (std::memcmp(now, kExpected, sizeof now) != 0) {
                // Azimuth is still writing the image (tested 1 Nov: read too early once); give up after 20 s.
                if (++mismatches > 20000) {
                    plog("intro skip: the intro's length isn't where it should be in this exe; left alone");
                    return;
                }
                Sleep(1);
                continue;
            }
            char* at = base + RVA_INTRO_LENGTH + 2;
            DWORD old = 0;
            if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old)) {
                Sleep(1);
                continue;
            }
            const double zero = 0.0;
            std::memcpy(at, &zero, sizeof zero);
            VirtualProtect(at, 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 8);
            plog("intro skip: the Turn 10 / Microsoft Studios / Playground Games video is set to 0 seconds");
            return;
        }
        Sleep(1);
    }
}

} // namespace

void start_intro_skip() { std::thread(patch).detach(); }

} // namespace fh3platform
