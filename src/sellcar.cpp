// SELL CAR in the garage: a line after "Remove car" in a car's action menu that pays a share of the car's price
// (and of what was paid for its upgrades), then removes the car with the game's own code. Remove car itself is
// untouched. Ported from the FH2 mod of the same name; the game's code is the same family, found again by its
// strings and shapes (Forza Horizon 3, forza_durango_release_final.exe 5 Jun 2018).
//
// How it works, all in memory:
//  - the function that builds a car's action popup (RVA_BUILD) adds each entry through the game's item
//    creation (RVA_ITEM_NEW) and the list's AddItem (slot 8 of RVA_LIST_VTABLE). Right after the Remove car
//    entry (tag 2) one more entry is added, with a tag of ours (40);
//  - choosing it (RVA_HANDLER, the popup's "item chosen") builds the same popup again, but with only our
//    two entries (sell, back) and the price in the title; the texts are served from the game's text
//    lookup (RVA_GETTEXT) for ids of ours;
//  - "sell" adds the credits and calls the game's removal (RVA_REMOVE) with what its own confirmation passes.
#include "platform.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace fh3platform {

namespace {

constexpr uint32_t RVA_BUILD       = 0x143C320;
constexpr uint32_t RVA_ITEM_NEW    = 0x142AB50;
constexpr uint32_t RVA_LOC_NEW     = 0x1CA7BC0;
constexpr uint32_t RVA_LOC_TABLE   = 0x2E977B0;
constexpr uint32_t RVA_LIST_VTABLE = 0x44A7D50;
constexpr uint32_t RVA_HANDLER     = 0x1438CD0;
constexpr uint32_t RVA_REMOVE      = 0x142F9D0;   // the removal's body: (capture {page, context, garage id})
constexpr uint32_t RVA_ITEM_FREE   = 0x31EB20;
constexpr uint32_t RVA_GETTEXT     = 0x1CABFD0;   // (loc reference) -> UTF-16 text
constexpr uint32_t RVA_KEY_ACTION  = 0x4E56F64;   // text id of the popup's title ("Select an action")
constexpr uint32_t RVA_GUID_GARAGE = 0x2D243CC;   // argument of the context's "garage id" query (vtable slot 8)
constexpr uint32_t RVA_SESSION     = 0x1498950;   // the game's session (account at +8)
constexpr uint32_t RVA_CREDITS_GET = 0x323A00, RVA_CREDITS_VAL = 0x1A2FDA0, RVA_CREDITS_DROP = 0x1A2DFF0, RVA_CREDITS_GUID = 0x2D39008;
constexpr uint32_t RVA_CREDITS_SET = 0x337440;
constexpr uint32_t RVA_SQL_EXEC    = 0x421D0;
constexpr int kRemoveTag = 2;
constexpr int SELL_TAG = 40, CONFIRM_SELL_TAG = 41, CONFIRM_BACK_TAG = 42;
constexpr uint32_t KEY_ITEM = (0x7E57u << 16) | 0xA600u, KEY_BACK = (0x7E57u << 16) | 0xA601u;
constexpr long kMaxCredits = 999999999L;

char* g_game = nullptr;
int g_base_pct = 50, g_up_pct = 30, g_reason = 1;

typedef void* (*BuildFn)(void*, void*, void*, void*);
typedef void** (*ItemNewFn)(void**, int*, void**);
typedef void** (*LocNewFn)(void**, const void*, unsigned);
typedef void* (*AddItemFn)(void*, int, void*);
typedef const wchar_t* (*GetTextFn)(void*);
typedef void* (*HandlerFn)(void*, void*, void*, void*);
BuildFn g_real_build = nullptr;
HandlerFn g_real_handler = nullptr;
ItemNewFn g_real_item_new = nullptr;
AddItemFn g_real_add = nullptr;
GetTextFn g_real_gettext = nullptr;

thread_local bool t_in_builder = false;
thread_local bool t_added = false;
thread_local int t_last_tag = -1;
std::atomic<bool> g_want_confirm{false};   // the next build is the sell popup
std::atomic<bool> g_confirm{false};        // the popup being built / shown is the sell popup
std::atomic<long> g_sell_gid{-1};
std::atomic<long> g_sell_value{-1};
wchar_t g_t_item[24] = L"SELL CAR", g_t_back[24] = L"BACK", g_t_title[200] = L"SELL THIS CAR?";

bool mapped(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(m.BaseAddress) + m.RegionSize;
}

bool code_is(uint32_t rva, const unsigned char* want, std::size_t n) {
    return mapped(g_game + rva, n) && std::memcmp(g_game + rva, want, n) == 0;
}

void say(const std::string& t) { log_line("Sell Car", t); }

// ---- the price, from the game's own database ----
long g_query_value = -1;
int collect(void*, int cols, char** vals, char**) {
    if (cols > 0 && vals && vals[0]) g_query_value = atol(vals[0]);
    return 0;
}
bool price_of(long gid, long* out) {
    void* db = upgrades_live_db();
    if (!db) return false;
    char sql[800];
    std::snprintf(sql, sizeof sql,
                  "SELECT CAST((IFNULL((SELECT BaseCost FROM Data_Car WHERE Id=(SELECT CarId FROM Profile0_Career_Garage WHERE Id=%ld)),0)*%d"
                  "+IFNULL((SELECT SUM(PricePaid) FROM Profile0_Career_PurchasedParts WHERE GarageId=%ld),0)*%d)/100 AS INTEGER) AS V",
                  gid, g_base_pct, gid, g_up_pct);
    g_query_value = -1;
    char* err = nullptr;
    const int rc = reinterpret_cast<int (*)(void*, const char*, void*, void*, char**)>(g_game + RVA_SQL_EXEC)(db, sql, reinterpret_cast<void*>(&collect), nullptr, &err);
    if (rc != 0) {
        say("price query failed (" + std::to_string(rc) + "): " + (err && mapped(err, 4) ? err : "?"));
        return false;
    }
    *out = g_query_value;
    return g_query_value >= 0;
}

// ---- credits ----
bool give_credits(long amount) {
    void* sess = reinterpret_cast<void* (*)()>(g_game + RVA_SESSION)();
    if (!sess || !mapped(sess, 16)) return false;
    void* acct = *reinterpret_cast<void**>(static_cast<char*>(sess) + 8);
    if (!acct) return false;
    alignas(16) unsigned char tmp[0x100];
    std::memset(tmp, 0, sizeof tmp);
    void* v = reinterpret_cast<void* (*)(void*, void*, const void*)>(g_game + RVA_CREDITS_GET)(acct, tmp, g_game + RVA_CREDITS_GUID);
    const unsigned cur = reinterpret_cast<unsigned (*)(void*)>(g_game + RVA_CREDITS_VAL)(v);
    reinterpret_cast<void (*)(void*)>(g_game + RVA_CREDITS_DROP)(tmp);
    if (cur > static_cast<unsigned>(kMaxCredits)) {
        say("credits read as " + std::to_string(cur) + ": not changing anything");
        return false;
    }
    long nv = static_cast<long>(cur) + amount;
    if (nv > kMaxCredits) nv = kMaxCredits;
    // the reason record the game passes with a credit change (see its own spending code)
    alignas(16) unsigned char why[0x28];
    std::memset(why, 0, sizeof why);
    *reinterpret_cast<int*>(why + 4)    = -1;
    *reinterpret_cast<int*>(why + 8)    = -1;
    *reinterpret_cast<int*>(why + 0xC)  = -1;
    *reinterpret_cast<int*>(why + 0x14) = g_reason;
    reinterpret_cast<void (*)(void*, unsigned, void*)>(g_game + RVA_CREDITS_SET)(acct, static_cast<unsigned>(nv), why);
    say("sold a car: credits " + std::to_string(cur) + " -> " + std::to_string(nv) + " (+" + std::to_string(amount) + ")");
    return true;
}

// ---- menu entries ----
void add_entry(void* list, int type, int tag, uint32_t key) {
    void* text = nullptr;
    void* item_holder = nullptr;
    void** t = reinterpret_cast<LocNewFn>(g_game + RVA_LOC_NEW)(&text, g_game + RVA_LOC_TABLE, key);
    void** i = g_real_item_new(&item_holder, &tag, t);
    if (i && *i) g_real_add(list, type, *i);
    if (item_holder) {
        volatile LONG* rc = reinterpret_cast<volatile LONG*>(static_cast<char*>(item_holder) + 0x10);
        if (InterlockedDecrement(rc) == 0) reinterpret_cast<void (*)(void*)>(g_game + RVA_ITEM_FREE)(item_holder);
    }
    if (text) {
        void** tv = *reinterpret_cast<void***>(text);
        reinterpret_cast<void (*)(void*)>(tv[3])(text);
    }
}

void* build_hook(void* a, void* b, void* c, void* d) {
    g_confirm = g_want_confirm.exchange(false);
    t_in_builder = true;
    t_added      = false;
    t_last_tag   = -1;
    void* r      = g_real_build(a, b, c, d);
    t_in_builder = false;
    return r;
}

void** item_new_hook(void** out, int* tag, void** text) {
    if (t_in_builder && tag && mapped(tag, 4)) t_last_tag = *tag;
    return g_real_item_new(out, tag, text);
}

void* add_hook(void* list, int type, void* item) {
    if (t_in_builder && g_confirm.load()) {
        // the sell popup: the game's entries are dropped, ours go in at the first call
        if (!t_added) {
            t_added = true;
            add_entry(list, type, CONFIRM_SELL_TAG, KEY_ITEM);
            add_entry(list, type, CONFIRM_BACK_TAG, KEY_BACK);
        }
        return nullptr;
    }
    void* r = g_real_add(list, type, item);
    if (t_in_builder && t_last_tag == kRemoveTag && !t_added) {
        t_added = true;
        add_entry(list, type, SELL_TAG, KEY_ITEM);
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 3) say("SELL CAR entry added to the car action menu");
    }
    return r;
}

