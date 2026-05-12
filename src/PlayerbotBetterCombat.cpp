/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * released under GNU AGPL v3 license.
 */

#include "Channel.h"
#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PositionValue.h"
#include "RtiTargetValue.h"
#include "ScriptMgr.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldScript.h"

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "TravelMgr.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr char const* CONF_ENABLE = "PlayerbotBetterCombat.Enable";
constexpr uint32 PULL_UPDATE_INTERVAL_MS = 200;
constexpr uint32 PULL_ACQUIRE_TIMEOUT_MS = 15000;
constexpr uint32 PULL_WAIT_TIMEOUT_MS = 15000;
constexpr uint32 SAP_UPDATE_INTERVAL_MS = 200;
constexpr uint32 SAP_TIMEOUT_MS = 30000;
constexpr uint32 SAP_STEALTH_SETTLE_MS = 500;
constexpr uint32 SAP_AI_SUPPRESS_MS = 1000;
constexpr int32 CONTROLLED_CC_REFRESH_MS = 4000;
constexpr float SAP_MELEE_STANDOFF = 0.5f;
constexpr int8 CROSS_RTI_INDEX = 6;
constexpr int8 SKULL_RTI_INDEX = 7;
constexpr float PULL_RANGE_BUFFER = 2.0f;
constexpr float PULL_MIN_RANGE_BUFFER = 1.0f;
constexpr float SAP_DISTRACT_STANDOFF = 20.0f;
constexpr float SAP_DISTRACT_BEHIND_DISTANCE = 2.5f;
constexpr float SAP_RETURN_EXTRA_DISTANCE = 1.0f;
constexpr uint32 GROUND_MARKER_SPELL_ID = 30758; // Aedm
constexpr uint32 GROUND_MARKER_VISUAL_ENTRY = 15631;
constexpr float TANK_MARKER_REACH_DISTANCE = 1.5f;
constexpr float TANK_PULL_SEARCH_STEP = 1.0f;
constexpr char const* TANK_MARKER_RTSC_NAME = "bettercombat_tank_marker";
constexpr char const* OFFTANK_MARKER_RTSC_NAME = "bettercombat_offtank_marker";

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::string TrimCopy(std::string value)
{
    auto const isSpace = [](unsigned char c) { return std::isspace(c) != 0; };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string NormalizeCommand(std::string const& value)
{
    return ToLower(TrimCopy(value));
}

char const* GetRangedWeaponTypeName(uint32 subClass)
{
    switch (subClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            return "gun";
        case ITEM_SUBCLASS_WEAPON_BOW:
            return "bow";
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return "crossbow";
        case ITEM_SUBCLASS_WEAPON_THROWN:
            return "thrown";
        default:
            return "unknown";
    }
}

char const* GetProjectileTypeName(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return "none";

    if (itemTemplate->Class != ITEM_CLASS_PROJECTILE)
        return "non-projectile";

    switch (itemTemplate->SubClass)
    {
        case ITEM_SUBCLASS_ARROW:
            return "arrow";
        case ITEM_SUBCLASS_BULLET:
            return "bullet";
        default:
            return "unknown projectile";
    }
}

std::string ToDisplayWords(std::string value)
{
    bool capitalize = true;

    for (char& c : value)
    {
        if (std::isspace(static_cast<unsigned char>(c)) != 0)
        {
            capitalize = true;
            continue;
        }

        c = capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                       : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        capitalize = false;
    }

    return value;
}

bool IsModuleEnabled()
{
    return sConfigMgr->GetOption<bool>(CONF_ENABLE, true);
}

void SendSystemMessage(Player* player, std::string const& text)
{
    if (!player || !player->GetSession())
        return;

    ChatHandler handler(player->GetSession());
    handler.SendSysMessage(("mod-playerbot-bettercombat: " + text).c_str());
}

void SendPlainSystemMessage(Player* player, std::string const& text)
{
    if (!player || !player->GetSession())
        return;

    ChatHandler handler(player->GetSession());
    handler.SendSysMessage(text.c_str());
}

bool IsLiveBot(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    return botAI && !botAI->IsRealPlayer();
}

std::vector<Player*> CollectControlledBots(Player* owner)
{
    std::vector<Player*> result;

    if (!owner)
        return result;

    Group* group = owner->GetGroup();
    if (!group)
        return result;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!IsLiveBot(member))
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
        if (!botAI || botAI->GetMaster() != owner)
            continue;

        result.push_back(member);
    }

    return result;
}

bool IsValidPullTarget(Player* player, Unit* target)
{
    if (!player || !target)
        return false;

    if (!target->IsInWorld() || target->isDead())
        return false;

    if (target == player || player->IsFriendlyTo(target))
        return false;

    return player->IsValidAttackTarget(target);
}

Unit* ResolveBotAssignedRtiCcTarget(Player* owner, Player* bot)
{
    if (!owner || !IsLiveBot(bot))
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || botAI->GetMaster() != owner)
        return nullptr;

    if (Unit* valueTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("rti cc target")->Get(); IsValidPullTarget(owner, valueTarget))
        return valueTarget;

    Group* group = owner->GetGroup();
    if (!group)
        return nullptr;

    std::string const rtiName = botAI->GetAiObjectContext()->GetValue<std::string>("rti cc")->Get();
    int32 const rtiIndex = RtiTargetValue::GetRtiIndex(rtiName);
    if (rtiIndex < 0)
        return nullptr;

    ObjectGuid const guid = group->GetTargetIcon(static_cast<uint8>(rtiIndex));
    if (guid.IsEmpty())
        return nullptr;

    Unit* iconTarget = botAI->GetUnit(guid);
    return IsValidPullTarget(owner, iconTarget) ? iconTarget : nullptr;
}

bool TargetHasAuraNamed(Unit* target, std::string const& expectedName)
{
    if (!target || expectedName.empty())
        return false;

    Unit::AuraApplicationMap const& auras = target->GetAppliedAuras();
    for (auto const& [spellId, auraApp] : auras)
    {
        Aura* aura = auraApp ? auraApp->GetBase() : nullptr;
        SpellInfo const* spellInfo = aura ? aura->GetSpellInfo() : nullptr;
        if (!spellInfo || !spellInfo->SpellName[0])
            continue;

        if (NormalizeCommand(spellInfo->SpellName[0]) == expectedName)
            return true;
    }

    return false;
}

std::string NormalizeProtectedCcName(std::string const& spellName)
{
    std::string const normalized = NormalizeCommand(spellName);
    if (normalized.rfind("polymorph", 0) == 0)
        return "polymorph";

    return normalized;
}

bool IsProtectedCcName(std::string const& spellName)
{
    std::string const normalized = NormalizeProtectedCcName(spellName);
    return normalized == "sap" ||
           normalized == "shackle undead" ||
           normalized == "hibernate" ||
           normalized == "entangling roots" ||
           normalized == "cyclone" ||
           normalized == "repentance" ||
           normalized == "hex" ||
           normalized == "banish" ||
           normalized == "polymorph";
}

std::string ResolveProtectedCcFamily(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo || !spellInfo->SpellName[0])
        return std::string();

    std::string const family = NormalizeProtectedCcName(spellInfo->SpellName[0]);
    return IsProtectedCcName(family) ? family : std::string();
}

Aura* FindProtectedCcAura(Unit* target, ObjectGuid const* casterGuid = nullptr, std::string const* family = nullptr)
{
    if (!target)
        return nullptr;

    Unit::AuraApplicationMap const& auras = target->GetAppliedAuras();
    for (auto const& [spellId, auraApp] : auras)
    {
        Aura* aura = auraApp ? auraApp->GetBase() : nullptr;
        SpellInfo const* spellInfo = aura ? aura->GetSpellInfo() : nullptr;
        if (!aura || !spellInfo || !spellInfo->SpellName[0])
            continue;

        std::string const auraFamily = NormalizeProtectedCcName(spellInfo->SpellName[0]);
        if (!IsProtectedCcName(auraFamily))
            continue;

        if (casterGuid && aura->GetCasterGUID() != *casterGuid)
            continue;

        if (family && auraFamily != *family)
            continue;

        return aura;
    }

    return nullptr;
}

bool TargetHasProtectedCcAura(Unit* target)
{
    return FindProtectedCcAura(target) != nullptr;
}

bool IsTankProtectedCcTarget(Group* group, Unit* target)
{
    if (!group || !target)
        return false;

    if (group->GetTargetIcon(SKULL_RTI_INDEX) == target->GetGUID())
        return false;

    return TargetHasProtectedCcAura(target);
}

void MarkTargetWithSkull(Player* marker, Unit* target)
{
    if (!marker || !target)
        return;

    Group* group = marker->GetGroup();
    if (!group)
        return;

    group->SetTargetIcon(SKULL_RTI_INDEX, marker->GetGUID(), target->GetGUID());
}

enum class PullOpenerType
{
    None,
    Action,
    Spell
};

struct PullOpener
{
    PullOpenerType type = PullOpenerType::None;
    std::string name;
    std::string rangeSpell;
    std::vector<uint32> spellIds;
    bool checkHasSpell = false;
    bool usesEquippedRangedSpell = false;
    float minRange = 0.0f;
    float maxRange = 0.0f;

    bool IsValid() const
    {
        return type != PullOpenerType::None && !name.empty() && !spellIds.empty() && maxRange > 0.0f;
    }
};

Player* FindControlledMainTank(Player* owner, std::vector<Player*> const& bots)
{
    if (!owner)
        return nullptr;

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsAlive())
            continue;

        if (PlayerbotAI::IsMainTank(bot))
            return bot;
    }

    return nullptr;
}

bool IsGroupMainAssist(Player* player)
{
    if (!player)
        return false;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    Group::MemberSlotList const& slots = group->GetMemberSlots();
    for (Group::member_citerator itr = slots.begin(); itr != slots.end(); ++itr)
    {
        if (itr->guid == player->GetGUID())
            return (itr->flags & MEMBER_FLAG_MAINASSIST) != 0;
    }

    return false;
}

Player* FindControlledOffTank(Player* owner, std::vector<Player*> const& bots, Player* mainTank)
{
    if (!owner)
        return nullptr;

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsAlive() || bot == mainTank)
            continue;

        if (!IsGroupMainAssist(bot) || !ResolveBotAssignedRtiCcTarget(owner, bot))
            continue;

        if (PlayerbotAI::IsAssistTank(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsTank(bot, true))
            return bot;
    }

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsAlive() || bot == mainTank)
            continue;

        if (!ResolveBotAssignedRtiCcTarget(owner, bot))
            continue;

        if (PlayerbotAI::IsAssistTankOfIndex(bot, 0, true))
            return bot;
    }

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsAlive() || bot == mainTank)
            continue;

        if (!ResolveBotAssignedRtiCcTarget(owner, bot))
            continue;

        if (PlayerbotAI::IsAssistTank(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsTank(bot, true))
            return bot;
    }

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsAlive() || bot == mainTank)
            continue;

        if (PlayerbotAI::IsAssistTankOfIndex(bot, 0, true))
            return bot;
    }

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsAlive() || bot == mainTank)
            continue;

        if (PlayerbotAI::IsAssistTank(bot))
            return bot;
    }

    return nullptr;
}

std::string ResolveRangedPullSpellName(Player* bot);
std::string ResolveRangedKnownSpellKey(Player* bot);
std::vector<uint32> ResolveGlobalPullSpellIds(std::string const& spellName, bool requireAutoRepeat);
void ResolveSpellRangeWindow(Player* bot, std::vector<uint32> const& spellIds, float& minRange, float& maxRange);
uint32 ResolvePullSpellId(PlayerbotAI* botAI, std::string const& spellName);
float ResolveSpellMaxRange(Player* bot, uint32 spellId);
float ResolveSpellMinRange(uint32 spellId);

struct EquippedFiringSpell
{
    std::string expectedKey;
    uint32 spellId = 0;
    SpellInfo const* spellInfo = nullptr;
};

uint32 GetReadyAmmoCount(Player* bot);
EquippedFiringSpell ResolveEquippedFiringSpell(Player* bot);

uint32 GetReadyAmmoCount(Player* bot)
{
    if (!bot)
        return 0;

    uint32 const ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
    return ammoId ? bot->GetItemCount(ammoId) : 0;
}

float ResolveEquippedRangedWeaponRange(Player* bot)
{
    if (!bot)
        return 0.0f;

    Item* rangedWeapon = bot->GetWeaponForAttack(RANGED_ATTACK, true);
    if (!rangedWeapon || !rangedWeapon->GetTemplate())
        return 0.0f;

    std::string const rangedSpell = ResolveRangedPullSpellName(bot);
    if (rangedSpell.empty())
        return 0.0f;

    std::vector<uint32> const spellIds = ResolveGlobalPullSpellIds(
        rangedSpell, rangedWeapon->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_THROWN);

    float minRange = 0.0f;
    float maxRange = 0.0f;
    ResolveSpellRangeWindow(bot, spellIds, minRange, maxRange);
    return maxRange;
}

EquippedFiringSpell ResolveEquippedFiringSpell(Player* bot)
{
    EquippedFiringSpell result;
    result.expectedKey = ResolveRangedKnownSpellKey(bot);

    if (!bot || result.expectedKey.empty())
        return result;

    Item* rangedWeapon = bot->GetWeaponForAttack(RANGED_ATTACK, true);
    if (!rangedWeapon)
        return result;

    auto const spellMatches = [&](SpellInfo const* spellInfo)
    {
        if (!spellInfo || spellInfo->IsPassive())
            return false;

        char const* spellName = spellInfo->SpellName[LOCALE_enUS];
        if (!spellName || ToLower(spellName) != result.expectedKey)
            return false;

        if (!spellInfo->IsRangedWeaponSpell())
            return false;

        return rangedWeapon->IsFitToSpellRequirements(spellInfo);
    };

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellMatches(spellInfo))
            continue;

        if (!result.spellInfo ||
            (!result.spellInfo->IsAutoRepeatRangedSpell() && spellInfo->IsAutoRepeatRangedSpell()) ||
            (result.spellInfo->IsAutoRepeatRangedSpell() == spellInfo->IsAutoRepeatRangedSpell() && spellId > result.spellId))
        {
            result.spellId = spellId;
            result.spellInfo = spellInfo;
        }
    }

    return result;
}

SpellCastResult CheckEquippedFiringSpellCast(Player* bot, uint32 spellId, Unit* target)
{
    if (!bot || !spellId)
        return SPELL_FAILED_ERROR;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return SPELL_FAILED_SPELL_UNAVAILABLE;

    if (!target)
        target = bot;

    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    spell->m_targets.SetUnitTarget(target);

    Item* itemTarget = nullptr;
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        itemTarget = botAI->GetAiObjectContext()->GetValue<Item*>("item for spell", std::to_string(spellId))->Get();

    spell->m_targets.SetItemTarget(itemTarget);
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    return result;
}

SpellCastResult CheckBotSpellCast(Player* bot, uint32 spellId, Unit* target, Item* itemTarget = nullptr)
{
    if (!bot || !spellId)
        return SPELL_FAILED_ERROR;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return SPELL_FAILED_SPELL_UNAVAILABLE;

    if (!target)
        target = bot;

    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    spell->m_targets.SetUnitTarget(target);
    spell->m_targets.SetItemTarget(itemTarget);
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    return result;
}

SpellCastResult CheckBotSpellCast(Player* bot, uint32 spellId, float x, float y, float z, Item* itemTarget = nullptr)
{
    if (!bot || !spellId)
        return SPELL_FAILED_ERROR;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return SPELL_FAILED_SPELL_UNAVAILABLE;

    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    spell->m_targets.SetDst(x, y, z, 0.0f, bot->GetMapId());
    spell->m_targets.SetItemTarget(itemTarget);
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    return result;
}

bool IsUsableSpellCastResult(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_CAST_OK:
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
            return true;
        default:
            return false;
    }
}

bool ShouldRetrySpellCastLater(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_FAILED_NOT_READY:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_FAILED_MOVING:
            return true;
        default:
            return false;
    }
}

bool IsPotentialSapCastResult(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_CAST_OK:
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_ONLY_SHAPESHIFT:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_FAILED_NOT_READY:
        case SPELL_FAILED_ONLY_STEALTHED:
            return true;
        default:
            return false;
    }
}

bool IsPotentialPullOpenerCastResult(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_CAST_OK:
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_LINE_OF_SIGHT:
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_FAILED_NOT_SHAPESHIFT:
        case SPELL_FAILED_ONLY_SHAPESHIFT:
            return true;
        default:
            return false;
    }
}

