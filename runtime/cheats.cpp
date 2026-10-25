// The cheat menu (F1). It edits the game's own data in memory: the player
// profile the save is made from, and reads names and demon stats from the
// game's tables in code.bin. Layouts are the save's, the same in 1.0 and
// 1.1; the tables are checked before use.
//
// Profile (at *(0x56F4A0) + 0x30):
//   +0x000 PartyMember[16] (0x24 bytes)   +0x240 stock CharRecord[27] (0x20)
//   +0x5F6/+0x60A/+0x61A skills obtained (command/auto/racial bit arrays)
//   +0x650 money                          +0xC7C compendium CharRecord[280]
#include "cheats.h"

#include "kernel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace rt {
namespace cheat {

namespace {

#include "cheat_font.h"
#include "cheat_sjis.h"

// ---- the game's data ----

constexpr u32 PROFILE_PTR = 0x0056F4A0;
constexpr u32 DEMON_TABLE = 0x005669C8, DEMON_COUNT = 0x1C8, DEMON_SIZE = 0x38;
constexpr u32 DEMON_NAMES = 0x0056D70B;
constexpr u32 GROWTH_TABLE = 0x0056CD88;
constexpr u32 STOCK = 0x240, STOCK_SIZE = 27, MONEY = 0x650, COMPENDIUM = 0xC7C, COMPENDIUM_SIZE = 0x118;
constexpr u32 MAX_MONEY = 999999;

struct SkillList {
    u32 pool, table, count, known;
    const char* label;
};
const SkillList kSkills[3] = {
    {0x0056F4B8, 0x005702C4, 0xAD, 0x5F6, "Command skills"},
    {0x0057041E, 0x00570CAA, 0x6C, 0x60A, "Auto skills"},
    {0x00570D82, 0x00571078, 0x28, 0x61A, "Racial skills"},
};

#pragma pack(push, 1)
struct DemonInfo {
    u16 id;
    s16 listIndex;
    u16 nameOffset;
    u16 unk06;
    u8 baseLevel, unk09, unk0A, unk0B;
    u16 hpRate, mpRate, hpFixed, mpFixed;
    u8 stats[4];
    struct Learn { u8 skill, level; } commandSkills[3], autoSkills[3];
    u8 race, unk25, unk26, unk27, flags28, unk29[6], unk2F, racialUpgradeLevel, unk31, growthProfile, unk33;
    u16 resistProfile, unk36;
};
struct CharRecord {
    u16 charId, demonId;
    int8_t team;
    u8 level, unk06, unk07;
    u16 exp;
    u8 stats[4];
    u16 maxHp, maxMp;
    u8 commandSkills[3], autoSkills[3], racialSkill, noCap, flags1A, unk1B[5];
};
struct PartyMember : CharRecord {
    u8 flags;
    int8_t fate;
    u8 unk22, unk23;
};
#pragma pack(pop)
static_assert(sizeof(DemonInfo) == 0x38, "DemonInfo");
static_assert(sizeof(CharRecord) == 0x20, "CharRecord");
static_assert(sizeof(PartyMember) == 0x24, "PartyMember");

// Full-width Shift-JIS (how the game stores names) to ASCII.
std::string Ascii(u32 va)
{
    std::string s;
    for (int i = 0; i < 64 && mem::IsMapped(va, 2); ++i) {
        const u8 a = mem::R8(va), b = mem::R8(va + 1);
        if (!a) break;
        if (a < 0x80) { s += (char)a; va += 1; continue; }
        char c = 0;
        if (a == 0x82 && b >= 0x4F && b <= 0x58) c = (char)('0' + b - 0x4F);
        else if (a == 0x82 && b >= 0x60 && b <= 0x79) c = (char)('A' + b - 0x60);
        else if (a == 0x82 && b >= 0x81 && b <= 0x9A) c = (char)('a' + b - 0x81);
        else if (a == 0x81 && b >= 0x40) c = (char)kSjis81[b - 0x40];
        s += c ? c : '?';
        va += 2;
    }
    return s;
}

bool TablesOk()
{
    static int ok = -1;
    if (ok < 0) {
        // The demon name pool starts "@", "Ronaldo"; the command skill pool "NONE", "Attack".
        ok = mem::IsMapped(DEMON_TABLE, DEMON_COUNT * DEMON_SIZE) && Ascii(DEMON_NAMES + 3) == "Ronaldo" &&
             Ascii(kSkills[0].pool) == "NONE" && Ascii(kSkills[1].pool) == "NONE" && Ascii(kSkills[2].pool) == "NONE" &&
             mem::R16(kSkills[0].table) == 0 && mem::R16(DEMON_TABLE + DEMON_SIZE) == 1;
        if (!ok) Log(LOG_WARN, "cheat", "the game's tables are not where expected: cheats are off");
    }
    return ok == 1;
}

const DemonInfo* Demon(u32 id)
{
    return id < DEMON_COUNT ? reinterpret_cast<const DemonInfo*>(mem::Ptr(DEMON_TABLE + id * DEMON_SIZE)) : nullptr;
}

std::string DemonName(u32 id)
{
    const DemonInfo* d = Demon(id);
    return d ? Ascii(DEMON_NAMES + d->nameOffset) : "?";
}

std::string SkillName(int kind, u32 id)
{
    const SkillList& k = kSkills[kind];
    if (id >= k.count) return "?";
    return Ascii(k.pool + mem::R16(k.table + 2 * id));
}

u8* Profile()
{
    const u32 p = mem::R32(PROFILE_PTR);
    if (!p || !mem::IsMapped(p + 0x30, COMPENDIUM + COMPENDIUM_SIZE * 0x20)) return nullptr;
    return mem::Ptr(p + 0x30);
}

PartyMember* Members(u8* prof) { return reinterpret_cast<PartyMember*>(prof); }
CharRecord* Stock(u8* prof) { return reinterpret_cast<CharRecord*>(prof + STOCK); }

// ---- demon records (as the game makes them: Record_Create 0x2AE9C0 and
// friends, from the decompilation) ----

bool IsDemonKind(const DemonInfo* d) { return d && d->race != 0 && d->race != 0x21 && d->race != 0x42; }

u8 RacialSkill(const DemonInfo* d, u32 level)
{
    struct Pair { u8 basic, upgraded; };
    static const Pair kUpgrading[0x17] = {
        {0, 0},       {0x1E, 0x22}, {0x01, 0x23}, {0x02, 0x24}, {0x03, 0x25}, {0x04, 0x26},
        {0x05, 0x27}, {0x06, 0x28}, {0x07, 0x29}, {0x08, 0x2A}, {0x09, 0x2B}, {0x0A, 0x2C},
        {0x0B, 0x2D}, {0x0C, 0x2E}, {0x0D, 0x2F}, {0x0E, 0x30}, {0x0F, 0x31}, {0x10, 0x32},
        {0x11, 0x33}, {0x12, 0x34}, {0x1F, 0x35}, {0x21, 0x37}, {0x20, 0x36},
    };
    static const u8 kFixed[0x59] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
        0,
        0x1B, 0x1C, 0x1D, 0x57, 0x3F, 0x38, 0x39, 0x3A,
        0x3B, 0x3C, 0x3D, 0x3E, 0x40, 0x41, 0x42, 0x43,
        0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
        0x4C, 0x4D, 0x4E, 0,    0x50, 0x52, 0x51, 0x53,
        0,    0x55, 0x56, 0x58, 0x59, 0x5A, 0x6A, 0x5B,
        0x5C, 0x5F, 0x61, 0x63, 0x64, 0x6C, 0x65, 0x66,
        0x67, 0x68, 0x5D, 0x5E, 0x69, 0x6B, 0x6D,
    };
    const u8 race = d->race;
    if (race >= 1 && race <= 0x16) {
        const Pair p = kUpgrading[race];
        return d->racialUpgradeLevel != 0xFF && d->racialUpgradeLevel <= level ? p.upgraded : p.basic;
    }
    return race < sizeof kFixed ? kFixed[race] : 0;
}

u16 MaxStat(const CharRecord* r, u32 rate, u32 stat)
{
    u32 v = (u32(r->stats[stat]) + r->level) * rate >> 8;
    if (v > 0xFFFF) v = 0xFFFF;
    if (!r->noCap && v > 999) v = 999;
    return (u16)v;
}

void UpdateHpMp(CharRecord* r)
{
    const DemonInfo* d = Demon(r->demonId);
    if (!d) return;
    u32 hp = d->hpRate, mp = d->mpRate;
    if (r->flags1A & 1) hp += 0x80;
    if (r->flags1A & 2) hp += 0x100;
    if (r->flags1A & 4) mp += 0x40;
    if (r->flags1A & 8) mp += 0x80;
    r->maxHp = MaxStat(r, hp, 2);
    r->maxMp = MaxStat(r, mp, 1);
}

// The order stat points are given out in (Growth_Order).
u32 GrowthOrder(const DemonInfo* d, u8 order[16])
{
    u8 w[4];
    for (u32 i = 0; i < 4; ++i) w[i] = mem::R8(GROWTH_TABLE + d->growthProfile * 4 + i);
    const s32 total = w[0] + w[1] + w[2] + w[3];
    if (total == 0 || total > 16) return 0;
    if (d->race != 0) {
        s32 n = 0;
        for (;;) {
            s32 best = -1, most = 0;
            for (s32 i = 0; i < 4; ++i)
                if (w[i] > most) most = w[i], best = i;
            if (best == -1) break;
            --w[best];
            order[n++] = (u8)best;
        }
        return (u32)n;
    }
    // Humans: spread evenly (a simpler round robin than the game's; only
    // the order of points within a cycle differs).
    s32 n = 0;
    while (n < total)
        for (u32 i = 0; i < 4 && n < total; ++i)
            if (w[i]) { --w[i]; order[n++] = (u8)i; }
    return (u32)n;
}

bool LevelUp(CharRecord* r)
{
    const DemonInfo* d = Demon(r->demonId);
    if (!d || r->level >= 99) return false;
    ++r->level;
    if (r->charId != 0) {
        u8 order[16];
        const s32 n = (s32)GrowthOrder(d, order);
        if (n) {
            s32 diff = (s32)r->level - (s32)d->baseLevel;
            if (diff < 0) diff = ~diff;
            s32 i = diff % n;
            for (s32 tries = 0; tries < 16; ++tries) {
                u8& stat = r->stats[order[i]];
                if (stat < 40) { ++stat; break; }
                if (++i >= n) i = 0;
            }
        }
    }
    if (!(r->flags1A & 0x10))
        for (u32 k = 0; k < 3; ++k) {
            const auto& l = d->commandSkills[k];
            if (l.skill && l.level <= r->level && !r->commandSkills[k]) r->commandSkills[k] = l.skill;
        }
    if (!(r->flags1A & 0x20))
        for (u32 k = 0; k < 3; ++k) {
            const auto& l = d->autoSkills[k];
            if (l.skill && l.level <= r->level && !r->autoSkills[k]) r->autoSkills[k] = l.skill;
        }
    if (IsDemonKind(d) && d->racialUpgradeLevel <= r->level) r->racialSkill = RacialSkill(d, r->level);
    UpdateHpMp(r);
    return true;
}

void CreateRecord(u32 demonId, s32 level, CharRecord* r)
{
    const DemonInfo* d = Demon(demonId);
    r->demonId = (u16)demonId;
    r->exp = 0;
    r->noCap = 0;
    r->flags1A = 0;
    for (u32 i = 0; i < 4; ++i) r->stats[i] = d->stats[i];
    r->unk06 = d->unk09;
    r->unk07 = d->unk0A;
    std::memset(r->commandSkills, 0, 3);
    std::memset(r->autoSkills, 0, 3);
    r->racialSkill = 0;
    const s32 target = std::clamp(level, 1, 99);
    r->level = target < d->baseLevel ? (u8)target : d->baseLevel;
    for (u32 k = 0; k < 3; ++k) {
        if (d->commandSkills[k].skill && d->commandSkills[k].level <= r->level) r->commandSkills[k] = d->commandSkills[k].skill;
        if (d->autoSkills[k].skill && d->autoSkills[k].level <= r->level) r->autoSkills[k] = d->autoSkills[k].skill;
    }
    while (r->level < target && LevelUp(r)) {
    }
    if (IsDemonKind(d)) r->racialSkill = RacialSkill(d, r->level);
    UpdateHpMp(r);
}

// Puts a demon in the compendium (by its list index) if it is not there yet.
bool Register(u8* prof, const CharRecord& r, bool force)
{
    const DemonInfo* d = Demon(r.demonId);
    if (!d || d->listIndex < 0 || (u32)d->listIndex >= COMPENDIUM_SIZE) return false;
    CharRecord* e = reinterpret_cast<CharRecord*>(prof + COMPENDIUM) + d->listIndex;
    if (!force && e->level) return false;
    *e = r;
    e->charId = 0xFFFF;
    e->team = -1;
    return true;
}

// ---- the menu ----

enum Page { MAIN, DEMONS, LEVEL, PARTY, MEMBER, SKILL_KINDS, SKILLS };

struct Row {
    std::string text;
    int id = 0;
};

struct State {
    bool open = false;
    Page page = MAIN;
    std::vector<Page> back;
    int cursor = 0, scroll = 0;
    std::string filter;
    std::string message;
    int demon = 0, level = 1;       // DEMONS -> LEVEL
    int member = 0;                 // PARTY -> MEMBER: <16 party member, >=16 stock slot + 16
    int kind = 0;                   // SKILL_KINDS -> SKILLS
};

std::mutex s_lock;
std::deque<int> s_keys;
State s;

void Go(Page p)
{
    s.back.push_back(s.page);
    s.page = p;
    s.cursor = s.scroll = 0;
    s.filter.clear();
}

void GoBack()
{
    if (s.back.empty()) { s.open = false; return; }
    s.page = s.back.back();
    s.back.pop_back();
    s.cursor = s.scroll = 0;
    s.filter.clear();
}

std::string Lower(std::string t)
{
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    return t;
}

CharRecord* MemberRecord(u8* prof, int m)
{
    if (m < 16) return Members(prof) + m;
    return Stock(prof) + (m - 16);
}

std::string MemberLabel(u8* prof, int m)
{
    const CharRecord* r = MemberRecord(prof, m);
    std::string name = DemonName(r->demonId);
    if (name == "@" || name.empty()) name = "Protagonist";
    char b[96];
    std::snprintf(b, sizeof b, "%-18s Lv %2u  St %2u Ma %2u Vi %2u Ag %2u  HP %3u", name.c_str(), r->level, r->stats[0],
                  r->stats[1], r->stats[2], r->stats[3], r->maxHp);
    return b;
}

std::vector<Row> Rows(u8* prof)
{
    std::vector<Row> rows;
    switch (s.page) {
    case MAIN: {
        char b[64];
        std::snprintf(b, sizeof b, "Max Macca (now %u)", prof ? *reinterpret_cast<u32*>(prof + MONEY) : 0u);
        rows = {{b, 0}, {"Add a demon to the stock...", 1}, {"Party and stock: level, stats...", 2},
                {"Learn skills (also what add-ons give)...", 3}, {"Max Fate with everyone", 4},
                {"Fill the compendium", 5}, {"Close", 6}};
        break;
    }
    case DEMONS:
        for (u32 id = 0; id < DEMON_COUNT; ++id) {
            const DemonInfo* d = Demon(id);
            if (!IsDemonKind(d)) continue;
            const std::string name = DemonName(id);
            if (name.empty() || name == "?" || name == "@") continue;
            char b[96];
            std::snprintf(b, sizeof b, "%-20s base Lv %2u%s", name.c_str(), d->baseLevel, d->listIndex < 0 ? "  (story)" : "");
            rows.push_back({b, (int)id});
        }
        break;
    case LEVEL: {
        char b[96];
        std::snprintf(b, sizeof b, "Level: < %d >   (Left/Right 1, PgUp/PgDn 10)", s.level);
        rows = {{b, 0}, {"Add to the stock", 1}};
        break;
    }
    case PARTY:
        for (int m = 0; m < 16; ++m) {
            const PartyMember& pm = Members(prof)[m];
            if (pm.charId == 0xFFFF || !(pm.flags & 1)) continue;
            rows.push_back({MemberLabel(prof, m), m});
        }
        for (u32 i = 0; i < STOCK_SIZE; ++i)
            if (Stock(prof)[i].charId != 0xFFFF) rows.push_back({MemberLabel(prof, 16 + (int)i), 16 + (int)i});
        break;
    case MEMBER:
        rows = {{"Level 99", 0}, {"All stats 40", 1}, {"Both of the above", 2}};
        if (s.member < 16) rows.push_back({"Max Fate (5)", 3});
        break;
    case SKILL_KINDS:
        for (int k = 0; k < 3; ++k) rows.push_back({kSkills[k].label, k});
        rows.push_back({"Learn every skill", 3});
        break;
    case SKILLS: {
        const SkillList& k = kSkills[s.kind];
        for (u32 id = 1; id < k.count; ++id) {
            const std::string name = SkillName(s.kind, id);
            if (name.empty() || name == "?") continue;
            const bool known = prof && (prof[k.known + (id >> 3)] >> (id & 7) & 1);
            rows.push_back({std::string(known ? "[x] " : "[ ] ") + name, (int)id});
        }
        break;
    }
    }
    if (!s.filter.empty()) {
        const std::string f = Lower(s.filter);
        std::vector<Row> kept;
        for (auto& r : rows)
            if (Lower(r.text).find(f) != std::string::npos) kept.push_back(r);
        rows.swap(kept);
    }
    return rows;
}

void Activate(u8* prof, const Row& row)
{
    switch (s.page) {
    case MAIN:
        switch (row.id) {
        case 0:
            *reinterpret_cast<u32*>(prof + MONEY) = MAX_MONEY;
            s.message = "Macca set to 999,999.";
            break;
        case 1: Go(DEMONS); break;
        case 2: Go(PARTY); break;
        case 3: Go(SKILL_KINDS); break;
        case 4: {
            int n = 0;
            for (int m = 0; m < 16; ++m) {
                PartyMember& pm = Members(prof)[m];
                if (pm.charId == 0xFFFF || pm.charId == 0) continue;
                pm.fate = 5;
                pm.flags |= 0x30;
                ++n;
            }
            s.message = "Fate 5 with " + std::to_string(n) + " characters.";
            break;
        }
        case 5: {
            int n = 0;
            for (u32 id = 0; id < DEMON_COUNT; ++id) {
                const DemonInfo* d = Demon(id);
                if (!IsDemonKind(d) || d->listIndex < 0) continue;
                CharRecord r{};
                r.charId = 0xFFFF;
                r.team = -1;
                CreateRecord(id, d->baseLevel, &r);
                if (Register(prof, r, false)) ++n;
            }
            s.message = std::to_string(n) + " demons added to the compendium.";
            break;
        }
        case 6: s.open = false; break;
        }
        break;
    case DEMONS:
        s.demon = row.id;
        s.level = std::max<int>(1, Demon(row.id)->baseLevel);
        Go(LEVEL);
        break;
    case LEVEL:
        if (row.id == 1) {
            CharRecord* stock = Stock(prof);
            int slot = -1;
            bool used[STOCK_SIZE] = {};
            for (u32 i = 0; i < STOCK_SIZE; ++i) {
                if (stock[i].charId == 0xFFFF) { if (slot < 0) slot = (int)i; }
                else if ((u16)(stock[i].charId - 0x11) < STOCK_SIZE) used[stock[i].charId - 0x11] = true;
            }
            int id = -1;
            for (u32 i = 0; i < STOCK_SIZE && id < 0; ++i)
                if (!used[i]) id = 0x11 + (int)i;
            if (slot < 0 || id < 0) {
                s.message = "The stock is full (27 demons).";
                break;
            }
            CharRecord r{};
            r.charId = (u16)id;
            r.team = -1;
            CreateRecord((u32)s.demon, s.level, &r);
            stock[slot] = r;
            Register(prof, r, false);
            s.message = DemonName((u32)s.demon) + " (Lv " + std::to_string(r.level) + ") joined the stock.";
        }
        break;
    case PARTY:
        s.member = row.id;
        Go(MEMBER);
        break;
    case MEMBER: {
        CharRecord* r = MemberRecord(prof, s.member);
        if (row.id == 0 || row.id == 2) {
            while (r->level < 99 && LevelUp(r)) {
            }
            r->exp = 0;
        }
        if (row.id == 1 || row.id == 2) {
            for (u8& st : r->stats) st = 40;
            UpdateHpMp(r);
        }
        if (row.id == 3) {
            PartyMember* pm = Members(prof) + s.member;
            pm->fate = 5;
            pm->flags |= 0x30;
        }
        s.message = "Done: " + MemberLabel(prof, s.member);
        break;
    }
    case SKILL_KINDS:
        if (row.id < 3) {
            s.kind = row.id;
            Go(SKILLS);
        } else {
            int n = 0;
            for (int k = 0; k < 3; ++k)
                for (u32 id = 1; id < kSkills[k].count; ++id) {
                    u8& b = prof[kSkills[k].known + (id >> 3)];
                    if (!(b >> (id & 7) & 1)) ++n;
                    b |= (u8)(1 << (id & 7));
                }
            s.message = std::to_string(n) + " skills learned. Equip them from the game's menu.";
        }
        break;
    case SKILLS: {
        u8& b = prof[kSkills[s.kind].known + (row.id >> 3)];
        b ^= (u8)(1 << (row.id & 7));
        s.message = SkillName(s.kind, (u32)row.id) + ((b >> (row.id & 7) & 1) ? " learned." : " forgotten.");
        break;
    }
    }
}

const char* Title()
{
    switch (s.page) {
    case MAIN: return "Cheats";
    case DEMONS: return "Add a demon (type to search)";
    case LEVEL: return "Add a demon";
    case PARTY: return "Party and stock";
    case MEMBER: return "Character";
    case SKILL_KINDS: return "Learn skills";
    case SKILLS: return kSkills[s.kind].label;
    }
    return "";
}

constexpr int VISIBLE = 15;

void HandleKey(int key, u8* prof)
{
    const std::vector<Row> rows = Rows(prof);
    const int n = (int)rows.size();
    switch (key) {
    case KEY_UP: s.cursor = n ? (s.cursor + n - 1) % n : 0; break;
    case KEY_DOWN: s.cursor = n ? (s.cursor + 1) % n : 0; break;
    case KEY_PAGE_UP:
        if (s.page == LEVEL) s.level = std::min(99, s.level + 10);
        else s.cursor = std::max(0, s.cursor - VISIBLE);
        break;
    case KEY_PAGE_DOWN:
        if (s.page == LEVEL) s.level = std::max(1, s.level - 10);
        else s.cursor = std::min(std::max(0, n - 1), s.cursor + VISIBLE);
        break;
    case KEY_LEFT: if (s.page == LEVEL) s.level = std::max(1, s.level - 1); break;
    case KEY_RIGHT: if (s.page == LEVEL) s.level = std::min(99, s.level + 1); break;
    case KEY_ENTER:
        if (s.cursor < n) Activate(prof, rows[(size_t)s.cursor]);
        break;
    case KEY_BACK: GoBack(); break;
    case KEY_ERASE:
        if (!s.filter.empty()) { s.filter.pop_back(); s.cursor = 0; }
        else GoBack();
        break;
    default:
        if (key >= 32 && key < 127 && (s.page == DEMONS || s.page == SKILLS || s.page == PARTY)) {
            s.filter += (char)key;
            s.cursor = 0;
        }
        break;
    }
}

// ---- drawing ----

constexpr int W = 400, H = 240, CW = 6, CH = 11;

void Text(u32* fb, int x, int y, const std::string& t, u32 color)
{
    for (char ch : t) {
        if (x + CW > W) break;
        const int c = (unsigned char)ch;
        if (c >= 32 && c < 127)
            for (int r = 0; r < CH; ++r) {
                const u8 bits = kFont[c - 32][r];
                for (int b = 0; b < CW; ++b)
                    if (bits >> (CW - 1 - b) & 1) {
                        const int px = x + b, py = y + r;
                        if (px >= 0 && px < W && py >= 0 && py < H) fb[py * W + px] = color;
                    }
            }
        x += CW;
    }
}

void Fill(u32* fb, int x0, int y0, int x1, int y1, u32 color)
{
    for (int y = std::max(0, y0); y < std::min(H, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x1); ++x) fb[y * W + x] = color;
}

void Draw(u32* fb, u8* prof)
{
    for (int i = 0; i < W * H; ++i) {  // dim the game
        const u32 p = fb[i];
        fb[i] = 0xFF000000u | ((p >> 2) & 0x3F3F3F);
    }
    const u32 white = 0xFFFFFFFF, grey = 0xFFA0A8B8, accent = 0xFF57C7FF, bar = 0xFF1F4E8C;
    Fill(fb, 0, 0, W, CH + 3, 0xFF10203A);
    std::string title = Title();
    if (!s.filter.empty()) title += "   search: " + s.filter;
    Text(fb, 4, 2, title, accent);
    if (!prof) {
        Text(fb, 4, 20, "Load a game first.", white);
    } else if (!TablesOk()) {
        Text(fb, 4, 20, "This game version isn't supported by the cheat menu.", white);
    } else {
        const std::vector<Row> rows = Rows(prof);
        const int n = (int)rows.size();
        if (s.cursor >= n) s.cursor = std::max(0, n - 1);
        if (s.cursor < s.scroll) s.scroll = s.cursor;
        if (s.cursor >= s.scroll + VISIBLE) s.scroll = s.cursor - VISIBLE + 1;
        for (int i = 0; i < VISIBLE && s.scroll + i < n; ++i) {
            const int y = CH + 6 + i * (CH + 2);
            const bool sel = s.scroll + i == s.cursor;
            if (sel) Fill(fb, 0, y - 1, W, y + CH + 1, bar);
            Text(fb, 6, y, rows[(size_t)(s.scroll + i)].text, sel ? white : grey);
        }
        if (n > VISIBLE) {
            char b[32];
            std::snprintf(b, sizeof b, "%d/%d", s.cursor + 1, n);
            Text(fb, W - 6 * (int)std::strlen(b) - 4, 2, b, grey);
        }
        if (n == 0) Text(fb, 6, CH + 6, "(nothing)", grey);
    }
    Fill(fb, 0, H - 2 * (CH + 2) - 2, W, H, 0xFF10203A);
    Text(fb, 4, H - 2 * (CH + 2), s.message, accent);
    Text(fb, 4, H - (CH + 2), "Arrows move  Enter choose  Esc back  F1 close", grey);
}

}  // namespace

bool PushKey(int key)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (key == KEY_TOGGLE || s.open) {
        s_keys.push_back(key);
        return true;
    }
    return false;
}

bool Open()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s.open;
}

void Frame(u32* top)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (!s.open && s_keys.empty()) return;
    // Game memory changes under the kernel lock, between the game's system calls.
    auto lk = kernel::Lock();
    u8* prof = Profile();
    const bool usable = prof && TablesOk();
    while (!s_keys.empty()) {
        const int key = s_keys.front();
        s_keys.pop_front();
        if (key == KEY_TOGGLE) {
            s.open = !s.open;
            if (s.open) {
                s.page = MAIN;
                s.back.clear();
                s.cursor = s.scroll = 0;
                s.filter.clear();
                s.message = "Changes are saved when you save in the game.";
            }
            continue;
        }
        if (!s.open) continue;
        if (usable) HandleKey(key, prof);
        else if (key == KEY_BACK || key == KEY_ERASE) s.open = false;
    }
    if (s.open && top) Draw(top, prof);
}

}  // namespace cheat
}  // namespace rt