// ---- choosing ----
long garage_id_of(void* ctx) {
    if (ctx && mapped(ctx, 8)) {
        void** vt = *reinterpret_cast<void***>(ctx);
        if (mapped(vt, 9 * sizeof(void*)) && vt[8])
            return reinterpret_cast<int (*)(void*, const void*)>(vt[8])(ctx, g_game + RVA_GUID_GARAGE);
    }
    return -1;
}

void do_sale(void* page, void* ctx) {
    const long gid = garage_id_of(ctx);
    if (gid != g_sell_gid.load() || g_sell_value.load() < 0) {
        say("sell: the chosen car is not the one that was priced; nothing done");
        return;
    }
    const long value = g_sell_value.load();
    if (value > 0 && !give_credits(value)) {
        say("sell: could not add the credits; the car was NOT removed");
        return;
    }
    // the game's own removal, with the same capture its confirmation passes
    uintptr_t capture[3] = {reinterpret_cast<uintptr_t>(page), reinterpret_cast<uintptr_t>(ctx), 0};
    *reinterpret_cast<int*>(&capture[2]) = static_cast<int>(gid);
    reinterpret_cast<void (*)(void*)>(g_game + RVA_REMOVE)(capture);
}

std::string grouped(long v) {
    std::string s = std::to_string(v), o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return o;
}