std::string BuildTankRangedWeaponReport(Player* tank, Unit* target)
{
    Item* rangedWeapon = tank ? tank->GetWeaponForAttack(RANGED_ATTACK, true) : nullptr;
    std::string const weaponName = (rangedWeapon && rangedWeapon->GetTemplate()) ? rangedWeapon->GetTemplate()->Name1 : "<none>";
    uint32 const ammoCount = GetReadyAmmoCount(tank);
    uint32 const ammoId = tank ? tank->GetUInt32Value(PLAYER_AMMO_ID) : 0;
    ItemTemplate const* ammoProto = ammoId ? sObjectMgr->GetItemTemplate(ammoId) : nullptr;
    std::string const ammoName = ammoProto ? ammoProto->Name1 : "<none>";

    std::ostringstream out;
    out << "Ranged weapon " << weaponName << " has " << ammoCount << " ammo ready.";

    if (!tank || !rangedWeapon || !rangedWeapon->GetTemplate())
        return out.str();

    out << " Equipped ammo " << ammoName;
    if (ammoProto)
    {
        bool const ammoCompatible = tank->CheckAmmoCompatibility(ammoProto);
        out << " is " << GetProjectileTypeName(ammoProto) << " for "
            << GetRangedWeaponTypeName(rangedWeapon->GetTemplate()->SubClass)
            << " and is " << (ammoCompatible ? "compatible." : "incompatible.");
    }
    else
    {
        out << " is unavailable.";
    }

    EquippedFiringSpell const firingSpell = ResolveEquippedFiringSpell(tank);
    std::string const requiredSpellKey = firingSpell.expectedKey;
    std::string const requiredSpellName = ToDisplayWords(requiredSpellKey);

    if (requiredSpellKey.empty())
    {
        out << " No firing spell is required for this weapon type.";
        return out.str();
    }

    if (!firingSpell.spellId || !firingSpell.spellInfo || !tank->HasSpell(firingSpell.spellId))
    {
        out << " No firing spell (" << requiredSpellName << ") known.";
        return out.str();
    }

    char const* spellName = firingSpell.spellInfo->SpellName[LOCALE_enUS];
    float const minRange = firingSpell.spellInfo->GetMinRange(false);
    float const maxRange = firingSpell.spellInfo->GetMaxRange(false, tank, nullptr);
    out << " Firing spell " << (spellName && *spellName ? spellName : requiredSpellName) << " has range "
        << std::fixed << std::setprecision(1) << minRange << "-" << maxRange << ".";

    if (target)
    {
        SpellCastResult const castResult = CheckEquippedFiringSpellCast(tank, firingSpell.spellId, target);
        out << " CheckCast on target returned " << EnumUtils::ToTitle(castResult) << " (" << uint32(castResult) << ").";
    }

    return out.str();
}

std::string ResolveRangedPullSpellName(Player* bot)
{
    if (!bot)
        return {};

    Item* rangedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!rangedWeapon)
        return {};

    ItemTemplate const* proto = rangedWeapon->GetTemplate();
    if (!proto)
        return {};

    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            return "shoot gun";
        case ITEM_SUBCLASS_WEAPON_BOW:
            return "shoot bow";
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return "shoot crossbow";
        case ITEM_SUBCLASS_WEAPON_THROWN:
            return "throw";
        default:
            return {};
    }
}

std::string ResolveRangedKnownSpellKey(Player* bot)
{
    if (!bot)
        return {};

    Item* rangedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!rangedWeapon)
        return {};

    ItemTemplate const* proto = rangedWeapon->GetTemplate();
    if (!proto)
        return {};

    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return "shoot";
        case ITEM_SUBCLASS_WEAPON_THROWN:
            return "throw";
        default:
            return {};
    }
}

float ResolveSpellMaxRange(Player* bot, uint32 spellId)
{
    if (!bot || !spellId)
        return 0.0f;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return 0.0f;

    return spellInfo->GetMaxRange(false, bot, nullptr);
}

float ResolveSpellMinRange(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return 0.0f;

    return spellInfo->GetMinRange(false);
}

float ResolveSpellMaxRange(PlayerbotAI* botAI, std::string const& spellName)
{
    if (!botAI || spellName.empty())
        return 0.0f;

    return ResolveSpellMaxRange(botAI->GetBot(), ResolvePullSpellId(botAI, spellName));
}

float ResolveSpellMinRange(PlayerbotAI* botAI, std::string const& spellName)
{
    if (!botAI || spellName.empty())
        return 0.0f;

    return ResolveSpellMinRange(ResolvePullSpellId(botAI, spellName));
}

std::vector<uint32> ResolveGlobalPullSpellIds(std::string const& spellName, bool requireAutoRepeat)
{
    std::vector<uint32> spellIds;
    std::vector<uint32> fallbackSpellIds;
    if (spellName.empty())
        return spellIds;

    std::string const normalizedSpellName = ToLower(spellName);
    spellIds.reserve(4);
    fallbackSpellIds.reserve(4);

    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsPassive())
            continue;

        char const* rawName = spellInfo->SpellName[LOCALE_enUS];
        if (!rawName || !*rawName)
            continue;

        if (ToLower(rawName) != normalizedSpellName)
            continue;

        fallbackSpellIds.push_back(spellId);

        if (!requireAutoRepeat || spellInfo->IsAutoRepeatRangedSpell())
            spellIds.push_back(spellId);
    }

    return spellIds.empty() ? fallbackSpellIds : spellIds;
}

bool BuildEquippedRangedPullOpener(Player* bot, Unit* target, PullOpener& opener)
{
    opener = {};

    if (!bot)
        return false;

    Item* rangedWeapon = bot->GetWeaponForAttack(RANGED_ATTACK, true);
    if (!rangedWeapon || !rangedWeapon->GetTemplate())
        return false;

    uint32 const ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
    ItemTemplate const* ammoProto = ammoId ? sObjectMgr->GetItemTemplate(ammoId) : nullptr;
    if (!ammoProto || !bot->CheckAmmoCompatibility(ammoProto) || GetReadyAmmoCount(bot) == 0)
        return false;

    EquippedFiringSpell const firingSpell = ResolveEquippedFiringSpell(bot);
    if (!firingSpell.spellId || !firingSpell.spellInfo || !bot->HasSpell(firingSpell.spellId))
        return false;

    SpellCastResult const castCheck = CheckEquippedFiringSpellCast(bot, firingSpell.spellId, target);
    if (!IsPotentialPullOpenerCastResult(castCheck) || castCheck == SPELL_FAILED_NOT_READY)
        return false;

    opener.type = PullOpenerType::Spell;
    opener.name = ToDisplayWords(firingSpell.expectedKey);
    opener.spellIds.push_back(firingSpell.spellId);
    opener.checkHasSpell = true;
    opener.usesEquippedRangedSpell = true;
    opener.minRange = firingSpell.spellInfo->GetMinRange(false);
    opener.maxRange = firingSpell.spellInfo->GetMaxRange(false, bot, nullptr);
    return opener.IsValid();
}

bool BuildNamedSpellPullOpener(Player* bot, Unit* target, std::string const& spellName, PullOpener& opener)
{
    opener = {};

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !target || spellName.empty())
        return false;

    uint32 const spellId = ResolvePullSpellId(botAI, spellName);
    if (!spellId || !bot->HasSpell(spellId))
        return false;

    SpellCastResult const castCheck = CheckBotSpellCast(bot, spellId, target);
    if (!IsPotentialPullOpenerCastResult(castCheck) || castCheck == SPELL_FAILED_NOT_READY)
        return false;

    opener.type = PullOpenerType::Spell;
    opener.name = spellName;
    opener.rangeSpell = spellName;
    opener.spellIds.push_back(spellId);
    opener.checkHasSpell = true;
    opener.minRange = ResolveSpellMinRange(spellId);
    opener.maxRange = ResolveSpellMaxRange(bot, spellId);
    return opener.IsValid();
}

std::vector<std::string> BuildClassPullSpellPriority(Player* bot)
{
    if (!bot)
        return {};

    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return {"heroic throw"};
        case CLASS_DRUID:
            return {"faerie fire (feral)", "faerie fire", "moonfire"};
        case CLASS_PALADIN:
            return {"avenger's shield", "hand of reckoning", "exorcism"};
        case CLASS_DEATH_KNIGHT:
            return {"death grip", "icy touch"};
        default:
            return {};
    }
}

std::string BuildMissingPullOpenerMessage(Player* tank)
{
    if (!tank)
        return "the main tank is not available.";

    switch (tank->getClass())
    {
        case CLASS_WARRIOR:
            return "the main tank does not have a usable pull opener (heroic throw or ranged weapon).";
        case CLASS_DRUID:
            return "the main tank does not have a usable pull opener (faerie fire (feral), faerie fire, or moonfire).";
        case CLASS_PALADIN:
            return "the main tank does not have a usable pull opener (avenger's shield, hand of reckoning, or exorcism).";
        case CLASS_DEATH_KNIGHT:
            return "the main tank does not have a usable pull opener (death grip or icy touch).";
        default:
            return "the main tank does not have a usable supported pull opener.";
    }
}

void ResolveSpellRangeWindow(Player* bot, std::vector<uint32> const& spellIds, float& minRange, float& maxRange)
{
    minRange = 0.0f;
    maxRange = 0.0f;

    for (uint32 spellId : spellIds)
    {
        maxRange = std::max(maxRange, ResolveSpellMaxRange(bot, spellId));
        minRange = std::max(minRange, ResolveSpellMinRange(spellId));
    }
}

PullOpener ResolvePullOpener(Player* bot, Unit* target)
{
    if (!bot || !target)
        return {};

    PullOpener opener;
    for (std::string const& spellName : BuildClassPullSpellPriority(bot))
    {
        if (BuildNamedSpellPullOpener(bot, target, spellName, opener))
            return opener;
    }

    if (bot->getClass() == CLASS_WARRIOR && BuildEquippedRangedPullOpener(bot, target, opener))
        return opener;

    return {};
}

SpellCastResult CheckPullOpenerCast(Player* bot, PullOpener const& opener, Unit* target)
{
    if (!bot || !target || !opener.IsValid() || opener.spellIds.empty())
        return SPELL_FAILED_ERROR;

    uint32 const spellId = opener.spellIds.front();
    return opener.usesEquippedRangedSpell ? CheckEquippedFiringSpellCast(bot, spellId, target)
                                          : CheckBotSpellCast(bot, spellId, target);
}

bool IsPullOpenerShapeshiftFailure(SpellCastResult result)
{
    return result == SPELL_FAILED_NOT_SHAPESHIFT || result == SPELL_FAILED_ONLY_SHAPESHIFT;
}

bool IsDruidFeralPullOpener(PullOpener const& opener)
{
    return NormalizeCommand(opener.name) == "faerie fire (feral)";
}

bool IsDruidInBearPullForm(Player* tank)
{
    if (!tank || tank->getClass() != CLASS_DRUID)
        return false;

    ShapeshiftForm const form = tank->GetShapeshiftForm();
    return form == FORM_BEAR || form == FORM_DIREBEAR;
}

bool TryPrepareDruidTankPullForm(Player* tank, PlayerbotAI* tankAI, PullOpener const& opener)
{
    if (!tank || !tankAI || tank->getClass() != CLASS_DRUID || !IsDruidFeralPullOpener(opener))
        return false;

    for (std::string const& formName : {"dire bear form", "bear form"})
    {
        uint32 const formSpellId = ResolvePullSpellId(tankAI, formName);
        if (!formSpellId || !tank->HasSpell(formSpellId))
            continue;

        SpellCastResult const castCheck = CheckBotSpellCast(tank, formSpellId, tank);
        if (ShouldRetrySpellCastLater(castCheck))
            return true;

        if (!IsUsableSpellCastResult(castCheck) && castCheck != SPELL_FAILED_NOT_SHAPESHIFT)
            continue;

        if (tank->isMoving())
            tank->StopMoving();

        return tankAI->CastSpell(formSpellId, tank);
    }

    return false;
}

uint32 ResolvePullSpellId(PlayerbotAI* botAI, std::string const& spellName)
{
    if (!botAI || spellName.empty())
        return 0;

    return botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spellName)->Get();
}

float ResolveDesiredPullDistance(float minRange, float maxRange)
{
    if (maxRange <= 0.0f)
        return 0.0f;

    float const desired = std::max(minRange + PULL_MIN_RANGE_BUFFER, maxRange - PULL_RANGE_BUFFER);
    return std::min(desired, maxRange);
}

bool IsWithinPullWindow(Player* tank, Unit* target, float minRange, float maxRange)
{
    if (!tank || !target || maxRange <= 0.0f)
        return false;

    float const distance = tank->GetDistance(target);
    return distance <= maxRange && distance >= std::max(0.0f, minRange + 0.1f) && tank->IsWithinLOSInMap(target);
}

bool HasPullAlreadyCollapsedIntoMelee(Player* tank, Unit* target)
{
    if (!tank || !target)
        return false;

    float const engagementDistance = tank->GetCombatReach() + target->GetCombatReach() + 1.0f;
    return tank->IsWithinMeleeRange(target) ||
           target->IsWithinMeleeRange(tank) ||
           ((tank->GetVictim() == target || target->GetVictim() == tank) &&
            tank->IsWithinCombatRange(target, engagementDistance));
}

uint32 ResolvePullShotSettleDelay(Player* tank, PullOpener const& opener)
{
    if (!tank || !opener.IsValid() || opener.spellIds.empty())
        return 0;

    uint32 settleDelay = 0;
    if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(opener.spellIds.front()))
        settleDelay = spellInfo->CalcCastTime(tank);

    if (opener.usesEquippedRangedSpell)
    {
        uint32 const rangedAttackDelay = std::clamp<uint32>(tank->GetAttackTime(RANGED_ATTACK) + 250, 750, 4000);
        settleDelay = std::max(settleDelay, rangedAttackDelay);
    }
    else if (settleDelay > 0)
    {
        settleDelay += 100;
    }

    return settleDelay;
}

bool IsPullShotStillResolving(Player* tank)
{
    if (!tank)
        return false;

    if (tank->HasUnitState(UNIT_STATE_CASTING))
        return true;

    return tank->IsNonMeleeSpellCast(false, false, true, false, false);
}

void StopPetCombat(Player* bot)
{
    Pet* pet = bot ? bot->GetPet() : nullptr;
    if (!pet)
        return;

    pet->AttackStop();
    pet->SetTarget(ObjectGuid::Empty);

    if (CharmInfo* charmInfo = pet->GetCharmInfo())
    {
        charmInfo->SetCommandState(COMMAND_FOLLOW);
        charmInfo->SetIsCommandFollow(true);
        charmInfo->IsReturning();
    }

    pet->InterruptNonMeleeSpells(false);
    pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
}

void StopBotCombat(Player* bot, bool dropTarget)
{
    if (!IsLiveBot(bot))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (dropTarget)
        botAI->DoSpecificAction("drop target", Event(), true);

    bot->InterruptNonMeleeSpells(false);

    if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

    bot->AttackStop();

    if (bot->isMoving())
        bot->StopMoving();

    bot->ClearUnitState(UNIT_STATE_CHASE);
    bot->ClearUnitState(UNIT_STATE_FOLLOW);
    StopPetCombat(bot);
}

void ClearBotCombatStateForRetreat(Player* bot)
{
    if (!IsLiveBot(bot))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    botAI->GetAiObjectContext()->GetValue<Unit*>("old target")->Set(nullptr);
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
    botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Reset();

    bot->InterruptNonMeleeSpells(false);

    if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

    bot->AttackStop();

    if (bot->isMoving())
        bot->StopMoving();

    bot->ClearUnitState(UNIT_STATE_CHASE);
    bot->ClearUnitState(UNIT_STATE_FOLLOW);
    StopPetCombat(bot);
}

void ResetBotMovementState(PlayerbotAI* botAI)
{
    if (!botAI)
        return;

    botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
    botAI->GetAiObjectContext()->GetValue<time_t>("stay time")->Set(0);
}

void EnsureStrategy(PlayerbotAI* botAI, bool& addedFlag, BotState state, std::string const& strategy)
{
    if (!botAI || botAI->HasStrategy(strategy, state))
        return;

    botAI->ChangeStrategy("+" + strategy, state);
    addedFlag = true;
}

