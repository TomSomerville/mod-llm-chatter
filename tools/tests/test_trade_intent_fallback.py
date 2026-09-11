#!/usr/bin/env python3
"""Regression checks for the marker-less trade fallback."""

import importlib
import sys
import types
from pathlib import Path


def _ensure_module(name):
    module = sys.modules.get(name)
    if module is None:
        module = types.ModuleType(name)
        sys.modules[name] = module
    return module


for dependency in ('anthropic',):
    try:
        importlib.import_module(dependency)
    except ModuleNotFoundError:
        module = _ensure_module(dependency)
        setattr(module, 'Anthropic', type('Anthropic', (), {}))

try:
    importlib.import_module('mysql.connector')
except ModuleNotFoundError:
    mysql_module = _ensure_module('mysql')
    connector_module = _ensure_module('mysql.connector')
    setattr(mysql_module, 'connector', connector_module)

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from chatter_shared import (  # noqa: E402
    _looks_like_handover_reply,
    extract_trade_action,
    infer_trade_action_via_llm,
    should_infer_trade_action,
)

FACTS = {
    'inventory': [
        {'name': 'Rage Potion', 'count': 19},
        {'name': 'Linen Cloth', 'count': 2},
        {'name': 'Skinning Knife', 'count': 1},
    ],
}
HISTORY = (
    "Beacher: how about linen cloth? got any of that?\n"
    "Darah: Yeah, I've got two pieces. Want them both?"
)


class _FakeClient:
    """Minimal stand-in for the Anthropic client."""

    def __init__(self, answer):
        self.answer = answer
        self.calls = []
        outer = self

        class _Messages:
            def create(self, **kwargs):
                outer.calls.append(kwargs)
                text = outer.answer
                return types.SimpleNamespace(
                    content=[types.SimpleNamespace(text=text)]
                )

        self.messages = _Messages()


CONFIG = {'LLMChatter.GroupChatter.TradeIntentFallback': 1}


def test_handover_reply_detection():
    assert _looks_like_handover_reply("Cool, there you go.")
    assert _looks_like_handover_reply("Sure thing, here ya go!")
    assert _looks_like_handover_reply("*hands over two pieces* enjoy")
    assert not _looks_like_handover_reply(
        "Yeah, I've got two pieces. Want them both or just one?"
    )
    assert not _looks_like_handover_reply("")


def test_marker_still_wins_over_fallback():
    text, action = extract_trade_action(
        "Here you go! <<TRADE|Linen Cloth|2>>",
        player_message="yes please",
    )
    assert action == "TRADE|Linen Cloth|2"
    assert text == "Here you go!"


def test_should_infer_needs_item_context():
    # Plain politeness with no item discussed: no call.
    assert not should_infer_trade_action(
        "please heal me", "on it", FACTS, ""
    )
    # Confirmation after an offer about an inventory item.
    assert should_infer_trade_action(
        "yes please", "Cool, there you go.", FACTS, HISTORY
    )
    # Plural last-word match ("potions" -> Rage Potion).
    assert should_infer_trade_action(
        "ill take them", "there you go", FACTS,
        "Beacher: got any potions?"
    )
    # Availability question is never a handover.
    assert not should_infer_trade_action(
        "got any linen cloth?", "Yeah, two pieces. Want them?",
        FACTS, HISTORY
    )
    # No inventory sheet: nothing to validate against.
    assert not should_infer_trade_action(
        "yes please", "there you go", None, HISTORY
    )


def test_infer_canonicalizes_and_clamps():
    client = _FakeClient('{"item": "linen cloth", "count": 5}')
    action = infer_trade_action_via_llm(
        client, CONFIG, 'Darah', FACTS, HISTORY,
        'Beacher', 'Ill take them yes',
        'Cool, there you go.',
    )
    assert action == "TRADE|Linen Cloth|2"
    assert len(client.calls) == 1
    prompt = client.calls[0]['messages'][0]['content']
    assert 'Linen Cloth x2' in prompt
    assert 'Ill take them yes' in prompt


def test_infer_rejects_unknown_item_and_null():
    client = _FakeClient('{"item": "Frostmourne", "count": 1}')
    assert infer_trade_action_via_llm(
        client, CONFIG, 'Darah', FACTS, HISTORY,
        'Beacher', 'yes please', 'there you go',
    ) is None
    client = _FakeClient('{"item": null}')
    assert infer_trade_action_via_llm(
        client, CONFIG, 'Darah', FACTS, HISTORY,
        'Beacher', 'yes please', 'want them?',
    ) is None
    client = _FakeClient('not json at all')
    assert infer_trade_action_via_llm(
        client, CONFIG, 'Darah', FACTS, HISTORY,
        'Beacher', 'yes please', 'there you go',
    ) is None


def test_infer_respects_config_switch():
    client = _FakeClient('{"item": "Linen Cloth", "count": 1}')
    assert infer_trade_action_via_llm(
        client,
        {'LLMChatter.GroupChatter.TradeIntentFallback': 0},
        'Darah', FACTS, HISTORY, 'Beacher', 'yes please',
        'there you go',
    ) is None
    assert client.calls == []


if __name__ == '__main__':
    names = [n for n in dir() if n.startswith('test_')]
    for name in names:
        globals()[name]()
        print('ok', name)