void* handler_hook(void* page, void* tag_, void* ctx, void* d) {
    const int tag = static_cast<int>(reinterpret_cast<uintptr_t>(tag_));
    if (tag == SELL_TAG) {
        long gid = garage_id_of(ctx), value = 0;
        if (gid < 0 || !price_of(gid, &value)) {
            say("sell: could not read the price, nothing done");
            return nullptr;
        }
        const std::string t = "SELL THIS CAR FOR " + grouped(value) + " CR?";
        for (std::size_t i = 0; i < t.size() && i + 1 < 200; ++i) g_t_title[i] = static_cast<wchar_t>(t[i]), g_t_title[i + 1] = 0;
        g_sell_gid   = gid;
        g_sell_value = value;
        say("sell: car in garage slot " + std::to_string(gid) + ", price " + std::to_string(value) + " CR");
        g_want_confirm = true;
        return build_hook(page, ctx, nullptr, nullptr);   // the sell popup
    }
    if (tag == CONFIRM_SELL_TAG) {
        g_confirm = false;
        do_sale(page, ctx);
        return nullptr;
    }
    if (tag == CONFIRM_BACK_TAG) {
        g_confirm = false;
        return nullptr;
    }
    return g_real_handler(page, tag_, ctx, d);
}

// ---- texts ----
const wchar_t* gettext_hook(void* obj) {
    if (obj && mapped(static_cast<char*>(obj) + 8, 8)) {
        const uint32_t key = *reinterpret_cast<uint32_t*>(static_cast<char*>(obj) + 0xC);
        if (key == KEY_ITEM) return g_t_item;
        if (key == KEY_BACK) return g_t_back;
        if (g_confirm.load() && g_game && mapped(g_game + RVA_KEY_ACTION, 4) && key == *reinterpret_cast<uint32_t*>(g_game + RVA_KEY_ACTION))
            return g_t_title;
    }
    return g_real_gettext(obj);
}