void RemoveStrategy(PlayerbotAI* botAI, bool addedFlag, BotState state, std::string const& strategy)
{
    if (!botAI || !addedFlag)
        return;

    botAI->ChangeStrategy("-" + strategy, state);
}

void SetTankTargetContext(Player* tank, Unit* target)
{
    if (!IsLiveBot(tank) || !target)
        return;

    PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
    if (!tankAI)
        return;

    tankAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    tankAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(target->GetGUID());
    tank->SetSelection(target->GetGUID());
    tank->SetTarget(target->GetGUID());
}

void SetBotTargetContext(Player* bot, Unit* target)
{
    if (!IsLiveBot(bot) || !target)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    bot->SetSelection(target->GetGUID());
    bot->SetTarget(target->GetGUID());
}

void FaceBotTowardsTarget(Player* bot, Unit* target)
{
    if (!bot || !target)
        return;

    ServerFacade::instance().SetFacingTo(bot, target);
}

bool IsFacingSpellTarget(Player* bot, Unit* target)
{
    if (!bot || !target)
        return false;

    return bot->HasInArc(static_cast<float>(M_PI), target);
}

bool NeedsFacingAdjustment(SpellCastResult result)
{
    return result == SPELL_FAILED_NOT_INFRONT || result == SPELL_FAILED_UNIT_NOT_INFRONT;
}

bool IsPotentialPullCcCastResult(SpellCastResult result)
{
    switch (result)
    {
        case SPELL_CAST_OK:
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_LINE_OF_SIGHT:
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_FAILED_NOT_READY:
            return true;
        default:
            return false;
    }
}

struct PullSession;

bool ArmTankPullStrategy(PullSession& session, Player* tank, Unit* target);

struct BotHoldState
{
    ObjectGuid guid = ObjectGuid::Empty;
    ObjectGuid ccTargetGuid = ObjectGuid::Empty;
    bool addedCombatPassive = false;
    bool addedCombatStay = false;
    bool addedNonCombatPassive = false;
    bool addedNonCombatStay = false;
    bool isTank = false;
    uint32 ccSpellId = 0;
    float ccMinRange = 0.0f;
    float ccMaxRange = 0.0f;
    bool savedPositions = false;
    bool hadNonCombatFollow = false;
    bool ccCastDone = false;
    bool ccReturning = false;
    PositionInfo previousStayPosition;
    PositionInfo previousReturnPosition;
};

struct StoredWorldPoint
{
    uint32 mapId = MAPID_INVALID;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    bool IsValid() const
    {
        return mapId != MAPID_INVALID;
    }
};

struct TankMarkerState
{
    bool awaitingPlacement = false;
    ObjectGuid sourceBotGuid = ObjectGuid::Empty;
    StoredWorldPoint position;
};

enum class PullStage
{
    MoveTankToPullRange,
    FirePullShot,
    WaitForPullShotResolve,
    FireFollowupPulls,
    WaitForFollowupResolve,
    ReturnTankToHoldPoint,
    WaitForMobArrival,
    WaitForCombatEnd
};

struct PullSession
{
    ObjectGuid ownerGuid = ObjectGuid::Empty;
    ObjectGuid tankGuid = ObjectGuid::Empty;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    ObjectGuid offTankGuid = ObjectGuid::Empty;
    ObjectGuid offTankTargetGuid = ObjectGuid::Empty;
    PullOpener opener;
    PullOpener offTankOpener;
    PullStage stage = PullStage::MoveTankToPullRange;
    uint32 startedAtMs = 0;
    uint32 pullShotAtMs = 0;
    uint32 offTankPullShotAtMs = 0;
    bool tankPullArmed = false;
    bool addedTankPullStrategy = false;
    bool moveStagePrepared = false;
    bool returnStagePrepared = false;
    bool waitStagePrepared = false;
    bool usesTankMarker = false;
    bool offTankMoveStagePrepared = false;
    bool offTankReturnStagePrepared = false;
    bool offTankWaitStagePrepared = false;
    bool usesOffTankMarker = false;
    bool mainTankCombatReleased = false;
    bool offTankCombatReleased = false;
    StoredWorldPoint tankMarkerPosition;
    StoredWorldPoint offTankMarkerPosition;
    std::vector<BotHoldState> botStates;
};

bool ArmTankPullStrategy(PullSession& session, Player* tank, Unit* target)
{
    if (!IsLiveBot(tank) || !target)
        return false;

    PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
    if (!tankAI)
        return false;

    SetTankTargetContext(tank, target);
    tankAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(target->GetGUID());

    bool const hadPull = tankAI->HasStrategy("pull", BOT_STATE_COMBAT);
    if (!hadPull)
        tankAI->ChangeStrategy("+pull", BOT_STATE_COMBAT);

    if (!tankAI->HasStrategy("pull", BOT_STATE_COMBAT))
        return false;

    session.addedTankPullStrategy = !hadPull;
    session.tankPullArmed = true;

    ResetBotMovementState(tankAI);
    tankAI->ChangeEngine(BOT_STATE_COMBAT);
    tankAI->DoNextAction();
    return true;
}

BotHoldState* FindBotState(PullSession& session, ObjectGuid const& guid)
{
    for (BotHoldState& state : session.botStates)
    {
        if (state.guid == guid)
            return &state;
    }

    return nullptr;
}

bool HasOffTankAssignment(PullSession const& session)
{
    return !session.offTankGuid.IsEmpty() && !session.offTankTargetGuid.IsEmpty() && session.offTankOpener.IsValid();
}

bool HasPendingSupportCrowdControl(PullSession const& session)
{
    for (BotHoldState const& state : session.botStates)
    {
        if (!state.isTank && state.ccSpellId && !state.ccTargetGuid.IsEmpty() && !state.ccCastDone)
            return true;
    }

    return false;
}

class PullMovementController final : public MovementAction
{
public:
    explicit PullMovementController(PlayerbotAI* botAI)
        : MovementAction(botAI, "bettercombat pull movement")
    {
    }

    bool MoveIntoRange(Unit* target, float distance)
    {
        return MoveTo(target, distance, MovementPriority::MOVEMENT_COMBAT);
    }

    bool MoveIntoMelee(Unit* target, float extraDistance)
    {
        return ReachCombatTo(target, extraDistance);
    }

    bool MoveToPoint(uint32 mapId, float x, float y, float z)
    {
        return MoveTo(mapId, x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
    }

    bool MoveToRangedLos(Unit* target)
    {
        return MoveToLOS(target, true);
    }

    bool FollowUnit(Unit* target, float distance)
    {
        return Follow(target, distance);
    }
};

class PullManager
{
public:
    static PullManager& Instance()
    {
        static PullManager instance;
        return instance;
    }

    bool TryHandleCommand(Player* owner, std::string const& msg)
    {
        if (!IsModuleEnabled() || !owner || NormalizeCommand(msg) != "pull")
            return false;

        std::vector<Player*> controlledBots = CollectControlledBots(owner);
        if (controlledBots.empty())
        {
            SendPlainSystemMessage(owner, "No controlled bots are available for pull.");
            return true;
        }

        Player* tank = FindControlledMainTank(owner, controlledBots);
        if (!tank)
        {
            SendPlainSystemMessage(owner, "No controlled main tank bot is available for pull.");
            return true;
        }

        StartSession(owner, controlledBots);
        return true;
    }

    bool TryHandleTankCommand(Player* owner, std::string const& msg)
    {
        if (!IsModuleEnabled() || !owner)
            return false;

        std::string const normalized = NormalizeCommand(msg);
        if (normalized != "tank" && normalized != "tank clear")
            return false;

        if (normalized == "tank clear")
        {
            std::vector<Player*> const controlledBots = CollectControlledBots(owner);
            if (Player* tank = FindControlledMainTank(owner, controlledBots))
            {
                if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
                    tankAI->HandleCommand(CHAT_MSG_SAY, std::string("rtsc unsave ") + TANK_MARKER_RTSC_NAME, owner);
            }

            tankMarkers.erase(owner->GetGUID());
            SendSystemMessage(owner, "tank pull position cleared.");
            return true;
        }

        std::vector<Player*> const controlledBots = CollectControlledBots(owner);
        Player* tank = FindControlledMainTank(owner, controlledBots);
        if (!tank)
        {
            SendSystemMessage(owner, "tank requires a controlled main tank bot in your current group or raid.");
            return true;
        }

        if (!EnsureGroundMarkerSpell(owner))
        {
            SendSystemMessage(owner, "tank marker could not enable Aedm.");
            return true;
        }

        ArmMarkerPlacement(owner, tank, tankMarkers, TANK_MARKER_RTSC_NAME, "tank marker armed. Cast Aedm to place the tank pull position.");
        return true;
    }

    bool TryHandleOffTankCommand(Player* owner, std::string const& msg)
    {
        if (!IsModuleEnabled() || !owner)
            return false;

        std::string const normalized = NormalizeCommand(msg);
        if (normalized != "offtank" && normalized != "offtank clear")
            return false;

        std::vector<Player*> const controlledBots = CollectControlledBots(owner);
        Player* mainTank = FindControlledMainTank(owner, controlledBots);
        Player* offTank = FindControlledOffTank(owner, controlledBots, mainTank);

        if (normalized == "offtank clear")
        {
            if (offTank)
            {
                if (PlayerbotAI* offTankAI = GET_PLAYERBOT_AI(offTank))
                    offTankAI->HandleCommand(CHAT_MSG_SAY, std::string("rtsc unsave ") + OFFTANK_MARKER_RTSC_NAME, owner);
            }

            offTankMarkers.erase(owner->GetGUID());
            SendSystemMessage(owner, "offtank pull position cleared.");
            return true;
        }

        if (!offTank)
        {
            SendSystemMessage(owner, "offtank requires a controlled off-tank bot in your current group or raid.");
            return true;
        }

        if (!EnsureGroundMarkerSpell(owner))
        {
            SendSystemMessage(owner, "offtank marker could not enable Aedm.");
            return true;
        }

        ArmMarkerPlacement(owner, offTank, offTankMarkers, OFFTANK_MARKER_RTSC_NAME,
                           "offtank marker armed. Cast Aedm to place the off-tank pull position.");
        return true;
    }

    void HandlePlayerSpellCast(Player* owner, Spell* spell)
    {
        if (!IsModuleEnabled() || !owner || !spell || !spell->m_spellInfo || spell->m_spellInfo->Id != GROUND_MARKER_SPELL_ID)
            return;

        if (HandleMarkerSpellCast(owner, spell, tankMarkers, TANK_MARKER_RTSC_NAME, "tank pull position saved.",
                                  "tank marker cancelled because no destination was selected."))
        {
            return;
        }

        HandleMarkerSpellCast(owner, spell, offTankMarkers, OFFTANK_MARKER_RTSC_NAME, "offtank pull position saved.",
                              "offtank marker cancelled because no destination was selected.");
    }

    bool TryHandleAutoMarkCommand(Player* owner, std::string const& msg)
    {
        if (!IsModuleEnabled() || !owner)
            return false;

        std::string const normalized = NormalizeCommand(msg);
        if (normalized != "automark" && normalized.rfind("automark ", 0) != 0)
            return false;

        bool enabled = IsAutoMarkEnabled(owner);
        if (normalized == "automark")
        {
            enabled = !enabled;
        }
        else
        {
            std::string const mode = TrimCopy(normalized.substr(std::char_traits<char>::length("automark ")));
            if (mode == "on")
                enabled = true;
            else if (mode == "off")
                enabled = false;
            else
            {
                SendSystemMessage(owner, "automark accepts `on`, `off`, or no argument.");
                return true;
            }
        }

        SetAutoMarkEnabled(owner, enabled);
        SendSystemMessage(owner, std::string("automark ") + (enabled ? "enabled." : "disabled."));
        return true;
    }

    void Update(uint32 diff)
    {
        if (!IsModuleEnabled())
            return;

        updateAccumulator += diff;
        if (updateAccumulator < PULL_UPDATE_INTERVAL_MS)
            return;

        updateAccumulator = 0;
        UpdateCombatRaidIcons();
        UpdateControlledCrowdControl();

        if (sessions.empty())
            return;

        for (auto it = sessions.begin(); it != sessions.end();)
        {
            if (!ProcessSession(it->second))
                it = sessions.erase(it);
            else
                ++it;
        }
    }

private:
    PullManager() = default;

    Unit* ResolveSessionTarget(PlayerbotAI* botAI, ObjectGuid const& guid)
    {
        if (!botAI || guid.IsEmpty())
            return nullptr;

        return botAI->GetUnit(guid);
    }

    bool MoveTankIntoPullPosition(PullSession& session, Player* tank, PlayerbotAI* tankAI, Unit* target, PullOpener const& opener,
                                  bool& prepared)
    {
        if (!tank || !tankAI || !target || !opener.IsValid())
            return false;

        if (!prepared)
        {
            PrepareTankForApproach(session, tank, target);
            prepared = true;
        }

        float const desiredDistance = ResolveDesiredPullDistance(opener.minRange, opener.maxRange);
        if (desiredDistance <= 0.0f)
            return false;

        float const distance = tank->GetDistance(target);
        float const minimumDistance = std::max(0.0f, opener.minRange + 0.1f);
        bool const withinPullDistance = distance <= opener.maxRange && distance >= minimumDistance;
        if (withinPullDistance && !tank->IsWithinLOSInMap(target))
        {
            PullMovementController movement(tankAI);
            movement.MoveToRangedLos(target);
            return false;
        }

        if (IsWithinPullWindow(tank, target, opener.minRange, opener.maxRange))
        {
            if (tank->isMoving())
                tank->StopMoving();

            if (!IsFacingSpellTarget(tank, target))
            {
                FaceBotTowardsTarget(tank, target);
                return false;
            }

            HoldTankBeforeShot(session, tank, target);
            return true;
        }

        float pullX = 0.0f;
        float pullY = 0.0f;
        float pullZ = 0.0f;
        target->GetNearPoint(tank, pullX, pullY, pullZ, 0.0f, desiredDistance, target->GetAngle(tank));
        HoldTankDuringApproach(session, tank, target, pullX, pullY, pullZ);

        PullMovementController movement(tankAI);
        if (!movement.MoveToPoint(target->GetMapId(), pullX, pullY, pullZ))
            movement.MoveIntoRange(target, desiredDistance);

        return false;
    }

    bool HasTankReachedCombatTrigger(Player* tank, Unit* target)
    {
        if (!tank || !target)
            return false;

        return tank->IsWithinMeleeRange(target) ||
               target->IsWithinMeleeRange(tank) ||
               (target->GetVictim() == tank &&
                tank->IsWithinCombatRange(target, tank->GetCombatReach() + target->GetCombatReach() + 1.0f));
    }

    void ClearOffTankAssignment(PullSession& session)
    {
        session.offTankGuid = ObjectGuid::Empty;
        session.offTankTargetGuid = ObjectGuid::Empty;
        session.offTankOpener = {};
        session.offTankPullShotAtMs = 0;
        session.offTankMoveStagePrepared = false;
        session.offTankReturnStagePrepared = false;
        session.offTankWaitStagePrepared = false;
        session.offTankCombatReleased = false;
    }

    bool EnsureGroundMarkerSpell(Player* owner)
    {
        if (!owner)
            return false;

        if (owner->HasSpell(GROUND_MARKER_SPELL_ID))
            return true;

        std::vector<Player*> const controlledBots = CollectControlledBots(owner);
        Player* tank = FindControlledMainTank(owner, controlledBots);
        Player* sourceBot = tank ? tank : (!controlledBots.empty() ? controlledBots.front() : nullptr);
        if (PlayerbotAI* botAI = sourceBot ? GET_PLAYERBOT_AI(sourceBot) : nullptr)
            botAI->HandleCommand(CHAT_MSG_SAY, "rtsc", owner);

        if (!owner->HasSpell(GROUND_MARKER_SPELL_ID))
            owner->learnSpell(GROUND_MARKER_SPELL_ID, false);

        return owner->HasSpell(GROUND_MARKER_SPELL_ID);
    }

    void ArmMarkerPlacement(Player* owner, Player* sourceBot, std::map<ObjectGuid, TankMarkerState>& markers,
                            char const* rtscName, std::string const& armedMessage)
    {
        if (!owner || !sourceBot || !rtscName)
            return;

        TankMarkerState& markerState = markers[owner->GetGUID()];
        markerState.awaitingPlacement = true;
        markerState.sourceBotGuid = sourceBot->GetGUID();
        markerState.position = StoredWorldPoint();

        if (PlayerbotAI* sourceBotAI = GET_PLAYERBOT_AI(sourceBot))
        {
            sourceBotAI->HandleCommand(CHAT_MSG_SAY, std::string("rtsc unsave ") + rtscName, owner);
            sourceBotAI->HandleCommand(CHAT_MSG_SAY, std::string("rtsc save ") + rtscName, owner);
        }

        SendSystemMessage(owner, armedMessage);
    }

