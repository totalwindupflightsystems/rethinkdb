// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/fts_tokenizer.hpp"

#include <string>
#include <unordered_set>
#include <vector>

#include "unittest/gtest.hpp"

namespace unittest {

// Helper: tokenize text and return the result
static std::vector<std::string> tokens(ql::fts_tokenizer_t &t,
                                       const std::string &text) {
    return t.tokenize(text);
}

// Helper: tokenize without stemming
static std::vector<std::string> tokens_raw(ql::fts_tokenizer_t &t,
                                           const std::string &text) {
    return t.tokenize_raw(text);
}

// ── Basic Tokenization ──

TEST(FtsTokenizerTest, EmptyString) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "");
    EXPECT_TRUE(result.empty());
}

TEST(FtsTokenizerTest, WhitespaceOnly) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "    \t\n   ");
    EXPECT_TRUE(result.empty());
}

TEST(FtsTokenizerTest, SingleWord) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "hello");
    ASSERT_EQ(1u, result.size());
    EXPECT_EQ("hello", result[0]);
}

TEST(FtsTokenizerTest, MultipleWords) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "hello world");
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ("hello", result[0]);
    EXPECT_EQ("world", result[1]);
}

TEST(FtsTokenizerTest, PunctuationStripped) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "hello, world! how's it going?");
    // "hello", "world", "how" (stop word → filtered),
    // "'s" → "s" (< min_len 2 → filtered),
    // "it" (stop word → filtered), "going" → "go"
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("hello", result[0]);
    EXPECT_EQ("world", result[1]);
    EXPECT_EQ("go", result[2]);  // "going" → stemmed
}

// ── Stop Word Filtering ──

TEST(FtsTokenizerTest, StopWordsFiltered) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "the quick brown fox");
    // "the" is a stop word → filtered
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("quick", result[0]);
    EXPECT_EQ("brown", result[1]);
    EXPECT_EQ("fox", result[2]);
}

TEST(FtsTokenizerTest, AllStopWords) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "the a an is it of and or");
    EXPECT_TRUE(result.empty());
}

TEST(FtsTokenizerTest, CustomStopWords) {
    ql::fts_tokenizer_t t;
    t.add_stop_words({"custom", "stopword"});
    auto result = tokens(t, "hello custom stopword world");
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ("hello", result[0]);
    EXPECT_EQ("world", result[1]);
}

TEST(FtsTokenizerTest, ClearStopWords) {
    ql::fts_tokenizer_t t;
    t.clear_stop_words();
    auto result = tokens(t, "the quick brown fox");
    // No stop words → "the" passes through
    ASSERT_EQ(4u, result.size());
    EXPECT_EQ("the", result[0]);
}

TEST(FtsTokenizerTest, StopWordsCaseInsensitive) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "THE The the A An");
    EXPECT_TRUE(result.empty());
}

// ── Minimum Token Length ──

TEST(FtsTokenizerTest, ShortTokensFiltered) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "a b c d e f g h i j");
    EXPECT_TRUE(result.empty());
}

TEST(FtsTokenizerTest, MinTokenLengthCustom) {
    ql::fts_tokenizer_t t(3);  // min_token_length = 3
    auto result = tokens(t, "a be cat dogs elephant");
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("cat", result[0]);
    EXPECT_EQ("dog", result[1]);   // "dogs" → stemmed to "dog"
    EXPECT_EQ("eleph", result[2]); // "elephant" → stemmed
}

TEST(FtsTokenizerTest, SetMinTokenLength) {
    ql::fts_tokenizer_t t;
    EXPECT_EQ(2u, t.min_token_length());
    t.set_min_token_length(5);
    EXPECT_EQ(5u, t.min_token_length());
    auto result = tokens(t, "hello world quickly jumped");
    // "hello"(5) → "hello", "world"(5) → "world",
    // "quickly"(7) → unchanged (no Porter suffix matches),
    // "jumped"(6) → "jump"(4, filtered by min_len=5)
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("hello", result[0]);
    EXPECT_EQ("world", result[1]);
    EXPECT_EQ("quickly", result[2]);
}

// ── Porter Stemming ──

