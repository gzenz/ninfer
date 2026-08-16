// Verify the turn-closure checkpoint tracks the LAST reasoning-stripped assistant
// turn (so it stays near the frontier) rather than the first.
#include "targets/qwen3_6/impl/frontend/chat_template.h"
#include <fstream>
#include <iostream>
#include <sstream>

namespace fi = ninfer::targets::qwen3_6::frontend_internal;

static std::string fixture(const char* p) {
    std::ifstream in(p); std::ostringstream ss; ss << in.rdbuf(); return ss.str();
}
static int check(bool ok, const char* label) {
    if (!ok) std::cerr << "FAIL: " << label << "\n";
    return ok ? 0 : 1;
}
static fi::ChatMessage msg(ninfer::ChatRole r, std::string t) {
    fi::ChatMessage m; m.role = r; m.parts.push_back(fi::ChatPart::text_part(std::move(t))); return m;
}

int main() {
    const std::string src = fixture(
        NINFER_SOURCE_DIR "/tests/fixtures/frontend/froggeric_v22_chat_template.jinja");
    fi::CompiledChatTemplate tpl = fi::CompiledChatTemplate::resolve(src);
    int f = 0;

    // One user query followed by several assistant/user exchanges. With
    // preserve_thinking=false every assistant turn past the query is stripped.
    std::vector<fi::ChatMessage> msgs;
    msgs.push_back(msg(ninfer::ChatRole::User, "do a multi-step task"));
    for (int i = 0; i < 4; ++i) {
        msgs.push_back(msg(ninfer::ChatRole::Assistant, "step " + std::to_string(i)));
        msgs.push_back(msg(ninfer::ChatRole::Tool, "result " + std::to_string(i)));
    }

    fi::ChatRenderOptions o;
    o.preserve_thinking = false;
    o.enable_thinking   = false;
    o.add_generation_prompt = true;
    fi::RenderedChat out = tpl.render(msgs, o);

    f += check(out.rewrite_checkpoint.has_value(), "checkpoint was produced");
    if (out.rewrite_checkpoint) {
        const std::size_t off = out.rewrite_checkpoint->offset;
        const std::size_t total = out.text.size();
        std::cout << "checkpoint offset=" << off << " / total=" << total
                  << "  (" << (100 * off / (total ? total : 1)) << "% through prompt)\n";
        // The first stripped assistant turn sits early; the last sits near the end.
        // Require the anchor to be in the latter half - i.e. it tracked the last turn.
        f += check(off * 2 > total, "checkpoint is in the LATTER half (tracks last turn)");
    }

    // Sanity: with preserve_thinking=true there should be a ResponseReplay
    // checkpoint at the generation prologue instead (unchanged behaviour).
    fi::ChatRenderOptions o2;
    o2.preserve_thinking = true;
    o2.enable_thinking   = false;
    o2.add_generation_prompt = true;
    fi::RenderedChat out2 = tpl.render(msgs, o2);
    f += check(out2.rewrite_checkpoint.has_value(), "preserve_thinking=true still checkpoints");

    if (f == 0) std::cout << "CHECKPOINT PLACEMENT OK\n";
    return f == 0 ? 0 : 1;
}
