#ifndef NEWS_TEXT_H
#define NEWS_TEXT_H

/*
 * Text helpers for News (pure C, no ESP-IDF: tested on the host).
 *
 *   nw_clean()      HTML entities, tags, CDATA and spaces of a feed field
 *   nw_keywords()   the words that tell what a headline is about, as stems
 *                   ("Iran, attacco a Teheran" -> iran, attacc, teheran)
 *   nw_date()       RFC 822 (RSS) or ISO 8601 (Atom) date to unix time
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NW_KEY_MAX  12          /* keywords kept per headline */
#define NW_WORD     24          /* longest keyword text */

typedef struct
{
    uint32_t hash;              /* of the stem */
    char word[NW_WORD];         /* the word as written (lower case), for display */
} nw_key_t;

/* In place: decode entities (&amp; &#8217; &egrave;...), drop tags and CDATA
 * markers, collapse white space, trim. Returns the new length. */
size_t nw_clean(char *text);

/* Keywords of a headline: no stop words, at least 4 letters (or an acronym
 * in capitals such as "UE", "USA", "NATO"), accents folded, stem = the first
 * 6 letters. Unique, in order of appearance. Returns the count. */
int nw_keywords(const char *title, nw_key_t *out, int max);

/* Stem hash of one search word ("Iraniani" -> same as "iran..." stems). 0 if it is a stop word. */
uint32_t nw_stem_hash(const char *word);

/* Lower case ASCII, accents folded, anything else one space: "Città, l'UE" -> "citta l ue". */
void nw_fold(const char *in, char *out, size_t size);

/* Search words of a query as folded stem prefixes (max 6 letters, no stop
 * words): "la guerra in Iran" -> "guerra", "iran". Returns the count. */
int nw_needles(const char *query, char needles[][8], int max);

/* Every needle starts a word of the folded title. */
bool nw_match(const char *folded_title, char needles[][8], int count);

/* 32-bit FNV-1a. */
uint32_t nw_hash(const char *text);

/* "Tue, 14 Oct 2026 16:05:00 +0200" or "2026-10-14T16:05:00+02:00" -> unix
 * seconds (UTC). 0 if not understood. */
int64_t nw_date(const char *text);

/* Copy at most size-1 bytes without cutting a UTF-8 character. */
void nw_copy(char *out, size_t size, const char *in);

#endif
