/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotPopulation.h"
#include "BotMgr.h"
#include "BotQuesting.h"
#include "ProgressionMgr.h" // mod-progression: bots follow the players' patch

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Config.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>

namespace
{
    // How often the patch the bots follow is checked, and online bots behind it move up.
    constexpr uint32 TARGET_CHECK_MS = 10 * IN_MILLISECONDS;

    // Characters created per world tick while the population is being built.
    constexpr uint32 CREATIONS_PER_TICK = 10;

    // Length of the window ".bot population" reports world tick times over.
    constexpr uint32 TICK_WINDOW_MS = 60 * IN_MILLISECONDS;

    std::string RandomPassword()
    {
        static char const characters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::string password;
        for (int i = 0; i < 16; ++i)
            password += characters[urand(0, sizeof(characters) - 2)];

        return password;
    }

    // A random appearance the server accepts: each feature in turn takes a random value among those
    // that are valid with the features chosen so far.
    void RandomizeAppearance(WorldPackets::Character::CharacterCreateInfo& info)
    {
        constexpr uint8 MAX_VALUE = 32;
        uint8* features[] = { &info.Skin, &info.Face, &info.HairStyle, &info.HairColor, &info.FacialHairStyle };
        for (uint8* feature : features)
        {
            std::vector<uint8> valid;
            for (uint8 value = 0; value < MAX_VALUE; ++value)
            {
                *feature = value;
                if (Player::ValidateAppearance(info.Race, info.Class, info.Sex, info.HairStyle, info.HairColor, info.Face,
                    info.FacialHairStyle, info.Skin, true))
                    valid.push_back(value);
            }

            *feature = valid.empty() ? 0 : valid[urand(0, valid.size() - 1)];
        }
    }
}

BotPopulation& BotPopulation::Instance()
{
    static BotPopulation instance;
    return instance;
}

void BotPopulation::LoadConfig()
{
    _count = uint32(std::max(0, sConfigMgr->GetIntDefault("Bots.Population.Count", 1000)));
    _loginsPerSecond = uint32(std::max(1, sConfigMgr->GetIntDefault("Bots.Population.LoginsPerSecond", 10)));
    _charactersPerAccount = uint32(std::clamp(sConfigMgr->GetIntDefault("Bots.Population.CharactersPerAccount", 10), 1, 10));
}

void BotPopulation::Load()
{
    LoadCensus();
    LoadNames();
    LoadBots();
    _loaded = true;

    TC_LOG_INFO("module", "mod-bots: {} bot characters ({} Alliance, {} Horde) on {} accounts; {} unused names.",
        _factionOf.size(), _bots[FACTION_ALLIANCE].size(), _bots[FACTION_HORDE].size(), _accounts.size(), _names.size());
}

