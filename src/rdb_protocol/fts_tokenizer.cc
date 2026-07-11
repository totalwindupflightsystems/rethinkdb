// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/fts_tokenizer.hpp"

#include <re2/re2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace ql {

// ── Default English stop words ──
// Sourced from the standard SMART information retrieval system stop list.
const std::unordered_set<std::string> &fts_tokenizer_t::default_stop_words() {
    static const std::unordered_set<std::string> words = {
        "a", "an", "and", "are", "as", "at", "be", "but", "by", "for",
        "if", "in", "into", "is", "it", "no", "not", "of", "on", "or",
        "such", "that", "the", "their", "then", "there", "these", "they",
        "this", "to", "was", "will", "with", "i", "me", "my", "myself",
        "we", "our", "ours", "ourselves", "you", "your", "yours",
        "he", "him", "his", "himself", "she", "her", "hers", "herself",
        "it", "its", "itself", "they", "them", "their", "theirs",
        "themselves", "what", "which", "who", "whom", "this", "that",
        "these", "those", "am", "is", "are", "was", "were", "be",
        "been", "being", "have", "has", "had", "having", "do", "does",
        "did", "doing", "would", "could", "should", "ought", "might",
        "shall", "can", "need", "dare", "used", "must", "may",
        "here", "there", "when", "where", "why", "how", "all", "each",
        "every", "both", "few", "more", "most", "other", "some", "any",
        "only", "own", "same", "so", "than", "too", "very", "just",
        "because", "as", "until", "while", "about", "between", "through",
        "during", "before", "after", "above", "below", "from", "up",
        "down", "out", "off", "over", "under", "again", "further",
        "once", "here", "there", "when", "where", "why", "how",
        "which", "who", "whom", "what", "whose",
    };
    return words;
}

// ── Construction ──

fts_tokenizer_t::fts_tokenizer_t()
    : token_re_("(\\w+)"),           // re2: match word characters (capture group required for FindAndConsume)
      min_token_length_(2),
      language_("english"),
      stop_words_(default_stop_words())
{}

fts_tokenizer_t::fts_tokenizer_t(
    size_t min_token_length,
    const std::string &language,
    const std::unordered_set<std::string> &extra_stop_words)
    : token_re_("(\\w+)"),
      min_token_length_(min_token_length),
      language_(language),
      stop_words_(default_stop_words())
{
    for (const auto &w : extra_stop_words) {
        stop_words_.insert(w);
    }
}

// ── Public API ──

std::vector<std::string> fts_tokenizer_t::tokenize(const std::string &text) const {
    std::vector<std::string> result;

    if (text.empty()) return result;

    // Use re2 to find all word tokens
    re2::StringPiece input(text);
    std::string match;

    while (RE2::FindAndConsume(&input, token_re_, &match)) {
        if (match.empty()) continue;

        // Convert to lowercase
        std::string lower = match;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Filter short tokens
        if (lower.size() < min_token_length_) continue;

        // Filter stop words
        if (is_stop_word(lower)) continue;

        // Apply Porter stemming
        std::string stemmed = porter_stem(lower);
        if (stemmed.size() < min_token_length_) continue;

        result.push_back(std::move(stemmed));
    }

    return result;
}

std::vector<std::string> fts_tokenizer_t::tokenize_raw(const std::string &text) const {
    std::vector<std::string> result;

    if (text.empty()) return result;

    re2::StringPiece input(text);
    std::string match;

    while (RE2::FindAndConsume(&input, token_re_, &match)) {
        if (match.empty()) continue;

        std::string lower = match;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (lower.size() < min_token_length_) continue;

        result.push_back(std::move(lower));
    }

    return result;
}

void fts_tokenizer_t::add_stop_words(const std::unordered_set<std::string> &words) {
    for (const auto &w : words) {
        stop_words_.insert(w);
    }
}

void fts_tokenizer_t::clear_stop_words() {
    stop_words_.clear();
}

bool fts_tokenizer_t::is_stop_word(const std::string &word) const {
    return stop_words_.find(word) != stop_words_.end();
}