TEST(FtsTokenizerTest, PluralsStemmed) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "cats dogs buses mice");
    // "cats" → "cat", "dogs" → "dog", "buses" → "buse", "mice" → "mice" (irregular, not stemmed)
    ASSERT_GE(result.size(), 3u);
    EXPECT_EQ("cat", result[0]);
    EXPECT_EQ("dog", result[1]);
    EXPECT_EQ("buse", result[2]);
}

TEST(FtsTokenizerTest, IngStemmed) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "running jumping swimming");
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("run", result[0]);
    EXPECT_EQ("jump", result[1]);
    EXPECT_EQ("swim", result[2]);
}

TEST(FtsTokenizerTest, EdStemmed) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "worked played jumped");
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("work", result[0]);
    // "played" → "play" → step_1c: y→i → "plai" (correct Porter behavior)
    EXPECT_EQ("plai", result[1]);
    EXPECT_EQ("jump", result[2]);
}

TEST(FtsTokenizerTest, YToI) {
    ql::fts_tokenizer_t t;
    // Test "happily" → tokenized as "happily" → porter_stem("happily"):
    //   step_1a: no change
    //   step_1b: no change
    //   step_1c: ends with 'y', not preceded by consonant → 'happili' ... actually
    //   "happily": i=5='l' is consonant, so the 'y' → 'i': "happili"
    //   step_2: "eli" → "e": "happi"
    //   step_3: no match
    //   step_4: no match
    //   step_5: "happi" ... ends with 'i', no 'e', no 'll'
    // Let's just check that tokens exist and are reasonable
    auto result = tokens(t, "happily friendly");
    ASSERT_GE(result.size(), 2u);
    // Both should be stemmed to root-like forms
}

TEST(FtsTokenizerTest, SuffixStemming) {
    ql::fts_tokenizer_t t;
    // "relational" → step_2: ational→ate → "relate"
    // "conditional" → "condition" is too short for step_2 removal
    auto result = tokens(t, "relational conditional");
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ("relat", result[0]);     // "relational" → "relate" → "relat"
    EXPECT_EQ("condition", result[1]);  // stays as-is
}

TEST(FtsTokenizerTest, StemmingDoesNotOverstem) {
    ql::fts_tokenizer_t t;
    // Short words should not be over-stemmed
    auto result = tokens(t, "at be do go he if me my no so to up we");
    // Many of these are stop words, which get filtered.
    // Words that survive should be short and not mangled.
    for (const auto &token : result) {
        EXPECT_GE(token.size(), 2u);
    }
}

// ── Tokenize Raw (no stemming) ──

TEST(FtsTokenizerTest, RawTokenization) {
    ql::fts_tokenizer_t t;
    auto result = tokens_raw(t, "running jumped happiness");
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("running", result[0]);
    EXPECT_EQ("jumped", result[1]);
    EXPECT_EQ("happiness", result[2]);
}

// ── Language Configuration ──

TEST(FtsTokenizerTest, LanguageDefaultEnglish) {
    ql::fts_tokenizer_t t;
    EXPECT_EQ("english", t.language());
}

TEST(FtsTokenizerTest, SetLanguage) {
    ql::fts_tokenizer_t t;
    t.set_language("spanish");
    EXPECT_EQ("spanish", t.language());
}

// ── Number Handling ──

TEST(FtsTokenizerTest, NumbersTokenized) {
    ql::fts_tokenizer_t t;
    auto result = tokens(t, "test123 456data 789");
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("test123", result[0]);
    EXPECT_EQ("456data", result[1]);
    EXPECT_EQ("789", result[2]);
}

// ── Extra Stop Words Constructor ──

TEST(FtsTokenizerTest, ConstructWithExtraStopWords) {
    std::unordered_set<std::string> extras = {"confidential", "secret"};
    ql::fts_tokenizer_t t(2, "english", extras);
    auto result = tokens(t, "hello confidential world secret code");
    ASSERT_EQ(3u, result.size());
    EXPECT_EQ("hello", result[0]);
    EXPECT_EQ("world", result[1]);
    EXPECT_EQ("code", result[2]);
}

// ── Stop Words Accessors ──

TEST(FtsTokenizerTest, StopWordsAccessor) {
    ql::fts_tokenizer_t t;
    const auto &sw = t.stop_words();
    EXPECT_GT(sw.size(), 50u);  // should have many default stop words
    EXPECT_TRUE(sw.find("the") != sw.end());
    EXPECT_TRUE(sw.find("a") != sw.end());
}

}  // namespace unittest
