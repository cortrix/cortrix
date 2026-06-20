"""Cortrix Agent configuration (design section 6.1 — 4-layer priority).

Priority (highest -> lowest):
  1. Real environment variables (e.g. export LLM_PROVIDER=claude)
  2. cortrix-agent/.env file         (local override)
  3. build/config.yaml -> agent_llm  (main unified config; D3.5 -> IGlobalConfig)
  4. Hardcoded defaults              (mock / safe defaults)
"""

import os
from pathlib import Path
from pydantic_settings import BaseSettings, SettingsConfigDict
from typing import Literal


def _load_yaml_agent_llm() -> dict:
    """Read agent_llm section from cortrix build/config.yaml, return as env-var dict."""
    yaml_path = os.environ.get(
        "CORTRIX_CONFIG_PATH",
        str(Path(__file__).parent.parent / "build" / "config.yaml"),
    )
    try:
        import yaml  # type: ignore

        with open(yaml_path) as f:
            cfg = yaml.safe_load(f) or {}
        a = cfg.get("agent_llm", {})
        if not a:
            return {}

        # Map config.yaml field names → env var names used by Settings below
        provider = a.get("provider", "")
        api_key  = a.get("api_key", "")
        model    = a.get("model", "")
        base_url = a.get("base_url", "")

        result: dict = {}
        if provider:
            result["LLM_PROVIDER"] = provider
            # Map model to the right provider-specific env var
            if model:
                key_map = {
                    "openai":     "OPENAI_MODEL",
                    "claude":     "CLAUDE_MODEL",
                    "ollama":     "OLLAMA_MODEL",
                    "glm":        "GLM_MODEL",
                    "deepseek":   "DEEPSEEK_MODEL",
                }
                model_key = key_map.get(provider)
                if model_key:
                    result[model_key] = model
            # Map api_key to the right provider-specific env var
            if api_key:
                key_map = {
                    "openai":  "OPENAI_API_KEY",
                    "claude":  "ANTHROPIC_API_KEY",
                    "glm":     "GLM_API_KEY",
                    "deepseek": "DEEPSEEK_API_KEY",
                }
                api_key_var = key_map.get(provider)
                if api_key_var:
                    result[api_key_var] = api_key
            # Map base_url for providers that support it
            if base_url:
                url_map = {
                    "openai":  "OPENAI_BASE_URL",
                    "ollama":  "OLLAMA_BASE_URL",
                    "glm":     "GLM_BASE_URL",
                    "deepseek": "DEEPSEEK_BASE_URL",
                }
                url_var = url_map.get(provider)
                if url_var:
                    result[url_var] = base_url
        return result
    except Exception:
        return {}


# Apply config.yaml agent_llm as env var defaults (only if not already set by user)
for _k, _v in _load_yaml_agent_llm().items():
    if _k not in os.environ:
        os.environ[_k] = _v


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    # Cortrix Backend
    cortrix_base_url: str = "http://localhost:8420"
    cortrix_api_key: str = ""
    cortrix_namespace: str = "default"

    # LLM Provider
    llm_provider: Literal["openai", "claude", "ollama", "glm", "mock", "deepseek"] = "mock"

    # OpenAI
    openai_api_key: str = ""
    openai_model: str = "gpt-4o-mini"
    openai_base_url: str = "https://api.openai.com/v1"

    # Anthropic Claude
    anthropic_api_key: str = ""
    claude_model: str = "claude-haiku-4-5-20251001"
    claude_max_tokens: int = 4096

    # Ollama
    ollama_base_url: str = "http://localhost:11434"
    ollama_model: str = "llama3.2"

    # GLM (Zhipu AI)
    glm_api_key: str = ""
    glm_model: str = "glm-4-flash"
    glm_base_url: str = "https://open.bigmodel.cn/api/paas/v4"

    # DeepSeek
    deepseek_api_key: str = ""
    deepseek_model: str = "deepseek-chat"
    deepseek_base_url: str = "https://api.deepseek.com/v1"

    # RAG / context
    max_context_blocks: int = 5
    stream_response: bool = True

    # Server
    agent_port: int = 8001
    log_level: str = "INFO"


settings = Settings()
