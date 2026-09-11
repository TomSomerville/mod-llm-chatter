/*
 * mod-llm-chatter - registration coordinator
 */

#include "LLMChatterBG.h"
#include "LLMChatterGuild.h"
#include "LLMChatterGroup.h"
#include "LLMChatterRaid.h"
#include "LLMChatterShared.h"
#include "LLMChatterTrade.h"

void AddLLMChatterCommandScripts();
void AddLLMChatterCommandRelayScripts();

void AddLLMChatterScripts()
{
    AddLLMChatterWorldScripts();
    AddLLMChatterGuildScripts();
    AddLLMChatterGroupScripts();
    AddLLMChatterPlayerScripts();
    AddLLMChatterBGScripts();
    AddLLMChatterRaidScripts();
    AddLLMChatterCommandScripts();
    AddLLMChatterTradeScripts();
    AddLLMChatterCommandRelayScripts();
}
