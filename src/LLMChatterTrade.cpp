/*
 * mod-llm-chatter - scripted item handover
 *
 * The LLM only decides *that* a handover was agreed in
 * chat and emits a machine-readable action on the
 * delivered line ("TRADE|Item Name|count"). Everything
 * after that is deterministic game scripting done here:
 *
 *  1. validate: the bot may trade with this player
 *     (master or group member, otherwise the stock
 *     playerbots TradeStatusAction cancels), both are in
 *     trade range, neither side is trading with someone
 *     else; resolve the item stacks to hand over;
 *  2. open the trade window from the bot's side
 *     (CMSG_INITIATE_TRADE);
 *  3. wait until the player's client has actually opened
 *     the window. The core sends SMSG_TRADE_STATUS /
 *     TRADE_STATUS_OPEN_WINDOW to the bot only after the
 *     client answered with CMSG_BEGIN_TRADE. Items placed
 *     before that round trip exist server-side but the
 *     client never shows them, which was the "window
 *     opens but stays empty" symptom;
 *  4. place the stacks (CMSG_SET_TRADE_ITEM) and accept
 *     from the bot's side so the player only has to click
 *     Trade;
 *  5. keep the bot's acceptance current until the trade
 *     completes or is cancelled. This sidesteps the stock
 *     playerbots price haggling, which would otherwise
 *     make a random bot demand gold for an item it just
 *     agreed to give away.
 */

#include "LLMChatterTrade.h"
#include "LLMChatterShared.h"

#include "Bag.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
// Core TRADE_DISTANCE.
constexpr float kTradeDistance = 11.0f;
// How often the handover state machine looks at the
// trade while it is in flight.
constexpr uint32 kTickMs = 100;
// Give up (cancel + apologise) if the player's client
// never opened the window.
constexpr time_t kWindowOpenTimeoutSec = 12;
// Stop scripting (but leave the window alone) if the
// player sits on an open trade this long.
constexpr time_t kLifetimeSec = 120;
// Ignore actions attached to lines delivered long ago.
constexpr time_t kStaleActionSec = 120;
// Throttle for re-accepting after the player's own
// changes reset the bot's acceptance.
constexpr time_t kAcceptRetrySec = 1;

struct PendingHandover
{
    uint32 serial = 0;
    ObjectGuid playerGuid;
    std::vector<ObjectGuid> itemGuids;
    std::string itemName;
    time_t startedAt = 0;
    time_t lastAcceptAt = 0;
    // Player's client acknowledged the window
    // (bot received TRADE_STATUS_OPEN_WINDOW).
    bool windowOpen = false;
    // Stacks were placed after the window opened.
    bool slotted = false;
};

std::mutex g_pendingMutex;
// Keyed by bot GUID: a bot runs at most one handover.
std::map<ObjectGuid, PendingHandover> g_pending;
uint32 g_nextSerial = 1;

void ForgetHandover(ObjectGuid botGuid, uint32 serial)
{
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    auto it = g_pending.find(botGuid);
    if (it != g_pending.end()
        && (!serial || it->second.serial == serial))
        g_pending.erase(it);
}

bool SnapshotHandover(
    ObjectGuid botGuid, uint32 serial,
    PendingHandover& out)
{
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    auto it = g_pending.find(botGuid);
    if (it == g_pending.end()
        || it->second.serial != serial)
        return false;
    out = it->second;
    return true;
}

void MarkSlotted(ObjectGuid botGuid, uint32 serial)
{
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    auto it = g_pending.find(botGuid);
    if (it != g_pending.end()
        && it->second.serial == serial)
        it->second.slotted = true;
}

void MarkAccepted(
    ObjectGuid botGuid, uint32 serial, time_t when)
{
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    auto it = g_pending.find(botGuid);
    if (it != g_pending.end()
        && it->second.serial == serial)
        it->second.lastAcceptAt = when;
}

void CancelBotTrade(Player* bot)
{
    if (!bot->GetTradeData())
        return;
    WorldPacket packet;
    bot->GetSession()->HandleCancelTradeOpcode(packet);
}

void WhisperPlayer(
    Player* bot, Player* player, char const* text)
{
    if (bot && player && player->IsInWorld())
        bot->Whisper(text, LANG_UNIVERSAL, player);
}

