"""Unit tests for main.py (app assembly + LLM provider factory + /health)."""

import pytest
from fastapi.testclient import TestClient

import main
from llm import DeepSeekAdapter, MockAdapter, OpenAIAdapter


def test_model_name_deepseek_and_mock(monkeypatch):
    monkeypatch.setattr(main.settings, "llm_provider", "deepseek")
    monkeypatch.setattr(main.settings, "deepseek_model", "deepseek-chat")
    assert main._model_name() == "deepseek-chat"
    monkeypatch.setattr(main.settings, "llm_provider", "mock")
    assert main._model_name() == "mock-model"


def test_create_llm_adapter_mock(monkeypatch):
    monkeypatch.setattr(main.settings, "llm_provider", "mock")
    assert isinstance(main._create_llm_adapter(), MockAdapter)


def test_create_llm_adapter_openai(monkeypatch):
    monkeypatch.setattr(main.settings, "llm_provider", "openai")
    monkeypatch.setattr(main.settings, "openai_api_key", "sk-test")
    assert isinstance(main._create_llm_adapter(), OpenAIAdapter)


def test_create_llm_adapter_deepseek(monkeypatch):
    monkeypatch.setattr(main.settings, "llm_provider", "deepseek")
    monkeypatch.setattr(main.settings, "deepseek_api_key", "sk-test")
    assert isinstance(main._create_llm_adapter(), DeepSeekAdapter)


def test_create_llm_adapter_unknown_falls_back_to_mock(monkeypatch):
    monkeypatch.setattr(main.settings, "llm_provider", "nonexistent")
    assert isinstance(main._create_llm_adapter(), MockAdapter)


def test_health_endpoint():
    app = main.build_app()
    with TestClient(app) as client:
        resp = client.get("/health")
        assert resp.status_code == 200
        body = resp.json()
        assert body["status"] == "ready"
        assert "cortrix_server" in body
        assert "llm_reachable" in body


def test_current_config_view_masks_key():
    view = main._current_config_view()
    assert view.llm_provider
    # api_key_masked is None (no key) or masked with asterisks — never the raw key.
    assert view.api_key_masked is None or "*" in view.api_key_masked