void BotPopulation::LoadCensus()
{
    for (auto& entries : _census)
        entries.clear();
    _censusTotal = { };

    QueryResult result = WorldDatabase.Query("SELECT `race`, `class`, `weight` FROM `bot_census` WHERE `weight` > 0");
    if (!result)
    {
        TC_LOG_ERROR("module", "mod-bots: bot_census is missing or empty; no bot characters can be created.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        CensusEntry entry{ fields[0].GetUInt8(), fields[1].GetUInt8(), fields[2].GetUInt32() };
        if (!sObjectMgr->GetPlayerInfo(entry.Race, entry.Class))
        {
            TC_LOG_ERROR("module", "mod-bots: bot_census has race {} class {}, which can't be created; skipped.",
                entry.Race, entry.Class);
            continue;
        }

        Faction faction = Player::TeamForRace(entry.Race) == ALLIANCE ? FACTION_ALLIANCE : FACTION_HORDE;
        _census[faction].push_back(entry);
        _censusTotal[faction] += entry.Weight;
    } while (result->NextRow());
}

void BotPopulation::LoadNames()
{
    _names.clear();

    QueryResult result = WorldDatabase.Query("SELECT `name`, `gender` FROM `bot_names`");
    if (!result)
    {
        TC_LOG_ERROR("module", "mod-bots: bot_names is missing or empty; no bot characters can be created.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        std::string name = fields[0].GetString();
        // Names already taken by a character (a bot made before, or a player) are left out.
        if (sCharacterCache->GetCharacterCacheByName(name))
            continue;

        _names.push_back({ std::move(name), fields[1].GetUInt8() });
    } while (result->NextRow());

    Trinity::Containers::RandomShuffle(_names);
}

void BotPopulation::LoadBots()
{
    _accounts.clear();
    for (auto& bots : _bots)
        bots.clear();
    _factionOf.clear();

    // Bots whose character was deleted are forgotten.
    CharacterDatabase.DirectExecute("DELETE FROM `bot_characters` WHERE `guid` NOT IN (SELECT `guid` FROM `characters`)");

    if (QueryResult result = CharacterDatabase.Query("SELECT a.`id`, COUNT(b.`guid`) FROM `bot_accounts` a "
        "LEFT JOIN `bot_characters` b ON b.`account` = a.`id` GROUP BY a.`id`"))
    {
        do
        {
            Field* fields = result->Fetch();
            BotAccount account;
            account.Id = fields[0].GetUInt32();
            account.Characters = uint32(fields[1].GetUInt64());
            if (!AccountMgr::GetName(account.Id, account.Username))
                continue; // The account was deleted

            _accounts.push_back(std::move(account));
        } while (result->NextRow());
    }

    // With their patch: the hidden patch quest mod-progression gives each character.
    if (QueryResult result = CharacterDatabase.Query("SELECT b.`guid`, c.`race`, c.`class`, "
        "(SELECT MAX(q.`quest`) FROM `character_queststatus_rewarded` q WHERE q.`guid` = b.`guid` AND q.`quest` BETWEEN 90001 AND 90099) "
        "FROM `bot_characters` b JOIN `characters` c ON c.`guid` = b.`guid`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt32());
            BotInfo info;
            info.Race = fields[1].GetUInt8();
            info.Class = fields[2].GetUInt8();
            info.PatchQuest = fields[3].IsNull() ? 0 : fields[3].GetUInt32();

            Faction faction = Player::TeamForRace(info.Race) == ALLIANCE ? FACTION_ALLIANCE : FACTION_HORDE;
            _bots[faction].push_back(guid);
            _factionOf[guid] = faction;
            _info[guid] = info;
        } while (result->NextRow());
    }
}

void BotPopulation::ResolvePatches()
{
    // mod-progression loads its patches at startup after mod-bots, so bots' patch quests are read here.
    for (auto& [guid, info] : _info)
    {
        if (info.Patch)
            continue;

        for (ProgressionPatch const& patch : sProgressionMgr.GetPatches())
            if (patch.QuestId == info.PatchQuest)
                info.Patch = patch.Id;

        if (!info.Patch)
            if (ProgressionPatch const* start = sProgressionMgr.GetStartPatch(info.Race, info.Class))
                info.Patch = start->Id;
    }
    _patchesResolved = true;
}

std::string BotPopulation::Reset()
{
    if (_resetting)
        return "The bot population is already being reset.";

    if (_pendingCreations[FACTION_ALLIANCE] || _pendingCreations[FACTION_HORDE])
        return "Bots are still being created; try again in a moment.";

    _resetting = true;
    uint32 online = 0;
    for (auto const& [guid, faction] : _factionOf)
        if (Bot* bot = sBotMgr.Find(guid))
        {
            bot->RequestRemoval();
            ++online;
        }

    return Trinity::StringFormat("Resetting the bot population: {} bots are logging out, then all {} bot characters are "
        "deleted and the population is made again from level 1.", online, _factionOf.size());
}

void BotPopulation::FinishReset()
{
    uint32 deleted = 0;
    for (auto const& [guid, faction] : _factionOf)
    {
        CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByGuid(guid);
        if (!character)
            continue;

        // Removes it everywhere, and from the name cache, so its name can be used again.
        Player::DeleteFromDB(guid, character->AccountId, true, true);
        ++deleted;
    }

    CharacterDatabase.DirectExecute("DELETE FROM `bot_characters`");
    for (BotAccount& account : _accounts)
        account.Characters = 0;

    for (auto& bots : _bots)
        bots.clear();
    _factionOf.clear();
    _info.clear();
    _loggedIn.clear();
    _outOfNames = false;
    LoadNames();

    _resetting = false;
    TC_LOG_INFO("module", "mod-bots: reset the bot population: deleted {} characters; {} names free.", deleted, _names.size());
}

void BotPopulation::UpdateTargetPatch()
{
    if (!sProgressionMgr.IsEnabled())
    {
        _targetPatch = 0;
        return;
    }

    // The patch most online players are in; with nobody online, the one it had (at first, the start patch).
    std::unordered_map<uint8, uint32> players;
    for (auto const& [accountId, session] : sWorld->GetAllSessions())
        if (Player* player = session->GetPlayer(); player && player->IsInWorld() && !session->IsBot())
            if (ProgressionPatch const* patch = sProgressionMgr.GetEffectivePatch(player))
                ++players[patch->Id];

    uint8 target = _targetPatch;
    uint32 most = 0;
    for (auto const& [patchId, count] : players)
        if (count > most || (count == most && patchId < target))
        {
            target = patchId;
            most = count;
        }

    if (!target)
        if (ProgressionPatch const* start = sProgressionMgr.GetStartPatch(RACE_HUMAN, CLASS_WARRIOR))
            target = start->Id;

    if (target != _targetPatch)
    {
        if (ProgressionPatch const* patch = sProgressionMgr.GetPatch(target))
            TC_LOG_INFO("module", "mod-bots: the bot population follows patch {}.", patch->Version);
        _targetPatch = target;
    }
}

bool BotPopulation::IsEligible(ObjectGuid guid) const
{
    if (!_targetPatch)
        return true;

    auto itr = _info.find(guid);
    if (itr == _info.end())
        return false;

    ProgressionPatch const* start = sProgressionMgr.GetStartPatch(itr->second.Race, itr->second.Class);
    return start && start->Id <= _targetPatch && itr->second.Patch <= _targetPatch;
}

void BotPopulation::AdvanceOnlineBots()
{
    if (!_targetPatch)
        return;

    ProgressionPatch const* target = sProgressionMgr.GetPatch(_targetPatch);
    if (!target)
        return;

    for (auto& [guid, info] : _info)
    {
        if (info.Patch >= _targetPatch)
            continue;

        Bot* bot = sBotMgr.Find(guid);
        Player* player = bot && bot->GetSession() ? bot->GetSession()->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            continue;

        // mod-progression gives a new character its start patch on its first login; then it moves up.
        ProgressionPatch const* current = sProgressionMgr.GetCharacterPatch(player);
        if (!current)
            continue;

        if (current->Id < _targetPatch)
            sProgressionMgr.SetCharacterPatch(player, *target);
        info.Patch = std::max(current->Id, _targetPatch);
    }
}

void BotPopulation::Update(uint32 diff)
{
    _tickTimes.push_back(diff);
    _tickTimeTotal += diff;
    while (_tickTimeTotal > TICK_WINDOW_MS && _tickTimes.size() > 1)
    {
        _tickTimeTotal -= _tickTimes.front();
        _tickTimes.pop_front();
    }

    _saveCallbacks.ProcessReadyCallbacks();

    if (!_loaded || _shuttingDown)
        return;

    // Resetting: once none of them is logged in any more, they are deleted and made again.
    if (_resetting)
    {
        for (auto const& [guid, faction] : _factionOf)
            if (sBotMgr.Find(guid))
                return;

        FinishReset();
        return;
    }

    _targetCheckMs = _targetCheckMs > diff ? _targetCheckMs - diff : 0;
    if (!_targetCheckMs)
    {
        _targetCheckMs = TARGET_CHECK_MS;
        if (sProgressionMgr.IsEnabled() && !_patchesResolved)
            ResolvePatches();
        UpdateTargetPatch();
        AdvanceOnlineBots();
    }

    CreateCharacters();
    LogOutExtraBots();
    LogInBots(diff);
}

void BotPopulation::CreateCharacters()
{
    // Enough bots that can be online for the patch the population follows; more characters are made when
    // races or classes that don't exist yet leave too few (the others wait for later patches).
    uint32 target = GetTargetPerFaction();
    std::array<uint32, FACTION_COUNT> eligible = { };
    for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
        for (ObjectGuid guid : _bots[faction])
            eligible[faction] += IsEligible(guid);

    for (uint32 i = 0; i < CREATIONS_PER_TICK && !_outOfNames; ++i)
    {
        uint32 alliance = eligible[FACTION_ALLIANCE] + _pendingCreations[FACTION_ALLIANCE];
        uint32 horde = eligible[FACTION_HORDE] + _pendingCreations[FACTION_HORDE];
        if (alliance >= target && horde >= target)
            break;

        // Always the faction that has fewer, so the two stay level while the population grows.
        Faction faction = (alliance <= horde && alliance < target) || horde >= target ? FACTION_ALLIANCE : FACTION_HORDE;
        if (!CreateCharacter(faction))
            break;
    }

    // Creation is done once nothing is pending: the sessions it used can go.
    if (_pendingCreations[FACTION_ALLIANCE] == 0 && _pendingCreations[FACTION_HORDE] == 0 && !_creationSessions.empty())
        _creationSessions.clear();
}

bool BotPopulation::CreateCharacter(Faction faction)
{
    if (_census[faction].empty())
        return false;

    WorldPackets::Character::CharacterCreateInfo info;

    // Race and class by the census weights of the faction, among those that exist in the patch the
    // population follows (the weights of the others are left out, so the rest keep their proportions).
    auto exists = [this](CensusEntry const& entry)
    {
        if (!_targetPatch)
            return true;
        ProgressionPatch const* start = sProgressionMgr.GetStartPatch(entry.Race, entry.Class);
        return start && start->Id <= _targetPatch;
    };

    uint64 total = 0;
    for (CensusEntry const& entry : _census[faction])
        if (exists(entry))
            total += entry.Weight;
    if (!total)
        return false;

    uint64 roll = urand(0, uint32(total - 1));
    for (CensusEntry const& entry : _census[faction])
    {
        if (!exists(entry))
            continue;

        if (roll < entry.Weight)
        {
            info.Race = entry.Race;
            info.Class = entry.Class;
            break;
        }

        roll -= entry.Weight;
    }

    info.Sex = urand(0, 1) ? GENDER_FEMALE : GENDER_MALE;
    info.Name = TakeName(info.Sex);
    if (info.Name.empty())
    {
        TC_LOG_ERROR("module", "mod-bots: no unused names left in bot_names; add more to create more bots.");
        _outOfNames = true;
        return false;
    }

    RandomizeAppearance(info);

    BotAccount* account = GetAccountWithRoom();
    if (!account)
        return false;

    WorldSession* session = GetCreationSession(*account);

    std::shared_ptr<Player> player(new Player(session), [](Player* ptr)
    {
        ptr->CleanupsBeforeDelete();
        delete ptr;
    });
    player->GetMotionMaster()->Initialize();
    if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
    {
        TC_LOG_ERROR("module", "mod-bots: couldn't create {} (race {}, class {}).", info.Name, info.Race, info.Class);
        return true; // The name is used up; try the next one
    }

    player->setCinematic(1); // Bots don't watch the intro
    player->SetAtLoginFlag(AT_LOGIN_FIRST);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    player->SaveToDB(trans, true);
    trans->Append(Trinity::StringFormat("INSERT INTO `bot_characters` (`guid`, `account`) VALUES ({}, {})",
        player->GetGUID().GetCounter(), account->Id).c_str());

    ++account->Characters;
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_REP_REALM_CHARACTERS);
    stmt->setUInt32(0, account->Characters);
    stmt->setUInt32(1, account->Id);
    stmt->setUInt32(2, realm.Id.Realm);
    LoginDatabase.Execute(stmt);

    ++_pendingCreations[faction];
    uint32 accountId = account->Id;
    _saveCallbacks.AddCallback(CharacterDatabase.AsyncCommitTransaction(trans)).AfterComplete(
        [this, player, faction, accountId](bool success)
    {
        --_pendingCreations[faction];
        if (!success)
        {
            TC_LOG_ERROR("module", "mod-bots: saving the new bot {} failed.", player->GetName());
            return;
        }

        // Only now can it log in: the character is in the database.
        sScriptMgr->OnPlayerCreate(player.get());
        sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(), player->GetNativeGender(),
            player->GetRace(), player->GetClass(), player->GetLevel());
        _bots[faction].push_back(player->GetGUID());
        _factionOf[player->GetGUID()] = faction;

        BotInfo& botInfo = _info[player->GetGUID()];
        botInfo.Race = player->GetRace();
        botInfo.Class = player->GetClass();
        if (ProgressionPatch const* start = sProgressionMgr.GetStartPatch(botInfo.Race, botInfo.Class))
            botInfo.Patch = start->Id;
    });

    return true;
}

