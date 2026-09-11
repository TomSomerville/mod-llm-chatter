/*
 * mod-llm-chatter - playerbot command relay
 *
 * Stock mod-playerbots hands every party, whisper, guild
 * and channel line to its bots as a potential command and
 * matches progressively shorter prefixes of it, so a
 * conversational "trade me the potion" runs the "trade"
 * command (instantly opening a trade window) while the
 * LLM is still composing its answer.
 *
 * mod-playerbots stays untouched. Instead:
 *
 *  - AiPlayerbot.CommandPrefix is set (e.g. "~") in
 *    playerbots.conf, so playerbots' own chat hooks ignore
 *    anything that does not start with the prefix;
 *  - this script mirrors playerbots' chat dispatch, but only
 *    for lines the fork's classifier already treats as bot
 *    commands (IsLikelyPlayerbotControlCommand, the same
 *    test that keeps them out of the LLM queue), and hands
 *    them over with the prefix prepended.
 *
 * Net effect: typed commands ("follow", "stay", "@tank
 * attack") keep working exactly as before, an explicit
 * "~command" always works, and plain speech never reaches
 * playerbots' command parser. With an empty
 * AiPlayerbot.CommandPrefix this script does nothing and
 * stock behaviour applies.
 */

#include "LLMChatterGroupInternal.h"
#include "LLMChatterShared.h"

#include "Channel.h"
#include "Group.h"
#include "Guild.h"
#include "Language.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <string>

void AddLLMChatterCommandRelayScripts();

namespace
{
// Returns the prefixed command text to relay, or empty
// when the line must not be relayed: relay disabled (no
// prefix configured), hidden addon traffic, the player
// already typed the prefix (playerbots handles it), or
// the line is speech rather than a command.
std::string RelayText(
    uint32 lang, std::string const& msg,
    bool requireCommandShape)
{
    std::string const& prefix =
        sPlayerbotAIConfig.commandPrefix;
    if (prefix.empty() || msg.empty()
        || lang == LANG_ADDON)
        return std::string();

    if (msg.rfind(prefix, 0) == 0)
        return std::string();

    if (requireCommandShape
        && !IsLikelyPlayerbotControlCommand(msg))
        return std::string();

    return prefix + msg;
}

void RelayToBot(
    Player* bot, Player* from, uint32 type,
    std::string const& text)
{
    if (!bot)
        return;
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        botAI->HandleCommand(type, text, from);
}

class LLMChatterCommandRelayScript : public PlayerScript
{
public:
    LLMChatterCommandRelayScript()
        : PlayerScript(
              "LLMChatterCommandRelayScript",
              {PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
               PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
               PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
               PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT})
    {
    }

    using PlayerScript::OnPlayerCanUseChat;

    // Whisper to a bot. Whispers are not LLM conversation
    // in this module, so every whisper to a bot is relayed
    // as a command, exactly like stock playerbots.
    bool OnPlayerCanUseChat(
        Player* player, uint32 type, uint32 lang,
        std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || !player
            || !receiver || !GET_PLAYERBOT_AI(receiver))
            return true;

        std::string text = RelayText(lang, msg, false);
        if (text.empty())
            return true;

        RelayToBot(receiver, player, type, text);

        // Same guard stock playerbots applies: the bot is
        // logging out now, do not deliver the whisper.
        if (msg == "logout")
            return false;
        return true;
    }

    // Party / raid chat: every bot in the group, but only
    // for command-shaped lines.
    bool OnPlayerCanUseChat(
        Player* player, uint32 type, uint32 lang,
        std::string& msg, Group* group) override
    {
        if (!player || !group)
            return true;

        std::string text = RelayText(lang, msg, true);
        if (text.empty())
            return true;

        for (GroupReference* itr = group->GetFirstMember();
             itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && GET_PLAYERBOT_AI(member))
                RelayToBot(member, player, type, text);
        }
        return true;
    }

    // Guild chat: the speaker's own bots in the same guild.
    bool OnPlayerCanUseChat(
        Player* player, uint32 type, uint32 lang,
        std::string& msg, Guild* /*guild*/) override
    {
        if (type != CHAT_MSG_GUILD || !player)
            return true;

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
            return true;

        std::string text = RelayText(lang, msg, true);
        if (text.empty())
            return true;

        for (auto it = mgr->GetPlayerBotsBegin();
             it != mgr->GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (bot && bot->GetGuildId() == player->GetGuildId())
                RelayToBot(bot, player, type, text);
        }
        return true;
    }

    // Channel chat: the speaker's own bots on general-type
    // channels, plus every random bot (stock playerbots
    // lets anyone use unsecured commands such as "invite"
    // or "wts" there).
    bool OnPlayerCanUseChat(
        Player* player, uint32 type, uint32 lang,
        std::string& msg, Channel* channel) override
    {
        if (!player || !channel)
            return true;

        std::string text = RelayText(lang, msg, true);
        if (text.empty())
            return true;

        if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player))
            if (channel->GetFlags() & 0x18)
                mgr->HandleCommand(type, text);

        sRandomPlayerbotMgr.HandleCommand(
            type, text, player);
        return true;
    }
};
} // namespace

void AddLLMChatterCommandRelayScripts()
{
    new LLMChatterCommandRelayScript();
}