    bool HandleMarkerSpellCast(Player* owner, Spell* spell, std::map<ObjectGuid, TankMarkerState>& markers, char const* rtscName,
                               std::string const& savedMessage, std::string const& cancelledMessage)
    {
        if (!owner || !spell || !rtscName)
            return false;

        auto markerItr = markers.find(owner->GetGUID());
        if (markerItr == markers.end() || !markerItr->second.awaitingPlacement)
            return false;

        markerItr->second.awaitingPlacement = false;
        Player* sourceBot = ObjectAccessor::FindPlayer(markerItr->second.sourceBotGuid);
        PlayerbotAI* sourceBotAI = sourceBot ? GET_PLAYERBOT_AI(sourceBot) : nullptr;
        if (sourceBotAI)
        {
            WorldPosition const rtscPosition =
                sourceBotAI->GetAiObjectContext()->GetValue<WorldPosition>("RTSC saved location", rtscName)->Get();
            if (rtscPosition)
            {
                markerItr->second.position.mapId = rtscPosition.GetMapId();
                markerItr->second.position.x = rtscPosition.GetPositionX();
                markerItr->second.position.y = rtscPosition.GetPositionY();
                markerItr->second.position.z = rtscPosition.GetPositionZ();
                SendSystemMessage(owner, savedMessage);
                return true;
            }
        }

        if (!spell->m_targets.HasDst() || !spell->m_targets.GetDstPos())
        {
            SendSystemMessage(owner, cancelledMessage);
            return true;
        }

        WorldLocation const* destination = spell->m_targets.GetDstPos();
        markerItr->second.position.mapId = destination->GetMapId();
        markerItr->second.position.x = destination->GetPositionX();
        markerItr->second.position.y = destination->GetPositionY();
        markerItr->second.position.z = destination->GetPositionZ();

        SendSystemMessage(owner, savedMessage);
        return true;
    }

    StoredWorldPoint const* GetStoredMarker(Player const* owner, Player const* sourceBot, std::map<ObjectGuid, TankMarkerState>& markers,
                                            char const* rtscName)
    {
        if (!owner || !rtscName)
            return nullptr;

        auto const itr = markers.find(owner->GetGUID());
        if (sourceBot)
        {
            PlayerbotAI* sourceBotAI = GET_PLAYERBOT_AI(const_cast<Player*>(sourceBot));
            if (sourceBotAI)
            {
                WorldPosition const rtscPosition =
                    sourceBotAI->GetAiObjectContext()->GetValue<WorldPosition>("RTSC saved location", rtscName)->Get();
                if (rtscPosition)
                {
                    auto& markerState = markers[owner->GetGUID()];
                    markerState.sourceBotGuid = sourceBot->GetGUID();
                    markerState.position.mapId = rtscPosition.GetMapId();
                    markerState.position.x = rtscPosition.GetPositionX();
                    markerState.position.y = rtscPosition.GetPositionY();
                    markerState.position.z = rtscPosition.GetPositionZ();
                    return &markerState.position;
                }
            }
        }

        if (itr == tankMarkers.end() || !itr->second.position.IsValid())
            return nullptr;

        return &itr->second.position;
    }

    StoredWorldPoint const* GetStoredTankMarker(Player const* owner, Player const* tank)
    {
        return GetStoredMarker(owner, tank, tankMarkers, TANK_MARKER_RTSC_NAME);
    }

    StoredWorldPoint const* GetStoredOffTankMarker(Player const* owner, Player const* offTank)
    {
        return GetStoredMarker(owner, offTank, offTankMarkers, OFFTANK_MARKER_RTSC_NAME);
    }

    bool IsAutoMarkEnabled(Player const* owner) const
    {
        if (!owner)
            return false;

        return disabledAutoMarkOwners.find(owner->GetGUID()) == disabledAutoMarkOwners.end();
    }

    void SetAutoMarkEnabled(Player const* owner, bool enabled)
    {
        if (!owner)
            return;

        if (enabled)
            disabledAutoMarkOwners.erase(owner->GetGUID());
        else
            disabledAutoMarkOwners.insert(owner->GetGUID());
    }

    int8 GetRaidIconIndexForTarget(Group* group, ObjectGuid const& guid) const
    {
        if (!group || guid.IsEmpty())
            return -1;

        for (int8 index = 0; index <= SKULL_RTI_INDEX; ++index)
        {
            if (group->GetTargetIcon(index) == guid)
                return index;
        }

        return -1;
    }

    bool HasAnyRaidIcon(Group* group, ObjectGuid const& guid) const
    {
        return GetRaidIconIndexForTarget(group, guid) >= 0;
    }

    void AddCombatTargetCandidate(Player* owner, Unit* target, std::vector<Unit*>& targets, std::vector<ObjectGuid>& seenGuids) const
    {
        if (!owner || !target || !target->IsInCombat() || !IsValidPullTarget(owner, target))
            return;

        ObjectGuid const guid = target->GetGUID();
        if (std::find(seenGuids.begin(), seenGuids.end(), guid) != seenGuids.end())
            return;

        seenGuids.push_back(guid);
        targets.push_back(target);
    }

    void AddCombatTargetsFromUnit(Player* owner, Unit* source, std::vector<Unit*>& targets, std::vector<ObjectGuid>& seenGuids) const
    {
        if (!owner || !source || !source->IsInWorld())
            return;

        AddCombatTargetCandidate(owner, source->GetVictim(), targets, seenGuids);

        if (Player* playerSource = source->ToPlayer())
        {
            AddCombatTargetCandidate(owner, ObjectAccessor::GetUnit(*playerSource, playerSource->GetTarget()), targets, seenGuids);

            if (PlayerbotAI* sourceAI = GET_PLAYERBOT_AI(playerSource))
                AddCombatTargetCandidate(owner, sourceAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get(), targets, seenGuids);
        }

        for (Unit* attacker : source->getAttackers())
            AddCombatTargetCandidate(owner, attacker, targets, seenGuids);
    }

    std::vector<Unit*> CollectActiveCombatTargets(Player* owner, std::vector<Player*> const& controlledBots) const
    {
        std::vector<Unit*> targets;
        std::vector<ObjectGuid> seenGuids;

        AddCombatTargetsFromUnit(owner, owner, targets, seenGuids);

        for (Player* bot : controlledBots)
        {
            if (!IsLiveBot(bot))
                continue;

            AddCombatTargetsFromUnit(owner, bot, targets, seenGuids);
        }

        return targets;
    }

    Unit* FindCombatTargetByGuid(std::vector<Unit*> const& targets, ObjectGuid const& guid) const
    {
        if (guid.IsEmpty())
            return nullptr;

        for (Unit* target : targets)
        {
            if (target && target->GetGUID() == guid)
                return target;
        }

        return nullptr;
    }

    Unit* PickRandomUnmarkedCombatTarget(Group* group, std::vector<Unit*> const& targets, ObjectGuid const& excludeGuid) const
    {
        std::vector<Unit*> candidates;
        candidates.reserve(targets.size());

        for (Unit* target : targets)
        {
            if (!target)
                continue;

            ObjectGuid const guid = target->GetGUID();
            if (!guid.IsEmpty() && guid == excludeGuid)
                continue;

            if (HasAnyRaidIcon(group, guid))
                continue;

            candidates.push_back(target);
        }

        if (candidates.empty())
            return nullptr;

        uint32 const index = candidates.size() == 1 ? 0 : urand(0, candidates.size() - 1);
        return candidates[index];
    }

    Unit* PickRandomMarkedCombatTarget(Group* group, std::vector<Unit*> const& targets, ObjectGuid const& excludeGuid) const
    {
        std::vector<Unit*> candidates;
        candidates.reserve(targets.size());

        for (Unit* target : targets)
        {
            if (!target)
                continue;

            ObjectGuid const guid = target->GetGUID();
            if (!guid.IsEmpty() && guid == excludeGuid)
                continue;

            int8 const iconIndex = GetRaidIconIndexForTarget(group, guid);
            if (iconIndex < 0 || iconIndex == CROSS_RTI_INDEX || iconIndex == SKULL_RTI_INDEX)
                continue;

            candidates.push_back(target);
        }

        if (candidates.empty())
            return nullptr;

        uint32 const index = candidates.size() == 1 ? 0 : urand(0, candidates.size() - 1);
        return candidates[index];
    }

    Unit* FindPreferredSkullTarget(Player* tank, Group* group, std::vector<Unit*> const& activeTargets) const
    {
        if (!IsLiveBot(tank) || !group)
            return nullptr;

        auto const isEligible = [&](Unit* target)
        {
            if (!target)
                return false;

            bool found = false;
            for (Unit* activeTarget : activeTargets)
            {
                if (activeTarget && activeTarget->GetGUID() == target->GetGUID())
                {
                    found = true;
                    break;
                }
            }

            if (!found)
                return false;

            if (IsTankProtectedCcTarget(group, target))
                return false;

            int8 const existingIcon = GetRaidIconIndexForTarget(group, target->GetGUID());
            return existingIcon < 0 || existingIcon == CROSS_RTI_INDEX || existingIcon == SKULL_RTI_INDEX;
        };

        if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
        {
            if (Unit* currentTarget = tankAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get(); isEligible(currentTarget))
                return currentTarget;
        }

        if (Unit* victim = tank->GetVictim(); isEligible(victim))
            return victim;

        if (Unit* selected = ObjectAccessor::GetUnit(*tank, tank->GetTarget()); isEligible(selected))
            return selected;

        return nullptr;
    }

    Unit* PickRandomAttackableCombatTarget(Group* group, std::vector<Unit*> const& targets, ObjectGuid const& excludeGuid) const
    {
        std::vector<Unit*> candidates;
        candidates.reserve(targets.size());

        for (Unit* target : targets)
        {
            if (!target)
                continue;

            ObjectGuid const guid = target->GetGUID();
            if (!guid.IsEmpty() && guid == excludeGuid)
                continue;

            if (IsTankProtectedCcTarget(group, target))
                continue;

            candidates.push_back(target);
        }

        if (candidates.empty())
            return nullptr;

        uint32 const index = candidates.size() == 1 ? 0 : urand(0, candidates.size() - 1);
        return candidates[index];
    }

    void MaintainCombatRaidIcons(Player* owner, Player* tank, std::vector<Player*> const& controlledBots)
    {
        if (!owner || !IsLiveBot(tank))
            return;

        Group* group = owner->GetGroup();
        if (!group)
            return;

        ObjectGuid const skullGuid = group->GetTargetIcon(SKULL_RTI_INDEX);
        Unit* resolvedSkull = !skullGuid.IsEmpty() ? ObjectAccessor::GetUnit(*owner, skullGuid) : nullptr;
        if (!skullGuid.IsEmpty() && (!resolvedSkull || resolvedSkull->isDead() || !IsValidPullTarget(owner, resolvedSkull)))
            group->SetTargetIcon(SKULL_RTI_INDEX, tank->GetGUID(), ObjectGuid::Empty);

        ObjectGuid const crossGuid = group->GetTargetIcon(CROSS_RTI_INDEX);
        Unit* resolvedCross = !crossGuid.IsEmpty() ? ObjectAccessor::GetUnit(*owner, crossGuid) : nullptr;
        if (!crossGuid.IsEmpty() && (!resolvedCross || resolvedCross->isDead() || !IsValidPullTarget(owner, resolvedCross)))
            group->SetTargetIcon(CROSS_RTI_INDEX, tank->GetGUID(), ObjectGuid::Empty);

        std::vector<Unit*> const activeTargets = CollectActiveCombatTargets(owner, controlledBots);
        if (activeTargets.empty())
            return;

        ObjectGuid skullGuidActive = group->GetTargetIcon(SKULL_RTI_INDEX);
        Unit* skullTarget = FindCombatTargetByGuid(activeTargets, skullGuidActive);
        if (!skullTarget && !skullGuidActive.IsEmpty())
            group->SetTargetIcon(SKULL_RTI_INDEX, tank->GetGUID(), ObjectGuid::Empty);

        ObjectGuid crossGuidActive = group->GetTargetIcon(CROSS_RTI_INDEX);
        Unit* crossTarget = FindCombatTargetByGuid(activeTargets, crossGuidActive);
        if (!crossTarget && !crossGuidActive.IsEmpty())
            group->SetTargetIcon(CROSS_RTI_INDEX, tank->GetGUID(), ObjectGuid::Empty);

        Unit* const preferredSkull = FindPreferredSkullTarget(tank, group, activeTargets);
        if (!skullTarget)
        {
            if (preferredSkull)
            {
                group->SetTargetIcon(SKULL_RTI_INDEX, tank->GetGUID(), preferredSkull->GetGUID());
                skullTarget = preferredSkull;
                skullGuidActive = preferredSkull->GetGUID();

                if (crossTarget && crossTarget->GetGUID() == skullGuidActive)
                {
                    group->SetTargetIcon(CROSS_RTI_INDEX, tank->GetGUID(), ObjectGuid::Empty);
                    crossTarget = nullptr;
                }
            }
            else if (crossTarget)
            {
                group->SetTargetIcon(SKULL_RTI_INDEX, tank->GetGUID(), crossTarget->GetGUID());
                group->SetTargetIcon(CROSS_RTI_INDEX, tank->GetGUID(), ObjectGuid::Empty);
                skullTarget = crossTarget;
                crossTarget = nullptr;
                skullGuidActive = skullTarget->GetGUID();
            }
            else if (Unit* replacement = PickRandomUnmarkedCombatTarget(group, activeTargets, ObjectGuid::Empty))
            {
                group->SetTargetIcon(SKULL_RTI_INDEX, tank->GetGUID(), replacement->GetGUID());
                skullTarget = replacement;
                skullGuidActive = replacement->GetGUID();
            }
            else if (Unit* replacement = PickRandomMarkedCombatTarget(group, activeTargets, ObjectGuid::Empty))
            {
                group->SetTargetIcon(SKULL_RTI_INDEX, tank->GetGUID(), replacement->GetGUID());
                skullTarget = replacement;
                skullGuidActive = replacement->GetGUID();
            }
        }

        if (!crossTarget)
        {
            if (Unit* replacement = PickRandomUnmarkedCombatTarget(group, activeTargets, skullGuidActive))
                group->SetTargetIcon(CROSS_RTI_INDEX, tank->GetGUID(), replacement->GetGUID());
        }
    }

    void UpdateCombatRaidIcons()
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());

