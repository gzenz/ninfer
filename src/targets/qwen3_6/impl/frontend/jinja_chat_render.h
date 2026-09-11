#pragma once

// Executes the registered chat template source with the vendored jinja engine
// (third_party/jinja) and reconstructs the structured render description that the
// tokenizer and context-cache stages consume. The engine's render trace supplies the
// structure (per-message frontiers, printed-value provenance), so nothing is recovered
// by scanning the rendered text for markers that message content could forge.

#include "targets/qwen3_6/impl/frontend/chat_template.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::targets::qwen3_6::frontend_internal {

// A compiled template is immutable once built and safe to share across renders.
class JinjaChatTemplate {
public:
    // Throws std::invalid_argument when the template does not compile or is empty.
    [[nodiscard]] static JinjaChatTemplate compile(std::string source);

    [[nodiscard]] RenderedChat render(const std::vector<ChatMessage>& messages,
                                      const ChatRenderOptions& options) const;

    [[nodiscard]] const std::string& source() const noexcept;

private:
    struct Impl;
    explicit JinjaChatTemplate(std::shared_ptr<const Impl> impl);

    std::shared_ptr<const Impl> impl_;
};

} // namespace ninfer::targets::qwen3_6::frontend_internal
