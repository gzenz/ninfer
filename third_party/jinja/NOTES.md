# jinja.cpp (vendored)

Upstream: https://github.com/wangzhaode/jinja.cpp
Pinned at: a1d18d5979b3582a17883aac9c2bed2e26c7a578 (2026-03-04, main)
License: Apache-2.0 (see LICENSE)

Single-header Jinja2 subset engine for LLM chat templates, with the
nlohmann JSON backend (third_party/nlohmann). Vendored 2026-09-10 for the
Qwen3.6 family frontend so `frontend/chat_template.jinja` is executed, not
only identity-hashed (see plan.md "Frontend Template Execution").

## Local patches (relative to the pinned upstream)

All patches are required for froggeric v22/v22.5 parity; each is verified by
tests/test_jinja_template.cpp (byte-exact against Python jinja2 as oracle).

1. `make_unique` polyfill guarded by `__cpp_lib_make_unique` — the upstream
   polyfill is unguarded and ambiguous with `std::make_unique` under modern
   standards (C++14+ with <memory>).
2. `join` filter added (positional separator; string-element conversion via
   `to_python_string`). Froggeric uses `| join('')` 20x for inline-tag
   stripping; upstream has no join filter.
3. `tojson` aligned to jinja2 semantics: sort_keys=True, separators
   (", ", ": "), UTF-8 preserved. Upstream used a custom tool-canonical key
   order.
4. String slicing branch in GetItemExpr (`s[:n]`, `s[a:b]`, negative
   indices) — upstream sliced arrays only; the template's truncation and
   JSON-payload detection use string slices.
5. Parenthesized comma lists parse as array literals (Python tuples) —
   upstream parsed `(...)` as grouped expression/call, breaking
   `x in ('a', 'low')` effort-alias branches.

## Host-registered globals

- `raise_exception`: registered by the frontend via `add_function` as a
  throwing function; `render()` propagates exceptions (verified).
