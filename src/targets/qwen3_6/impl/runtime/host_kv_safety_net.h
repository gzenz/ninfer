#pragma once



// Host-KV safety net: when the pressure planner evicts a continuation, the device KV pages

// are copied to host RAM before release. When a matching prefix returns, the host copy is

// found by prefix matching and restored via H2D.



#include "core/host_kv_arena.h"

#include "targets/qwen3_6/impl/runtime/prefix_identity.h"



#include <algorithm>

#include <chrono>

#include <cstddef>

#include <cstdint>

#include <optional>

#include <span>

#include <vector>

#include <stdexcept>

#include <cstdio>

#include <utility>

#include <vector>



namespace ninfer::targets::qwen3_6::detail {



struct HostKVSafetyNetEntry {

    // Identity for prefix matching against incoming prompts.

    ResidentPrefixIdentity prefix_identity;

    std::vector<TokenId> ledger;

    std::uint32_t execution_frontier = 0;



    // Raw host KV allocations from HostKVArena. Text may span multiple

    // allocations (scatter-gather) when the arena is fragmented and no

    // single contiguous block is large enough.

    std::vector<HostKVAllocation> text_allocations;

    std::optional<HostKVAllocation> backend_host;



    // Page layout info (copied from the arena's supported layouts).

    std::uint32_t text_page_count    = 0;

    std::uint32_t backend_page_count = 0;



    // Raw pinned-host buffer for the continuation state image.

    std::vector<std::byte> state_host;

    std::size_t state_bytes = 0;



    // Checkpoint (turn-closure/rewrite) fallback. Follow-up prompts diverge

    // from the ledger exactly where the previous turn's generation began

    // (reasoning/replay drops), so the full execution_frontier never matches

    // them. The checkpoint frontier is where the prompt still matches; its

    // state image rides along so a checkpoint-level restore is exact.

    bool checkpoint_valid     = false;

    std::uint32_t checkpoint_frontier = 0;

    std::vector<std::byte> checkpoint_state_host;

    std::size_t checkpoint_state_bytes = 0;



    // Timestamp for LRU eviction.

    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();



    // Pin flag: when set, the spill's LRU eviction loop skips this entry.

    // Used by the restore flow to prevent eviction during the reserve→start

    // pipeline without removing the entry from the net (which would hold

    // arena bytes hostage as a taken entry).

    bool pinned = false;



    // Unique ID for stable reference across vector modifications.

    // pin() stores this ID; take_pinned() re-finds the entry by ID.

    std::uint64_t entry_id = 0;



    // Session key from the catalog entry at spill time. Used as a fallback

    // for thinking-mode follow-ups where prefix matching fails (preserve_thinking=off

    // drops reasoning from all turns, diverging the prompt from the ledger at the

    // first reasoning point). The session key lets the safety-find match by

    // session identity instead of prefix content.

    std::optional<qwen3_6::PreparedSessionKey> session_key;



    // Compact prefix (token IDs without reasoning) for matching thinking-mode

    // follow-ups. When non-empty, safety-find uses this instead of the full

    // ledger for prefix matching.

    std::vector<TokenId> compact_prefix;

};



// Result of find(): which entry matched, how many tokens are reusable, and

// whether the match is at the checkpoint frontier (state selection differs).

struct HostKVSafetyNetMatch {

    std::size_t index         = 0;

    std::uint32_t reuse_tokens = 0;

    bool checkpoint           = false;

};



class HostKVSafetyNet {

public:

    HostKVSafetyNet() = default;





    HostKVSafetyNet(const HostKVSafetyNet&)            = delete;

    HostKVSafetyNet& operator=(const HostKVSafetyNet&) = delete;

    HostKVSafetyNet(HostKVSafetyNet&&)                 = default;

    HostKVSafetyNet& operator=(HostKVSafetyNet&&)      = default;



    // Find entry whose prefix_identity and ledger match the given prompt for `count` tokens.

    // Returns the index of the best (longest) match, or std::nullopt if none.

    // Two-level find, the old HostKvCache pattern: try the full execution

    // frontier first (prompt extends the cached turn), then the checkpoint

    // frontier (prompt rewinds: reasoning/replay dropped the generated turn).

    [[nodiscard]] std::optional<HostKVSafetyNetMatch>

