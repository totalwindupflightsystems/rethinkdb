// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_FTS_TOKENIZER_HPP_
#define RDB_PROTOCOL_FTS_TOKENIZER_HPP_

#include <re2/re2.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace ql {

/* `fts_tokenizer_t` splits text into searchable tokens for GIN-style full-text indexes.
 *
 * Pipeline: input text → regex tokenization → lowercase → stop-word filter → Porter stemming
 *
 * The tokenizer uses re2 (already bundled in RethinkDB) for regex pattern matching
 * and a self-contained Porter stemmer implementation (no external dependency).
 */
class fts_tokenizer_t {
public:
    /* Construct a tokenizer with default settings:
     *   - min_token_length: 2
     *   - language: "english"
     *   - stop_words: default English stop word list */
    fts_tokenizer_t();

    /* Construct a tokenizer with custom settings. */
    explicit fts_tokenizer_t(
        size_t min_token_length,
        const std::string &language = "english",
        const std::unordered_set<std::string> &extra_stop_words = {});

    /* Tokenize text into stemmed search tokens. */
    std::vector<std::string> tokenize(const std::string &text) const;

    /* Tokenize text into tokens without stemming (for prefix queries). */
    std::vector<std::string> tokenize_raw(const std::string &text) const;

    /* Set minimum token length (default: 2). Tokens shorter than this are discarded. */
    void set_min_token_length(size_t len) { min_token_length_ = len; }
    size_t min_token_length() const { return min_token_length_; }

    /* Set language for stemming (default: "english"). Only English is supported currently. */
    void set_language(const std::string &lang) { language_ = lang; }
    const std::string &language() const { return language_; }

    /* Add custom stop words. */
    void add_stop_words(const std::unordered_set<std::string> &words);
    void clear_stop_words();

    /* Get the current stop word set. */
    const std::unordered_set<std::string> &stop_words() const { return stop_words_; }

private:
    /* Porter stemming algorithm — self-contained implementation. */
    static std::string porter_stem(const std::string &word);

    /* Check if a word is a stop word (case-insensitive). */
    bool is_stop_word(const std::string &word) const;

    /* Tokenization regex pattern. */
    re2::RE2 token_re_;

    /* Configuration. */
    size_t min_token_length_;
    std::string language_;
    std::unordered_set<std::string> stop_words_;

    /* Default English stop words. */
    static const std::unordered_set<std::string> &default_stop_words();
};

}  // namespace ql

#endif  // RDB_PROTOCOL_FTS_TOKENIZER_HPP_
