#include "ActionSwitcher.h"
#include <array>
#include <limits>
#include <tlhelp32.h>
#include <vector>

namespace switcher
{
namespace aoe
{
namespace
{
bool typesLoaded = false;

AreaShotType ValidType(INT value)
{
    return value >= 0 && value <= 3 ? static_cast<AreaShotType>(value) : AreaShotType::NONE;
}

// ---------------------------------------------------------------------------------------------
// Typhon edition MCrEdit ("Master of Puppets - Creatures Editor") link.
//
// The editor keeps its table in EraPlugins\MCrEdit\MonstersSetup.mop. The file has no header:
// it is a list of fields, each holding 1000 creature records. The "Advanced shooting" drop-down
// (None / Fireball / Death cloud) is one byte per creature at kTyphonAreaTypeOffset:
//   16-byte battle DEF names (16000) + 5-byte sound prefixes (5000) + 4-byte flags (4000)
//   + eight 1-byte fields (8000) = 33000 = 0x80E8.
// The "Advanced shooting" check box is the native area-shot flag, which the editor already applies
// to the game's creature table, so only the drop-down is read here.
// ---------------------------------------------------------------------------------------------
constexpr DWORD kTyphonRecords = 1000;
constexpr DWORD kTyphonFileSize = 254000;
constexpr DWORD kTyphonAreaTypeOffset = 0x80E8;
constexpr DWORD kTyphonDefNameSize = 16;

bool typhonTried = false;
bool typhonLoaded = false;
std::array<BYTE, kTyphonRecords> typhonTypes{};
char typhonPath[MAX_PATH] = "";

bool ReadTyphonFile(const char *path)
{
    const HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    // One byte more than expected, so a larger file of another format is rejected.
    std::vector<BYTE> data(kTyphonFileSize + 1);
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!ok || bytesRead != kTyphonFileSize)
        return false;

    // The first field must look like a battle DEF name, such as "c000psnt.def".
    char firstDef[kTyphonDefNameSize + 1] = {};
    memcpy(firstDef, data.data(), kTyphonDefNameSize);
    const size_t length = strlen(firstDef);
    if (length < 5 || _stricmp(firstDef + length - 4, ".def") != 0)
        return false;

    // The drop-down only offers 0 (None), 1 (Fireball) and 2 (Death cloud).
    for (DWORD i = 0; i < kTyphonRecords; ++i)
        if (data[kTyphonAreaTypeOffset + i] > 2)
            return false;

    memcpy(typhonTypes.data(), data.data() + kTyphonAreaTypeOffset, kTyphonRecords);
    lstrcpynA(typhonPath, path, MAX_PATH);
    return true;
}

// Returns the directory part of a path, including the trailing backslash.
void DirectoryOf(const char *path, char *dir)
{
    lstrcpynA(dir, path, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (!slash)
        slash = strrchr(dir, '/');
    if (slash)
        slash[1] = '\0';
    else
        dir[0] = '\0';
}

bool TryTyphonFile(const char *dir, const char *relative)
{
    char path[MAX_PATH];
    if (strlen(dir) + strlen(relative) >= MAX_PATH)
        return false;
    lstrcpynA(path, dir, MAX_PATH);
    lstrcatA(path, relative);
    return ReadTyphonFile(path);
}

bool LoadTyphonTypes()
{
    if (typhonTried)
        return typhonLoaded;
    typhonTried = true;

    char dir[MAX_PATH];
    char exePath[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    DirectoryOf(exePath, dir);

    // 1. Through ERA's virtual file system: the active mod's EraPlugins folder appears in the game folder,
    //    so this is the same file the editor plugin itself reads, whatever the mod is called.
    typhonLoaded = TryTyphonFile(dir, "EraPlugins\\MCrEdit\\MonstersSetup.mop");

    // 2. Next to any loaded plugin, in case the file is not visible through the game folder.
    if (!typhonLoaded)
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Module32First(snapshot, &entry); more && !typhonLoaded;
                 more = Module32Next(snapshot, &entry))
            {
                DirectoryOf(entry.szExePath, dir);
                typhonLoaded = dir[0] && (TryTyphonFile(dir, "MCrEdit\\MonstersSetup.mop") ||
                                          TryTyphonFile(dir, "MonstersSetup.mop"));
            }
            CloseHandle(snapshot);
        }
    }

