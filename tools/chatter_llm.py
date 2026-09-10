"""LLM call-layer helpers extracted from chatter_shared (N14).

Anthropic-only fork: Claude is the only supported provider.
"""

import logging
import threading
import time
from typing import Any, Optional

from chatter_constants import DEFAULT_ANTHROPIC_MODEL

logger = logging.getLogger(__name__)


def _split_prompt(prompt):
    """Extract system/user parts from a prompt.

    Returns (system_msg, user_msg). system_msg is
    None for plain str prompts.
    """
    from chatter_shared import PromptParts
    if isinstance(prompt, PromptParts) and prompt.system_prompt:
        return prompt.system_prompt, prompt.user_prompt
    return None, str(prompt)


def make_anthropic_client(key):
    """Build an Anthropic client from an API key or a Claude
    subscription OAuth token (sk-ant-oat..., from `claude
    setup-token`), which must be sent as a Bearer auth_token."""
    import anthropic
    if key and key.startswith('sk-ant-oat'):
        return anthropic.Anthropic(
            api_key=None,
            auth_token=key,
            default_headers={
                'anthropic-beta': 'oauth-2025-04-20',
            },
        )
    return anthropic.Anthropic(api_key=key)


def _build_anthropic_request_kwargs(
    model, max_tokens, temperature, sys_msg, user_msg
):
    """Build Anthropic SDK v1-compatible request arguments."""
    kwargs = {
        "model": model,
        "max_tokens": max_tokens,
        "messages": [{
            "role": "user",
            "content": user_msg,
        }],
        "extra_body": {
            "temperature": temperature,
        },
    }
    if sys_msg:
        kwargs["system"] = sys_msg
    return kwargs


def resolve_model(model_name: str) -> str:
    """Resolve friendly model aliases to provider model IDs."""
    normalized = (model_name or '').strip()
    aliases = {
        'haiku': DEFAULT_ANTHROPIC_MODEL,
    }
    return aliases.get(normalized.lower(), normalized)


_main_client = None
_main_client_provider = None
_main_client_lock = threading.Lock()


def get_llm_client(config):
    """Get or create the main LLM client.

    Thread-safe, lazily initialised, cached.
    Anthropic-only: any other configured provider
    falls back to the Anthropic client.
    """
    global _main_client, _main_client_provider

    provider = config.get(
        'LLMChatter.Provider', 'anthropic'
    ).lower()
    if provider != 'anthropic':
        logger.error(
            "LLMChatter.Provider=%r is not supported: this "
            "fork is Anthropic-only. Using the Anthropic "
            "client.", provider,
        )
        provider = 'anthropic'

    with _main_client_lock:
        if (
            _main_client is not None
            and _main_client_provider == provider
        ):
            return _main_client

        _main_client = make_anthropic_client(
            config.get(
                'LLMChatter.Anthropic.ApiKey',
                '',
            )
        )
        _main_client_provider = provider
        return _main_client


def call_llm(
    client: Any,
    prompt: str,
    config: dict,
    max_tokens_override: int = None,
    context: str = '',
    *,
    label: str = '',
    metadata: dict = None,
) -> str:
    """Call the Anthropic (Claude) API."""
    provider = 'anthropic'
    model = config.get(
        'LLMChatter.Model', DEFAULT_ANTHROPIC_MODEL
    )
    model = resolve_model(model)
    if max_tokens_override is not None:
        max_tokens = max_tokens_override
    else:
        max_tokens = int(
            config.get('LLMChatter.MaxTokens', 200)
        )
    temperature = float(
        config.get('LLMChatter.Temperature', 0.85)
    )

    t0 = time.monotonic()
    result = None
    sys_msg, user_msg = _split_prompt(prompt)
    sent_user_msg = user_msg  # tracks actual payload
    try:
        kwargs = _build_anthropic_request_kwargs(
            model,
            max_tokens,
            temperature,
            sys_msg,
            user_msg,
        )
        response = client.messages.create(
            **kwargs
        )
        result = response.content[0].text.strip()
    except Exception as exc:
        logger.error(
            "LLM call failed (%s): %s", label, exc
        )
        result = None
    finally:
        duration_ms = int(
            (time.monotonic() - t0) * 1000
        )
        try:
            from chatter_request_logger import (
                log_request,
            )
            log_request(
                label, sent_user_msg, result,
                model, provider, duration_ms,
                metadata=metadata,
                system_prompt=sys_msg,
            )
        except Exception:
            pass
    return result


# Cached client for quick analyze when its key
# differs from the main client's
_quick_analyze_client = None
_quick_analyze_provider = None
_quick_analyze_lock = threading.Lock()


def _get_quick_analyze_client(config):
    """Get or create the LLM client for quick
    analyze calls. Returns (client, provider).

    Anthropic-only: if QuickAnalyze.Provider is
    empty or 'anthropic', returns (None,
    'anthropic') so the caller uses the main
    client. Any other value is ignored with the
    same fallback.

    Thread-safe: lazy init protected by lock.
    """
    qa_provider = config.get(
        'LLMChatter.QuickAnalyze.Provider', ''
    ).strip().lower()

    if qa_provider and qa_provider != 'anthropic':
        logger.error(
            "LLMChatter.QuickAnalyze.Provider=%r is not "
            "supported: this fork is Anthropic-only. Using "
            "the main client.", qa_provider,
        )
    return None, 'anthropic'


def quick_llm_analyze(
    client: Any,
    config: dict,
    prompt: str,
    max_tokens: int = 50,
    *,
    label: str = '',
    metadata: dict = None,
) -> Optional[str]:
    """Fast LLM call for pre-processing analysis.

    Uses the configured QuickAnalyze model, or
    defaults to Claude Haiku.

    Useful for tasks like:
    - Determining which bot a player is addressing
    - Classifying message intent or sentiment
    - Summarizing context before a full prompt

    Returns raw text response, or None on error.
    """
    qa_client, provider = (
        _get_quick_analyze_client(config)
    )
    active_client = qa_client if qa_client is not None else client

    # Resolve model
    qa_model = config.get(
        'LLMChatter.QuickAnalyze.Model', ''
    ).strip()
    model = qa_model or DEFAULT_ANTHROPIC_MODEL
    model = resolve_model(model)

    t0 = time.monotonic()
    result = None
    sys_msg, user_msg = _split_prompt(prompt)
    sent_user_msg = user_msg
    try:
        kwargs = _build_anthropic_request_kwargs(
            model,
            max_tokens,
            0.1,
            sys_msg,
            user_msg,
        )
        response = (
            active_client.messages.create(
                **kwargs
            )
        )
        result = response.content[0].text.strip()
    except Exception as exc:
        logger.error(
            "LLM call failed (%s): %s", label, exc
        )
        result = None
    finally:
        duration_ms = int(
            (time.monotonic() - t0) * 1000
        )
        try:
            from chatter_request_logger import (
                log_request,
            )
            log_request(
                label, sent_user_msg, result,
                model, provider, duration_ms,
                metadata=metadata,
                system_prompt=sys_msg,
            )
        except Exception:
            pass
    return result