// ── Porter Stemmer ──
// Implementation based on the classic Porter stemming algorithm (Porter, 1980).
// "An algorithm for suffix stripping" - Program, 14(3): 130-137.
// Self-contained, no external dependencies.

namespace {

bool is_consonant(const std::string &word, size_t i) {
    switch (word[i]) {
    case 'a': case 'e': case 'i': case 'o': case 'u':
        return false;
    case 'y':
        return i == 0 || !is_consonant(word, i - 1);
    default:
        return true;
    }
}

// Measure of a word: count of VC (vowel-consonant) sequences
size_t measure(const std::string &word) {
    size_t m = 0;
    bool prev_vowel = false;
    bool in_vc = false;
    for (size_t i = 0; i < word.size(); ++i) {
        bool cons = is_consonant(word, i);
        if (prev_vowel && cons) {
            if (!in_vc) {
                ++m;
                in_vc = true;
            }
        } else if (!cons) {
            in_vc = false;
        }
        prev_vowel = !cons;
    }
    return m;
}

bool has_vowel(const std::string &word) {
    for (size_t i = 0; i < word.size(); ++i) {
        if (!is_consonant(word, i)) return true;
    }
    return false;
}

bool ends_with(const std::string &word, const std::string &suffix) {
    if (suffix.size() > word.size()) return false;
    return word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ends_double_consonant(const std::string &word) {
    if (word.size() < 2) return false;
    char c = word.back();
    if (c != word[word.size() - 2]) return false;
    return is_consonant(word, word.size() - 1);
}

bool ends_osc(const std::string &word) {
    // Ends with consonant-vowel-consonant (or consonant-vowel-consonant-like)
    // where the final consonant is not w, x, or y.
    if (word.size() < 3) return false;
    char c = word.back();
    if (c == 'w' || c == 'x' || c == 'y') return false;
    return is_consonant(word, word.size() - 1)
        && !is_consonant(word, word.size() - 2)
        && is_consonant(word, word.size() - 3);
}

std::string replace_suffix(const std::string &word, const std::string &old_sfx,
                          const std::string &new_sfx) {
    if (!ends_with(word, old_sfx)) return word;
    return word.substr(0, word.size() - old_sfx.size()) + new_sfx;
}

// Step 1a: plurals and -ed/-ing
std::string step_1a(const std::string &w) {
    std::string word = w;

    // SSES → SS
    if (ends_with(word, "sses")) return word.substr(0, word.size() - 2);
    // IES → I (with >1 letter)
    if (ends_with(word, "ies") && word.size() > 3) return word.substr(0, word.size() - 2);
    // SS → SS (keep)
    if (ends_with(word, "ss")) return word;
    // S → ∅ (if preceded by a vowel, not at end of short word)
    if (ends_with(word, "s") && word.size() > 2 && !ends_with(word, "ss")) {
        // Check there's a vowel before the 's'
        for (size_t i = 0; i < word.size() - 1; ++i) {
            if (!is_consonant(word, i)) {
                return word.substr(0, word.size() - 1);
            }
        }
    }

    return word;
}

// Step 1b: (e)d, -ing
std::string step_1b(const std::string &w) {
    std::string word = w;

    // (e)d → replace or ∅
    if (ends_with(word, "eed")) {
        if (measure(word.substr(0, word.size() - 3)) > 0) {
            word = word.substr(0, word.size() - 1); // eed → ee
        }
        return word;
    }

    bool done = false;
    if (ends_with(word, "ed")) {
        std::string stem = word.substr(0, word.size() - 2);
        if (has_vowel(stem)) {
            word = stem;
            done = true;
        }
    }
    if (!done && ends_with(word, "ing")) {
        std::string stem = word.substr(0, word.size() - 3);
        if (has_vowel(stem)) {
            word = stem;
            done = true;
        }
    }

    if (done) {
        // Additional transforms
        if (ends_with(word, "at") || ends_with(word, "bl") || ends_with(word, "iz")) {
            word += "e";
        } else if (ends_double_consonant(word) && word.back() != 'l'
                   && word.back() != 's' && word.back() != 'z') {
            word.pop_back();
        } else if (measure(word) == 1 && ends_osc(word)) {
            word += "e";
        }
    }

    return word;
}

// Step 1c: y → i
std::string step_1c(const std::string &w) {
    std::string word = w;
    if (word.size() > 2 && ends_with(word, "y") && !is_consonant(word, word.size() - 2)) {
        word.back() = 'i';
    }
    return word;
}

// Step 2: double suffixes
std::string step_2(const std::string &w) {
    static const char *pairs[][2] = {
        {"ational", "ate"}, {"tional", "tion"}, {"enci", "ence"}, {"anci", "ance"},
        {"izer", "ize"}, {"abli", "able"}, {"alli", "al"}, {"entli", "ent"},
        {"eli", "e"}, {"ousli", "ous"}, {"ization", "ize"}, {"ation", "ate"},
        {"ator", "ate"}, {"alism", "al"}, {"iveness", "ive"}, {"fulness", "ful"},
        {"ousness", "ous"}, {"aliti", "al"}, {"iviti", "ive"}, {"biliti", "ble"},
        {nullptr, nullptr}
    };
    std::string word = w;
    for (int i = 0; pairs[i][0] != nullptr; ++i) {
        std::string sfx(pairs[i][0]);
        if (ends_with(word, sfx)) {
            std::string stem = word.substr(0, word.size() - sfx.size());
            if (measure(stem) > 0) {
                return stem + pairs[i][1];
            }
            return word;
        }
    }
    if (ends_with(word, "logi")) {
        if (measure(word.substr(0, word.size() - 4)) > 0) {
            return word.substr(0, word.size() - 1); // logi → log
        }
    }
    return word;
}

// Step 3: additional suffixes
std::string step_3(const std::string &w) {
    static const char *pairs[][2] = {
        {"icate", "ic"}, {"ative", ""}, {"alize", "al"}, {"iciti", "ic"},
        {"ical", "ic"}, {"ful", ""}, {"ness", ""},
        {nullptr, nullptr}
    };
    std::string word = w;
    for (int i = 0; pairs[i][0] != nullptr; ++i) {
        std::string sfx(pairs[i][0]);
        if (ends_with(word, sfx)) {
            std::string stem = word.substr(0, word.size() - sfx.size());
            if (measure(stem) > 0) {
                return stem + pairs[i][1];
            }
            return word;
        }
    }
    return word;
}

// Step 4: -ant, -ence, etc.
std::string step_4(const std::string &w) {
    static const char *suffixes[] = {
        "al", "ance", "ence", "er", "ic", "able", "ible", "ant",
        "ement", "ment", "ent", "ism", "ate", "iti", "ous",
        "ive", "ize",
        nullptr
    };
    std::string word = w;
    for (int i = 0; suffixes[i] != nullptr; ++i) {
        std::string sfx(suffixes[i]);
        // Must have measure > 1 for removal
        if (ends_with(word, sfx) && word.size() > sfx.size()) {
            std::string stem = word.substr(0, word.size() - sfx.size());
            if (measure(stem) > 1) {
                return stem;
            }
        }
    }
    // Special: remove final e if measure > 1
    if (ends_with(word, "e") && measure(word.substr(0, word.size() - 1)) > 1) {
        return word.substr(0, word.size() - 1);
    }
    return word;
}

// Step 5: final tidying
std::string step_5(const std::string &w) {
    std::string word = w;
    // e removal and double-l reduction
    if (ends_with(word, "e")) {
        std::string stem = word.substr(0, word.size() - 1);
        size_t m = measure(stem);
        if (m > 1 || (m == 1 && !ends_osc(stem))) {
            return stem;
        }
    }
    if (ends_with(word, "ll") && measure(word) > 1) {
        word.pop_back();
    }
    return word;
}

} // anonymous namespace

std::string fts_tokenizer_t::porter_stem(const std::string &word) {
    if (word.size() <= 2) return word;

    std::string w = word;
    w = step_1a(w);
    w = step_1b(w);
    w = step_1c(w);
    if (w.size() <= 2) return w;
    w = step_2(w);
    if (w.size() <= 2) return w;
    w = step_3(w);
    if (w.size() <= 2) return w;
    w = step_4(w);
    if (w.size() <= 2) return w;
    w = step_5(w);
    return w;
}

}  // namespace ql