std::string BotPopulation::TakeName(uint8 gender)
{
    // From the back, where removing a name moves the fewest others. The list is shuffled at load.
    for (std::size_t i = _names.size(); i > 0; --i)
    {
        if (_names[i - 1].Gender != gender && _names[i - 1].Gender != 2)
            continue;

        std::string name = std::move(_names[i - 1].Name);
        _names.erase(_names.begin() + (i - 1));

        // Taken by a player since startup, or refused by the server's name rules: try the next one.
        if (sCharacterCache->GetCharacterCacheByName(name) || sObjectMgr->IsReservedName(name) ||
            ObjectMgr::CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
            continue;

        return name;
    }

    return {};
}

BotPopulation::BotAccount* BotPopulation::GetAccountWithRoom()
{
    for (BotAccount& account : _accounts)
        if (account.Characters < _charactersPerAccount)
            return &account;

    // A new account: BOT0001, BOT0002, ... with a password nobody knows.
    for (uint32 number = 1; number < 100000; ++number)
    {
        std::string username = Trinity::StringFormat("BOT{:04}", number);
        if (AccountMgr::GetId(username))
            continue;

        if (sAccountMgr->CreateAccount(username, RandomPassword()) != AccountOpResult::AOR_OK)
        {
            TC_LOG_ERROR("module", "mod-bots: couldn't create the bot account {}.", username);
            return nullptr;
        }

        BotAccount account;
        account.Id = AccountMgr::GetId(username);
        account.Username = username;
        CharacterDatabase.DirectExecute(Trinity::StringFormat("INSERT INTO `bot_accounts` (`id`) VALUES ({})", account.Id).c_str());
        TC_LOG_INFO("module", "mod-bots: created the bot account {}.", username);

        _accounts.push_back(std::move(account));
        return &_accounts.back();
    }

    return nullptr;
}

WorldSession* BotPopulation::GetCreationSession(BotAccount const& account)
{
    std::unique_ptr<WorldSession>& session = _creationSessions[account.Id];
    if (!session)
    {
        std::string username = account.Username;
        session = std::make_unique<WorldSession>(account.Id, std::move(username), nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), Minutes(0), LOCALE_enUS, 0, false);
        // Marks it as a bot session: it isn't updated and sends nowhere.
        session->SetBotPacketHandler([](WorldPacket const&) { });
    }

    return session.get();
}