bool NameEqualsNoCase(
    std::string const& left, std::string const& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (std::tolower(
                static_cast<unsigned char>(left[i]))
            != std::tolower(
                static_cast<unsigned char>(right[i])))
            return false;
    }
    return true;
}

// Place the pending stacks into free trade slots.
// Returns how many of them are in the window afterward.
uint8 PlacePendingStacks(
    Player* bot, std::vector<ObjectGuid> const& itemGuids)
{
    uint8 placed = 0;
    for (ObjectGuid guid : itemGuids)
    {
        TradeData* trade = bot->GetTradeData();
        if (!trade)
            break;

        if (trade->HasItem(guid))
        {
            ++placed; // already in (merged request)
            continue;
        }

        Item* item = bot->GetItemByGuid(guid);
        if (!item || !item->CanBeTraded(false, true))
            continue;

        int8 freeSlot = -1;
        for (uint8 slot = 0;
             slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        {
            if (!trade->GetItem(TradeSlots(slot)))
            {
                freeSlot = static_cast<int8>(slot);
                break;
            }
        }
        if (freeSlot < 0)
            break;

        // Same packet the client (and playerbots'
        // TradeAction) sends when dragging an item in.
        WorldPacket packet(CMSG_SET_TRADE_ITEM, 3);
        packet << static_cast<uint8>(freeSlot);
        packet << static_cast<uint8>(item->GetBagSlot());
        packet << static_cast<uint8>(item->GetSlot());
        bot->GetSession()->HandleSetTradeItemOpcode(packet);

        trade = bot->GetTradeData();
        if (trade
            && trade->GetItem(TradeSlots(freeSlot)) == item)
            ++placed;
    }
    return placed;
}

// Drives one handover from "window requested" to
// "trade finished". Runs on the bot's event queue, so
// it executes in the bot's own update like the bot's
// other actions do.
class HandoverTickEvent : public BasicEvent
{
public:
    HandoverTickEvent(ObjectGuid botGuid, uint32 serial)
        : _botGuid(botGuid)
        , _serial(serial)
    {
    }

    bool Execute(uint64 /*time*/,
                 uint32 /*diff*/) override
    {
        Player* bot =
            ObjectAccessor::FindPlayer(_botGuid);
        if (!bot || !bot->IsInWorld()
            || !bot->GetSession())
        {
            ForgetHandover(_botGuid, _serial);
            return true;
        }

        PendingHandover pending;
        if (!SnapshotHandover(_botGuid, _serial, pending))
            return true; // superseded or finished

        Player* player =
            ObjectAccessor::FindPlayer(pending.playerGuid);
        TradeData* trade = bot->GetTradeData();
        if (!player || !trade
            || bot->GetTrader() != player)
        {
            // Trade completed or was cancelled by either
            // side (or the player left). Nothing left to
            // script.
            LOG_DEBUG("module",
                "llm-chatter trade: {} -> {}: trade "
                "closed ({})",
                bot->GetName(),
                player ? player->GetName() : "?",
                pending.slotted ? "after slotting"
                                : "before slotting");
            ForgetHandover(_botGuid, _serial);
            return true;
        }

        time_t now = time(nullptr);
        if (now - pending.startedAt > kLifetimeSec)
        {
            // Player is sitting on the window; leave it
            // to them.
            ForgetHandover(_botGuid, _serial);
            return true;
        }

        if (!pending.slotted)
        {
            if (!pending.windowOpen)
            {
                if (now - pending.startedAt
                    > kWindowOpenTimeoutSec)
                {
                    CancelBotTrade(bot);
                    WhisperPlayer(bot, player,
                        "The trade window never opened "
                        "on your side - try again when "
                        "you're free.");
                    ForgetHandover(_botGuid, _serial);
                    return true;
                }
                Reschedule(bot);
                return true;
            }

            uint8 placed =
                PlacePendingStacks(bot, pending.itemGuids);
            if (!placed)
            {
                CancelBotTrade(bot);
                WhisperPlayer(bot, player,
                    "Hm, I couldn't put it in the "
                    "window - ask me again in a moment.");
                LOG_INFO("module",
                    "llm-chatter trade: {} -> {}: could "
                    "not place any stack of '{}'",
                    bot->GetName(), player->GetName(),
                    pending.itemName);
                ForgetHandover(_botGuid, _serial);
                return true;
            }

            MarkSlotted(_botGuid, _serial);
            pending.slotted = true;
            pending.lastAcceptAt = 0;
            LOG_INFO("module",
                "llm-chatter trade: {} -> {}: placed {} "
                "stack(s) of '{}'",
                bot->GetName(), player->GetName(),
                placed, pending.itemName);
        }

        // Accept from the bot's side so the player only
        // has to click Trade. If the player changes their
        // own side the core resets both acceptances, so
        // re-accept (throttled) until the trade ends.
        trade = bot->GetTradeData();
        if (trade && !trade->IsAccepted()
            && now - pending.lastAcceptAt
                >= kAcceptRetrySec)
        {
            WorldPacket packet;
            bot->GetSession()
                ->HandleAcceptTradeOpcode(packet);
            MarkAccepted(_botGuid, _serial, now);
        }

        if (bot->GetTradeData())
            Reschedule(bot);
        else
            ForgetHandover(_botGuid, _serial);
        return true;
    }

private:
    void Reschedule(Player* bot)
    {
        bot->m_Events.AddEvent(
            new HandoverTickEvent(_botGuid, _serial),
            bot->m_Events.CalculateTime(kTickMs));
    }

    ObjectGuid _botGuid;
    uint32 _serial;
};

// Sees every packet the core sends to any player. We
// only care about SMSG_TRADE_STATUS / OPEN_WINDOW
// reaching a bot with a handover in flight: that is the
// moment the player's client has the window up and will
// display whatever the bot places.
class LLMChatterTradeScript : public PlayerbotScript
{
public:
    LLMChatterTradeScript()
        : PlayerbotScript("LLMChatterTradeScript")
    {
    }

    void OnPlayerbotPacketSent(
        Player* player, WorldPacket const* packet) override
    {
        if (!player || !packet
            || packet->GetOpcode() != SMSG_TRADE_STATUS
            || packet->size() < sizeof(uint32))
            return;

        if (packet->read<uint32>(0)
            != TRADE_STATUS_OPEN_WINDOW)
            return;

        std::lock_guard<std::mutex> lock(g_pendingMutex);
        auto it = g_pending.find(player->GetGUID());
        if (it != g_pending.end())
            it->second.windowOpen = true;
    }
};
} // namespace

