# MODS page port: FH2 (24 Nov 2015) -> FH3 (5 Jun 2018) addresses

| What | FH2 | FH3 | Notes |
|---|---|---|---|
| AVUI::Type::FindProperty(type, name) | 0x157FF60 | 0x200C1D0 | identical code |
| AVUI::Object ctor | 0x143EB80 | 0x1F6E7C0 | |
| object free (refcount 0) | 0x257730 | 0x31EB20 | FH3 refcount at +0x10, not locked |
| box int / bool / float | 0x4702D0 / 0x336BA0 / 0x3F5990 | 0x5592B0 / 0x5592D0 / 0x495110 | |
| int type / bool type | 0x15C8B20 / 0x15C9550 | 0x1F5E6E0 / 0x1F5F000 | (0) -> type |
| NotifyPropertyChanged(obj, desc) | 0xCF9C60 | 0x56D4F0 | 1059 callers |
| model property (EnableDrivatarLiveries) | 0xD58660 | 0x14FD740 | FH3 descriptor: +0x28 getter, +0x30 setter, +0x38 0, +0x40 name, +0x48 value type, **+0x50 owner type (new)**, **flags +0x58** (FH2 +0x50) |
| float property (Gamma) | 0xED6E90 | 0x1712650 | same layout as above |
| getter / setter signature | (out, desc, obj) / (desc, obj, boxed value: int at +0x28) | same | FH3 checks the owner type first |
| HudOptions scene record build | 0xB3A3E0 (id 0x49) | 0x1182CB0 (id 0x7A) | same record layout: wstring at +0x28, size +0x38, cap +0x40 |
| HudOptions page vtable | 0x2DF0238 (182 slots) | 0x4566590 (189 slots) | |
| page "focus FirstItem" | slot 147 0xC7A730 | slot 148 0x13A19A0 | |
| page buttons (page, key, pressed) | slot 175 (+0x578) 0xCEC3D0 | slot 179 (+0x598) 0x14981A0 | FH3 B = 0xF423F (999999), not 45: FH3 keys are action ids |
| AVUI string from char* / free | 0x1663220 / 0x16650A0 | 0x2094100 / 0x2095E20 | |
| find named element | 0xC63120(String**, page) | 0x20EA170(page, String*, scratch*) | FH3 then checks the type (0x20ECBD0(0), IsInstanceOf 0x1F85750) |
| element's input interface | +0xB8 | +0x88 | |
| focus(out, iface) | 0x14D01D0 | 0x203BF60 | |
| UI sound player | global 0x365A298 | getter 0x1EED710() | play: player->vt[1](player, string) |
| back sound name / string make | 0x2C5EFF8 / 0x215C20 | 0x42DFAE0 / 0x304C60 | |
