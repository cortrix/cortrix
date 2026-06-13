"""Lightweight dataclass <- dict parsing (forward-compatible).

Response types are plain ``@dataclass`` objects (P03 design § 3.2). The server
JSON may carry extra fields (forward compat, AGENT_FRIENDLY principle 7) or omit
optional ones; :func:`parse_model` constructs the dataclass tolerantly:

  - unknown keys are ignored,
  - missing keys fall back to the field default (or ``None`` for Optional),
  - nested dataclass / ``List[dataclass]`` fields are parsed recursively.
"""

from __future__ import annotations

import dataclasses
import typing
from typing import Any, Type, TypeVar, Union, get_args, get_origin

T = TypeVar("T")

_NoneType = type(None)


def _is_optional(tp: Any) -> bool:
    return get_origin(tp) is Union and _NoneType in get_args(tp)


def _unwrap_optional(tp: Any) -> Any:
    if get_origin(tp) is Union:
        non_none = [a for a in get_args(tp) if a is not _NoneType]
        if len(non_none) == 1:
            return non_none[0]
    return tp


def _coerce(tp: Any, value: Any) -> Any:
    if value is None:
        return None
    tp = _unwrap_optional(tp)
    origin = get_origin(tp)

    # List[X] / list[X]
    if origin in (list, typing.List) and isinstance(value, list):
        (inner,) = get_args(tp) or (Any,)
        return [_coerce(inner, v) for v in value]

    # Nested dataclass
    if dataclasses.is_dataclass(tp) and isinstance(value, dict):
        return parse_model(tp, value)  # type: ignore[arg-type]

    return value


def parse_model(model: Type[T], data: Any) -> T:
    """Construct dataclass ``model`` from ``data`` (a dict), tolerantly."""
    if not dataclasses.is_dataclass(model):
        return typing.cast(T, data)
    if not isinstance(data, dict):
        raise TypeError(f"Cannot parse {model.__name__} from {type(data).__name__}")

    hints = typing.get_type_hints(model)
    kwargs: dict[str, Any] = {}
    for field in dataclasses.fields(model):
        tp = hints.get(field.name, Any)
        if field.name in data:
            kwargs[field.name] = _coerce(tp, data[field.name])
        elif (
            field.default is not dataclasses.MISSING
            or field.default_factory is not dataclasses.MISSING
        ):
            continue  # use dataclass default
        elif _is_optional(tp):
            kwargs[field.name] = None
        # else: required field absent -> let dataclass raise a clear TypeError
    return typing.cast(T, model(**kwargs))
