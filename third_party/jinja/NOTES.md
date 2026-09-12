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
6. Object method calls in `MethodCallExpr`: `items()`, `keys()`, `values()`,
   `get()`, `contains()` on mappings. `dict.items()` is the branch froggeric
   uses to render `<parameter=...>` blocks for structured tool arguments;
   upstream supported these names on strings only.
7. Order-faithful JSON objects: `ujson`'s nlohmann backend is aliased to
   `nlohmann::ordered_json`. jinja2 iterates Python dicts in insertion order,
   so `tc.arguments.items()` must preserve wire order; nlohmann::json sorts
   keys and silently reordered every multi-key `<parameter=...>` block.
8. Context scopes store variables in C++ containers instead of writing them
   into the context JSON document. Upstream inserted `set` targets into the
   document that also holds template data; with vector-backed ordered objects
   that insertion relocates the document and invalidates outstanding
   iterators/pointers (observed as dropped or truncated tool arguments).
   The `tojson` filter builds sorted member strings instead of sorting JSON
   wrappers, because the wrapper's `operator=` writes through the shared
   pointer and sorting wrappers corrupted the documents.

9. Optional render trace: `Template::render(context, RenderTrace*)` records a
   loop-iteration span (offsets plus the item's role) for every executed
   `for` body and an output span for every printed expression. The frontend
   derives prompt structure (per-message byte frontiers, content provenance)
   from one render instead of scanning the concatenated text for markers,
   which content can forge. Also: direct iteration over a mapping no longer
   sorts keys, matching jinja2 insertion order.

10. `trim`/`strip`/`lstrip`/`rstrip` use Python's whitespace set (str.isspace():
   form feed, vertical tab, NBSP and the Unicode separators) instead of ASCII only.
11. `Macro::~Macro` is inline. It was defined out of line in a header-only library,
   so a second translation unit that included the header failed to link.
12. `default`/`d` and `safe` filters added, and an unimplemented filter now throws
   instead of silently returning its input. `reasoning_effort|default('xhigh')` is
   load-bearing in the reasoning-effort template, and the sentinel value that marks
   an undefined variable was passing straight through it.
13. `loop.previtem` / `loop.nextitem` added to the loop object; the thinking-toggle
   and reasoning-effort templates group consecutive tool results with them.
14. `RenderTrace::LoopIteration` carries an `invocation` id, one per executed `for`
   statement, so the frontend groups a loop's iterations by identity. A loop nested inside
   an iteration records its own iterations first, so record order alone does not group them
   and adjacency-based grouping splits the enclosing loop. Verified by
   `tests/targets/qwen3_6/test_jinja_vision_mapping.cpp`.
15. Macro bodies render with tracing suspended (`context.set_trace(nullptr)` around the
   body). A macro renders into its own buffer, so its print and loop offsets address that
   buffer rather than the traced output, and recording them published spans over the wrong
   text. Verified by `tests/targets/qwen3_6/test_jinja_vision_mapping.cpp` and
   `tests/targets/qwen3_6/test_jinja_frontend_render.cpp`.

## Render environment

The engine matches the HuggingFace `transformers` Jinja environment
(`trim_blocks=True`, `lstrip_blocks=True`, `keep_trailing_newline=True`), which is
what the registered templates are written for; the lexer implements both flags.
Oracles must be generated with that environment, not jinja2's defaults.

## Host-registered globals

- `raise_exception`: registered by the frontend via `add_function` as a
  throwing function; `render()` propagates exceptions (verified).

## Verification

`tests/test_jinja_template.cpp` compares the engine byte-for-byte against the Python
jinja2 3.1.6 oracle for the registered froggeric v22.5 template: 32 contexts (tools in
both formats, tool history and error tiers, string- and mapping-shaped tool arguments,
reasoning replay, effort aliases, multi-system merge, reasoning-field variants,
vision/vision-id/video_url, multi tool call, literal think-close, whitespace trimming,
both `raise_exception` paths) plus a ~200k-token scale context (byte-identical).

`tests/targets/qwen3_6/test_jinja_frontend_render.cpp` executes all three registered
templates through the frontend context builder: 84 renders byte-exact against the
Python oracle for the same file, with structured-output invariants on top.
