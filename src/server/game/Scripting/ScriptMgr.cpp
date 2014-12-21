/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "ScriptLoader.h"
#include "ScriptSystem.h"
#include "Transport.h"
#include "Vehicle.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "GossipDef.h"
#include "CreatureAIImpl.h"
#include "SpellAuraEffects.h"

namespace MS { namespace Game { namespace Scripting 
{
    namespace
    {
        typedef std::set<Interfaces::ScriptObject*> ExampleScriptContainer;
        ExampleScriptContainer ExampleScripts;
    }

    // This is the global static registry of scripts.
    template<class TScript> class ScriptRegistry
    {
        public:

            typedef std::map<uint32, TScript*> ScriptMap;
            typedef typename ScriptMap::iterator ScriptMapIterator;

            // The actual list of scripts. This will be accessed concurrently, so it must not be modified
            // after server startup.
            static ScriptMap ScriptPointerList;

            static void AddScript(TScript* const script)
            {
                ASSERT(script);

                // See if the script is using the same memory as another script. If this happens, it means that
                // someone forgot to allocate new memory for a script.
                for (ScriptMapIterator it = ScriptPointerList.begin(); it != ScriptPointerList.end(); ++it)
                {
                    if (it->second == script)
                    {
                        sLog->outError(LOG_FILTER_TSCR, "Script '%s' has same memory pointer as '%s'.",
                            script->GetName().c_str(), it->second->GetName().c_str());

                        return;
                    }
                }

                if (script->IsDatabaseBound())
                {
                    // Get an ID for the script. An ID only exists if it's a script that is assigned in the database
                    // through a script name (or similar).
                    uint32 id = sObjectMgr->GetScriptId(script->GetName().c_str());
                    if (id)
                    {
                        // Try to find an existing script.
                        bool existing = false;
                        for (ScriptMapIterator it = ScriptPointerList.begin(); it != ScriptPointerList.end(); ++it)
                        {
                            // If the script names match...
                            if (it->second->GetName() == script->GetName())
                            {
                                // ... It exists.
                                existing = true;
                                break;
                            }
                        }

                        // If the script isn't assigned -> assign it!
                        if (!existing)
                        {
                            ScriptPointerList[id] = script;
                            sScriptMgr->IncrementScriptCount();
                        }
                    }
                    else
                    {
                        // The script uses a script name from database, but isn't assigned to anything.
                        if (script->GetName().find("example") == std::string::npos && script->GetName().find("Smart") == std::string::npos)
                            sLog->outError(LOG_FILTER_SQL, "Script named '%s' does not have a script name assigned in database.",
                                script->GetName().c_str());

                         // These scripts don't get stored anywhere so throw them into this to avoid leaking memory
                        ExampleScripts.insert(script);
                    }
                }
                else
                {
                    // We're dealing with a code-only script; just add it.
                    ScriptPointerList[_scriptIdCounter++] = script;
                    sScriptMgr->IncrementScriptCount();
                }
            }

            // Gets a script by its ID (assigned by ObjectMgr).
            static TScript* GetScriptById(uint32 id)
            {
                ScriptMapIterator it = ScriptPointerList.find(id);
                if (it != ScriptPointerList.end())
                    return it->second;

                return nullptr;
            }

        private:

            // Counter used for code-only scripts.
            static uint32 _scriptIdCounter;
    };

    // Utility macros to refer to the script registry.
    #define SCR_REG_MAP(T) ScriptRegistry<T>::ScriptMap
    #define SCR_REG_ITR(T) ScriptRegistry<T>::ScriptMapIterator
    #define SCR_REG_LST(T) ScriptRegistry<T>::ScriptPointerList

    // Utility macros for looping over scripts.
    #define FOR_SCRIPTS(T, C, E) \
        if (SCR_REG_LST(T).empty()) \
            return; \
        for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
            C != SCR_REG_LST(T).end(); ++C)
    #define FOR_SCRIPTS_RET(T, C, E, R) \
        if (SCR_REG_LST(T).empty()) \
            return R; \
        for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
            C != SCR_REG_LST(T).end(); ++C)
    #define FOREACH_SCRIPT(T) \
        FOR_SCRIPTS(T, itr, end) \
        itr->second

    // Utility macros for finding specific scripts.
    #define GET_SCRIPT_NO_RET(T, I, V) \
        T* V = ScriptRegistry<T>::GetScriptById(I);

    #define GET_SCRIPT(T, I, V) \
        T* V = ScriptRegistry<T>::GetScriptById(I); \
        if (!V) \
            return;

    #define GET_SCRIPT_RET(T, I, V, R) \
        T* V = ScriptRegistry<T>::GetScriptById(I); \
        if (!V) \
            return R;