void BotPopulation::LogInBots(uint32 diff)
{
    std::array<uint32, FACTION_COUNT> online = { };
    for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
        for (ObjectGuid guid : _bots[faction])
            if (sBotMgr.Find(guid) && IsEligible(guid))
                ++online[faction];

    uint32 target = GetTargetPerFaction();
    if (online[FACTION_ALLIANCE] >= target && online[FACTION_HORDE] >= target)
    {
        _loginCredit = 0.0f;
        return;
    }

    _loginCredit = std::min(_loginCredit + diff * _loginsPerSecond / 1000.0f, float(_loginsPerSecond));
    while (_loginCredit >= 1.0f)
    {
        // The faction with fewer bots online, so the world stays 50/50 while they log in.
        Faction faction = online[FACTION_ALLIANCE] <= online[FACTION_HORDE] ? FACTION_ALLIANCE : FACTION_HORDE;
        if (online[faction] >= target)
            faction = faction == FACTION_ALLIANCE ? FACTION_HORDE : FACTION_ALLIANCE;
        if (online[faction] >= target)
            break;

        // A random bot of that faction that isn't online yet.
        std::vector<ObjectGuid> const& bots = _bots[faction];
        bool loggedIn = false;
        std::size_t start = bots.empty() ? 0 : urand(0, bots.size() - 1);
        for (std::size_t i = 0; i < bots.size() && !loggedIn; ++i)
        {
            ObjectGuid guid = bots[(start + i) % bots.size()];
            if (sBotMgr.Find(guid) || !IsEligible(guid))
                continue;

            if (sBotMgr.Add(guid, 0).empty())
            {
                _loggedIn.insert(guid);
                loggedIn = true;
            }
        }

        if (!loggedIn)
            break; // Every bot of the faction is online or can't log in; the rest are still being created

        ++online[faction];
        _loginCredit -= 1.0f;
    }
}