        for (auto const& [ownerGuid, owner] : ObjectAccessor::GetPlayers())
        {
            if (!owner || !owner->IsInWorld() || owner->IsDuringRemoveFromWorld() || IsLiveBot(owner))
                continue;

            std::vector<Player*> const controlledBots = CollectControlledBots(owner);
            if (controlledBots.empty())
                continue;

            auto sessionIt = sessions.find(ownerGuid);
            if (sessionIt != sessions.end() && sessionIt->second.stage != PullStage::WaitForCombatEnd)
                continue;

            Player* tank = FindControlledMainTank(owner, controlledBots);
            if (!tank)
                continue;

            if (IsAutoMarkEnabled(owner))
                MaintainCombatRaidIcons(owner, tank, controlledBots);

            EnforceTankCombatTarget(owner, tank, controlledBots);
        }
    }

    void MaintainAssignedCrowdControl(Player* owner, Player* bot)
    {
        if (!owner || !IsLiveBot(bot) || PlayerbotAI::IsTank(bot) || PlayerbotAI::IsTank(bot, true))
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || botAI->GetMaster() != owner)
            return;

        Unit* ccTarget = ResolveBotAssignedRtiCcTarget(owner, bot);
        if (!IsValidPullTarget(owner, ccTarget))
            return;

        Group* group = owner->GetGroup();
        if (group && group->GetTargetIcon(SKULL_RTI_INDEX) == ccTarget->GetGUID())
            return;

        if (!owner->IsInCombat() && !bot->IsInCombat() && !ccTarget->IsInCombat())
            return;

        uint32 spellId = 0;
        float minRange = 0.0f;
        float maxRange = 0.0f;
        if (!ResolveSupportCcSpell(bot, ccTarget, spellId, minRange, maxRange))
            return;

        std::string const family = ResolveProtectedCcFamily(spellId);
        if (family.empty())
            return;

        ObjectGuid const casterGuid = bot->GetGUID();
        Aura* ownedAura = FindProtectedCcAura(ccTarget, &casterGuid, &family);
        if (ownedAura)
        {
            if (ownedAura->IsPermanent() || ownedAura->GetDuration() > CONTROLLED_CC_REFRESH_MS)
                return;
        }
        else if (FindProtectedCcAura(ccTarget))
        {
            return;
        }

        SpellCastResult const castCheck = CheckBotSpellCast(bot, spellId, ccTarget);
        if (ShouldRetrySpellCastLater(castCheck))
            return;

        if (NeedsFacingAdjustment(castCheck))
        {
            if (bot->isMoving())
                bot->StopMoving();

            FaceBotTowardsTarget(bot, ccTarget);
            return;
        }

        if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
        {
            PullMovementController movement(botAI);
            float const desiredDistance = ResolveDesiredPullDistance(minRange, maxRange);
            if (!ccTarget->IsWithinLOSInMap(bot))
                movement.MoveToRangedLos(ccTarget);
            else
                movement.MoveIntoRange(ccTarget, desiredDistance);
            return;
        }

        if (castCheck == SPELL_FAILED_LINE_OF_SIGHT)
        {
            PullMovementController movement(botAI);
            movement.MoveToRangedLos(ccTarget);
            return;
        }

        if (castCheck == SPELL_FAILED_MOVING)
        {
            if (bot->isMoving())
                bot->StopMoving();
            return;
        }

        if (!IsUsableSpellCastResult(castCheck))
            return;

        if (bot->isMoving())
        {
            bot->StopMoving();
            return;
        }

        SetBotTargetContext(bot, ccTarget);
        botAI->CastSpell(spellId, ccTarget);
    }

    void UpdateControlledCrowdControl()
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());

        for (auto const& [ownerGuid, owner] : ObjectAccessor::GetPlayers())
        {
            if (!owner || !owner->IsInWorld() || owner->IsDuringRemoveFromWorld() || IsLiveBot(owner))
                continue;

            auto sessionIt = sessions.find(ownerGuid);
            if (sessionIt != sessions.end() && sessionIt->second.stage != PullStage::WaitForCombatEnd)
                continue;

            std::vector<Player*> const controlledBots = CollectControlledBots(owner);
            if (controlledBots.empty())
                continue;

            for (Player* bot : controlledBots)
                MaintainAssignedCrowdControl(owner, bot);
        }
    }

    void EnforceTankCombatTarget(Player* owner, Player* tank, std::vector<Player*> const& controlledBots)
    {
        if (!owner || !IsLiveBot(tank))
            return;

        Group* group = owner->GetGroup();
        if (!group)
            return;

        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI)
            return;

        std::vector<Unit*> const activeTargets = CollectActiveCombatTargets(owner, controlledBots);
        if (activeTargets.empty())
            return;

        Unit* currentTarget = tankAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        Unit* victim = tank->GetVictim();
        Unit* selected = ObjectAccessor::GetUnit(*tank, tank->GetTarget());

        bool const isOnProtectedCc =
            IsTankProtectedCcTarget(group, currentTarget) || IsTankProtectedCcTarget(group, victim) || IsTankProtectedCcTarget(group, selected);

        ObjectGuid const skullGuid = group->GetTargetIcon(SKULL_RTI_INDEX);
        Unit* desiredTarget = FindCombatTargetByGuid(activeTargets, skullGuid);
        if (!desiredTarget)
            desiredTarget = FindPreferredSkullTarget(tank, group, activeTargets);
        if (!desiredTarget)
            desiredTarget = PickRandomAttackableCombatTarget(group, activeTargets, ObjectGuid::Empty);

        if (!desiredTarget || !IsValidPullTarget(owner, desiredTarget))
            return;

        bool const alreadyOnDesiredTarget =
            (currentTarget && currentTarget->GetGUID() == desiredTarget->GetGUID()) ||
            (victim && victim->GetGUID() == desiredTarget->GetGUID()) ||
            (selected && selected->GetGUID() == desiredTarget->GetGUID());

        if (!isOnProtectedCc && alreadyOnDesiredTarget)
            return;

        if (!isOnProtectedCc && !tank->IsInCombat())
            return;

        SetTankTargetContext(tank, desiredTarget);
        tankAI->ChangeEngine(BOT_STATE_COMBAT);
        tankAI->DoSpecificAction("attack", Event(), true);
    }

    PositionMap& GetPositionMap(PlayerbotAI* botAI)
    {
        return botAI->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
    }

    void SavePositions(BotHoldState& state, PlayerbotAI* botAI)
    {
        if (!botAI || state.savedPositions)
            return;

        PositionMap& positions = GetPositionMap(botAI);
        auto const stayIt = positions.find("stay");
        auto const returnIt = positions.find("return");
        state.previousStayPosition = stayIt != positions.end() ? stayIt->second : PositionInfo();
        state.previousReturnPosition = returnIt != positions.end() ? returnIt->second : PositionInfo();
        state.savedPositions = true;
    }

    void SetHoldPosition(BotHoldState& state, PlayerbotAI* botAI, float x, float y, float z)
    {
        if (!botAI)
            return;

        SavePositions(state, botAI);
        PositionMap& positions = GetPositionMap(botAI);
        positions["stay"].Set(x, y, z, botAI->GetBot()->GetMapId());
        positions["return"].Set(x, y, z, botAI->GetBot()->GetMapId());
    }

    void RestorePositions(BotHoldState const& state, PlayerbotAI* botAI)
    {
        if (!botAI || !state.savedPositions)
            return;

        PositionMap& positions = GetPositionMap(botAI);
        positions["stay"] = state.previousStayPosition;
        positions["return"] = state.previousReturnPosition;
    }

    void RestoreFollowShortcutState(PlayerbotAI* botAI)
    {
        if (!botAI)
            return;

        botAI->ChangeStrategy("+follow,-passive,-grind,-move from group", BOT_STATE_NON_COMBAT);
        botAI->ChangeStrategy("-stay,-follow,-passive,-grind,-move from group", BOT_STATE_COMBAT);
        botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Reset();

        PositionMap& positions = GetPositionMap(botAI);
        positions["return"].Reset();
        positions["stay"].Reset();
    }

    void CreateGroundMarkerVisual(Player* source, StoredWorldPoint const& point)
    {
        if (!source || !point.IsValid())
            return;

        if (Creature* marker = source->SummonCreature(GROUND_MARKER_VISUAL_ENTRY, point.x, point.y, point.z,
                                                      source->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, 2000.0f))
        {
            marker->SetObjectScale(0.5f);
        }
    }

    bool IsStoredPointValidForUnit(StoredWorldPoint const& point, WorldObject const* unit)
    {
        return unit && point.IsValid() && unit->GetMapId() == point.mapId;
    }

    bool HasReachedStoredWorldPoint(Player* bot, StoredWorldPoint const& point, float distance)
    {
        return bot && IsStoredPointValidForUnit(point, bot) && bot->GetDistance(point.x, point.y, point.z) <= distance;
    }

    bool ResolveSupportCcSpell(Player* bot, Unit* target, uint32& spellId, float& minRange, float& maxRange) const
    {
        spellId = 0;
        minRange = 0.0f;
        maxRange = 0.0f;

        if (!IsLiveBot(bot) || !target)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        std::vector<std::string> candidateNames;
        switch (bot->getClass())
        {
            case CLASS_MAGE:
                candidateNames = {"polymorph"};
                break;
            case CLASS_PRIEST:
                candidateNames = {"shackle undead"};
                break;
            case CLASS_DRUID:
                candidateNames = {"hibernate", "entangling roots", "cyclone"};
                break;
            case CLASS_PALADIN:
                candidateNames = {"repentance"};
                break;
            case CLASS_SHAMAN:
                candidateNames = {"hex"};
                break;
            case CLASS_WARLOCK:
                candidateNames = {"banish"};
                break;
            default:
                return false;
        }

        for (std::string const& candidateName : candidateNames)
        {
            uint32 const candidateSpellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", candidateName)->Get();
            if (!candidateSpellId || !bot->HasSpell(candidateSpellId))
                continue;

            SpellCastResult const castCheck = CheckBotSpellCast(bot, candidateSpellId, target);
            if (!IsPotentialPullCcCastResult(castCheck))
                continue;

            float const candidateMaxRange = ResolveSpellMaxRange(bot, candidateSpellId);
            if (candidateMaxRange <= 0.0f)
                continue;

            spellId = candidateSpellId;
            minRange = ResolveSpellMinRange(candidateSpellId);
            maxRange = candidateMaxRange;
            return true;
        }

        return false;
    }

    void InitializeSupportCc(PullSession& session, Player* owner, std::vector<Player*> const& controlledBots, Unit* pullTarget,
                             Unit* offTankTarget)
    {
        for (BotHoldState& state : session.botStates)
        {
            if (state.isTank)
                continue;

            Player* bot = ObjectAccessor::FindPlayer(state.guid);
            if (!IsLiveBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || botAI->GetMaster() != owner)
                continue;

            Unit* ccTarget = ResolveBotAssignedRtiCcTarget(owner, bot);
            if (!IsValidPullTarget(owner, ccTarget))
                continue;

            if (pullTarget && ccTarget->GetGUID() == pullTarget->GetGUID())
                continue;

            if (offTankTarget && ccTarget->GetGUID() == offTankTarget->GetGUID())
                continue;

            uint32 spellId = 0;
            float minRange = 0.0f;
            float maxRange = 0.0f;
            if (!ResolveSupportCcSpell(bot, ccTarget, spellId, minRange, maxRange))
                continue;

            state.ccTargetGuid = ccTarget->GetGUID();
            state.ccSpellId = spellId;
            state.ccMinRange = minRange;
            state.ccMaxRange = maxRange;
            state.ccCastDone = false;
            state.ccReturning = false;
        }
    }

    void ManageSupportCrowdControl(PullSession& session, Player* owner, bool allowCast)
    {
        Player* master = owner;
        if (!master || !master->IsInWorld())
            return;

        for (BotHoldState& state : session.botStates)
        {
            if (state.isTank || !state.ccSpellId || state.ccTargetGuid.IsEmpty())
                continue;

            Player* bot = ObjectAccessor::FindPlayer(state.guid);
            if (!IsLiveBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || botAI->GetMaster() != owner)
                continue;

            Unit* ccTarget = botAI->GetUnit(state.ccTargetGuid);
            if (!IsValidPullTarget(owner, ccTarget))
            {
                state.ccCastDone = true;
                state.ccReturning = true;
                continue;
            }

            SetBotTargetContext(bot, ccTarget);

            if (state.ccCastDone)
            {
                if (bot->HasUnitState(UNIT_STATE_CASTING) || bot->IsNonMeleeSpellCast(false, false, true, false, false))
                    continue;

                state.ccReturning = true;
                PullMovementController movement(botAI);
                movement.FollowUnit(master, sPlayerbotAIConfig.followDistance);
                continue;
            }

            SpellCastResult const castCheck = CheckBotSpellCast(bot, state.ccSpellId, ccTarget);
            if (!allowCast)
            {
                if (state.addedNonCombatStay)
                {
                    if (castCheck == SPELL_FAILED_OUT_OF_RANGE ||
                        castCheck == SPELL_FAILED_LINE_OF_SIGHT ||
                        NeedsFacingAdjustment(castCheck) ||
                        castCheck == SPELL_FAILED_MOVING)
                    {
                        botAI->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);
                        state.addedNonCombatStay = false;
                    }
                }

                if (NeedsFacingAdjustment(castCheck))
                {
                    if (bot->isMoving())
                        bot->StopMoving();

                    FaceBotTowardsTarget(bot, ccTarget);
                }
                else if (castCheck == SPELL_FAILED_OUT_OF_RANGE || castCheck == SPELL_FAILED_MOVING)
                {
                    PullMovementController movement(botAI);
                    float const desiredDistance = ResolveDesiredPullDistance(state.ccMinRange, state.ccMaxRange);
                    if (!ccTarget->IsWithinLOSInMap(bot))
                        movement.MoveToRangedLos(ccTarget);
                    else
                        movement.MoveIntoRange(ccTarget, desiredDistance);
                }
                else if (castCheck == SPELL_FAILED_LINE_OF_SIGHT)
                {
                    PullMovementController movement(botAI);
                    movement.MoveToRangedLos(ccTarget);
                }
                else if (bot->isMoving())
                {
                    bot->StopMoving();
                }
                else
                {
                    SetHoldPosition(state, botAI, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                    EnsureStrategy(botAI, state.addedNonCombatStay, BOT_STATE_NON_COMBAT, "stay");
                }

                continue;
            }

            if (state.addedNonCombatStay)
            {
                botAI->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);
                state.addedNonCombatStay = false;
            }

            if (ShouldRetrySpellCastLater(castCheck))
                continue;

            if (NeedsFacingAdjustment(castCheck))
            {
                if (bot->isMoving())
                    bot->StopMoving();

                FaceBotTowardsTarget(bot, ccTarget);
                continue;
            }

            if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
            {
                PullMovementController movement(botAI);
                float const desiredDistance = ResolveDesiredPullDistance(state.ccMinRange, state.ccMaxRange);
                if (!ccTarget->IsWithinLOSInMap(bot))
                    movement.MoveToRangedLos(ccTarget);
                else
                    movement.MoveIntoRange(ccTarget, desiredDistance);
                continue;
            }

            if (castCheck == SPELL_FAILED_LINE_OF_SIGHT)
            {
                PullMovementController movement(botAI);
                movement.MoveToRangedLos(ccTarget);
                continue;
            }

            if (!IsUsableSpellCastResult(castCheck))
            {
                state.ccCastDone = true;
                state.ccReturning = true;
                continue;
            }

            if (bot->isMoving())
            {
                bot->StopMoving();
                continue;
            }

            if (!botAI->CastSpell(state.ccSpellId, ccTarget))
                continue;

            state.ccCastDone = true;
            state.ccReturning = true;
        }
    }

    bool IsSupportCrowdControlReady(BotHoldState& state, Player* owner)
    {
        if (state.isTank || !state.ccSpellId || state.ccTargetGuid.IsEmpty() || state.ccCastDone)
            return true;

        Player* bot = ObjectAccessor::FindPlayer(state.guid);
        if (!IsLiveBot(bot))
            return true;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || botAI->GetMaster() != owner)
            return true;

        Unit* ccTarget = botAI->GetUnit(state.ccTargetGuid);
        if (!IsValidPullTarget(owner, ccTarget))
            return true;

        SetBotTargetContext(bot, ccTarget);

        SpellCastResult const castCheck = CheckBotSpellCast(bot, state.ccSpellId, ccTarget);
        if (castCheck == SPELL_FAILED_OUT_OF_RANGE ||
            castCheck == SPELL_FAILED_LINE_OF_SIGHT ||
            NeedsFacingAdjustment(castCheck) ||
            castCheck == SPELL_FAILED_MOVING)
        {
            return false;
        }

        return true;
    }

    bool AreSupportCrowdControlsReady(PullSession& session, Player* owner)
    {
        for (BotHoldState& state : session.botStates)
        {
            if (!IsSupportCrowdControlReady(state, owner))
                return false;
        }

        return true;
    }

    void StartSession(Player* owner, std::vector<Player*> const& controlledBots)
    {
        Unit* target = ObjectAccessor::GetUnit(*owner, owner->GetTarget());
        if (!IsValidPullTarget(owner, target))
        {
            SendSystemMessage(owner, "pull requires a live hostile target.");
            return;
        }

        Player* tank = FindControlledMainTank(owner, controlledBots);
        if (!tank)
        {
            SendSystemMessage(owner, "pull requires a controlled main tank bot in your current group or raid.");
            return;
        }

        PullOpener const opener = ResolvePullOpener(tank, target);
        if (!opener.IsValid())
        {
            SendSystemMessage(owner, BuildMissingPullOpenerMessage(tank));
            return;
        }

        auto existing = sessions.find(owner->GetGUID());
        if (existing != sessions.end())
            Finalize(existing, "previous pull replaced", false);

        PullSession session;
        session.ownerGuid = owner->GetGUID();
        session.tankGuid = tank->GetGUID();
        session.targetGuid = target->GetGUID();
        session.opener = opener;
        session.stage = PullStage::MoveTankToPullRange;
        session.startedAtMs = getMSTime();

        if (StoredWorldPoint const* tankMarker = GetStoredTankMarker(owner, tank); tankMarker && tankMarker->mapId == owner->GetMapId())
        {
            session.usesTankMarker = true;
            session.tankMarkerPosition = *tankMarker;
        }

        std::string offTankFallbackMessage;
        if (Player* offTank = FindControlledOffTank(owner, controlledBots, tank))
        {
            Unit* offTankTarget = ResolveBotAssignedRtiCcTarget(owner, offTank);
            bool const hasDistinctOffTankTarget =
                IsValidPullTarget(owner, offTankTarget) && offTankTarget->GetGUID() != target->GetGUID();

            if (hasDistinctOffTankTarget)
            {
                PullOpener const offTankOpener = ResolvePullOpener(offTank, offTankTarget);
                StoredWorldPoint const* offTankMarker = GetStoredOffTankMarker(owner, offTank);

                if (!offTankOpener.IsValid())
                {
                    offTankFallbackMessage = "off-tank assignment ignored because the off-tank has no usable pull opener.";
                }
                else
                {
                    session.offTankGuid = offTank->GetGUID();
                    session.offTankTargetGuid = offTankTarget->GetGUID();
                    session.offTankOpener = offTankOpener;

                    if (offTankMarker && offTankMarker->mapId == owner->GetMapId())
                    {
                        session.usesOffTankMarker = true;
                        session.offTankMarkerPosition = *offTankMarker;
                    }
                }
            }
            else
            {
                offTankFallbackMessage = "off-tank assignment ignored because the off-tank has no live rti cc target.";
            }
        }

        session.botStates.reserve(controlledBots.size());
        for (Player* bot : controlledBots)
        {
            BotHoldState state;
            state.guid = bot->GetGUID();
            state.isTank = bot->GetGUID() == tank->GetGUID() || bot->GetGUID() == session.offTankGuid;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                state.hadNonCombatFollow = botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);

            session.botStates.push_back(state);
        }

        Unit* offTankTarget = nullptr;
        if (HasOffTankAssignment(session))
        {
            if (PlayerbotAI* offTankAI = GET_PLAYERBOT_AI(ObjectAccessor::FindPlayer(session.offTankGuid)))
                offTankTarget = offTankAI->GetUnit(session.offTankTargetGuid);
        }

        InitializeSupportCc(session, owner, controlledBots, target, offTankTarget);

        sessions[owner->GetGUID()] = session;
        if (!offTankFallbackMessage.empty())
            SendSystemMessage(owner, offTankFallbackMessage);

        SendSystemMessage(owner, "pull started on " + std::string(target->GetName()) + ".");
    }

    bool ProcessSession(PullSession& session)
    {
        Player* owner = ObjectAccessor::FindConnectedPlayer(session.ownerGuid);
        if (!owner || !owner->IsInWorld() || !owner->GetGroup())
        {
            RestoreBotStates(session);
            return false;
        }

        Player* tank = ObjectAccessor::FindPlayer(session.tankGuid);
        if (!IsLiveBot(tank))
        {
            SendSystemMessage(owner, "pull cancelled because the tank bot is no longer available.");
            RestoreBotStates(session);
            return false;
        }

        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI || tankAI->GetMaster() != owner)
        {
            SendSystemMessage(owner, "pull cancelled because the tank bot is no longer under your control.");
            RestoreBotStates(session);
            return false;
        }

        Player* offTank = nullptr;
        PlayerbotAI* offTankAI = nullptr;
        if (HasOffTankAssignment(session))
        {
            offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
            if (!IsLiveBot(offTank))
            {
                SendSystemMessage(owner, "pull cancelled because the off-tank bot is no longer available.");
                RestoreBotStates(session);
                return false;
            }

            offTankAI = GET_PLAYERBOT_AI(offTank);
            if (!offTankAI || offTankAI->GetMaster() != owner)
            {
                SendSystemMessage(owner, "pull cancelled because the off-tank bot is no longer under your control.");
                RestoreBotStates(session);
                return false;
            }
        }

        Unit* target = tankAI->GetUnit(session.targetGuid);
        if (session.stage != PullStage::WaitForCombatEnd && !IsValidPullTarget(owner, target))
        {
            RestoreBotStates(session);
            return false;
        }

        switch (session.stage)
        {
            case PullStage::MoveTankToPullRange:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, false);
                SetTankTargetContext(tank, target);
                return ProcessMoveStage(session, owner, tank, tankAI, target);
            case PullStage::FirePullShot:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, false);
                SetTankTargetContext(tank, target);
                return ProcessShotStage(session, owner, tank, tankAI, target);
            case PullStage::WaitForPullShotResolve:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, false);
                SetTankTargetContext(tank, target);
                return ProcessShotResolveStage(session, owner, tank, tankAI, target);
            case PullStage::FireFollowupPulls:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, true);
                SetTankTargetContext(tank, target);
                return ProcessFollowupStage(session, owner, tank, tankAI, target);
            case PullStage::WaitForFollowupResolve:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, true);
                SetTankTargetContext(tank, target);
                return ProcessFollowupResolveStage(session, owner, tank, tankAI, target);
            case PullStage::ReturnTankToHoldPoint:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, session.pullShotAtMs != 0);
                return ProcessReturnToTankHoldStage(session, owner, tank, tankAI, target);
            case PullStage::WaitForMobArrival:
                HoldSupportBots(session, owner, tank);
                ManageSupportCrowdControl(session, owner, session.pullShotAtMs != 0);
                SetTankTargetContext(tank, target);
                return ProcessWaitStage(session, owner, tank, tankAI, target);
            case PullStage::WaitForCombatEnd:
                return ProcessPostCombatStage(session, owner);
        }

        RestoreBotStates(session);
        return false;
    }

    bool ProcessMoveStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        if (!session.opener.IsValid())
        {
            SendSystemMessage(owner, "pull cancelled because the main tank does not have a usable ranged pull opener.");
            RestoreBotStates(session);
            return false;
        }

        if (HasPullAlreadyCollapsedIntoMelee(tank, target))
        {
            ReleaseIntoCombat(session, owner, tank, tankAI, target);
            return true;
        }

        if (!session.moveStagePrepared)
        {
            PrepareTankForApproach(session, tank, target);
            session.moveStagePrepared = true;
        }

        if (getMSTimeDiff(session.startedAtMs, getMSTime()) > PULL_ACQUIRE_TIMEOUT_MS)
        {
            if (IsWithinPullWindow(tank, target, session.opener.minRange, session.opener.maxRange) &&
                !AreSupportCrowdControlsReady(session, owner))
            {
                SendSystemMessage(owner, "pull cancelled because support cc bots could not reach cast range.");
            }
            else
            {
                SendSystemMessage(owner, "pull cancelled because the tank could not reach shoot range.");
            }
            RestoreBotStates(session);
            return false;
        }

        bool offTankReady = true;
        if (HasOffTankAssignment(session))
        {
            Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
            PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
            Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
            if (!IsLiveBot(offTank) || !offTankAI || offTankAI->GetMaster() != owner || !IsValidPullTarget(owner, offTankTarget))
            {
                ClearOffTankAssignment(session);
                SendSystemMessage(owner, "off-tank assignment dropped because the off-tank target is no longer valid.");
            }
            else if (HasPullAlreadyCollapsedIntoMelee(offTank, offTankTarget))
            {
                session.offTankCombatReleased = true;
            }
            else
            {
                offTankReady = MoveTankIntoPullPosition(session, offTank, offTankAI, offTankTarget, session.offTankOpener,
                                                        session.offTankMoveStagePrepared);
            }
        }

        bool const mainTankReady = MoveTankIntoPullPosition(session, tank, tankAI, target, session.opener, session.moveStagePrepared);
        if (!mainTankReady)
            return true;

        if (!offTankReady)
            return true;

        if (!AreSupportCrowdControlsReady(session, owner))
        {
            HoldTankBeforeShot(session, tank, target);
            return true;
        }

        session.stage = PullStage::FirePullShot;
        return ProcessShotStage(session, owner, tank, tankAI, target);
    }

    bool ProcessShotStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        if (!session.opener.IsValid() || session.opener.spellIds.empty())
        {
            SendSystemMessage(owner, "pull cancelled because the tank has no firing spell.");
            RestoreBotStates(session);
            return false;
        }

        if (!AreSupportCrowdControlsReady(session, owner))
        {
            session.stage = PullStage::MoveTankToPullRange;
            return true;
        }

        if (HasPullAlreadyCollapsedIntoMelee(tank, target))
        {
            ReleaseIntoCombat(session, owner, tank, tankAI, target);
            return true;
        }

        if (IsDruidFeralPullOpener(session.opener) && !IsDruidInBearPullForm(tank))
        {
            if (TryPrepareDruidTankPullForm(tank, tankAI, session.opener))
                return true;

            SendSystemMessage(owner, "pull cancelled because the druid tank could not enter bear form.");
            RestoreBotStates(session);
            return false;
        }

        SpellCastResult const castCheck = CheckPullOpenerCast(tank, session.opener, target);
        if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
        {
            session.stage = PullStage::MoveTankToPullRange;
            return true;
        }

        if (castCheck == SPELL_FAILED_LINE_OF_SIGHT)
        {
            PullMovementController movement(tankAI);
            movement.MoveToRangedLos(target);
            session.stage = PullStage::MoveTankToPullRange;
            return true;
        }

        if (NeedsFacingAdjustment(castCheck))
        {
            if (tank->isMoving())
                tank->StopMoving();

            FaceBotTowardsTarget(tank, target);
            return true;
        }

        if (ShouldRetrySpellCastLater(castCheck))
        {
            if (castCheck == SPELL_FAILED_MOVING && tank->isMoving())
                tank->StopMoving();

            return true;
        }

        if (IsPullOpenerShapeshiftFailure(castCheck))
        {
            if (TryPrepareDruidTankPullForm(tank, tankAI, session.opener))
                return true;
        }

        if (castCheck != SPELL_CAST_OK)
        {
            SendSystemMessage(owner,
                              "pull cancelled because the tank could not fire " + session.opener.name + " (" +
                                  EnumUtils::ToTitle(castCheck) + ").");
            RestoreBotStates(session);
            return false;
        }

        SetTankTargetContext(tank, target);
        if (!tankAI->CastSpell(session.opener.spellIds.front(), target))
        {
            SendSystemMessage(owner, "pull cancelled because the tank failed to cast " + session.opener.name + ".");
            RestoreBotStates(session);
            return false;
        }

        session.pullShotAtMs = getMSTime();
        session.waitStagePrepared = false;
        session.returnStagePrepared = false;
        session.offTankWaitStagePrepared = false;
        session.offTankReturnStagePrepared = false;
        session.stage = PullStage::WaitForPullShotResolve;
        return true;
    }

    bool ProcessShotResolveStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        (void)tankAI;

        HoldTankBeforeShot(session, tank, target);

        if (HasOffTankAssignment(session))
        {
            Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
            PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
            Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
            if (!IsLiveBot(offTank) || !offTankAI || offTankAI->GetMaster() != owner || !IsValidPullTarget(owner, offTankTarget))
            {
                ClearOffTankAssignment(session);
            }
            else if (HasTankReachedCombatTrigger(offTank, offTankTarget))
            {
                ReleaseTankLaneIntoCombat(session, offTank, offTankAI, offTankTarget);
                session.offTankCombatReleased = true;
            }
            else if (!session.offTankCombatReleased)
            {
                MoveTankIntoPullPosition(session, offTank, offTankAI, offTankTarget, session.offTankOpener,
                                         session.offTankMoveStagePrepared);
            }
        }

        if (HasPullAlreadyCollapsedIntoMelee(tank, target))
        {
            ReleaseIntoCombat(session, owner, tank, GET_PLAYERBOT_AI(tank), target);
            return true;
        }

        uint32 const settleDelay = ResolvePullShotSettleDelay(tank, session.opener);
        uint32 const elapsedMs = getMSTimeDiff(session.pullShotAtMs, getMSTime());

        if (elapsedMs < settleDelay)
            return true;

        if (IsPullShotStillResolving(tank))
            return true;

        if (HasOffTankAssignment(session) || HasPendingSupportCrowdControl(session))
        {
            session.stage = PullStage::FireFollowupPulls;
            return true;
        }

        session.stage = session.usesTankMarker ? PullStage::ReturnTankToHoldPoint : PullStage::WaitForMobArrival;
        return true;
    }

    bool ProcessFollowupStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        (void)tank;
        (void)tankAI;
        (void)target;

        if (!HasOffTankAssignment(session))
        {
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
        PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
        Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
        if (!IsLiveBot(offTank) || !offTankAI || offTankAI->GetMaster() != owner || !IsValidPullTarget(owner, offTankTarget))
        {
            ClearOffTankAssignment(session);
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        if (getMSTimeDiff(session.startedAtMs, getMSTime()) > PULL_ACQUIRE_TIMEOUT_MS)
        {
            SendSystemMessage(owner, "off-tank assignment dropped because the off-tank could not reach pull range.");
            ClearOffTankAssignment(session);
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        if (HasTankReachedCombatTrigger(offTank, offTankTarget))
        {
            ReleaseTankLaneIntoCombat(session, offTank, offTankAI, offTankTarget);
            session.offTankCombatReleased = true;
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        if (IsDruidFeralPullOpener(session.offTankOpener) && !IsDruidInBearPullForm(offTank))
        {
            if (TryPrepareDruidTankPullForm(offTank, offTankAI, session.offTankOpener))
                return true;

            SendSystemMessage(owner, "off-tank assignment dropped because the druid off-tank could not enter bear form.");
            ClearOffTankAssignment(session);
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        SpellCastResult const castCheck = CheckPullOpenerCast(offTank, session.offTankOpener, offTankTarget);
        if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
        {
            MoveTankIntoPullPosition(session, offTank, offTankAI, offTankTarget, session.offTankOpener,
                                     session.offTankMoveStagePrepared);
            return true;
        }

        if (castCheck == SPELL_FAILED_LINE_OF_SIGHT)
        {
            PullMovementController movement(offTankAI);
            movement.MoveToRangedLos(offTankTarget);
            return true;
        }

        if (NeedsFacingAdjustment(castCheck))
        {
            if (offTank->isMoving())
                offTank->StopMoving();

            FaceBotTowardsTarget(offTank, offTankTarget);
            return true;
        }

        if (ShouldRetrySpellCastLater(castCheck))
        {
            if (castCheck == SPELL_FAILED_MOVING && offTank->isMoving())
                offTank->StopMoving();

            return true;
        }

        if (IsPullOpenerShapeshiftFailure(castCheck))
        {
            if (TryPrepareDruidTankPullForm(offTank, offTankAI, session.offTankOpener))
                return true;
        }

        if (castCheck != SPELL_CAST_OK)
        {
            SendSystemMessage(owner,
                              "off-tank assignment dropped because the off-tank could not fire " + session.offTankOpener.name +
                                  " (" + EnumUtils::ToTitle(castCheck) + ").");
            ClearOffTankAssignment(session);
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        SetTankTargetContext(offTank, offTankTarget);
        if (!offTankAI->CastSpell(session.offTankOpener.spellIds.front(), offTankTarget))
            return true;

        session.offTankPullShotAtMs = getMSTime();
        session.stage = PullStage::WaitForFollowupResolve;
        return true;
    }

    bool ProcessFollowupResolveStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        (void)owner;
        (void)tank;
        (void)tankAI;
        (void)target;

        if (!HasOffTankAssignment(session) || !session.offTankPullShotAtMs)
        {
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
        PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
        Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
        if (!IsLiveBot(offTank) || !offTankAI || !offTankTarget)
        {
            ClearOffTankAssignment(session);
            session.stage = PullStage::ReturnTankToHoldPoint;
            return true;
        }

        HoldTankBeforeShot(session, offTank, offTankTarget);

        uint32 const settleDelay = ResolvePullShotSettleDelay(offTank, session.offTankOpener);
        uint32 const elapsedMs = getMSTimeDiff(session.offTankPullShotAtMs, getMSTime());
        if (elapsedMs < settleDelay)
            return true;

        if (IsPullShotStillResolving(offTank))
            return true;

        session.stage = PullStage::ReturnTankToHoldPoint;
        return true;
    }

    bool ProcessReturnToTankHoldStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        bool mainReady = !session.usesTankMarker || !IsStoredPointValidForUnit(session.tankMarkerPosition, tank);
        if (!mainReady)
        {
            if (!session.returnStagePrepared)
            {
                PrepareTankForApproach(session, tank, target);
                session.returnStagePrepared = true;
            }

            if (HasReachedStoredWorldPoint(tank, session.tankMarkerPosition, TANK_MARKER_REACH_DISTANCE))
            {
                if (tank->isMoving())
                    tank->StopMoving();

                mainReady = true;
            }
            else
            {
                HoldTankDuringApproach(session, tank, target, session.tankMarkerPosition.x, session.tankMarkerPosition.y,
                                       session.tankMarkerPosition.z);
                PullMovementController movement(tankAI);
                if (!tank->isMoving())
                {
                    movement.MoveToPoint(session.tankMarkerPosition.mapId, session.tankMarkerPosition.x, session.tankMarkerPosition.y,
                                         session.tankMarkerPosition.z);
                }
            }
        }

        bool offTankReady = true;
        if (HasOffTankAssignment(session))
        {
            Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
            PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
            Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
            if (!IsLiveBot(offTank) || !offTankAI || !IsValidPullTarget(owner, offTankTarget))
            {
                ClearOffTankAssignment(session);
            }
            else if (!session.usesOffTankMarker || !IsStoredPointValidForUnit(session.offTankMarkerPosition, offTank))
            {
                offTankReady = true;
            }
            else
            {
                offTankReady = false;
                if (!session.offTankReturnStagePrepared)
                {
                    PrepareTankForApproach(session, offTank, offTankTarget);
                    session.offTankReturnStagePrepared = true;
                }

                if (HasReachedStoredWorldPoint(offTank, session.offTankMarkerPosition, TANK_MARKER_REACH_DISTANCE))
                {
                    if (offTank->isMoving())
                        offTank->StopMoving();

                    offTankReady = true;
                }
                else
                {
                    HoldTankDuringApproach(session, offTank, offTankTarget, session.offTankMarkerPosition.x,
                                           session.offTankMarkerPosition.y, session.offTankMarkerPosition.z);
                    PullMovementController movement(offTankAI);
                    if (!offTank->isMoving())
                    {
                        movement.MoveToPoint(session.offTankMarkerPosition.mapId, session.offTankMarkerPosition.x,
                                             session.offTankMarkerPosition.y, session.offTankMarkerPosition.z);
                    }
                }
            }
        }

        if (mainReady && offTankReady)
        {
            session.stage = PullStage::WaitForMobArrival;
            return ProcessWaitStage(session, owner, tank, tankAI, target);
        }

        return true;
    }

    bool ProcessWaitStage(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        SetTankTargetContext(tank, target);

        if (!session.mainTankCombatReleased)
        {
            HoldTankDuringWait(session, tank, target);
            session.waitStagePrepared = true;

            if (HasTankReachedCombatTrigger(tank, target))
            {
                if (IsAutoMarkEnabled(owner))
                    MarkTargetWithSkull(tank, target);

                ReleaseTankLaneIntoCombat(session, tank, tankAI, target);
                session.mainTankCombatReleased = true;
            }
        }

        if (HasOffTankAssignment(session) && !session.offTankCombatReleased)
        {
            Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
            PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
            Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
            if (!IsLiveBot(offTank) || !offTankAI || !IsValidPullTarget(owner, offTankTarget))
            {
                ClearOffTankAssignment(session);
            }
            else
            {
                HoldTankDuringWait(session, offTank, offTankTarget);
                session.offTankWaitStagePrepared = true;

                if (HasTankReachedCombatTrigger(offTank, offTankTarget))
                {
                    ReleaseTankLaneIntoCombat(session, offTank, offTankAI, offTankTarget);
                    session.offTankCombatReleased = true;
                }
            }
        }

        if (session.mainTankCombatReleased && (!HasOffTankAssignment(session) || session.offTankCombatReleased))
        {
            ReleaseIntoCombat(session, owner, tank, tankAI, target);
            return true;
        }

        if (getMSTimeDiff(session.startedAtMs, getMSTime()) > PULL_WAIT_TIMEOUT_MS)
        {
            Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
            PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
            Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
            if (tank->IsInCombat() || target->IsInCombat() || (offTank && offTank->IsInCombat()) ||
                (offTankTarget && offTankTarget->IsInCombat()))
            {
                SendSystemMessage(owner, "pull wait timed out; releasing the group back into combat.");
                ReleaseIntoCombat(session, owner, tank, tankAI, target);
                return true;
            }
            else
            {
                SendSystemMessage(owner, "pull cancelled because the tank never started the pull.");
                RestoreBotStates(session);
            }
            return false;
        }

        return true;
    }

    bool ProcessPostCombatStage(PullSession& session, Player* owner)
    {
        for (BotHoldState const& state : session.botStates)
        {
            Player* bot = ObjectAccessor::FindPlayer(state.guid);
            if (!IsLiveBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || botAI->GetMaster() != owner)
                continue;

            if (bot->IsInCombat())
                return true;
        }

        for (BotHoldState const& state : session.botStates)
        {
            Player* bot = ObjectAccessor::FindPlayer(state.guid);
            if (!IsLiveBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || botAI->GetMaster() != owner)
                continue;

            botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
            botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
            bot->SetSelection(ObjectGuid::Empty);
            bot->SetTarget(ObjectGuid::Empty);
            ResetBotMovementState(botAI);
            botAI->ChangeEngine(BOT_STATE_NON_COMBAT);

            if (!state.isTank || state.hadNonCombatFollow)
                RestoreFollowShortcutState(botAI);

            botAI->DoNextAction();
        }

        if (session.usesTankMarker)
            tankMarkers.erase(owner->GetGUID());

        if (session.usesOffTankMarker)
            offTankMarkers.erase(owner->GetGUID());

        return false;
    }

    void HoldSupportBots(PullSession& session, Player* owner, Player* tank)
    {
        for (BotHoldState& state : session.botStates)
        {
            if (state.isTank)
                continue;

            Player* bot = ObjectAccessor::FindPlayer(state.guid);
            if (!IsLiveBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || botAI->GetMaster() != owner)
                continue;

            EnsureStrategy(botAI, state.addedCombatPassive, BOT_STATE_COMBAT, "passive");
            botAI->SetNextCheckDelay(SAP_AI_SUPPRESS_MS);
            StopPetCombat(bot);
        }
    }

    void HoldTankDuringWait(PullSession& session, Player* tank, Unit* target)
    {
        BotHoldState* state = FindBotState(session, tank->GetGUID());
        if (!state)
            return;

        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI)
            return;

        EnsureStrategy(tankAI, state->addedCombatStay, BOT_STATE_COMBAT, "stay");
        SetHoldPosition(*state, tankAI, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
        SetTankTargetContext(tank, target);

        if (tank->isMoving())
            tank->StopMoving();
    }

    void HoldTankBeforeShot(PullSession& session, Player* tank, Unit* target)
    {
        BotHoldState* state = FindBotState(session, tank->GetGUID());
        if (!state)
            return;

        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI)
            return;

        EnsureStrategy(tankAI, state->addedCombatStay, BOT_STATE_COMBAT, "stay");
        SetHoldPosition(*state, tankAI, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
        SetTankTargetContext(tank, target);

        if (tank->isMoving())
            tank->StopMoving();
    }

    void PrepareTankForApproach(PullSession& session, Player* tank, Unit* target)
    {
        BotHoldState* state = FindBotState(session, tank->GetGUID());
        if (!state)
            return;

        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI)
            return;

        SetTankTargetContext(tank, target);
        ClearBotCombatStateForRetreat(tank);
        tankAI->GetAiObjectContext()->GetValue<Unit*>("old target")->Set(nullptr);
        tankAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
        tankAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Reset();
        tankAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
        tank->SetTarget(ObjectGuid::Empty);
        tank->SetSelection(ObjectGuid::Empty);
        ResetBotMovementState(tankAI);
    }

    void HoldTankDuringApproach(PullSession& session, Player* tank, Unit* target, float x, float y, float z)
    {
        BotHoldState* state = FindBotState(session, tank->GetGUID());
        if (!state)
            return;

        PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI)
            return;

        SetHoldPosition(*state, tankAI, x, y, z);
        SetTankTargetContext(tank, target);
    }

    void ReleaseTankLaneIntoCombat(PullSession& session, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        if (!IsLiveBot(tank) || !tankAI || !target)
            return;

        if (BotHoldState* state = FindBotState(session, tank->GetGUID()))
        {
            RemoveStrategy(tankAI, state->addedCombatStay, BOT_STATE_COMBAT, "stay");
            RemoveStrategy(tankAI, state->addedCombatPassive, BOT_STATE_COMBAT, "passive");
            state->addedCombatStay = false;
            state->addedCombatPassive = false;
        }

        if (tank->isMoving())
            tank->StopMoving();

        SetTankTargetContext(tank, target);
        ResetBotMovementState(tankAI);
        tankAI->ChangeEngine(BOT_STATE_COMBAT);
        tankAI->DoSpecificAction("attack", Event(), true);
    }

    void ReleaseIntoCombat(PullSession& session, Player* owner, Player* tank, PlayerbotAI* tankAI, Unit* target)
    {
        session.stage = PullStage::WaitForCombatEnd;
        Player* offTank = ObjectAccessor::FindPlayer(session.offTankGuid);
        PlayerbotAI* offTankAI = offTank ? GET_PLAYERBOT_AI(offTank) : nullptr;
        Unit* offTankTarget = ResolveSessionTarget(offTankAI, session.offTankTargetGuid);
        RestoreBotStates(session);

        if (target && target->IsInWorld() && !target->isDead())
        {
            if (IsAutoMarkEnabled(owner))
                MarkTargetWithSkull(tank, target);

            ReleaseTankLaneIntoCombat(session, tank, tankAI, target);
            session.mainTankCombatReleased = true;

            if (IsValidPullTarget(owner, offTankTarget) && IsLiveBot(offTank) && offTankAI)
            {
                ReleaseTankLaneIntoCombat(session, offTank, offTankAI, offTankTarget);
                session.offTankCombatReleased = true;
            }
            else if (IsLiveBot(offTank) && offTankAI && offTank != tank)
            {
                ReleaseTankLaneIntoCombat(session, offTank, offTankAI, target);
                session.offTankCombatReleased = true;
            }

            for (BotHoldState const& state : session.botStates)
            {
                if (state.isTank)
                    continue;

                Player* bot = ObjectAccessor::FindPlayer(state.guid);
                if (!IsLiveBot(bot))
                    continue;

                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                if (!botAI || botAI->GetMaster() != owner)
                    continue;

                botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({target->GetGUID()});
                botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(target->GetGUID());
                botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
                bot->SetTarget(target->GetGUID());
                bot->SetSelection(target->GetGUID());
                botAI->ChangeEngine(BOT_STATE_COMBAT);

                if (!botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
                    botAI->DoSpecificAction("attack", Event(), true);
                else
                    botAI->DoNextAction();
            }
        }

        SendSystemMessage(owner, "pull released.");
    }

    void RestoreBotStates(PullSession const& session)
    {
        for (BotHoldState const& state : session.botStates)
        {
            Player* bot = ObjectAccessor::FindPlayer(state.guid);
            if (!IsLiveBot(bot))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                continue;

            RemoveStrategy(botAI, state.addedCombatPassive, BOT_STATE_COMBAT, "passive");
            RemoveStrategy(botAI, state.addedCombatStay, BOT_STATE_COMBAT, "stay");
            RemoveStrategy(botAI, state.addedNonCombatPassive, BOT_STATE_NON_COMBAT, "passive");
            RemoveStrategy(botAI, state.addedNonCombatStay, BOT_STATE_NON_COMBAT, "stay");
            RestorePositions(state, botAI);

            if (state.hadNonCombatFollow && !botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);

            if (state.guid == session.tankGuid && session.addedTankPullStrategy)
                botAI->ChangeStrategy("-pull", BOT_STATE_COMBAT);

            if (bot->isMoving())
                bot->StopMoving();

            ResetBotMovementState(botAI);
        }
    }

    std::map<ObjectGuid, PullSession>::iterator Finalize(std::map<ObjectGuid, PullSession>::iterator it, std::string const& message, bool releaseCombat)
    {
        PullSession& session = it->second;
        Player* owner = ObjectAccessor::FindConnectedPlayer(session.ownerGuid);
        Player* tank = ObjectAccessor::FindPlayer(session.tankGuid);
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        Unit* target = (tankAI && session.targetGuid) ? tankAI->GetUnit(session.targetGuid) : nullptr;

        if (releaseCombat && owner && IsLiveBot(tank) && tankAI && target && target->IsInWorld() && !target->isDead())
            ReleaseIntoCombat(session, owner, tank, tankAI, target);
        else
            RestoreBotStates(session);

        if (owner && !message.empty() && !releaseCombat)
            SendSystemMessage(owner, message);

        return sessions.erase(it);
    }

    std::map<ObjectGuid, PullSession> sessions;
    std::map<ObjectGuid, TankMarkerState> tankMarkers;
    std::map<ObjectGuid, TankMarkerState> offTankMarkers;
    std::set<ObjectGuid> disabledAutoMarkOwners;
    uint32 updateAccumulator = 0;
};

enum class SapStage
{
    Stealth,
    MoveToDistractRange,
    CastDistract,
    MoveToSapRange,
    CastSap,
    VerifySap,
    ReturnToMaster
};

struct SapSession
{
    ObjectGuid ownerGuid = ObjectGuid::Empty;
    ObjectGuid rogueGuid = ObjectGuid::Empty;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    std::string rtiName;
    SapStage stage = SapStage::Stealth;
    uint32 startedAtMs = 0;
    uint32 stealthSpellId = 0;
    uint32 distractSpellId = 0;
    uint32 sapSpellId = 0;
    uint32 stealthRequestedAtMs = 0;
    uint32 lastDebugAtMs = 0;
    bool stealthRequested = false;
    bool usedDistract = false;
};

class SapManager
{
public:
    static SapManager& Instance()
    {
        static SapManager instance;
        return instance;
    }

    bool TryHandleCommand(Player* owner, std::string const& msg)
    {
        if (!IsModuleEnabled() || !owner || NormalizeCommand(msg) != "sap")
            return false;

        std::vector<Player*> const controlledBots = CollectControlledBots(owner);
        if (controlledBots.empty())
        {
            SendPlainSystemMessage(owner, "No controlled bots are available for sap.");
            return true;
        }

        uint32 startedCount = 0;
        std::string failureMessage;
        for (Player* bot : controlledBots)
        {
            if (!IsLiveBot(bot) || bot->getClass() != CLASS_ROGUE || bot->IsInCombat())
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || botAI->GetMaster() != owner)
                continue;

            std::string const rtiName = botAI->GetAiObjectContext()->GetValue<std::string>("rti cc")->Get();
            Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("rti cc target")->Get();
            if (!IsValidSapTarget(owner, target))
                continue;

            uint32 const stealthSpellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "stealth")->Get();
            uint32 const distractSpellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "distract")->Get();
            uint32 const sapSpellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", "sap")->Get();
            if (!stealthSpellId || !sapSpellId || !bot->HasSpell(stealthSpellId) || !bot->HasSpell(sapSpellId))
                continue;

            if (botAI->HasAura("sap", target))
            {
                failureMessage = "sap cancelled because the marked cc target is already sapped.";
                continue;
            }

            SpellCastResult const sapCheck = CheckBotSpellCast(bot, sapSpellId, target);
            if (!IsPotentialSapCastResult(sapCheck))
            {
                failureMessage = "sap cancelled because the rogue cannot sap that target (" +
                    std::string(EnumUtils::ToTitle(sapCheck)) + ").";
                continue;
            }

            SapSession session;
            StopBotCombat(bot, true);
            session.ownerGuid = owner->GetGUID();
            session.rogueGuid = bot->GetGUID();
            session.targetGuid = target->GetGUID();
            session.rtiName = rtiName;
            session.stage = SapStage::Stealth;
            session.startedAtMs = getMSTime();
            session.stealthSpellId = stealthSpellId;
            session.distractSpellId = distractSpellId;
            session.sapSpellId = sapSpellId;
            session.stealthRequestedAtMs = 0;
            session.lastDebugAtMs = 0;
            session.stealthRequested = false;
            session.usedDistract = distractSpellId && bot->HasSpell(distractSpellId);

            sessions[bot->GetGUID()] = session;
            ++startedCount;
        }

        if (!startedCount)
            SendSystemMessage(owner, failureMessage.empty() ? "sap requires a controlled rogue with a live rti cc target."
                                                            : failureMessage);
        else if (startedCount == 1)
            SendSystemMessage(owner, "sap started.");
        else
            SendSystemMessage(owner, "sap started for " + std::to_string(startedCount) + " rogues.");

        return true;
    }

    void Update(uint32 diff)
    {
        if (!IsModuleEnabled())
            return;

        if (sessions.empty())
            return;

        updateAccumulator += diff;
        if (updateAccumulator < SAP_UPDATE_INTERVAL_MS)
            return;

        updateAccumulator = 0;

        for (auto it = sessions.begin(); it != sessions.end();)
        {
            if (!ProcessSession(it->second))
                it = sessions.erase(it);
            else
                ++it;
        }
    }