    find(const PreparedPromptData& prompt, std::size_t max_count,

         const std::optional<qwen3_6::PreparedSessionKey>& session_key = {}) const {

        std::optional<HostKVSafetyNetMatch> best;

        {

            std::uint64_t prompt_hash = 1469598103934665603ULL;

            for (const TokenId t : prompt.token_ids) {

                prompt_hash ^= static_cast<std::uint64_t>(t);

                prompt_hash *= 1099511628211ULL;

            }

            std::fprintf(stderr,

                         "[safety-find] incoming: prompt_tokens=%zu prompt_hash=%llu max_count=%zu\n",

                         prompt.token_ids.size(),

                         static_cast<unsigned long long>(prompt_hash), max_count);

        }

        if (session_key) {

            std::fprintf(stderr,

                         "[safety-find] incoming session_key set: view=%.*s\n",

                         static_cast<int>(session_key->view().size()),

                         session_key->view().data());

        } else {

            std::fprintf(stderr,

                         "[safety-find] incoming session_key NOT SET (nullopt)\n");

        }

        for (std::size_t index = 0; index < entries_.size(); ++index) {

            const HostKVSafetyNetEntry& entry = entries_[index];

            {

                std::uint64_t cp_hash = 1469598103934665603ULL;

                for (const TokenId t : entry.compact_prefix) {

                    cp_hash ^= static_cast<std::uint64_t>(t);

                    cp_hash *= 1099511628211ULL;

                }

                std::fprintf(stderr,

                             "[safety-find] entry %zu: exec_frontier=%u ckpt_frontier=%u cp_size=%zu cp_hash=%llu ledger_size=%zu has_sk=%d\n",

                             index, entry.execution_frontier, entry.checkpoint_frontier,

                             entry.compact_prefix.size(),

                             static_cast<unsigned long long>(cp_hash),

                             entry.ledger.size(),

                             entry.session_key ? 1 : 0);

            }

            const std::size_t count =

                std::min(max_count, static_cast<std::size_t>(entry.execution_frontier));

            const std::size_t effective_count =
                entry.compact_prefix.empty() ? count
                    : std::min(count, entry.compact_prefix.size());

            std::uint32_t reuse = 0;

            bool checkpoint = false;

            if (effective_count != 0 &&

                prefix_matches(prompt,

                               entry.compact_prefix.empty()

                                   ? std::span<const TokenId>(entry.ledger.data(), effective_count)

                                   : std::span<const TokenId>(entry.compact_prefix.data(), effective_count),

                               entry.prefix_identity, effective_count)) {

                reuse = static_cast<std::uint32_t>(effective_count);

            } else if (entry.checkpoint_valid && entry.checkpoint_frontier != 0 &&

                       entry.checkpoint_frontier <= max_count &&

                       static_cast<std::size_t>(entry.checkpoint_frontier) <= entry.ledger.size() &&

                       prefix_matches(prompt,

                                      entry.compact_prefix.empty()

                                          ? std::span<const TokenId>(entry.ledger.data(), entry.checkpoint_frontier)

                                          : std::span<const TokenId>(entry.compact_prefix.data(), std::min(static_cast<std::size_t>(entry.checkpoint_frontier), entry.compact_prefix.size())),

                                      entry.prefix_identity, entry.checkpoint_frontier)) {

                reuse = entry.checkpoint_frontier;

                checkpoint = true;

            }

            // Session-key fallback: if prefix matching failed (thinking mode

            // drops reasoning from all turns, diverging the prompt from the

            // ledger at the first reasoning point), match by session identity.

            // The session key confirms this is the right continuation; use the

            // checkpoint frontier as the reuse level (the turn boundary where

            // the stored context still matches the prompt).

            if (reuse == 0 && session_key && entry.session_key) {

                const bool keys_match = (*session_key == *entry.session_key);

                std::fprintf(stderr,

                             "[safety-find] session-key compare: match=%d incoming_view=%.*s entry_view=%.*s "

                             "frontier=%u ckpt_valid=%d ckpt_frontier=%u max_count=%zu\n",

                             static_cast<int>(keys_match),

                             static_cast<int>(session_key->view().size()), session_key->view().data(),

                             static_cast<int>(entry.session_key->view().size()), entry.session_key->view().data(),

                             entry.execution_frontier,

                             static_cast<int>(entry.checkpoint_valid),

                             entry.checkpoint_frontier, max_count);

                if (keys_match &&

                    entry.checkpoint_valid && entry.checkpoint_frontier != 0 &&

                    entry.checkpoint_frontier < max_count) {

                    reuse = entry.checkpoint_frontier;

                    checkpoint = true;

                    std::fprintf(stderr,

                                 "[safety-find] session-key HIT: frontier=%u checkpoint=%u\n",

                                 entry.execution_frontier, entry.checkpoint_frontier);

                } else if (keys_match) {

                    std::fprintf(stderr,

                                 "[safety-find] session-key match but conditions fail: "

                                 "ckpt_valid=%d ckpt_frontier=%u max_count=%zu\n",

                                 static_cast<int>(entry.checkpoint_valid),

                                 entry.checkpoint_frontier, max_count);

                }

            } else if (reuse == 0 && !session_key) {

                // Only log once per find() call

            }

            if (reuse == 0 && session_key && !entry.session_key) {

                std::fprintf(stderr,

                             "[safety-find] entry has no session_key (index=%zu frontier=%u)\n",

                             index, entry.execution_frontier);

            }

            if (reuse == 0 && !session_key) {

                // Incoming request has no session_key — can't do session-based fallback

            }

            // reuse == max_count (the entire prompt is the cached prefix,

            // e.g. a strict-prefix retry) leaves no target tail for prefill

            // and throws "zero-suffix reuse" downstream — reject it and let

            // the request prefill from scratch instead of failing.

            if (reuse != 0 && reuse != max_count && (!best || reuse > best->reuse_tokens)) {

                best = HostKVSafetyNetMatch{

                    .index = index, .reuse_tokens = reuse, .checkpoint = checkpoint};

            }

        }

        // One line per find() call, but only when something is in the net —

        // an empty net on a root admission is the common no-op case.

        if (!entries_.empty() || best) {

            std::fprintf(stderr,

                         "[safety-find] entries=%zu max_count=%zu match=%s frontier=%u checkpoint=%d\n",

                         entries_.size(), max_count, best ? "hit" : "miss",

                         best ? best->reuse_tokens : 0,

                         best ? static_cast<int>(best->checkpoint) : 0);

        }

        

        return best;

    }



