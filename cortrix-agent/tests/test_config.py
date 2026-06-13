"""Unit tests for config.py (yaml agent_llm -> env mapping; design section 6.1)."""

import pytest

import config


def test_settings_defaults():
    assert config.settings.agent_port == 8001
    assert config.settings.cortrix_namespace == "default"
    assert "deepseek" in str(config.Settings.model_fields["llm_provider"].annotation)


def test_load_yaml_missing_file(monkeypatch, tmp_path):
    monkeypatch.setenv("CORTRIX_CONFIG_PATH", str(tmp_path / "nonexistent.yaml"))
    assert config._load_yaml_agent_llm() == {}


def test_load_yaml_no_agent_llm_section(monkeypatch, tmp_path):
    f = tmp_path / "config.yaml"
    f.write_text("other_section: 1\n")
    monkeypatch.setenv("CORTRIX_CONFIG_PATH", str(f))
    assert config._load_yaml_agent_llm() == {}


def test_load_yaml_maps_deepseek_provider(monkeypatch, tmp_path):
    pytest.importorskip("yaml")
    f = tmp_path / "config.yaml"
    f.write_text(
        "agent_llm:\n"
        "  provider: deepseek\n"
        "  api_key: sk-xx\n"
        "  model: deepseek-chat\n"
        "  base_url: https://api.deepseek.com/v1\n"
    )
    monkeypatch.setenv("CORTRIX_CONFIG_PATH", str(f))
    result = config._load_yaml_agent_llm()
    assert result["LLM_PROVIDER"] == "deepseek"
    assert result["DEEPSEEK_API_KEY"] == "sk-xx"
    assert result["DEEPSEEK_MODEL"] == "deepseek-chat"
    assert result["DEEPSEEK_BASE_URL"] == "https://api.deepseek.com/v1"


def test_load_yaml_maps_claude_api_key(monkeypatch, tmp_path):
    pytest.importorskip("yaml")
    f = tmp_path / "config.yaml"
    f.write_text("agent_llm:\n  provider: claude\n  api_key: ak-yy\n  model: claude-x\n")
    monkeypatch.setenv("CORTRIX_CONFIG_PATH", str(f))
    result = config._load_yaml_agent_llm()
    assert result["LLM_PROVIDER"] == "claude"
    assert result["ANTHROPIC_API_KEY"] == "ak-yy"
    assert result["CLAUDE_MODEL"] == "claude-x"