    void DoScriptText(int32 iTextEntry, WorldObject* pSource, Unit* target)
    {
        if (!pSource)
        {
            sLog->outError(LOG_FILTER_TSCR, "DoScriptText entry %i, invalid Source pointer.", iTextEntry);
            return;
        }

        if (iTextEntry >= 0)
        {
            sLog->outError(LOG_FILTER_TSCR, "DoScriptText with source entry %u (TypeId=%u, guid=%u) attempts to process text entry %i, but text entry must be negative.", pSource->GetEntry(), pSource->GetTypeId(), pSource->GetGUIDLow(), iTextEntry);
            return;
        }

        const StringTextData* pData = sScriptSystemMgr->GetTextData(iTextEntry);

        if (!pData)
        {
            sLog->outError(LOG_FILTER_TSCR, "DoScriptText with source entry %u (TypeId=%u, guid=%u) could not find text entry %i.", pSource->GetEntry(), pSource->GetTypeId(), pSource->GetGUIDLow(), iTextEntry);
            return;
        }

        sLog->outDebug(LOG_FILTER_TSCR, "DoScriptText: text entry=%i, Sound=%u, Type=%u, Language=%u, Emote=%u", iTextEntry, pData->uiSoundId, pData->uiType, pData->uiLanguage, pData->uiEmote);

        if (pData->uiSoundId)
        {
            if (sSoundEntriesStore.LookupEntry(pData->uiSoundId))
                pSource->SendPlaySound(pData->uiSoundId, false);
            else
                sLog->outError(LOG_FILTER_TSCR, "DoScriptText entry %i tried to process invalid sound id %u.", iTextEntry, pData->uiSoundId);
        }

        if (pData->uiEmote)
        {
            if (pSource->GetTypeId() == TYPEID_UNIT || pSource->GetTypeId() == TYPEID_PLAYER)
                ((Unit*)pSource)->HandleEmoteCommand(pData->uiEmote);
            else
                sLog->outError(LOG_FILTER_TSCR, "DoScriptText entry %i tried to process emote for invalid TypeId (%u).", iTextEntry, pSource->GetTypeId());
        }

        switch (pData->uiType)
        {
            case CHAT_TYPE_SAY:
                pSource->MonsterYell(iTextEntry, pData->uiLanguage, target ? target->GetGUID() : 0);
                break;
            case CHAT_TYPE_YELL:
                pSource->MonsterYell(iTextEntry, pData->uiLanguage, target ? target->GetGUID() : 0);
                break;
            case CHAT_TYPE_TEXT_EMOTE:
                pSource->MonsterTextEmote(iTextEntry, target ? target->GetGUID() : 0);
                break;
            case CHAT_TYPE_BOSS_EMOTE:
                pSource->MonsterTextEmote(iTextEntry, target ? target->GetGUID() : 0, true);
                break;
            case CHAT_TYPE_WHISPER:
            {
                if (target && target->GetTypeId() == TYPEID_PLAYER)
                    pSource->MonsterWhisper(iTextEntry, target->GetGUID());
                else
                    sLog->outError(LOG_FILTER_TSCR, "DoScriptText entry %i cannot whisper without target unit (TYPEID_PLAYER).", iTextEntry);

                break;
            }
            case CHAT_TYPE_BOSS_WHISPER:
            {
                if (target && target->GetTypeId() == TYPEID_PLAYER)
                    pSource->MonsterWhisper(iTextEntry, target->GetGUID(), true);
                else
                    sLog->outError(LOG_FILTER_TSCR, "DoScriptText entry %i cannot whisper without target unit (TYPEID_PLAYER).", iTextEntry);

                break;
            }
            case CHAT_TYPE_ZONE_YELL:
                pSource->MonsterYellToZone(iTextEntry, pData->uiLanguage, target ? target->GetGUID() : 0);
                break;
        }
    }


    struct TSpellSummary
    {
        uint8 Targets;                                          // set of enum SelectTarget
        uint8 Effects;                                          // set of enum SelectEffect
    } *SpellSummary;

    ScriptMgr::ScriptMgr()
        : _scriptCount(0), _scheduledScripts(0)
    {
    }

    ScriptMgr::~ScriptMgr()
    {
    }

    void ScriptMgr::Initialize()
    {
        uint32 oldMSTime = getMSTime();

        LoadDatabase();

        sLog->outInfo(LOG_FILTER_SERVER_LOADING, "Loading C++ scripts");

        FillSpellSummary();
        AddScripts();

        sLog->outInfo(LOG_FILTER_SERVER_LOADING, ">> Loaded %u C++ scripts in %u ms", GetScriptCount(), GetMSTimeDiffToNow(oldMSTime));
    }

    void ScriptMgr::Unload()
    {
        #define SCR_CLEAR(T) \
            for (SCR_REG_ITR(T) itr = SCR_REG_LST(T).begin(); itr != SCR_REG_LST(T).end(); ++itr) \
                delete itr->second; \
            SCR_REG_LST(T).clear();

        // Clear scripts for every script type.
        SCR_CLEAR(Interfaces::SpellScriptLoader);
        SCR_CLEAR(Interfaces::ServerScript);
        SCR_CLEAR(Interfaces::WorldScript);
        SCR_CLEAR(Interfaces::FormulaScript);
        SCR_CLEAR(Interfaces::WorldMapScript);
        SCR_CLEAR(Interfaces::InstanceMapScript);
        SCR_CLEAR(Interfaces::BattlegroundMapScript);
        SCR_CLEAR(Interfaces::ItemScript);
        SCR_CLEAR(Interfaces::CreatureScript);
        SCR_CLEAR(Interfaces::GameObjectScript);
        SCR_CLEAR(Interfaces::AreaTriggerScript);
        SCR_CLEAR(Interfaces::BattlegroundScript);
        SCR_CLEAR(Interfaces::OutdoorPvPScript);
        SCR_CLEAR(Interfaces::CommandScript);
        SCR_CLEAR(Interfaces::WeatherScript);
        SCR_CLEAR(Interfaces::AuctionHouseScript);
        SCR_CLEAR(Interfaces::ConditionScript);
        SCR_CLEAR(Interfaces::VehicleScript);
        SCR_CLEAR(Interfaces::DynamicObjectScript);
        SCR_CLEAR(Interfaces::TransportScript);
        SCR_CLEAR(Interfaces::AchievementCriteriaScript);
        SCR_CLEAR(Interfaces::PlayerScript);
        SCR_CLEAR(Interfaces::GuildScript);
        SCR_CLEAR(Interfaces::GroupScript);
        SCR_CLEAR(Interfaces::AreaTriggerEntityScript);

        #undef SCR_CLEAR

        for (ExampleScriptContainer::iterator itr = ExampleScripts.begin(); itr != ExampleScripts.end(); ++itr)
            delete *itr;
        ExampleScripts.clear();

        delete[] SpellSummary;
        delete[] UnitAI::AISpellInfo;
    }

    void ScriptMgr::LoadDatabase()
    {
        sScriptSystemMgr->LoadScriptTexts();
        sScriptSystemMgr->LoadScriptTextsCustom();
        sScriptSystemMgr->LoadScriptWaypoints();
    }

    void ScriptMgr::FillSpellSummary()
    {
        UnitAI::FillAISpellInfo();

        SpellSummary = new TSpellSummary[sSpellMgr->GetSpellInfoStoreSize()];

        SpellInfo const* pTempSpell;

        for (uint32 i = 0; i < sSpellMgr->GetSpellInfoStoreSize(); ++i)
        {
            SpellSummary[i].Effects = 0;
            SpellSummary[i].Targets = 0;

            pTempSpell = sSpellMgr->GetSpellInfo(i);
            // This spell doesn't exist.
            if (!pTempSpell)
                continue;

            for (uint32 j = 0; j < MAX_SPELL_EFFECTS; ++j)
            {
                // Spell targets self.
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SELF-1);

                // Spell targets a single enemy.
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_ENEMY-1);

                // Spell targets AoE at enemy.
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_ENEMY-1);