private:
    SapManager() = default;

    bool IsValidSapTarget(Player* owner, Unit* target) const
    {
        if (!IsValidPullTarget(owner, target))
            return false;

        return !target->IsInCombat();
    }

    void BuildDistractDestination(Player* rogue, Unit* target, float& x, float& y, float& z) const
    {
        x = target->GetPositionX();
        y = target->GetPositionY();
        z = target->GetPositionZ();

        if (!rogue || !target)
            return;

        float const dx = target->GetPositionX() - rogue->GetPositionX();
        float const dy = target->GetPositionY() - rogue->GetPositionY();
        float const distance2d = std::sqrt(dx * dx + dy * dy);
        if (distance2d <= 0.01f)
            return;

        float const nx = dx / distance2d;
        float const ny = dy / distance2d;

        x = target->GetPositionX() + nx * SAP_DISTRACT_BEHIND_DISTANCE;
        y = target->GetPositionY() + ny * SAP_DISTRACT_BEHIND_DISTANCE;
        z = target->GetPositionZ();
    }

    void RestoreRogueState(Player* rogue, Player* owner) const
    {
        if (!IsLiveBot(rogue))
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(rogue);
        if (!botAI || (owner && botAI->GetMaster() != owner))
            return;

        botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
        rogue->SetSelection(ObjectGuid::Empty);
        rogue->SetTarget(ObjectGuid::Empty);
        ResetBotMovementState(botAI);
        botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
        botAI->DoNextAction();
    }

    void SetRogueTargetContext(Player* rogue, Unit* target) const
    {
        if (!IsLiveBot(rogue) || !target)
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(rogue);
        if (!botAI)
            return;

        botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
        rogue->SetSelection(target->GetGUID());
        rogue->SetTarget(target->GetGUID());
    }

    void HoldRogueAi(Player* rogue) const
    {
        if (!IsLiveBot(rogue))
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(rogue);
        if (!botAI)
            return;

        botAI->SetNextCheckDelay(SAP_AI_SUPPRESS_MS);
    }

    void SendSapDebug(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target,
                      std::string const& phase, SpellCastResult result = SPELL_CAST_OK, bool includeResult = false) const
    {
        if (!owner || !rogue || !rogueAI)
            return;

        uint32 const now = getMSTime();
        if (session.lastDebugAtMs && getMSTimeDiff(session.lastDebugAtMs, now) < 1000)
            return;

        session.lastDebugAtMs = now;

        Unit* const currentTarget = rogueAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        Unit* const selectedTarget = rogue->GetSelectedUnit();

        std::ostringstream out;
        out << "sap debug: " << phase;
        out << " target=" << (target ? target->GetName() : "<none>");
        out << " current=" << (currentTarget ? currentTarget->GetName() : "<none>");
        out << " selected=" << (selectedTarget ? selectedTarget->GetName() : "<none>");
        out << " stealthed=" << (rogueAI->HasAura("stealth", rogue) ? "yes" : "no");
        out << " moving=" << (rogue->isMoving() ? "yes" : "no");
        out << std::fixed << std::setprecision(1);
        out << " dist=" << (target ? rogue->GetDistance(target) : 0.0f);

        if (includeResult)
            out << " result=" << EnumUtils::ToTitle(result) << " (" << uint32(result) << ")";

        SendPlainSystemMessage(owner, out.str());
    }

    bool Finalize(std::map<ObjectGuid, SapSession>::iterator const& it, std::string const& message, bool success)
    {
        SapSession const& session = it->second;
        Player* owner = ObjectAccessor::FindConnectedPlayer(session.ownerGuid);
        Player* rogue = ObjectAccessor::FindPlayer(session.rogueGuid);

        RestoreRogueState(rogue, owner);

        if (owner && !message.empty())
        {
            if (success)
                SendSystemMessage(owner, message);
            else
                SendSystemMessage(owner, "sap cancelled: " + message);
        }

        return false;
    }

    bool ProcessSession(SapSession& session)
    {
        Player* owner = ObjectAccessor::FindConnectedPlayer(session.ownerGuid);
        if (!owner || !owner->IsInWorld() || !owner->GetGroup())
            return false;

        Player* rogue = ObjectAccessor::FindPlayer(session.rogueGuid);
        if (!IsLiveBot(rogue))
            return false;

        PlayerbotAI* rogueAI = GET_PLAYERBOT_AI(rogue);
        if (!rogueAI || rogueAI->GetMaster() != owner)
            return false;

        HoldRogueAi(rogue);

        if (rogue->IsInCombat())
        {
            SendSystemMessage(owner, "sap cancelled because " + std::string(rogue->GetName()) + " entered combat.");
            RestoreRogueState(rogue, owner);
            return false;
        }

        Unit* target = rogueAI->GetUnit(session.targetGuid);
        if (!IsValidSapTarget(owner, target))
        {
            SendSystemMessage(owner, "sap cancelled because the marked cc target is no longer valid.");
            RestoreRogueState(rogue, owner);
            return false;
        }

        SetRogueTargetContext(rogue, target);

        if (getMSTimeDiff(session.startedAtMs, getMSTime()) > SAP_TIMEOUT_MS)
        {
            SendSystemMessage(owner, "sap cancelled because the rogue took too long.");
            RestoreRogueState(rogue, owner);
            return false;
        }

        switch (session.stage)
        {
            case SapStage::Stealth:
                return ProcessStealthStage(session, owner, rogue, rogueAI, target);
            case SapStage::MoveToDistractRange:
                return ProcessMoveToDistractStage(session, owner, rogue, rogueAI, target);
            case SapStage::CastDistract:
                return ProcessCastDistractStage(session, owner, rogue, rogueAI, target);
            case SapStage::MoveToSapRange:
                return ProcessMoveToSapStage(session, owner, rogue, rogueAI, target);
            case SapStage::CastSap:
                return ProcessCastSapStage(session, owner, rogue, rogueAI, target);
            case SapStage::VerifySap:
                return ProcessVerifySapStage(session, owner, rogue, rogueAI, target);
            case SapStage::ReturnToMaster:
                return ProcessReturnStage(session, owner, rogue, rogueAI);
        }

        RestoreRogueState(rogue, owner);
        return false;
    }

    bool ProcessStealthStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target)
    {
        if (!rogueAI->HasAura("stealth", rogue))
        {
            if (session.stealthRequested)
            {
                if (getMSTimeDiff(session.stealthRequestedAtMs, getMSTime()) < SAP_STEALTH_SETTLE_MS)
                    return true;

                SendSystemMessage(owner, "sap cancelled because the rogue did not stay stealthed.");
                RestoreRogueState(rogue, owner);
                return false;
            }

            if (rogue->isMoving())
            {
                rogue->StopMoving();
                return true;
            }

            SpellCastResult const castCheck = CheckBotSpellCast(rogue, session.stealthSpellId, rogue);
            if (ShouldRetrySpellCastLater(castCheck))
                return true;

            if (!IsUsableSpellCastResult(castCheck))
            {
                SendSystemMessage(owner, "sap cancelled because the rogue could not enter stealth.");
                RestoreRogueState(rogue, owner);
                return false;
            }

            if (!rogueAI->CastSpell(session.stealthSpellId, rogue))
            {
                SendSystemMessage(owner, "sap cancelled because the rogue failed to cast stealth.");
                RestoreRogueState(rogue, owner);
                return false;
            }

            session.stealthRequested = true;
            session.stealthRequestedAtMs = getMSTime();
            return true;
        }

        session.stage = session.usedDistract ? SapStage::MoveToDistractRange : SapStage::MoveToSapRange;
        return ProcessSession(session);
    }

    bool ProcessMoveToDistractStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target)
    {
        if (!session.usedDistract || !session.distractSpellId || rogue->HasSpellCooldown(session.distractSpellId))
        {
            session.stage = SapStage::MoveToSapRange;
            return true;
        }

        if (!rogueAI->HasAura("stealth", rogue))
        {
            if (rogue->isMoving())
                rogue->StopMoving();

            SendSystemMessage(owner, "sap cancelled because the rogue lost stealth before distract.");
            RestoreRogueState(rogue, owner);
            return false;
        }

        float const distance = rogue->GetDistance(target);
        if (distance <= SAP_DISTRACT_STANDOFF + 0.5f && rogue->IsWithinLOSInMap(target))
        {
            if (rogue->isMoving())
                rogue->StopMoving();

            session.stage = SapStage::CastDistract;
            return true;
        }

        PullMovementController movement(rogueAI);
        if (!rogue->IsWithinLOSInMap(target))
            movement.MoveToRangedLos(target);
        else
            movement.MoveIntoRange(target, SAP_DISTRACT_STANDOFF);

        return true;
    }

    bool ProcessCastDistractStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target)
    {
        if (!session.usedDistract || !session.distractSpellId || rogue->HasSpellCooldown(session.distractSpellId))
        {
            session.stage = SapStage::MoveToSapRange;
            return true;
        }

        if (rogue->isMoving())
        {
            rogue->StopMoving();
            return true;
        }

        float distractX = 0.0f;
        float distractY = 0.0f;
        float distractZ = 0.0f;
        BuildDistractDestination(rogue, target, distractX, distractY, distractZ);

        SpellCastResult const castCheck = CheckBotSpellCast(rogue, session.distractSpellId, distractX, distractY, distractZ);
        if (ShouldRetrySpellCastLater(castCheck))
        {
            if (rogue->HasSpellCooldown(session.distractSpellId))
            {
                session.stage = SapStage::MoveToSapRange;
                return true;
            }

            return true;
        }

        if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
        {
            session.stage = SapStage::MoveToDistractRange;
            return true;
        }

        if (!IsUsableSpellCastResult(castCheck))
        {
            session.stage = SapStage::MoveToSapRange;
            return true;
        }

        if (!rogueAI->CastSpell(session.distractSpellId, distractX, distractY, distractZ))
        {
            session.stage = SapStage::MoveToSapRange;
            return true;
        }

        session.stage = SapStage::MoveToSapRange;
        return true;
    }

    bool ProcessMoveToSapStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target)
    {
        if (!rogueAI->HasAura("stealth", rogue))
        {
            if (rogue->isMoving())
                rogue->StopMoving();

            SendSystemMessage(owner, "sap cancelled because the rogue lost stealth before sap.");
            RestoreRogueState(rogue, owner);
            return false;
        }

        SpellCastResult const castCheck = CheckBotSpellCast(rogue, session.sapSpellId, target);
        if (ShouldRetrySpellCastLater(castCheck))
            return true;

        if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
        {
            PullMovementController movement(rogueAI);
            if (!rogue->IsWithinLOSInMap(target))
                movement.MoveToRangedLos(target);
            else
                movement.MoveIntoMelee(target, SAP_MELEE_STANDOFF);

            return true;
        }

        if (!IsUsableSpellCastResult(castCheck))
        {
            SendSystemMessage(owner, "sap cancelled because the rogue cannot sap that target (" +
                                         std::string(EnumUtils::ToTitle(castCheck)) + ").");
            RestoreRogueState(rogue, owner);
            return false;
        }

        if (rogue->isMoving())
            rogue->StopMoving();

        session.stage = SapStage::CastSap;
        return true;
    }

    bool ProcessCastSapStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target)
    {
        SetRogueTargetContext(rogue, target);

        if (rogue->isMoving())
        {
            SendSapDebug(session, owner, rogue, rogueAI, target, "cast blocked by movement");
            rogue->StopMoving();
            return true;
        }

        SpellCastResult const castCheck = CheckBotSpellCast(rogue, session.sapSpellId, target);
        SendSapDebug(session, owner, rogue, rogueAI, target, "cast check", castCheck, true);
        if (ShouldRetrySpellCastLater(castCheck))
            return true;

        if (castCheck == SPELL_FAILED_OUT_OF_RANGE)
        {
            session.stage = SapStage::MoveToSapRange;
            return true;
        }

        if (!IsUsableSpellCastResult(castCheck))
        {
            SendSystemMessage(owner, "sap cancelled because the rogue could not cast sap (" +
                                         std::string(EnumUtils::ToTitle(castCheck)) + ").");
            RestoreRogueState(rogue, owner);
            return false;
        }

        if (!rogueAI->CastSpell(session.sapSpellId, target))
        {
            SendSapDebug(session, owner, rogue, rogueAI, target, "CastSpell returned false");
            SendSystemMessage(owner, "sap cancelled because the rogue failed to cast sap.");
            RestoreRogueState(rogue, owner);
            return false;
        }

        session.stage = SapStage::VerifySap;
        return true;
    }

    bool ProcessVerifySapStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI, Unit* target)
    {
        if (rogueAI->HasAura("sap", target))
        {
            session.stage = SapStage::ReturnToMaster;
            return true;
        }

        if (rogue->HasSpellCooldown(session.sapSpellId))
            return true;

        session.stage = SapStage::MoveToSapRange;
        return true;
    }

    bool ProcessReturnStage(SapSession& session, Player* owner, Player* rogue, PlayerbotAI* rogueAI)
    {
        Player* master = rogueAI->GetMaster();
        if (!master || !master->IsInWorld())
        {
            RestoreRogueState(rogue, owner);
            return false;
        }

        float const followDistance = sPlayerbotAIConfig.followDistance + SAP_RETURN_EXTRA_DISTANCE;
        if (rogue->GetDistance(master) <= followDistance)
        {
            RestoreRogueState(rogue, owner);
            SendSystemMessage(owner, std::string(rogue->GetName()) + " sapped the " +
                                         (session.rtiName.empty() ? std::string("marked") : session.rtiName) +
                                         " target.");
            return false;
        }

        PullMovementController movement(rogueAI);
        movement.FollowUnit(master, sPlayerbotAIConfig.followDistance);
        return true;
    }

    std::map<ObjectGuid, SapSession> sessions;
    uint32 updateAccumulator = 0;
};

