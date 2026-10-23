// SHA-256 (runtime/cache_sha256.h): NIST/FIPS vectors, the 55/56/64-byte padding boundaries, and incremental
// updates that split the input at every position. The build cache's integrity ids depend on this being exact.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cache_sha256.h"
#include "harness.h"

static std::string hex_of(const void *p, size_t n) {
    CacheSha256 h;
    h.add(p, n);
    return h.finish();
}

static void check(const std::string &msg, const char *want, const char *what) {
    std::string got = hex_of(msg.data(), msg.size());
    if (got != want) {
        t_failures++;
        fprintf(stderr, "FAIL sha256(%s): %s\n  got  %s\n  want %s\n", what, msg.size() > 32 ? "<long>" : msg.c_str(),
                got.c_str(), want);
    }
    t_checks++;
}

static void test_vectors(void) {
    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "FIPS 448-bit");
    check(
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "FIPS 896-bit");
    check(std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "one million a");
}

// The length field's block: 55 bytes fit one block, 56 need the extra one.
static void test_padding_boundaries(void) {
    static const struct {
        int n;
        const char *hex;
    } cases[] = {
        {54, "675f28acc0b90a72d1c3a570fe83ac565555db358cf01826dc8eefb2bf7ca0f3"},
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {57, "2fe741af801cc238602ac0ec6a7b0c3a8a87c7fc7d7f02a3fe03d1c12eac4d8f"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {119, "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
        {120, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
        {127, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        {129, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
    };
    for (const auto &c : cases) {
        std::vector<uint8_t> msg((size_t)c.n);
        for (int i = 0; i < c.n; i++) msg[(size_t)i] = (uint8_t)i;
        std::string got = hex_of(msg.data(), msg.size());
        if (got != c.hex) {
            t_failures++;
            fprintf(stderr, "FAIL sha256(%d bytes): %s != %s\n", c.n, got.c_str(), c.hex);
        }
        t_checks++;
    }
}

// The same input split at every byte: the streaming state must carry across add() calls.
static void test_incremental(void) {
    const std::string msg = "the quick brown fox jumps over the lazy dog, and then some more bytes to cross a block";
    const std::string want = hex_of(msg.data(), msg.size());
    for (size_t cut = 0; cut <= msg.size(); cut++) {
        CacheSha256 h;
        h.add(msg.data(), cut);
        h.add(msg.data() + cut, msg.size() - cut);
        if (h.finish() != want) {
            t_failures++;
            fprintf(stderr, "FAIL sha256 incremental at %zu\n", cut);
        }
        t_checks++;
    }
    // Many small adds, including zero-length ones.
    CacheSha256 h;
    for (size_t i = 0; i < msg.size(); i++) {
        h.add("", 0);
        h.add(msg.data() + i, 1);
    }
    std::string got = h.finish();
    CHECK_STR(got.c_str(), want.c_str());
}

int main() {
    test_vectors();
    test_padding_boundaries();
    test_incremental();
    TEST_END();
}
