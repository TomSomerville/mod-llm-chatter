#ifndef MOD_LLM_CHATTER_TRADE_H
#define MOD_LLM_CHATTER_TRADE_H

#include "Define.h"
#include <string>

class Player;

// Scripted item handover attached to a delivered chat
// line. Format: "TRADE|Item Name|count". The LLM only
// decides that a handover was agreed; everything from
// here on is deterministic game scripting (open the
// window, wait for the player's client to show it, slot
// the stacks, pre-accept). See LLMChatterTrade.cpp.
void ExecuteLLMChatterTradeAction(
    Player* bot,
    Player* player,
    std::string const& action,
    uint64 deliverAtEpoch);

void AddLLMChatterTradeScripts();

#endif
