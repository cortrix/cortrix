---
title: Framework adapters
description: Use Cortrix from LangChain, Claude Tools, and OpenAI function calling.
weight: 40
---

On this page you'll install `cortrix-skills`, call its tools directly, and turn them into tool definitions that Claude, OpenAI, or LangChain understand.

`cortrix-skills` provides a `CortrixToolKit` with 29 methods that map one-to-one to the MCP server's regular tools, with the same names and semantics. Three adapters turn the toolkit into each framework's native tool format, so you don't write tool glue by hand. Admin tools are out of scope for this package.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- Python 3.9 or later, in a virtual environment.

## Install {#install}

Install the package from PyPI. It brings in the [Python SDK](/docs/integrations/python-sdk/) as a dependency:

```bash
pip install cortrix-skills
```

```bash
pip list | grep -E '^cortrix'
```

```text
cortrix                   1.0.0rc2
cortrix-skills            1.0.0rc2
```

The framework SDKs are optional dependencies. Building Claude or OpenAI tool definitions needs no framework SDK at all, because the definitions are plain JSON. Install a framework's package when you need it, for example `pip install anthropic`.

## Call the tools directly {#use-the-toolkit}

```python {title="skills_check.py"}
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_claude_tools, as_openai_functions

with CortrixToolKit(base_url="http://127.0.0.1:8420", default_namespace="demo") as kit:
    result = kit.cortrix_query(
        query="What does semantic storage keep close to the agents that need it?",
        top_k=3,
    )
    print(type(result).__name__, str(result)[:80])

    claude_tools = as_claude_tools(kit)
    print(len(claude_tools), claude_tools[0]["name"], sorted(claude_tools[0].keys()))
    print(len(as_openai_functions(kit)))
```

```text
dict {'results': [{'child_id': '01M409ADX4M34WZD0EWVRYEMQJ', 'parent_id': '', 'co
29 cortrix_health ['description', 'input_schema', 'name']
29
```

`CortrixToolKit` is lazy: it makes no network request until you call a method.

| Parameter | Default | Description |
|---|---|---|
| `base_url` | — | Required unless you pass `client` |
| `api_key` | `None` | A Cortrix API key |
| `default_namespace` | `"default"` | Used when a method doesn't name a namespace |
| `client` | — | Pass in an existing `cortrix.Cortrix` client |

## Connect a framework {#frameworks}

The three examples below come from `cortrix-skills/README.md` in the repository. They need a key for the matching LLM service, and the full round trip with a model hasn't been tested for this page.

### Claude Tools {#claude-tools}

```python
from anthropic import Anthropic
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_claude_tools
from cortrix_skills.adapters.claude import dispatch_claude_tool_use

kit = CortrixToolKit(base_url="http://127.0.0.1:8420", default_namespace="demo")
tools = as_claude_tools(kit)

client = Anthropic(api_key="your-anthropic-api-key")
resp = client.messages.create(
    model="claude-...", max_tokens=4096, tools=tools,
    messages=[{"role": "user", "content": "What does semantic storage keep close to agents?"}],
)
for block in resp.content:
    if block.type == "tool_use":
        tool_result = dispatch_claude_tool_use(kit, block)
        # append tool_result to the messages of the next messages.create(...) call
```

### OpenAI function calling {#openai}

```python
from openai import OpenAI
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_openai_functions
from cortrix_skills.adapters.openai import dispatch_openai_tool_call

kit = CortrixToolKit(base_url="http://127.0.0.1:8420", default_namespace="demo")
tools = as_openai_functions(kit)

client = OpenAI(api_key="your-openai-api-key")
resp = client.chat.completions.create(
    model="gpt-4o-mini", tools=tools,
    messages=[{"role": "user", "content": "What does semantic storage keep close to agents?"}],
)
for call in resp.choices[0].message.tool_calls or []:
    content = dispatch_openai_tool_call(kit, call)
    # content is a JSON string for a message with role "tool"
```

### LangChain {#langchain}

```bash
pip install 'cortrix-skills[langchain]'
```

```python
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_langchain_tools

kit = CortrixToolKit(base_url="http://127.0.0.1:8420", default_namespace="demo")
tools = as_langchain_tools(kit)   # 29 StructuredTools
```

## Error handling {#errors}

Cortrix errors pass through unchanged from the Python SDK. This package adds no error codes of its own. Every error carries `code`, `retryable`, `category`, `retry_after_ms`, and `structured_data`, and each adapter surfaces them in its framework's native shape:

| Framework | How an error appears |
|---|---|
| LangChain | A `ToolException` whose message is these fields as JSON |
| Claude Tools | A `tool_result` with `is_error=True` whose content is these fields as JSON |
| OpenAI | These fields as JSON, returned as the tool message content |

An agent can use them to decide for itself whether to retry, wait, or report, without parsing free text.

## Next steps {#next-steps}

- [MCP server](/docs/integrations/mcp/): the full list of tools.
- [Python SDK](/docs/integrations/python-sdk/)
