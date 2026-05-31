"""
log_usage.py — helper to log Claude API usage to the dashboard backend.

Usage:
    from log_usage import log_usage

    import anthropic
    client = anthropic.Anthropic()
    msg = client.messages.create(
        model="claude-sonnet-4-6",
        max_tokens=1024,
        messages=[{"role": "user", "content": "Hello!"}],
    )
    log_usage(msg)          # automatically reads model + usage from response
    print(msg.content[0].text)
"""

import urllib.request
import json

DASHBOARD_URL = "http://localhost:3000"  # change if backend runs elsewhere


def log_usage(response, server: str = DASHBOARD_URL) -> None:
    """Post token usage from an Anthropic MessageResponse to the dashboard."""
    payload = json.dumps({
        "model":        getattr(response, "model", "unknown"),
        "inputTokens":  getattr(response.usage, "input_tokens",  0),
        "outputTokens": getattr(response.usage, "output_tokens", 0),
    }).encode()

    req = urllib.request.Request(
        f"{server}/api/usage/log",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        urllib.request.urlopen(req, timeout=3)
    except Exception as e:
        print(f"[log_usage] Warning — could not reach dashboard: {e}")


# ── Example (run this file directly to send a test record) ───────────────────
if __name__ == "__main__":
    import urllib.request, json

    test = json.dumps({
        "model": "claude-sonnet-4-6",
        "inputTokens": 1500,
        "outputTokens": 300,
    }).encode()
    req = urllib.request.Request(
        f"{DASHBOARD_URL}/api/usage/log",
        data=test,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        resp = urllib.request.urlopen(req, timeout=3)
        print("Logged test record:", resp.read().decode())
    except Exception as e:
        print(f"Error: {e} — is the backend running?")