class PlayerbotBetterCombatPlayerScript final : public PlayerScript
{
public:
    PlayerbotBetterCombatPlayerScript()
        : PlayerScript("PlayerbotBetterCombatPlayerScript",
                       { PLAYERHOOK_CAN_PLAYER_USE_CHAT,
                         PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
                         PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
                         PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
                         PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
                         PLAYERHOOK_ON_SPELL_CAST })
    {
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg) override
    {
        return !TryHandleCombatCommand(player, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Player* /*receiver*/) override
    {
        return !TryHandleCombatCommand(player, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Group* /*group*/) override
    {
        return !TryHandleCombatCommand(player, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Guild* /*guild*/) override
    {
        return !TryHandleCombatCommand(player, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Channel* /*channel*/) override
    {
        return !TryHandleCombatCommand(player, msg);
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        PullManager::Instance().HandlePlayerSpellCast(player, spell);
    }

private:
    bool TryHandleCombatCommand(Player* player, std::string const& msg)
    {
        return PullManager::Instance().TryHandleAutoMarkCommand(player, msg) ||
               PullManager::Instance().TryHandleTankCommand(player, msg) ||
               PullManager::Instance().TryHandleOffTankCommand(player, msg) ||
               PullManager::Instance().TryHandleCommand(player, msg) ||
               SapManager::Instance().TryHandleCommand(player, msg);
    }
};

class PlayerbotBetterCombatWorldScript final : public WorldScript
{
public:
    PlayerbotBetterCombatWorldScript()
        : WorldScript("PlayerbotBetterCombatWorldScript", { WORLDHOOK_ON_UPDATE })
    {
    }

    void OnUpdate(uint32 diff) override
    {
        PullManager::Instance().Update(diff);
        SapManager::Instance().Update(diff);
    }
};

} // namespace

void AddPlayerbotBetterCombatScripts()
{
    new PlayerbotBetterCombatPlayerScript();
    new PlayerbotBetterCombatWorldScript();
}
