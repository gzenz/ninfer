// The derived session key (V2's 613171bd, ported to v3): the ONLY thing that makes the Messages path's units
// session-keyed, and therefore the thing that gives a conversation a shared reuse domain and LiveSession
// retention. Four properties, and each one is load-bearing:
//
//   1. STABLE across a conversation's turns -- the preimage is the system prompt plus the FIRST user turn, so
//      a later turn must not change the key. If it did, every turn would be its own "session" and the fix
//      would be a no-op wearing the right name.
//   2. DISTINCT conversations differ -- two first-user turns must not collide.
//   3. NO USER TEXT means NO KEY -- V2's explicit choice to keep the pre-fix behaviour rather than invent a
//      key for an image-only first turn, where the preimage would be a system prompt shared by every such
//      request (which would put unrelated conversations in ONE domain -- worse than none).
//   4. THE SEPARATOR MAKES BOUNDARIES UNAMBIGUOUS -- system "ab" + user "c" must not collide with system "a"
//      + user "bc". Without the 0x1f unit separator those two concatenate identically.

#include "serve/request.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (condition) { std::printf("ok   %s\n", name); }
    else { std::printf("FAIL %s\n", name); ++failures; }
}

using ninfer::ChatRole;              // include/ninfer/types.h
using ninfer::serve::ChatTurn;
using ninfer::serve::ContentKind;    // serve/request.h
using ninfer::serve::ContentPart;

ChatTurn turn(ChatRole role, const std::string& text) {
    ChatTurn value;
    value.role = role;
    ContentPart part;
    part.kind = ContentKind::Text;
    part.text = text;
    value.content.push_back(std::move(part));
    return value;
}
}  // namespace

int main() {
    // 1. Stable across turns: same system + same first user, different later turns.
    const std::vector<ChatTurn> early = {turn(ChatRole::System, "S"), turn(ChatRole::User, "U1")};
    const std::vector<ChatTurn> later = {turn(ChatRole::System, "S"), turn(ChatRole::User, "U1"),
                                         turn(ChatRole::Assistant, "A1"), turn(ChatRole::User, "U2")};
    const auto a = ninfer::serve::derive_session_key(early);
    const auto b = ninfer::serve::derive_session_key(later);
    check(a.has_value() && b.has_value() && *a == *b,
          "the key is stable across a conversation's turns (this is the fix, or it is a no-op)");

    // 2. Distinct conversations differ.
    const std::vector<ChatTurn> other = {turn(ChatRole::System, "S"), turn(ChatRole::User, "U1-different")};
    const auto c = ninfer::serve::derive_session_key(other);
    check(c.has_value() && *c != *a, "a different first user turn yields a different key");

    // 3. No user text -> no key (image-only first turn).
    const std::vector<ChatTurn> no_user = {turn(ChatRole::System, "S")};
    check(!ninfer::serve::derive_session_key(no_user).has_value(),
          "no user text yields NO key rather than a key shared by every such request");
    ChatTurn image_only;
    image_only.role = ChatRole::User;
    ContentPart image;
    image.kind = ContentKind::Image;
    image_only.content.push_back(image);
    check(!ninfer::serve::derive_session_key(std::vector<ChatTurn>{image_only}).has_value(),
          "a user turn with no TEXT part yields no key either");

    // 4. The separator keeps the system/user boundary unambiguous.
    const std::vector<ChatTurn> split_a = {turn(ChatRole::System, "ab"), turn(ChatRole::User, "c")};
    const std::vector<ChatTurn> split_b = {turn(ChatRole::System, "a"), turn(ChatRole::User, "bc")};
    const auto sa = ninfer::serve::derive_session_key(split_a);
    const auto sb = ninfer::serve::derive_session_key(split_b);
    check(sa.has_value() && sb.has_value() && *sa != *sb,
          "system 'ab'+user 'c' does not collide with system 'a'+user 'bc' (the unit separator)");

    // 5. THE SYSTEM PROMPT IS IN THE PREIMAGE. A mutant that drops system text entirely, and one that hashes
    //    only the FIRST system turn, both SURVIVED every case above -- so nothing pinned the half of the
    //    preimage that the whole derivation is named for. Two conversations with the same first user turn and
    //    different system prompts must not share a key; if they do, every such request is one "session" and
    //    the retention and reuse-domain reasoning that depends on the key is wrong for all of them.
    {
        const std::vector<ChatTurn> sys_a = {turn(ChatRole::System, "SYSTEM-A"), turn(ChatRole::User, "U")};
        const std::vector<ChatTurn> sys_b = {turn(ChatRole::System, "SYSTEM-B"), turn(ChatRole::User, "U")};
        const auto ka = ninfer::serve::derive_session_key(sys_a);
        const auto kb = ninfer::serve::derive_session_key(sys_b);
        check(ka.has_value() && kb.has_value() && *ka != *kb,
              "the system prompt is in the preimage (same user, different system => different key)");

        // And MORE THAN ONE system turn contributes: an agentic request can carry several, and a key that
        // ignored the later ones would merge conversations that differ only there.
        const std::vector<ChatTurn> two_a = {turn(ChatRole::System, "S1"), turn(ChatRole::System, "S2"),
                                             turn(ChatRole::User, "U")};
        const std::vector<ChatTurn> two_b = {turn(ChatRole::System, "S1"), turn(ChatRole::System, "S3"),
                                             turn(ChatRole::User, "U")};
        const auto ta = ninfer::serve::derive_session_key(two_a);
        const auto tb = ninfer::serve::derive_session_key(two_b);
        check(ta.has_value() && tb.has_value() && *ta != *tb,
              "every system turn contributes, not only the first");
    }

    if (failures != 0) { std::printf("%d FAILURE(S)\n", failures); return 1; }
    std::printf("all derived-session-key checks passed\n");
    return 0;
}
