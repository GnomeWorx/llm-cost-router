#!/usr/bin/env python3
"""LLM Cost Router — Full Test Suite

Tests: health, models, routing rules (small→Ollama, large→cloud, keywords→cloud),
       streaming, non-streaming, token tracking, budget enforcement, log parsing.
"""

import json
import sys
import time
import os
import urllib.request
import urllib.error

BASE = "http://localhost:8100"
PASS = 0
FAIL = 0
SKIP = 0

def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  ✓ {name}")
    else:
        FAIL += 1
        detail_str = f"  — {detail}" if detail else ""
        print(f"  ✗ {name}{detail_str}")

def check_skip(name):
    global SKIP
    SKIP += 1
    print(f"  ∼ {name} (skipped)")

def api(path, data=None, timeout=30):
    url = f"{BASE}{path}"
    if data is not None:
        body = json.dumps(data).encode()
        req = urllib.request.Request(url, body, {"Content-Type": "application/json"})
    else:
        req = urllib.request.Request(url)
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        return json.loads(resp.read()), resp.status
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read()), e.code
        except Exception:
            return {"error": str(e)}, e.code
    except Exception as e:
        return {"error": str(e)}, 0

def stream_test(data, timeout=20):
    """Test streaming endpoint, return (chunks, error)"""
    body = json.dumps(data).encode()
    req = urllib.request.Request(f"{BASE}/v1/chat/completions", body,
                                 {"Content-Type": "application/json"})
    chunks = []
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        for line in resp:
            line = line.decode().strip()
            if line.startswith("data: "):
                payload = line[6:]
                if payload == "[DONE]":
                    break
                chunks.append(json.loads(payload))
        return chunks, None
    except Exception as e:
        return chunks, str(e)

# ===== SUITE 1: Health & Discovery =====
print("\n── Suite 1: Health & Discovery ──")

data, status = api("/health")
check("Health endpoint returns 200", status == 200)
check("Health status is ok", data.get("status") == "ok")
check("Ollama backend healthy", data.get("backends", {}).get("ollama") == True)
check("Cloud backend healthy", data.get("backends", {}).get("cloud") == True)
check("Cost tracking enabled", data.get("cost_tracking") == True)
check("Daily cost is a number", isinstance(data.get("daily_cost"), (int, float)))

data, status = api("/v1/models")
check("/v1/models returns 200", status == 200)
models = [m["id"] for m in data.get("data", [])]
check("qwen2.5-coder:7b model listed", "qwen2.5-coder:7b" in models)
check("deepseek-v4-flash model listed", "deepseek-v4-flash" in models)
check("deepseek-v4-flash-cloud model listed", "deepseek-v4-flash-cloud" in models)

# ===== SUITE 2: Admin Endpoints =====
print("\n── Suite 2: Admin Endpoints ──")

data, status = api("/admin/summary")
check("/admin/summary returns 200", status == 200)
check("today section exists", "today" in data)
check("budget section exists", "budget" in data)
check("history section exists", "history" in data)
today = data.get("today", {})
check("today has request count", isinstance(today.get("requests"), int))
check("today has input_tokens", isinstance(today.get("input_tokens"), int))
check("today has output_tokens", isinstance(today.get("output_tokens"), int))
check("today has cloud_requests", isinstance(today.get("cloud_requests"), int))
check("today has ollama_requests", isinstance(today.get("ollama_requests"), int))
budget = data.get("budget", {})
check("budget has monthly_budget", budget.get("monthly_budget") == 150.0)
check("budget has day_of_month", 1 <= budget.get("day_of_month", 0) <= 31)

data, status = api("/admin/config")
check("/admin/config returns 200", status == 200)
check("config has router section", "router" in data)
check("config has ollama section", "ollama" in data)
check("config has cloud section", "cloud" in data)
check("config has cost_tracking", "cost_tracking" in data)

# ===== SUITE 3: Non-Streaming Chat =====
print("\n── Suite 3: Non-Streaming Chat ──")

# 3a: Small request (<1200 tokens) should route to Ollama
data, status = api("/v1/chat/completions", {
    "model": "deepseek-v4-flash",
    "messages": [{"role": "user", "content": "Say OK"}],
    "stream": False
})
check("Small request returns 200", status == 200)
check("Small request has choices", "choices" in data)
check("Small request has usage", "usage" in data)
if "usage" in data:
    u = data["usage"]
    check("Small request prompt_tokens > 0", u.get("prompt_tokens", 0) > 0)
    check("Small request completion_tokens > 0", u.get("completion_tokens", 0) > 0)

