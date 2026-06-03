# LLM Cost Router

An OpenAI-compatible proxy that routes LLM requests between a local Ollama instance and a cloud provider (DeepSeek V4 Flash) based on configurable rules, with token/cost tracking and a monthly budget cap.

## Features

- **OpenAI-compatible API** — drop-in replacement for any OpenAI client
- **Dual backends** — routes to Ollama (free/local) or DeepSeek (cloud/paid)
- **Smart routing** — keyword escalation, model name matching, token threshold
- **Ollama concurrency handling** — when Ollama is busy (single-request constraint), falls back to cloud automatically
- **Retry-on-failure** — tries Ollama first on escalated requests, falls back to cloud if Ollama refuses
- **Cost tracking** — per-request token/cost logging with daily and monthly budget enforcement (prorated)
- **Daily usage starts at 12:01 AM** — aligns with billing cycles

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/v1/chat/completions` | Chat completion (streaming and non-streaming) |
| GET | `/v1/models` | List available models |
| GET | `/health` | Health check |
| GET | `/admin/summary` | Daily cost summary with Ollama/cloud split |
| GET | `/admin/config` | Current configuration |
| POST | `/admin/config` | Update configuration |
| GET | `/admin/refresh-usage` | Reload usage log from disk |

## Routing Logic

The router evaluates requests in this order:

1. **Model match** — if request model == Ollama model → Ollama
2. **Ollama busy** — if `ollamaFallbackOnBusy=true` and Ollama has active requests → cloud
3. **Header override** — `_meta.force_backend` in request body
4. **Model suffix** — model name ending in `-cloud` → cloud
5. **Token threshold** — estimated tokens < threshold → default backend (Ollama)
6. **Keyword escalation** — user/assistant messages containing escalation keywords → cloud (system prompt ignored)
7. **Default backend** — Ollama (configurable)

## Configuration

Config file: `~/.config/llm-cost-router/config.json`

Key fields:

```json
{
  "router": {
    "default_backend": "ollama",
    "token_threshold": 1200,
    "keyword_escalation": true,
    "escalate_keywords": ["architecture", "design", "refactor", "plan", "deadlock", "race condition"],
    "ollama_fallback_on_busy": false,
    "retry_on_failure": false,
    "monthly_budget_usd": 20.0
  },
  "ollama": {
    "base_url": "http://192.168.1.154:11434",
    "model": "qwen2.5-coder:7b"
  },
  "cloud": {
    "provider": "deepseek",
    "base_url": "https://api.deepseek.com",
    "model": "deepseek-v4-flash"
  }
}
```

## Building

```bash
cd llm-cost-router
cmake -B build
cmake --build build
./build/llm-cost-router
```

Requires C++17, CMake, nlohmann-json (bundled), httplib (bundled).