void BotPopulation::LogOutExtraBots()
{
    uint32 target = GetTargetPerFaction();
    for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
    {
        uint32 online = 0;
        for (ObjectGuid guid : _bots[faction])
        {
            Bot* bot = sBotMgr.Find(guid);
            if (!bot || bot->IsRemovalRequested() || !_loggedIn.count(guid))
                continue;

            // Not for this patch (a later one, or a race or class that doesn't exist yet), or too many.
            if (!IsEligible(guid) || ++online > target)
            {
                bot->RequestRemoval();
                _loggedIn.erase(guid);
            }
        }
    }
}

std::vector<std::string> BotPopulation::Describe() const
{
    std::array<uint32, FACTION_COUNT> online = { };
    std::array<uint32, FACTION_COUNT> loggingIn = { };
    for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
    {
        for (ObjectGuid guid : _bots[faction])
        {
            if (Bot* bot = sBotMgr.Find(guid))
            {
                if (bot->GetState() == Bot::State::InWorld)
                    ++online[faction];
                else
                    ++loggingIn[faction];
            }
        }
    }

    // What the questing bots are doing.
    std::array<uint32, size_t(BotQuester::Activity::Count)> activities = { };
    for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
        for (ObjectGuid guid : _bots[faction])
            if (Bot* bot = sBotMgr.Find(guid); bot && bot->GetQuester())
                ++activities[size_t(bot->GetQuester()->GetActivity())];

    // Where they are levelling: the busiest planned zones.
    std::unordered_map<uint32, uint32> zones;
    for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
        for (ObjectGuid guid : _bots[faction])
            if (Bot* bot = sBotMgr.Find(guid); bot && bot->GetQuester() && bot->GetQuester()->GetZoneId())
                ++zones[bot->GetQuester()->GetZoneId()];

    std::vector<std::pair<uint32, uint32>> busiest(zones.begin(), zones.end());
    std::sort(busiest.begin(), busiest.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    std::string zoneText;
    for (std::size_t i = 0; i < busiest.size() && i < 8; ++i)
        zoneText += Trinity::StringFormat("{}{} {}", zoneText.empty() ? "" : ", ", BotQuestData::GetZoneName(busiest[i].first),
            busiest[i].second);

    std::string doing;
    for (size_t i = 0; i < activities.size(); ++i)
        if (activities[i])
            doing += Trinity::StringFormat("{}{} {}", doing.empty() ? "" : ", ", activities[i],
                BotQuester::GetActivityName(BotQuester::Activity(i)));

    uint32 maxTick = _tickTimes.empty() ? 0 : *std::max_element(_tickTimes.begin(), _tickTimes.end());
    uint32 averageTick = _tickTimes.empty() ? 0 : uint32(_tickTimeTotal / _tickTimes.size());

    std::vector<std::string> lines;
    lines.push_back(Trinity::StringFormat("Bot population: target {} ({} per faction), {} bot accounts, {} unused names.",
        GetTargetPerFaction() * 2, GetTargetPerFaction(), _accounts.size(), _names.size()));
    if (ProgressionPatch const* patch = _targetPatch ? sProgressionMgr.GetPatch(_targetPatch) : nullptr)
    {
        uint32 eligible = 0;
        for (uint8 faction = 0; faction < FACTION_COUNT; ++faction)
            for (ObjectGuid guid : _bots[faction])
                eligible += IsEligible(guid);
        lines.push_back(Trinity::StringFormat("Following patch {} (the online players'): {} characters can be online in it.",
            patch->Title(), eligible));
    }
    lines.push_back(Trinity::StringFormat("Created: {} Alliance, {} Horde ({} being saved).", _bots[FACTION_ALLIANCE].size(),
        _bots[FACTION_HORDE].size(), _pendingCreations[FACTION_ALLIANCE] + _pendingCreations[FACTION_HORDE]));
    lines.push_back(Trinity::StringFormat("Online: {} Alliance, {} Horde; logging in: {}.", online[FACTION_ALLIANCE],
        online[FACTION_HORDE], loggingIn[FACTION_ALLIANCE] + loggingIn[FACTION_HORDE]));
    lines.push_back(Trinity::StringFormat("World tick over the last minute: average {} ms, worst {} ms ({} ticks).",
        averageTick, maxTick, _tickTimes.size()));
    if (!doing.empty())
        lines.push_back("Doing: " + doing + ".");
    if (!zoneText.empty())
        lines.push_back(Trinity::StringFormat("Levelling in ({} zones): {}.", busiest.size(), zoneText));
    return lines;
}