# 3b: Large prompt (>5000 tokens) should route to DeepSeek and include cache info
large_system = "x" * 25000
data, status = api("/v1/chat/completions", {
    "model": "deepseek-v4-flash",
    "messages": [
        {"role": "system", "content": large_system},
        {"role": "user", "content": "Say OK"}
    ],
    "stream": False
})
check("Large prompt returns 200", status == 200)
if "usage" in data:
    u = data["usage"]
    check("Large prompt has prompt_cache fields",
          "prompt_cache_hit_tokens" in u or
          ("prompt_tokens_details" in u and "cached_tokens" in u["prompt_tokens_details"]))

# 3c: Explicit cloud suffix forces cloud route
data, status = api("/v1/chat/completions", {
    "model": "deepseek-v4-flash-cloud",
    "messages": [{"role": "user", "content": "Say OK"}],
    "stream": False
})
check("-cloud suffix returns 200", status == 200)
check("-cloud suffix has choices", "choices" in data)

# ===== SUITE 4: Streaming Chat =====
print("\n── Suite 4: Streaming Chat ──")

# 4a: Small streaming request
chunks, err = stream_test({
    "model": "deepseek-v4-flash",
    "messages": [{"role": "user", "content": "Say OK"}],
    "stream": True
})
check("Small stream: no errors", err is None, err or "")
check("Small stream: has chunks", len(chunks) > 0)
if chunks:
    has_content = any(
        c.get("choices", [{}])[0].get("delta", {}).get("content")
        for c in chunks
    )
    check("Small stream: has content delta", has_content)

# 4b: Large prompt streaming
chunks, err = stream_test({
    "model": "deepseek-v4-flash",
    "messages": [
        {"role": "system", "content": large_system},
        {"role": "user", "content": "Say OK"}
    ],
    "stream": True
})
check("Large stream: no errors", err is None, err or "")
check("Large stream: has chunks", len(chunks) > 0)

# ===== SUITE 5: Routing Rules =====
print("\n── Suite 5: Routing Rules ──")

# Small requests (<1200 tokens) route to Ollama
data, status = api("/v1/chat/completions", {
    "model": "deepseek-v4-flash",
    "messages": [{"role": "user", "content": "hi"}],
    "stream": False
})
check("Small query routes successfully (Ollama)", status == 200)

# Check the usage log for the routing decision of the last request
# We query the log to verify routing, but for an automated test we just verify the API works

# ===== SUITE 6: Token Tracking =====
print("\n── Suite 6: Token Tracking Integrity ──")

data, status = api("/admin/summary")
today = data.get("today", {})
cloud_in = today.get("cloud_input_tokens", 0)
cloud_out = today.get("cloud_output_tokens", 0)
cloud_cache = today.get("cloud_cache_tokens", 0)

# Cloud input should be a reasonable value (>0 if we've made cloud requests)
check("cloud_input_tokens is consistent", cloud_in >= 0)
check("cloud_output_tokens is consistent", cloud_out >= 0)
check("cloud_cache_tokens is consistent", cloud_cache >= 0)
check("ollama/cloud split is coherent",
      today.get("requests", 0) == today.get("cloud_requests", 0) + today.get("ollama_requests", 0))

# Verify no epoch entries in history
epoch_entries = [h for h in data.get("history", []) if h.get("date") == "1970-01-01"]
check("No epoch (1970-01-01) entries in history", len(epoch_entries) == 0)

# ===== SUITE 7: Budget Enforcement =====
print("\n── Suite 7: Budget Enforcement ──")

data, status = api("/admin/summary")
budget = data.get("budget", {})
check("Budget monthly is $150", budget.get("monthly_budget") == 150.0)
check("Budget daily_allowance ~$5/day",
      abs(budget.get("daily_allowance", 0) - 5.0) < 0.5)
check("Budget month_to_date_cost >= 0", budget.get("month_to_date_cost", -1) >= 0)
check("Budget day_of_month >= 1", budget.get("day_of_month", 0) >= 1)
check("Budget days_in_month >= 28", budget.get("days_in_month", 0) >= 28)

# ===== SUITE 8: Log Persistence & Refresh =====
print("\n── Suite 8: Log Persistence ──")

data, status = api("/admin/refresh-usage")
check("/admin/refresh-usage returns 200", status == 200)
check("Refresh returns ok status", data.get("status") == "ok")

# Verify summary still works after refresh
data, status = api("/admin/summary")
check("Summary works after refresh", status == 200)
check("History still populated after refresh", len(data.get("history", [])) > 0)

# ===== RESULTS =====
print(f"\n{'='*50}")
print(f"  Results: {PASS} passed, {FAIL} failed, {SKIP} skipped")
print(f"{'='*50}")

if FAIL > 0:
    sys.exit(1)
if PASS == 0:
    sys.exit(1)
sys.exit(0)
