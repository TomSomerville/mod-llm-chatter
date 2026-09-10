# mod-llm-chatter (AI modified fork)

AI-powered conversations for [AzerothCore](https://www.azerothcore.org/) playerbots — bots that talk back, remember you, and feel alive.

> **Credit:** This is a personal fork of **[mod-llm-chatter](https://github.com/Hokken/mod-llm-chatter)** by **[Hokken](https://github.com/Hokken)**, who created and maintains the original module — all of the personality, memory, and conversation systems are his work. See the [original README](https://github.com/Hokken/mod-llm-chatter#readme) for full feature documentation, and his [Chatter Companion addon](https://github.com/Hokken/Chatter-Companion) for in-game personality editing. If you like this module, star and support the original.

## What this fork changes

- **Anthropic/Claude only** — all other LLM providers (OpenAI, Google, OpenRouter, Ollama) are stripped out for a simpler, smaller module.
- **Playerbot-only speech** — NPCs never answer players; only playerbots respond and chatter.
- **No dropped messages** — if a conversation's designated responder despawned or walked out of range, a fresh nearby bot answers instead of the message vanishing.
- **Claude subscription support** — authenticate with a Claude Pro/Max OAuth token from `claude setup-token` (uses your plan's included Agent SDK credits) instead of a paid API key. Paste the `sk-ant-oat...` token where the API key goes; it's detected automatically.

## Requirements

- [AzerothCore, Playerbot branch](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot) (the standard repo will not compile with playerbots)
- [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)
- Python 3.10+
- Claude access: an Anthropic API key, or a Claude Pro/Max subscription (via `claude setup-token`)

## Install

```bash
# 1. Clone into your AzerothCore modules directory
cd azerothcore-wotlk/modules
git clone https://github.com/TomSomerville/mod-llm-chatter.git

# 2. Rebuild the server
cd ../build
cmake .. && make -j$(nproc) install

# 3. Create the config from the template (adjust path to your install)
cp <server>/etc/modules/mod_llm_chatter.conf.dist <server>/etc/modules/mod_llm_chatter.conf

# 4. Install the Python bridge dependencies
python3 -m venv ~/llm-bridge-venv
~/llm-bridge-venv/bin/pip install -r mod-llm-chatter/tools/requirements.txt
```

## Set up

**1. Pick your LLM** — edit `mod_llm_chatter.conf`:

| Option | Set |
|---|---|
| Claude subscription (this fork) | `Provider = anthropic`, `Model = claude-haiku-4-5-20251001`, `ApiKey = <sk-ant-oat token from "claude setup-token">` |
| Anthropic API key | `Provider = anthropic`, `ApiKey = sk-ant-api...` |

**2. Set the database credentials** in the same file (`LLMChatter.Database.*`) to match your AzerothCore characters DB.

**3. Start the server once** — worldserver imports the module's SQL automatically.

**4. Start the bridge** (does all LLM work; the game server never blocks on it):

```bash
~/llm-bridge-venv/bin/python mod-llm-chatter/tools/llm_chatter_bridge.py \
  --config <server>/etc/modules/mod_llm_chatter.conf
```

Its startup health check tells you if anything is misconfigured.

**5. In game:** stand near a bot and `/say` hello. Target a bot (or use its name) to address it directly. Grouped bots chat in party; bots also answer you in General and guild chat.

## Staying current with upstream

```bash
git fetch upstream && git rebase upstream/master
```

(`upstream` = https://github.com/Hokken/mod-llm-chatter)

## License

Same license as the original module — see [LICENSE](LICENSE).