void install() {
    while (!find_game()) Sleep(250);
    g_game = static_cast<char*>(find_game());
    static const unsigned char build_head[]   = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD1};
    static const unsigned char item_head[]    = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40};
    static const unsigned char handler_head[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9};
    static const unsigned char add_head[]     = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x40, 0x98};
    static const unsigned char text_head[]    = {0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x0D};
    static const unsigned char remove_head[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
    void** vt = reinterpret_cast<void**>(g_game + RVA_LIST_VTABLE);
    unsigned char* add_fn = mapped(vt + 8, 8) ? static_cast<unsigned char*>(vt[8]) : nullptr;
    bool ok = code_is(RVA_BUILD, build_head, sizeof build_head) && code_is(RVA_ITEM_NEW, item_head, sizeof item_head) &&
              code_is(RVA_HANDLER, handler_head, sizeof handler_head) && code_is(RVA_GETTEXT, text_head, sizeof text_head) &&
              code_is(RVA_REMOVE, remove_head, sizeof remove_head) && add_fn && mapped(add_fn, sizeof add_head);
    if (ok) ok = std::memcmp(add_fn, add_head, sizeof add_head) == 0;
    if (!ok) {
        say("the game's garage menu code is not the expected build: Sell car stays off (nothing was changed)");
        return;
    }
    upgrades_start_hooks();   // the SQL hook also learns the profile's database connection
    void* a = hook_game_function(static_cast<uint32_t>(add_fn - reinterpret_cast<unsigned char*>(g_game)), reinterpret_cast<void*>(add_hook),
                                 "sell car: menu item add", reinterpret_cast<void**>(&g_real_add));
    void* b = hook_game_function(RVA_ITEM_NEW, reinterpret_cast<void*>(item_new_hook), "sell car: menu item create", reinterpret_cast<void**>(&g_real_item_new));
    void* c = hook_game_function(RVA_BUILD, reinterpret_cast<void*>(build_hook), "sell car: car action menu", reinterpret_cast<void**>(&g_real_build));
    void* d = hook_game_function(RVA_HANDLER, reinterpret_cast<void*>(handler_hook), "sell car: action chosen", reinterpret_cast<void**>(&g_real_handler));
    void* e = hook_game_function(RVA_GETTEXT, reinterpret_cast<void*>(gettext_hook), "sell car: texts", reinterpret_cast<void**>(&g_real_gettext));
    if (!a || !b || !c || !d || !e) {
        say("could not set up all hooks: Sell car stays off");
        return;
    }
    say("SELL CAR active: Garage > My Cars > a car's menu has SELL CAR after Remove car (" + std::to_string(g_base_pct) + "% of the price + " +
        std::to_string(g_up_pct) + "% of what was paid for upgrades)");
}

} // namespace

void sellcar_start() {
    const std::wstring ini = (azimuth_dir() / L"fh3-mods" / L"platform-sellcar.ini").wstring();
    const int on = static_cast<int>(GetPrivateProfileIntW(L"Sell", L"Enabled", -1, ini.c_str()));
    if (on < 0) WritePrivateProfileStringW(L"Sell", L"Enabled", L"1", ini.c_str());
    auto pct = [&](const wchar_t* k, int def) {
        const int v = static_cast<int>(GetPrivateProfileIntW(L"Sell", k, -1, ini.c_str()));
        if (v < 0 || v > 100) {
            WritePrivateProfileStringW(L"Sell", k, std::to_wstring(def).c_str(), ini.c_str());
            return def;
        }
        return v;
    };
    g_base_pct = pct(L"BasePercent", 50);
    g_up_pct   = pct(L"UpgradePercent", 30);
    const int r = static_cast<int>(GetPrivateProfileIntW(L"Sell", L"CreditReason", -1, ini.c_str()));
    g_reason    = r >= 0 && r <= 30 ? r : 1;
    if (r < 0) WritePrivateProfileStringW(L"Sell", L"CreditReason", L"1", ini.c_str());
    if (on == 0) return;
    std::thread(install).detach();
}

} // namespace fh3platform
