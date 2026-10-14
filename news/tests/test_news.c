/* News text and feed reader tests. Returns the failing line, 0 if all pass. */
#include "rss.h"
#include "text.h"

#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static const char s_rss[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><rss version=\"2.0\"><channel><title>ANSA</title>"
    "<item><title><![CDATA[Iran, attacco a Teheran: l&#8217;Ue condanna]]></title>"
    "<link>https://www.ansa.it/a.html</link>"
    "<description><![CDATA[<p>Il <b>governo</b> &egrave; riunito.</p>]]></description>"
    "<pubDate>Wed, 14 Oct 2026 16:05:00 +0200</pubDate><guid>x1</guid></item>"
    "<item><title>Città &amp; cinema: il festival</title><link>https://www.ansa.it/b.html</link>"
    "<pubDate>Wed, 14 Oct 2026 08:00:00 GMT</pubDate></item>"
    "</channel></rss>";

static const char s_atom[] =
    "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>INAF</title>"
    "<entry><title type=\"html\">Una nuova cometa</title>"
    "<link rel=\"replies\" href=\"https://x/comments\"/><link rel=\"alternate\" href=\"https://x/cometa\"/>"
    "<updated>2026-10-13T22:30:00Z</updated><summary>Scoperta &lt;rara&gt;</summary></entry></feed>";

static rss_item_t s_items[4];
static int s_count;
static char s_window[1024];

static bool collect(const rss_item_t *item, void *ctx)
{
    (void)ctx;
    if (s_count < 4)
    {
        s_items[s_count++] = *item;
    }
    return true;
}

static int feed_in_chunks(const char *xml, size_t chunk)
{
    rss_reader_t reader;
    s_count = 0;
    rss_init(&reader, s_window, sizeof(s_window), collect, NULL);
    size_t length = strlen(xml);
    for (size_t at = 0; at < length; at += chunk)
    {
        rss_feed(&reader, xml + at, length - at < chunk ? length - at : chunk);
    }
    return s_count;
}

static int has_key(const nw_key_t *keys, int count, const char *word)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(keys[i].word, word) == 0)
        {
            return 1;
        }
    }
    return 0;
}

int news_tests(void)
{
    /* Dates: RFC 822 with zone, GMT, ISO with Z and offset. */
    CHECK(nw_date("Wed, 14 Oct 2026 16:05:00 +0200") == 1791986700);
    CHECK(nw_date("14 Oct 2026 14:05:00 GMT") == 1791986700);
    CHECK(nw_date("2026-10-14T14:05:00Z") == 1791986700);
    CHECK(nw_date("2026-10-14T16:05:00.000+02:00") == 1791986700);
    CHECK(nw_date("mer, 14 ott 2026 16:05:00 +0200") == 1791986700);
    CHECK(nw_date("not a date") == 0);

    /* Cleaning: CDATA, tags, entities, spaces. */
    char text[96];
    strcpy(text, "  <![CDATA[<p>Il <b>governo</b> &egrave;   riunito &amp; l&#8217;Ue</p>]]> ");
    nw_clean(text);
    CHECK(strcmp(text, "Il governo \xc3\xa8 riunito & l'Ue") == 0);

    /* Keywords: stop words out, acronyms in, accents folded, same stem once. */
    nw_key_t keys[NW_KEY_MAX];
    int n = nw_keywords("Iran, attacco a Teheran: l'UE condanna l'attacco iraniano", keys, NW_KEY_MAX);
    CHECK(has_key(keys, n, "iran"));
    CHECK(has_key(keys, n, "attacco"));
    CHECK(has_key(keys, n, "teheran"));
    CHECK(has_key(keys, n, "ue"));
    CHECK(has_key(keys, n, "condanna"));
    CHECK(n == 6);   /* iran, attacco, teheran, ue, condanna, iraniano (stem "iranan" differs from "iran") */
    n = nw_keywords("Della nuova città, oggi", keys, NW_KEY_MAX);
    CHECK(n == 1 && strcmp(keys[0].word, "citt\xc3\xa0") == 0);
    CHECK(nw_stem_hash("Città") == keys[0].hash);
    CHECK(nw_stem_hash("della") == 0);
    CHECK(nw_stem_hash("Teheran") == nw_stem_hash("teherani"));

    /* RSS, whole and in tiny chunks (tags cut between chunks). */
    for (size_t chunk = 1; chunk <= 64; chunk = chunk * 3 + 1)
    {
        CHECK(feed_in_chunks(s_rss, chunk) == 2);
        CHECK(strcmp(s_items[0].title, "Iran, attacco a Teheran: l'Ue condanna") == 0);
        CHECK(strcmp(s_items[0].link, "https://www.ansa.it/a.html") == 0);
        CHECK(strcmp(s_items[0].summary, "Il governo \xc3\xa8 riunito.") == 0);
        CHECK(s_items[0].when == 1791986700);
        CHECK(strcmp(s_items[1].title, "Citt\xc3\xa0 & cinema: il festival") == 0);
    }

    /* Atom: the alternate link, the summary's escaped text. */
    CHECK(feed_in_chunks(s_atom, 5) == 1);
    CHECK(strcmp(s_items[0].link, "https://x/cometa") == 0);
    CHECK(strcmp(s_items[0].summary, "Scoperta <rara>") == 0);
    CHECK(s_items[0].when == nw_date("2026-10-13T22:30:00Z"));

    /* UTF-8 safe copy. */
    char small[8];
    nw_copy(small, 7, "Città bella");
    CHECK(strcmp(small, "Citt\xc3\xa0") == 0);
    nw_copy(small, 5, "Citt\xc3\xa0");
    CHECK(strcmp(small, "Citt") == 0);
    return 0;
}

#ifdef NEWS_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call news_tests\n ebreak\n .option pop\n");
}
#else
#include <stdio.h>
int main(void)
{
    int line = news_tests();
    if (line) printf("FAIL at line %d\n", line);
    return line;
}
#endif
