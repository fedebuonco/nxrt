// shader_facts.tsv parser (runtime/nvn/shader_facts.c).
#include <unistd.h>

#include "harness.h"
#include "shader_facts.h"

int main(void) {
    ShaderFact f;
    CHECK(shader_fact_parse("147c829e79fb322f\ta1=float,a2=uint,a3=sint,r0=uint", &f));
    CHECK_EQ(f.hash, 0x147c829e79fb322full);
    CHECK(f.attr_mask & (1u << 1));
    CHECK(f.attr_mask & (1u << 2));
    CHECK(f.attr_mask & (1u << 3));
    CHECK_EQ(f.attr_kind[1], 0);
    CHECK_EQ(f.attr_kind[2], 2);
    CHECK_EQ(f.attr_kind[3], 1);
    CHECK(f.rt_mask & 1u);
    CHECK_EQ(f.rt_kind[0], 2);
    CHECK(!(f.attr_mask & (1u << 0)));
    CHECK(!(f.rt_mask & (1u << 1)));

    // Whitespace and short names.
    CHECK(shader_fact_parse("  deadbeef  a0=f r7=u  ", &f));
    CHECK_EQ(f.hash, 0xdeadbeefull);
    CHECK_EQ(f.attr_kind[0], 0);
    CHECK_EQ(f.rt_kind[7], 2);

    // Comments, blank lines, and a line with no facts are skipped.
    CHECK(!shader_fact_parse("", &f));
    CHECK(!shader_fact_parse("   ", &f));
    CHECK(!shader_fact_parse("# a comment", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\t", &f));

    // Malformed rows are rejected, not partially applied.
    CHECK(!shader_fact_parse("not-a-hash\ta1=float", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\ta32=float", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\tr8=uint", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\ta1=double", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\ta1", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\tx1=float", &f));
    CHECK(!shader_fact_parse("147c829e79fb322f\ta1=float,", &f));

    // Lookup.
    ShaderFact table[2];
    CHECK(shader_fact_parse("1111\ta0=float", &table[0]));
    CHECK(shader_fact_parse("2222\tr0=sint", &table[1]));
    CHECK(shader_fact_find(table, 2, 0x1111) == &table[0]);
    CHECK(shader_fact_find(table, 2, 0x2222) == &table[1]);
    CHECK(shader_fact_find(table, 2, 0x3333) == NULL);

    // File load: comments, malformed rows and duplicates are skipped, valid rows kept.
    char path[] = "/tmp/nxrt_facts_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char *text = "# comment\n1111\ta0=float\nbad row\n2222\tr0=uint\n";
        CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        close(fd);
        ShaderFact loaded[4];
        CHECK_EQ(shader_facts_load(path, loaded, 4), 2);
        CHECK(loaded[0].hash == 0x1111 && loaded[1].hash == 0x2222);
        CHECK_EQ(shader_facts_load(path, loaded, 1), 1);
        unlink(path);
    }

    // Apply only what the fact pins.
    uint8_t attr[SHADER_FACT_ATTRS], rt[SHADER_FACT_RTS];
    memset(attr, 0xff, sizeof attr);
    memset(rt, 0xff, sizeof rt);
    CHECK(shader_fact_parse("3333\ta1=uint,r0=sint", &f));
    shader_fact_apply(&f, attr, rt);
    CHECK_EQ(attr[1], 2);
    CHECK_EQ(attr[0], 0xff);
    CHECK_EQ(rt[0], 1);
    CHECK_EQ(rt[1], 0xff);
    TEST_END();
}