    // ERM can show which file was used: !!IF:M^%s(aimedShotTyphonFile)^;
    Era::SetAssocVarStrValue("aimedShotTyphonFile", typhonLoaded ? typhonPath : "");
    return typhonLoaded;
}

bool ReadJsonType(const char *format, INT creature, AreaShotType &type)
{
    char key[96];
    sprintf(key, format, creature);
    bool found = false;
    const INT value = EraJS::readInt(key, found);
    if (found)
        type = ValidType(value);
    return found;
}

// Order of priority:
//   1. combat_switching_actions.area_shot_types.<ID> in a JSON file - explicit exception (0..3);
//   2. the Typhon editor's "Advanced shooting" drop-down;
//   3. only without the editor file: jsMod.shotAtArea.creatures.<ID>.type, then the original game
//      (45 Magog = fireball, 64/65 Lich and Power Lich = death cloud).
// explicit = the value came from a JSON exception, which may also grant the area-shot flag.
AreaShotType ReadConfiguredType(INT creature, bool &explicitType)
{
    AreaShotType type = AreaShotType::NONE;
    explicitType = ReadJsonType("combat_switching_actions.area_shot_types.%d", creature, type);
    if (explicitType)
        return type;

    if (LoadTyphonTypes())
        return creature >= 0 && creature < static_cast<INT>(kTyphonRecords) ? ValidType(typhonTypes[creature])
                                                                           : AreaShotType::NONE;

    explicitType = ReadJsonType("jsMod.shotAtArea.creatures.%d.type", creature, type);
    if (explicitType)
        return type;

    switch (creature)
    {
    case eCreature::MAGOG:
        return AreaShotType::FIRE;
    case eCreature::LICH:
    case eCreature::POWER_LICH:
        return AreaShotType::DEATH;
    default:
        return AreaShotType::NONE;
    }
}

AreaShotType ReadConfiguredType(INT creature)
{
    bool explicitType = false;
    return ReadConfiguredType(creature, explicitType);
}

void __stdcall LoadTypes(Era::TEvent *)
{
    ResetTargeting(P_CombatManager->Get());
    for (INT creature = 0; creature < static_cast<INT>(P_CreatureCount); ++creature)
    {
        bool explicitType = false;
        const AreaShotType type = ReadConfiguredType(creature, explicitType);
        char key[80];
        sprintf(key, "js_aimedShotType_%d", creature);
        Era::SetAssocVarIntValue(key, static_cast<INT>(type));
        // A JSON exception grants the area flag to shooters, as in the supplied source.
        // Typhon types do not: the editor's own check box decides whether the flag is set.
        if (explicitType && type != AreaShotType::NONE && P_CreatureInformation[creature].shooter)
            P_CreatureInformation[creature].fireballAttack = true;
    }
    typesLoaded = true;
}

bool AtCenter(const H3CombatCreature *target, INT hex)
{
    return target->position == hex || (target->info.doubleWide && target->GetSecondSquare() == hex);
}

bool ValidTarget(H3CombatManager *manager, H3CombatCreature *shooter, INT hex, bool bow)
{
    return hex >= 0 && hex < 187 && hex % 17 != 0 && hex % 17 != 16 && !AtCenter(shooter, hex) &&
           (bow ||
            (!THISCALL_3(BOOL8, 0x4670A0, manager, shooter->position, hex) &&
             (!shooter->info.doubleWide || !THISCALL_3(BOOL8, 0x4670A0, manager, shooter->GetSecondSquare(), hex))));
}

// The native valuation already accounts for friendly fire and combat AI weights.
// Override its immunity branch for all three configured types, retaining the
// direct-hit exception for fire-immune/nonliving creatures at the center.
_LHF_(EvaluateAreaTarget)
{
    if (!ActionSwitcher::Get().IsEnabled())
        return EXEC_DEFAULT;
    const auto shooter = reinterpret_cast<H3CombatCreature *>(c->edi);
    const auto target = reinterpret_cast<H3CombatCreature *>(c->esi);
    const AreaShotType type = GetAreaShotType(shooter);
    if (type == AreaShotType::NONE || !target)
        return EXEC_DEFAULT;
    const INT hex = IntAt(c->ebp - 0x14);
    c->return_address = AtCenter(target, hex) || CanDealAreaDamage(target, type) ? 0x41EFC4 : 0x41EFEF;
    return NO_EXEC_DEFAULT;
}

INT __stdcall SelectAreaTarget(HiHook *hook, H3CombatManager *manager, H3CombatCreature *shooter, DWORD *data,
                               INT *weight)
{
    if (!manager || !data || !weight || !IsAvailable(shooter))
        return THISCALL_4(INT, hook->GetDefaultFunc(), manager, shooter, data, weight);

    const H3Hero *hero = shooter->GetOwner();
    const bool bow = hero && hero->WearsArtifact(eArtifact::BOW_OF_THE_SHARPSHOOTER);
    const INT ourGroup = IntAt(reinterpret_cast<INT>(data) + 0x20);
    std::array<BYTE, sizeof(manager->massSpellTarget)> savedTargets;
    memcpy(savedTargets.data(), manager->massSpellTarget, savedTargets.size());
    INT bestHex = -1, bestWeight = 0, bestDistance = 187, bestRank = -1;
    for (INT hex = 0; hex < 187; ++hex)
    {
        if (!ValidTarget(manager, shooter, hex, bow))
            continue;
        const INT value = FASTCALL_4(INT, 0x41EF20, shooter, hex, ourGroup, data);
        if (value <= 0)
            continue;
        INT distance = FASTCALL_2(INT, 0x469250, shooter->position, hex);
        if (shooter->info.doubleWide)
            distance = (std::min)(distance, FASTCALL_2(INT, 0x469250, shooter->GetSecondSquare(), hex));
        const H3CombatCreature *target = manager->squares[hex].GetCreature();
        const INT rank = target ? (target->position == hex ? 2 : 1) : 0;
        if (value > bestWeight ||
            (value == bestWeight && (distance < bestDistance || (distance == bestDistance && rank > bestRank))))
        {
            bestHex = hex;
            bestWeight = value;
            bestDistance = distance;
            bestRank = rank;
        }
    }
    memcpy(manager->massSpellTarget, savedTargets.data(), savedTargets.size());
    *weight = bestWeight;
    return bestHex;
}

_LHF_(DispatchAreaDamage)
{
    auto shooter = reinterpret_cast<H3CombatCreature *>(c->esi);
    const AreaShotType type = GetAreaShotType(shooter);
    if (!ActionSwitcher::Get().IsEnabled() || !shooter || !shooter->info.fireballAttack || type == AreaShotType::NONE)
        return EXEC_DEFAULT;
    // This point follows projectile drawing and the single native ammo decrement.
    c->eax = shooter->type;

    if (type == AreaShotType::FIRE)
        c->return_address = 0x43F735;
    else if (type == AreaShotType::DEATH)
        c->return_address = 0x43FB27;
    else
    {
        IntAt(c->ebp - 0x14) = -1;
        c->return_address = 0x43F870;
    }
    return NO_EXEC_DEFAULT;
}
} // namespace

AreaShotType GetAreaShotType(const H3CombatCreature *stack)
{
    if (!stack)
        return AreaShotType::NONE;
    if (!typesLoaded)
        return ReadConfiguredType(stack->type);
    char key[80];
    sprintf(key, "js_aimedShotType_%d", stack->type);
    return ValidType(Era::GetAssocVarIntValue(key));
}

bool CanDealAreaDamage(const H3CombatCreature *target, AreaShotType type)
{
    return target && target->numberAlive > 0 && type != AreaShotType::NONE &&
           (type != AreaShotType::FIRE || !target->info.fireImmunity) &&
           (type != AreaShotType::DEATH || target->info.alive);
}

void InstallTypes(PatcherInstance *patcher)
{
    Era::RegisterHandler(LoadTypes, "OnAfterErmInstructions");
    patcher->WriteLoHook(0x43F729, DispatchAreaDamage);
    patcher->WriteHiHook(0x41ED00, SPLICE_, EXTENDED_, THISCALL_, SelectAreaTarget);
    patcher->WriteLoHook(0x41EF93, EvaluateAreaTarget);
}
} // namespace aoe
} // namespace switcher