    // Add a new entry. The safety net is bounded by host KV arena bytes,

    // not entry count — the spill function evicts smallest unpinned entries

    // to free arena memory before calling add(). A re-added entry (after

    // restore) already owns its arena allocation, so no eviction is needed.

    void add(HostKVSafetyNetEntry entry) {

        entry.entry_id = ++next_entry_id_;

        entry.pinned = false;  // re-added entries are unpinned

        entries_.push_back(std::move(entry));

    }



    // Take ownership of an entry, removing it from the store.

    [[nodiscard]] HostKVSafetyNetEntry take(std::size_t index) {

        if (index >= entries_.size()) {

            throw std::out_of_range("Host KV safety net index is out of range");

        }

        HostKVSafetyNetEntry out = std::move(entries_[index]);

        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));

        return out;

    }



    // Get a const reference to an entry.

    [[nodiscard]] const HostKVSafetyNetEntry& at(std::size_t index) const {

        if (index >= entries_.size()) {

            throw std::out_of_range("Host KV safety net index is out of range");

        }

        return entries_[index];

    }



    // Remove an entry (frees host KV allocations via their destructors).

    void remove(std::size_t index) {

        if (index >= entries_.size()) { return; }

        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));

    }



    // Pin an entry to prevent LRU eviction during the restore pipeline.

    // The entry stays in the net (arena bytes remain counted as net entries,

    // not held hostage as a taken entry). Returns the entry's unique ID for

    // later take_pinned/unpin — the index may shift due to vector mutations

    // from spill evictions, but the ID is stable.

    [[nodiscard]] std::uint64_t pin(std::size_t index) {

        if (index >= entries_.size()) { return 0; }

        entries_[index].pinned = true;

        return entries_[index].entry_id;

    }

    void unpin(std::uint64_t id) {

        for (auto& e : entries_) {

            if (e.entry_id == id) { e.pinned = false; return; }

        }

    }



    // Remove and return a pinned entry by its stable ID (used after H2D copy

    // to re-add with refreshed timestamp). The ID survives vector mutations

    // (spill evictions shifting indices) between pin() and take_pinned().

    [[nodiscard]] HostKVSafetyNetEntry take_pinned(std::uint64_t id) {

        for (auto it = entries_.begin(); it != entries_.end(); ++it) {

            if (it->entry_id == id) {

                HostKVSafetyNetEntry out = std::move(*it);

                entries_.erase(it);

                return out;

            }

        }

        throw std::out_of_range("Host KV safety net pinned entry ID not found");

    }



    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }



    void clear() noexcept { entries_.clear(); }



private:

    std::vector<HostKVSafetyNetEntry> entries_;

    std::uint64_t next_entry_id_ = 0;

};



} // namespace ninfer::targets::qwen3_6::detail