                // Spell targets an enemy.
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_ENEMY-1);

                // Spell targets a single friend (or self).
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_FRIEND-1);

                // Spell targets AoE friends.
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_FRIEND-1);

                // Spell targets any friend (or self).
                if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                    pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER)
                    SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_FRIEND-1);

                // Make sure that this spell includes a damage effect.
                if (pTempSpell->Effects[j].Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                    pTempSpell->Effects[j].Effect == SPELL_EFFECT_INSTAKILL ||
                    pTempSpell->Effects[j].Effect == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE ||
                    pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEALTH_LEECH)
                    SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_DAMAGE-1);

                // Make sure that this spell includes a healing effect (or an apply aura with a periodic heal).
                if (pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEAL ||
                    pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                    pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEAL_MECHANICAL ||
                    ((pTempSpell->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA || pTempSpell->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA_ON_PET)
                    && pTempSpell->Effects[j].ApplyAuraName == 8))
                    SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_HEALING-1);

                // Make sure that this spell applies an aura.
                if (pTempSpell->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA ||
                    pTempSpell->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA_ON_PET)
                    SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_AURA-1);
            }
        }
    }

    void ScriptMgr::CreateSpellScripts(uint32 spellId, std::list<SpellScript*>& scriptVector)
    {
        SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);

        for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            Interfaces::SpellScriptLoader* tmpscript = ScriptRegistry<Interfaces::SpellScriptLoader>::GetScriptById(itr->second);
            if (!tmpscript)
                continue;

            SpellScript* script = tmpscript->GetSpellScript();

            if (!script)
                continue;

            script->_Init(&tmpscript->GetName(), spellId);

            scriptVector.push_back(script);
        }
    }

    void ScriptMgr::CreateAuraScripts(uint32 spellId, std::list<AuraScript*>& scriptVector)
    {
        SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);

        for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            Interfaces::SpellScriptLoader* tmpscript = ScriptRegistry<Interfaces::SpellScriptLoader>::GetScriptById(itr->second);
            if (!tmpscript)
                continue;

            AuraScript* script = tmpscript->GetAuraScript();

            if (!script)
                continue;

            script->_Init(&tmpscript->GetName(), spellId);

            scriptVector.push_back(script);
        }
    }

    void ScriptMgr::CreateSpellScriptLoaders(uint32 spellId, std::vector<std::pair<Interfaces::SpellScriptLoader*, SpellScriptsContainer::iterator> >& scriptVector)
    {
        SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);
        scriptVector.reserve(std::distance(bounds.first, bounds.second));

        for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            Interfaces::SpellScriptLoader* tmpscript = ScriptRegistry<Interfaces::SpellScriptLoader>::GetScriptById(itr->second);
            if (!tmpscript)
                continue;

            scriptVector.push_back(std::make_pair(tmpscript, itr));
        }
    }

    void ScriptMgr::OnNetworkStart()
    {
        FOREACH_SCRIPT(Interfaces::ServerScript)->OnNetworkStart();
    }

    void ScriptMgr::OnNetworkStop()
    {
        FOREACH_SCRIPT(Interfaces::ServerScript)->OnNetworkStop();
    }

    void ScriptMgr::OnSocketOpen(WorldSocket* socket)
    {
        ASSERT(socket);

        FOREACH_SCRIPT(Interfaces::ServerScript)->OnSocketOpen(socket);
    }

    void ScriptMgr::OnSocketClose(WorldSocket* socket, bool wasNew)
    {
        ASSERT(socket);

        FOREACH_SCRIPT(Interfaces::ServerScript)->OnSocketClose(socket, wasNew);
    }

    void ScriptMgr::OnPacketReceive(WorldSocket* socket, WorldPacket packet)
    {
        ASSERT(socket);

        FOREACH_SCRIPT(Interfaces::ServerScript)->OnPacketReceive(socket, packet);
    }

    void ScriptMgr::OnPacketSend(WorldSocket* socket, WorldPacket packet)
    {
        ASSERT(socket);

        FOREACH_SCRIPT(Interfaces::ServerScript)->OnPacketSend(socket, packet);
    }

    void ScriptMgr::OnUnknownPacketReceive(WorldSocket* socket, WorldPacket packet)
    {
        ASSERT(socket);

        FOREACH_SCRIPT(Interfaces::ServerScript)->OnUnknownPacketReceive(socket, packet);
    }

    void ScriptMgr::OnOpenStateChange(bool open)
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnOpenStateChange(open);
    }

    void ScriptMgr::OnConfigLoad(bool reload)
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnConfigLoad(reload);
    }

    void ScriptMgr::OnMotdChange(std::string& newMotd)
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnMotdChange(newMotd);
    }

    void ScriptMgr::OnShutdownInitiate(ShutdownExitCode code, ShutdownMask mask)
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnShutdownInitiate(code, mask);
    }

    void ScriptMgr::OnShutdownCancel()
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnShutdownCancel();
    }

    void ScriptMgr::OnWorldUpdate(uint32 diff)
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnUpdate(diff);
    }

    void ScriptMgr::OnHonorCalculation(float& honor, uint8 level, float multiplier)
    {
        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnHonorCalculation(honor, level, multiplier);
    }

    void ScriptMgr::OnGrayLevelCalculation(uint8& grayLevel, uint8 playerLevel)
    {
        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnGrayLevelCalculation(grayLevel, playerLevel);
    }

    void ScriptMgr::OnColorCodeCalculation(XPColorChar& color, uint8 playerLevel, uint8 mobLevel)
    {
        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnColorCodeCalculation(color, playerLevel, mobLevel);
    }

    void ScriptMgr::OnZeroDifferenceCalculation(uint8& diff, uint8 playerLevel)
    {
        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnZeroDifferenceCalculation(diff, playerLevel);
    }

    void ScriptMgr::OnBaseGainCalculation(uint32& gain, uint8 playerLevel, uint8 mobLevel, ContentLevels content)
    {
        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnBaseGainCalculation(gain, playerLevel, mobLevel, content);
    }

    void ScriptMgr::OnGainCalculation(uint32& gain, Player* player, Unit* unit)
    {
        ASSERT(player);
        ASSERT(unit);

        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnGainCalculation(gain, player, unit);
    }

    void ScriptMgr::OnGroupRateCalculation(float& rate, uint32 count, bool isRaid)
    {
        FOREACH_SCRIPT(Interfaces::FormulaScript)->OnGroupRateCalculation(rate, count, isRaid);
    }

    #define SCR_MAP_BGN(M, V, I, E, C, T) \
        if (V->GetEntry() && V->GetEntry()->T()) \
        { \
            FOR_SCRIPTS(M, I, E) \
            { \
                MapEntry const* C = I->second->GetEntry(); \
                if (!C) \
                    continue; \
                if (C->MapID == V->GetId()) \
                {

    #define SCR_MAP_END \
                    return; \
                } \
            } \
        }

    void ScriptMgr::OnCreateMap(Map* map)
    {
        ASSERT(map);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnCreate(map);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnCreate((InstanceMap*)map);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnCreate((BattlegroundMap*)map);
        SCR_MAP_END;
    }

    void ScriptMgr::OnDestroyMap(Map* map)
    {
        ASSERT(map);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnDestroy(map);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnDestroy((InstanceMap*)map);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnDestroy((BattlegroundMap*)map);
        SCR_MAP_END;
    }

    void ScriptMgr::OnLoadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
    {
        ASSERT(map);
        ASSERT(gmap);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnLoadGridMap(map, gmap, gx, gy);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnLoadGridMap((InstanceMap*)map, gmap, gx, gy);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnLoadGridMap((BattlegroundMap*)map, gmap, gx, gy);
        SCR_MAP_END;
    }

    void ScriptMgr::OnUnloadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
    {
        ASSERT(map);
        ASSERT(gmap);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnUnloadGridMap(map, gmap, gx, gy);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnUnloadGridMap((InstanceMap*)map, gmap, gx, gy);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnUnloadGridMap((BattlegroundMap*)map, gmap, gx, gy);
        SCR_MAP_END;
    }

    void ScriptMgr::OnPlayerEnterMap(Map* map, Player* player)
    {
        ASSERT(map);
        ASSERT(player);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnPlayerEnter(map, player);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnPlayerEnter((InstanceMap*)map, player);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnPlayerEnter((BattlegroundMap*)map, player);
        SCR_MAP_END;
    }

    void ScriptMgr::OnPlayerLeaveMap(Map* map, Player* player)
    {
        ASSERT(map);
        ASSERT(player);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnPlayerLeave(map, player);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnPlayerLeave((InstanceMap*)map, player);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnPlayerLeave((BattlegroundMap*)map, player);
        SCR_MAP_END;
    }

    void ScriptMgr::OnMapUpdate(Map* map, uint32 diff)
    {
        ASSERT(map);

        SCR_MAP_BGN(Interfaces::WorldMapScript, map, itr, end, entry, IsWorldMap);
            itr->second->OnUpdate(map, diff);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::InstanceMapScript, map, itr, end, entry, IsDungeon);
            itr->second->OnUpdate((InstanceMap*)map, diff);
        SCR_MAP_END;

        SCR_MAP_BGN(Interfaces::BattlegroundMapScript, map, itr, end, entry, IsBattleground);
            itr->second->OnUpdate((BattlegroundMap*)map, diff);
        SCR_MAP_END;
    }

    #undef SCR_MAP_BGN
    #undef SCR_MAP_END

    InstanceScript* ScriptMgr::CreateInstanceData(InstanceMap* map)
    {
        ASSERT(map);

        GET_SCRIPT_RET(Interfaces::InstanceMapScript, map->GetScriptId(), tmpscript, NULL);
        return tmpscript->GetInstanceScript(map);
    }

    bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, Item* target)
    {
        ASSERT(caster);
        ASSERT(target);

        GET_SCRIPT_RET(Interfaces::ItemScript, target->GetScriptId(), tmpscript, false);
        return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
    }

    bool ScriptMgr::OnQuestAccept(Player* player, Item* item, Quest const* quest)
    {
        ASSERT(player);
        ASSERT(item);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::ItemScript, item->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestAccept(player, item, quest);
    }

    bool ScriptMgr::OnItemUse(Player* player, Item* item, SpellCastTargets const& targets)
    {
        ASSERT(player);
        ASSERT(item);

        GET_SCRIPT_RET(Interfaces::ItemScript, item->GetScriptId(), tmpscript, false);
        return tmpscript->OnUse(player, item, targets);
    }

    bool ScriptMgr::OnItemExpire(Player* player, ItemTemplate const* proto)
    {
        ASSERT(player);
        ASSERT(proto);

        GET_SCRIPT_RET(Interfaces::ItemScript, proto->ScriptId, tmpscript, false);
        return tmpscript->OnExpire(player, proto);
    }

    bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, Creature* target)
    {
        ASSERT(caster);
        ASSERT(target);

        GET_SCRIPT_RET(Interfaces::CreatureScript, target->GetScriptId(), tmpscript, false);
        return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
    }

    bool ScriptMgr::OnGossipHello(Player* player, Creature* creature)
    {
        ASSERT(player);
        ASSERT(creature);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnGossipHello(player, creature);
    }

    bool ScriptMgr::OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        ASSERT(player);
        ASSERT(creature);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        return tmpscript->OnGossipSelect(player, creature, sender, action);
    }

    bool ScriptMgr::OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, const char* code)
    {
        ASSERT(player);
        ASSERT(creature);
        ASSERT(code);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        return tmpscript->OnGossipSelectCode(player, creature, sender, action, code);
    }

    bool ScriptMgr::OnQuestAccept(Player* player, Creature* creature, Quest const* quest)
    {
        ASSERT(player);
        ASSERT(creature);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestAccept(player, creature, quest);
    }

    bool ScriptMgr::OnQuestSelect(Player* player, Creature* creature, Quest const* quest)
    {
        ASSERT(player);
        ASSERT(creature);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestSelect(player, creature, quest);
    }

    bool ScriptMgr::OnQuestComplete(Player* player, Creature* creature, Quest const* quest)
    {
        ASSERT(player);
        ASSERT(creature);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestComplete(player, creature, quest);
    }

    bool ScriptMgr::OnQuestReward(Player* player, Creature* creature, Quest const* quest, uint32 opt)
    {
        ASSERT(player);
        ASSERT(creature);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestReward(player, creature, quest, opt);
    }

    uint32 ScriptMgr::GetDialogStatus(Player* player, Creature* creature)
    {
        ASSERT(player);
        ASSERT(creature);

        // TODO: 100 is a funny magic number to have hanging around here...
        GET_SCRIPT_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript, 100);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->GetDialogStatus(player, creature);
    }

    CreatureAI* ScriptMgr::GetCreatureAI(Creature* creature)
    {
        ASSERT(creature);

        GET_SCRIPT_NO_RET(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript);
        GET_SCRIPT_NO_RET(Interfaces::VehicleScript, creature->GetScriptId(), tmpVehiclescript);

        if (tmpscript)
            return tmpscript->GetAI(creature);
        else if (tmpVehiclescript)
            return tmpVehiclescript->GetAI(creature);
        else
            return NULL;
    }

    GameObjectAI* ScriptMgr::GetGameObjectAI(GameObject* gameobject)
    {
        ASSERT(gameobject);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, gameobject->GetScriptId(), tmpscript, NULL);
        return tmpscript->GetAI(gameobject);
    }

    void ScriptMgr::OnCreatureUpdate(Creature* creature, uint32 diff)
    {
        ASSERT(creature);

        GET_SCRIPT(Interfaces::CreatureScript, creature->GetScriptId(), tmpscript);
        tmpscript->OnUpdate(creature, diff);
    }

    bool ScriptMgr::OnGossipHello(Player* player, GameObject* go)
    {
        ASSERT(player);
        ASSERT(go);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnGossipHello(player, go);
    }

    bool ScriptMgr::OnGossipSelect(Player* player, GameObject* go, uint32 sender, uint32 action)
    {
        ASSERT(player);
        ASSERT(go);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript, false);
        return tmpscript->OnGossipSelect(player, go, sender, action);
    }

    bool ScriptMgr::OnGossipSelectCode(Player* player, GameObject* go, uint32 sender, uint32 action, const char* code)
    {
        ASSERT(player);
        ASSERT(go);
        ASSERT(code);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript, false);
        return tmpscript->OnGossipSelectCode(player, go, sender, action, code);
    }

    bool ScriptMgr::OnQuestAccept(Player* player, GameObject* go, Quest const* quest)
    {
        ASSERT(player);
        ASSERT(go);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestAccept(player, go, quest);
    }

    bool ScriptMgr::OnQuestReward(Player* player, GameObject* go, Quest const* quest, uint32 opt)
    {
        ASSERT(player);
        ASSERT(go);
        ASSERT(quest);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript, false);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->OnQuestReward(player, go, quest, opt);
    }

    uint32 ScriptMgr::GetDialogStatus(Player* player, GameObject* go)
    {
        ASSERT(player);
        ASSERT(go);

        // TODO: 100 is a funny magic number to have hanging around here...
        GET_SCRIPT_RET(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript, 100);
        player->PlayerTalkClass->ClearMenus();
        return tmpscript->GetDialogStatus(player, go);
    }

    void ScriptMgr::OnGameObjectDestroyed(GameObject* go, Player* player)
    {
        ASSERT(go);

        GET_SCRIPT(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript);
        tmpscript->OnDestroyed(go, player);
    }

    void ScriptMgr::OnGameObjectDamaged(GameObject* go, Player* player)
    {
        ASSERT(go);

        GET_SCRIPT(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript);
        tmpscript->OnDamaged(go, player);
    }

    void ScriptMgr::OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit)
    {
        ASSERT(go);

        GET_SCRIPT(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript);
        tmpscript->OnLootStateChanged(go, state, unit);
    }

    void ScriptMgr::OnGameObjectStateChanged(GameObject* go, uint32 state)
    {
        ASSERT(go);

        GET_SCRIPT(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript);
        tmpscript->OnGameObjectStateChanged(go, state);
    }

    void ScriptMgr::OnGameObjectUpdate(GameObject* go, uint32 diff)
    {
        ASSERT(go);

        GET_SCRIPT(Interfaces::GameObjectScript, go->GetScriptId(), tmpscript);
        tmpscript->OnUpdate(go, diff);
    }

    bool ScriptMgr::OnGameObjectElevatorCheck(GameObject const* p_GameObject) const
    {
        ASSERT(p_GameObject);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, p_GameObject->GetScriptId(), tmpscript, true);
        return tmpscript->OnGameObjectElevatorCheck(p_GameObject);
    }

    bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, GameObject* target)
    {
        ASSERT(caster);
        ASSERT(target);

        GET_SCRIPT_RET(Interfaces::GameObjectScript, target->GetScriptId(), tmpscript, false);
        return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
    }

    bool ScriptMgr::OnAreaTrigger(Player* player, AreaTriggerEntry const* trigger)
    {
        ASSERT(player);
        ASSERT(trigger);

        GET_SCRIPT_RET(Interfaces::AreaTriggerScript, sObjectMgr->GetAreaTriggerScriptId(trigger->ID), tmpscript, false);
        return tmpscript->OnTrigger(player, trigger);
    }

    void ScriptMgr::OnCreateAreaTriggerEntity(AreaTrigger* p_AreaTrigger)
    {
        ASSERT(p_AreaTrigger);

        // On creation, we look for instanciating a new script, localy to the AreaTrigger.
        if (!p_AreaTrigger->GetScript())
        {
            Interfaces::AreaTriggerEntityScript* l_AreaTriggerScript = ScriptRegistry<Interfaces::AreaTriggerEntityScript>::GetScriptById(p_AreaTrigger->GetMainTemplate()->m_ScriptId);
            if (l_AreaTriggerScript == nullptr)
                return;

            p_AreaTrigger->SetScript(l_AreaTriggerScript->GetAI());
        }

        // This checks is usefull if you run out of memory.
        if (!p_AreaTrigger->GetScript())
            return;

        p_AreaTrigger->GetScript()->OnCreate(p_AreaTrigger);
    }


    void ScriptMgr::OnUpdateAreaTriggerEntity(AreaTrigger* p_AreaTrigger, uint32 p_Time)
    {
        ASSERT(p_AreaTrigger);

        if (!p_AreaTrigger->GetScript())
            return;

        p_AreaTrigger->GetScript()->OnUpdate(p_AreaTrigger, p_Time);
    }

    void ScriptMgr::OnRemoveAreaTriggerEntity(AreaTrigger* p_AreaTrigger, uint32 p_Time)
    {
        ASSERT(p_AreaTrigger);

        if (!p_AreaTrigger->GetScript())
            return;

        p_AreaTrigger->GetScript()->OnRemove(p_AreaTrigger, p_Time);
    }

    Battleground* ScriptMgr::CreateBattleground(BattlegroundTypeId /*typeId*/)
    {
        // TODO: Implement script-side battlegrounds.
        ASSERT(false);
        return NULL;
    }

    OutdoorPvP* ScriptMgr::CreateOutdoorPvP(OutdoorPvPData const* data)
    {
        ASSERT(data);

        GET_SCRIPT_RET(Interfaces::OutdoorPvPScript, data->ScriptId, tmpscript, NULL);
        return tmpscript->GetOutdoorPvP();
    }

    std::vector<ChatCommand*> ScriptMgr::GetChatCommands()
    {
        std::vector<ChatCommand*> table;

        FOR_SCRIPTS_RET(Interfaces::CommandScript, itr, end, table)
            table.push_back(itr->second->GetCommands());

        return table;
    }

    void ScriptMgr::OnWeatherChange(Weather* weather, WeatherState state, float grade)
    {
        ASSERT(weather);

        GET_SCRIPT(Interfaces::WeatherScript, weather->GetScriptId(), tmpscript);
        tmpscript->OnChange(weather, state, grade);
    }

    void ScriptMgr::OnWeatherUpdate(Weather* weather, uint32 diff)
    {
        ASSERT(weather);

        GET_SCRIPT(Interfaces::WeatherScript, weather->GetScriptId(), tmpscript);
        tmpscript->OnUpdate(weather, diff);
    }

    void ScriptMgr::OnAuctionAdd(AuctionHouseObject* ah, AuctionEntry* entry)
    {
        ASSERT(ah);
        ASSERT(entry);

        FOREACH_SCRIPT(Interfaces::AuctionHouseScript)->OnAuctionAdd(ah, entry);
    }

    void ScriptMgr::OnAuctionRemove(AuctionHouseObject* ah, AuctionEntry* entry)
    {
        ASSERT(ah);
        ASSERT(entry);

        FOREACH_SCRIPT(Interfaces::AuctionHouseScript)->OnAuctionRemove(ah, entry);
    }

    void ScriptMgr::OnAuctionSuccessful(AuctionHouseObject* ah, AuctionEntry* entry)
    {
        ASSERT(ah);
        ASSERT(entry);

        FOREACH_SCRIPT(Interfaces::AuctionHouseScript)->OnAuctionSuccessful(ah, entry);
    }

    void ScriptMgr::OnAuctionExpire(AuctionHouseObject* ah, AuctionEntry* entry)
    {
        ASSERT(ah);
        ASSERT(entry);

        FOREACH_SCRIPT(Interfaces::AuctionHouseScript)->OnAuctionExpire(ah, entry);
    }

    bool ScriptMgr::OnConditionCheck(Condition* condition, ConditionSourceInfo& sourceInfo)
    {
        ASSERT(condition);

        GET_SCRIPT_RET(Interfaces::ConditionScript, condition->ScriptId, tmpscript, true);
        return tmpscript->OnConditionCheck(condition, sourceInfo);
    }

    void ScriptMgr::OnInstall(Vehicle* veh)
    {
        ASSERT(veh);
        ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

        GET_SCRIPT(Interfaces::VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
        tmpscript->OnInstall(veh);
    }

    void ScriptMgr::OnUninstall(Vehicle* veh)
    {
        ASSERT(veh);
        ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

        GET_SCRIPT(Interfaces::VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
        tmpscript->OnUninstall(veh);
    }

    void ScriptMgr::OnReset(Vehicle* veh)
    {
        ASSERT(veh);
        ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

        GET_SCRIPT(Interfaces::VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
        tmpscript->OnReset(veh);
    }

    void ScriptMgr::OnInstallAccessory(Vehicle* veh, Creature* accessory)
    {
        ASSERT(veh);
        ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
        ASSERT(accessory);

        GET_SCRIPT(Interfaces::VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
        tmpscript->OnInstallAccessory(veh, accessory);
    }

    void ScriptMgr::OnAddPassenger(Vehicle* veh, Unit* passenger, int8 seatId)
    {
        ASSERT(veh);
        ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
        ASSERT(passenger);

        GET_SCRIPT(Interfaces::VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
        tmpscript->OnAddPassenger(veh, passenger, seatId);
    }

    void ScriptMgr::OnRemovePassenger(Vehicle* veh, Unit* passenger)
    {
        ASSERT(veh);
        ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
        ASSERT(passenger);

        GET_SCRIPT(Interfaces::VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
        tmpscript->OnRemovePassenger(veh, passenger);
    }

    void ScriptMgr::OnDynamicObjectUpdate(DynamicObject* dynobj, uint32 diff)
    {
        ASSERT(dynobj);

        FOR_SCRIPTS(Interfaces::DynamicObjectScript, itr, end)
            itr->second->OnUpdate(dynobj, diff);
    }

    void ScriptMgr::OnAddPassenger(Transport* transport, Player* player)
    {
        ASSERT(transport);
        ASSERT(player);

        GET_SCRIPT(Interfaces::TransportScript, transport->GetScriptId(), tmpscript);
        tmpscript->OnAddPassenger(transport, player);
    }

    void ScriptMgr::OnAddCreaturePassenger(Transport* transport, Creature* creature)
    {
        ASSERT(transport);
        ASSERT(creature);

        GET_SCRIPT(Interfaces::TransportScript, transport->GetScriptId(), tmpscript);
        tmpscript->OnAddCreaturePassenger(transport, creature);
    }

    void ScriptMgr::OnRemovePassenger(Transport* transport, Player* player)
    {
        ASSERT(transport);
        ASSERT(player);

        GET_SCRIPT(Interfaces::TransportScript, transport->GetScriptId(), tmpscript);
        tmpscript->OnRemovePassenger(transport, player);
    }

    void ScriptMgr::OnTransportUpdate(Transport* transport, uint32 diff)
    {
        ASSERT(transport);

        GET_SCRIPT(Interfaces::TransportScript, transport->GetScriptId(), tmpscript);
        tmpscript->OnUpdate(transport, diff);
    }

    void ScriptMgr::OnRelocate(Transport* transport, uint32 waypointId, uint32 mapId, float x, float y, float z)
    {
        GET_SCRIPT(Interfaces::TransportScript, transport->GetScriptId(), tmpscript);
        tmpscript->OnRelocate(transport, waypointId, mapId, x, y, z);
    }

    void ScriptMgr::OnStartup()
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnStartup();
    }

    void ScriptMgr::OnShutdown()
    {
        FOREACH_SCRIPT(Interfaces::WorldScript)->OnShutdown();
    }

    bool ScriptMgr::OnCriteriaCheck(uint32 scriptId, Player* source, Unit* target)
    {
        ASSERT(source);
        // target can be NULL.

        GET_SCRIPT_RET(Interfaces::AchievementCriteriaScript, scriptId, tmpscript, false);
        return tmpscript->OnCheck(source, target);
    }

    // Player
    void ScriptMgr::OnPVPKill(Player* killer, Player* killed)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnPVPKill(killer, killed);
    }

    void ScriptMgr::OnModifyPower(Player* killer, Powers power, int32 value)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnModifyPower(killer, power, value);
    }

    void ScriptMgr::OnCreatureKill(Player* killer, Creature* killed)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnCreatureKill(killer, killed);
    }

    void ScriptMgr::OnPlayerKilledByCreature(Creature* killer, Player* killed)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnPlayerKilledByCreature(killer, killed);
    }

    void ScriptMgr::OnQuestReward(Player* p_Player, const Quest* p_Quest)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnQuestReward(p_Player, p_Quest);
    }

    void ScriptMgr::OnObjectiveValidate(Player* p_Player, uint32 p_QuestId, uint32 p_ObjectiveId)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnObjectiveValidate(p_Player, p_QuestId, p_ObjectiveId);
    }

    void ScriptMgr::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnLevelChanged(player, oldLevel);
    }

    void ScriptMgr::OnPlayerTalentsReset(Player* player, bool noCost)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnTalentsReset(player, noCost);
    }

    void ScriptMgr::OnPlayerMoneyChanged(Player* player, int64& amount)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnMoneyChanged(player, amount);
    }

    void ScriptMgr::OnGivePlayerXP(Player* player, uint32& amount, Unit* victim)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnGiveXP(player, amount, victim);
    }

    void ScriptMgr::OnPlayerReputationChange(Player* player, uint32 factionID, int32& standing, bool incremental)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnReputationChange(player, factionID, standing, incremental);
    }

    void ScriptMgr::OnPlayerDuelRequest(Player* target, Player* challenger)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnDuelRequest(target, challenger);
    }

    void ScriptMgr::OnPlayerDuelStart(Player* player1, Player* player2)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnDuelStart(player1, player2);
    }

    void ScriptMgr::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnDuelEnd(winner, loser, type);
    }

    void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnChat(player, type, lang, msg);
    }

    void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnChat(player, type, lang, msg, receiver);
    }

    void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnChat(player, type, lang, msg, group);
    }

    void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* guild)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnChat(player, type, lang, msg, guild);
    }

    void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnChat(player, type, lang, msg, channel);
    }

    void ScriptMgr::OnPlayerEmote(Player* player, uint32 emote)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnEmote(player, emote);
    }

    void ScriptMgr::OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 soundIndex, uint64 guid)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnTextEmote(player, textEmote, soundIndex, guid);
    }

    void ScriptMgr::OnPlayerSpellLearned(Player* p_Player, uint32 p_SpellId)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnSpellLearned(p_Player, p_SpellId);
    }

    void ScriptMgr::OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnSpellCast(player, spell, skipCheck);
    }

    void ScriptMgr::OnPlayerLogin(Player* player)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnLogin(player);
    }

    void ScriptMgr::OnPlayerLogout(Player* player)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnLogout(player);
    }

    void ScriptMgr::OnPlayerCreate(Player* player)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnCreate(player);
    }

    void ScriptMgr::OnPlayerDelete(uint64 guid)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnDelete(guid);
    }

    void ScriptMgr::OnPlayerBindToInstance(Player* player, Difficulty difficulty, uint32 mapid, bool permanent)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnBindToInstance(player, difficulty, mapid, permanent);
    }

    void ScriptMgr::OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 p_OldZoneID, uint32 newArea)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnUpdateZone(player, newZone, p_OldZoneID, newArea);
    }

    void ScriptMgr::OnPlayerUpdateMovement(Player* p_Player)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnPlayerUpdateMovement(p_Player);
    }

    void ScriptMgr::OnPlayerChangeShapeshift(Player* p_Player, ShapeshiftForm p_Form)
    {
        FOREACH_SCRIPT(Interfaces::PlayerScript)->OnChangeShapeshift(p_Player, p_Form);
    }

    // Guild
    void ScriptMgr::OnGuildAddMember(Guild* guild, Player* player, uint8& plRank)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnAddMember(guild, player, plRank);
    }

    void ScriptMgr::OnGuildRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool isKicked)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnRemoveMember(guild, player, isDisbanding, isKicked);
    }

    void ScriptMgr::OnGuildMOTDChanged(Guild* guild, const std::string& newMotd)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnMOTDChanged(guild, newMotd);
    }

    void ScriptMgr::OnGuildInfoChanged(Guild* guild, const std::string& newInfo)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnInfoChanged(guild, newInfo);
    }

    void ScriptMgr::OnGuildCreate(Guild* guild, Player* leader, const std::string& name)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnCreate(guild, leader, name);
    }

    void ScriptMgr::OnGuildDisband(Guild* guild)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnDisband(guild);
    }

    void ScriptMgr::OnGuildMemberWitdrawMoney(Guild* guild, Player* player, uint64 &amount, bool isRepair)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnMemberWitdrawMoney(guild, player, amount, isRepair);
    }

    void ScriptMgr::OnGuildMemberDepositMoney(Guild* guild, Player* player, uint64 &amount)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnMemberDepositMoney(guild, player, amount);
    }

    void ScriptMgr::OnGuildItemMove(Guild* guild, Player* player, Item* pItem, bool isSrcBank, uint8 srcContainer, uint8 srcSlotId,
                bool isDestBank, uint8 destContainer, uint8 destSlotId)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnItemMove(guild, player, pItem, isSrcBank, srcContainer, srcSlotId, isDestBank, destContainer, destSlotId);
    }

    void ScriptMgr::OnGuildEvent(Guild* guild, uint8 eventType, uint32 playerGuid1, uint32 playerGuid2, uint8 newRank)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnEvent(guild, eventType, playerGuid1, playerGuid2, newRank);
    }

    void ScriptMgr::OnGuildBankEvent(Guild* guild, uint8 eventType, uint8 tabId, uint32 playerGuid, uint32 itemOrMoney, uint16 itemStackCount, uint8 destTabId)
    {
        FOREACH_SCRIPT(Interfaces::GuildScript)->OnBankEvent(guild, eventType, tabId, playerGuid, itemOrMoney, itemStackCount, destTabId);
    }

    // Group
    void ScriptMgr::OnGroupAddMember(Group* group, uint64 guid)
    {
        ASSERT(group);
        FOREACH_SCRIPT(Interfaces::GroupScript)->OnAddMember(group, guid);
    }

    void ScriptMgr::OnGroupInviteMember(Group* group, uint64 guid)
    {
        ASSERT(group);
        FOREACH_SCRIPT(Interfaces::GroupScript)->OnInviteMember(group, guid);
    }

    void ScriptMgr::OnGroupRemoveMember(Group* group, uint64 guid, RemoveMethod method, uint64 kicker, const char* reason)
    {
        ASSERT(group);
        FOREACH_SCRIPT(Interfaces::GroupScript)->OnRemoveMember(group, guid, method, kicker, reason);
    }

    void ScriptMgr::OnGroupChangeLeader(Group* group, uint64 newLeaderGuid, uint64 oldLeaderGuid)
    {
        ASSERT(group);
        FOREACH_SCRIPT(Interfaces::GroupScript)->OnChangeLeader(group, newLeaderGuid, oldLeaderGuid);
    }

    void ScriptMgr::OnGroupDisband(Group* group)
    {
        ASSERT(group);
        FOREACH_SCRIPT(Interfaces::GroupScript)->OnDisband(group);
    }

    namespace Interfaces 
    {
        /// Constructor
        /// @p_Name : Script name
        SpellScriptLoader::SpellScriptLoader(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<SpellScriptLoader>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script name
        ServerScript::ServerScript(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<ServerScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script Name
        WorldScript::WorldScript(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<WorldScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script Name
        FormulaScript::FormulaScript(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<FormulaScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name  : Script Name
        /// @p_MapID : Map Script map ID
        WorldMapScript::WorldMapScript(const char * p_Name, uint32 p_MapID)
            : ScriptObjectImpl(p_Name), MapScript<Map>(p_MapID)
        {
            if (GetEntry() && !GetEntry()->IsWorldMap())
                sLog->outError(LOG_FILTER_TSCR, "WorldMapScript for map %u is invalid.", p_MapID);

            ScriptRegistry<WorldMapScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name  : Script Name
        /// @p_MapID : Map Script map ID
        InstanceMapScript::InstanceMapScript(const char * p_Name, uint32 p_MapID)
            : ScriptObjectImpl(p_Name), MapScript<InstanceMap>(p_MapID)
        {
            if (GetEntry() && !GetEntry()->IsDungeon())
                sLog->outError(LOG_FILTER_TSCR, "InstanceMapScript for map %u is invalid.", p_MapID);

            ScriptRegistry<InstanceMapScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name  : Script Name
        /// @p_MapID : Map Script map ID
        BattlegroundMapScript::BattlegroundMapScript(const char * p_Name, uint32 p_MapID)
            : ScriptObjectImpl(p_Name), MapScript<BattlegroundMap>(p_MapID)
        {
            if (GetEntry() && !GetEntry()->IsBattleground())
                sLog->outError(LOG_FILTER_TSCR, "BattlegroundMapScript for map %u is invalid.", p_MapID);

            ScriptRegistry<BattlegroundMapScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script Name
        ItemScript::ItemScript(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<ItemScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script Name
        CreatureScript::CreatureScript(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<CreatureScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script Name
        GameObjectScript::GameObjectScript(const char * p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<GameObjectScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        AreaTriggerScript::AreaTriggerScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<AreaTriggerScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        BattlegroundScript::BattlegroundScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<BattlegroundScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        OutdoorPvPScript::OutdoorPvPScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<OutdoorPvPScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        CommandScript::CommandScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<CommandScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        WeatherScript::WeatherScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<WeatherScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        AuctionHouseScript::AuctionHouseScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<AuctionHouseScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        ConditionScript::ConditionScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<ConditionScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        VehicleScript::VehicleScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<VehicleScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        DynamicObjectScript::DynamicObjectScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<DynamicObjectScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        TransportScript::TransportScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<TransportScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        AchievementCriteriaScript::AchievementCriteriaScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<AchievementCriteriaScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        PlayerScript::PlayerScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<PlayerScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        GuildScript::GuildScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<GuildScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script p_Name
        GroupScript::GroupScript(const char* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<GroupScript>::AddScript(this);
        }

        /// Constructor
        /// @p_Name : Script Name
        AreaTriggerEntityScript::AreaTriggerEntityScript(char const* p_Name)
            : ScriptObjectImpl(p_Name)
        {
            ScriptRegistry<AreaTriggerEntityScript>::AddScript(this);
        }
    }   ///< Namespace Interfaces

    // Instantiate static members of ScriptRegistry.
    template<class TScript> std::map<uint32, TScript*> ScriptRegistry<TScript>::ScriptPointerList;
    template<class TScript> uint32 ScriptRegistry<TScript>::_scriptIdCounter = 0;

    // Specialize for each script type class like so:
    template class ScriptRegistry<Interfaces::SpellScriptLoader>;
    template class ScriptRegistry<Interfaces::ServerScript>;
    template class ScriptRegistry<Interfaces::WorldScript>;
    template class ScriptRegistry<Interfaces::FormulaScript>;
    template class ScriptRegistry<Interfaces::WorldMapScript>;
    template class ScriptRegistry<Interfaces::InstanceMapScript>;
    template class ScriptRegistry<Interfaces::BattlegroundMapScript>;
    template class ScriptRegistry<Interfaces::ItemScript>;
    template class ScriptRegistry<Interfaces::CreatureScript>;
    template class ScriptRegistry<Interfaces::GameObjectScript>;
    template class ScriptRegistry<Interfaces::AreaTriggerScript>;
    template class ScriptRegistry<Interfaces::BattlegroundScript>;
    template class ScriptRegistry<Interfaces::OutdoorPvPScript>;
    template class ScriptRegistry<Interfaces::CommandScript>;
    template class ScriptRegistry<Interfaces::WeatherScript>;
    template class ScriptRegistry<Interfaces::AuctionHouseScript>;
    template class ScriptRegistry<Interfaces::ConditionScript>;
    template class ScriptRegistry<Interfaces::VehicleScript>;
    template class ScriptRegistry<Interfaces::DynamicObjectScript>;
    template class ScriptRegistry<Interfaces::TransportScript>;
    template class ScriptRegistry<Interfaces::AchievementCriteriaScript>;
    template class ScriptRegistry<Interfaces::PlayerScript>;
    template class ScriptRegistry<Interfaces::GuildScript>;
    template class ScriptRegistry<Interfaces::GroupScript>;
    template class ScriptRegistry<Interfaces::AreaTriggerEntityScript>;

    // Undefine utility macros.
    #undef GET_SCRIPT_RET
    #undef GET_SCRIPT
    #undef FOREACH_SCRIPT
    #undef FOR_SCRIPTS_RET
    #undef FOR_SCRIPTS
    #undef SCR_REG_LST
    #undef SCR_REG_ITR
    #undef SCR_REG_MAP

}   ///< Namespace Scripting
}   ///< Namespace Game
}   ///< Namespace MS