void ExecuteLLMChatterTradeAction(
    Player* bot,
    Player* player,
    std::string const& action,
    uint64 deliverAtEpoch)
{
    if (!bot || !bot->IsInWorld() || !bot->GetSession())
        return;

    if (action.rfind("TRADE|", 0) != 0)
        return;

    size_t sep = action.rfind('|');
    if (sep <= 5 || sep + 1 >= action.size())
        return;

    std::string itemName = action.substr(6, sep - 6);
    uint32 requested = static_cast<uint32>(
        std::strtoul(
            action.c_str() + sep + 1, nullptr, 10));
    if (itemName.empty() || !requested)
        return;

    if (deliverAtEpoch
        && time(nullptr)
            > static_cast<time_t>(deliverAtEpoch)
                + kStaleActionSec)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (!player || !player->IsInWorld()
        || !player->GetSession()
        || player->GetMapId() != bot->GetMapId())
        return;

    // Stock playerbots (TradeStatusAction) cancels trades
    // with real players who are neither the bot's master
    // nor in its group. Say so instead of failing
    // silently.
    bool allowed = botAI->GetMaster() == player
        || (bot->GetGroup()
            && bot->GetGroup()->IsMember(
                player->GetGUID()));
    if (!allowed)
    {
        WhisperPlayer(bot, player,
            "Invite me to your group first and "
            "I'll hand it over.");
        return;
    }

    // Same gate TradeStatusAction applies to random bots.
    if (sPlayerbotAIConfig.enableRandomBotTrading == 0
        && (sRandomPlayerbotMgr.IsRandomBot(bot)
            || sRandomPlayerbotMgr.IsAddclassBot(bot)))
    {
        WhisperPlayer(bot, player,
            "Trading is switched off for bots like "
            "me, sorry.");
        return;
    }

    if (!bot->IsWithinDistInMap(
            player, kTradeDistance, false))
    {
        WhisperPlayer(bot, player,
            "Come closer and I'll trade you.");
        return;
    }

    if ((bot->GetTradeData()
            && bot->GetTrader() != player)
        || (player->GetTradeData()
            && player->GetTrader() != bot))
    {
        WhisperPlayer(bot, player,
            "Finish your current trade first.");
        return;
    }

    // Locate matching stacks in the same containers the
    // bot_facts sheet was built from (backpack + bags),
    // matching on ItemTemplate Name1 like the sheet.
    std::vector<Item*> stacks;
    uint32 available = 0;
    auto consider = [&](Item* item)
    {
        if (!item || !item->CanBeTraded(false, true))
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto
            || !NameEqualsNoCase(proto->Name1, itemName))
            return;
        stacks.push_back(item);
        available += item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START;
         slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(
            INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START;
         bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0;
                 slot < bag->GetBagSize(); ++slot)
                consider(bot->GetItemByPos(
                    bagSlot, slot));
    }

    if (stacks.empty())
    {
        WhisperPlayer(bot, player,
            "Looks like I don't have that any more, "
            "sorry.");
        return;
    }

    // Whole stacks only: cover the requested count with
    // the fewest stacks (largest first), max 6 slots.
    if (requested > available)
        requested = available;
    std::sort(
        stacks.begin(), stacks.end(),
        [](Item* left, Item* right)
        {
            return left->GetCount() > right->GetCount();
        });
    std::vector<ObjectGuid> itemGuids;
    uint32 covered = 0;
    for (Item* item : stacks)
    {
        if (covered >= requested
            || itemGuids.size() >= TRADE_SLOT_TRADED_COUNT)
            break;
        covered += item->GetCount();
        itemGuids.push_back(item->GetGUID());
    }
    if (itemGuids.empty())
        return;

    bool alreadyOpen = bot->GetTradeData()
        && bot->GetTrader() == player;

    uint32 serial = 0;
    bool startTick = true;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        PendingHandover& pending = g_pending[bot->GetGUID()];

        // A second agreed item while a handover with the
        // same player is still in flight: add it to the
        // same window instead of restarting.
        bool merge = alreadyOpen && pending.serial
            && pending.playerGuid == player->GetGUID();
        if (!merge)
        {
            pending = PendingHandover();
            pending.serial = g_nextSerial++;
            pending.playerGuid = player->GetGUID();
            pending.startedAt = time(nullptr);
            // A window that already exists was opened by
            // the player from their own client.
            pending.windowOpen = alreadyOpen;
        }
        else
            startTick = false;

        for (ObjectGuid guid : itemGuids)
            if (std::find(pending.itemGuids.begin(),
                          pending.itemGuids.end(), guid)
                == pending.itemGuids.end())
                pending.itemGuids.push_back(guid);
        pending.itemName = itemName;
        pending.slotted = false;
        serial = pending.serial;
    }

    if (!alreadyOpen)
    {
        if (IsSafeForChatterFacing(bot))
            bot->SetFacingToObject(player);

        // Same packet route playerbots' TradeAction uses
        // when a bot opens a trade with its master.
        WorldPacket packet(CMSG_INITIATE_TRADE);
        packet << player->GetGUID();
        bot->GetSession()->HandleInitiateTradeOpcode(packet);

        if (!bot->GetTradeData()
            || bot->GetTrader() != player)
        {
            ForgetHandover(bot->GetGUID(), serial);
            WhisperPlayer(bot, player,
                "I couldn't open a trade with you - "
                "are you busy, or blocking trades?");
            LOG_INFO("module",
                "llm-chatter trade: {} -> {}: initiate "
                "refused by core",
                bot->GetName(), player->GetName());
            return;
        }
    }

    LOG_INFO("module",
        "llm-chatter trade: {} -> {}: '{}' x{} as {} "
        "stack(s), window {}",
        bot->GetName(), player->GetName(), itemName,
        requested, itemGuids.size(),
        alreadyOpen ? "already open" : "requested");

    if (startTick)
        bot->m_Events.AddEvent(
            new HandoverTickEvent(bot->GetGUID(), serial),
            bot->m_Events.CalculateTime(kTickMs));
}

void AddLLMChatterTradeScripts()
{
    new LLMChatterTradeScript();
}
