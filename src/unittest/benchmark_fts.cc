// Copyright 2026 RethinkDB, all rights reserved.
//
// PERF-BENCH: FTS (Full-Text Search) match query latency benchmarks.
//
// Measures:
//   - Single-word tokenization throughput
//   - Multi-word text tokenization throughput
//   - Stemming overhead vs raw tokenization
//   - Stop-word filtering throughput
//   - Match query simulation (tokenize + set membership)
//
// Uses std::chrono for timing via GTest harness — no Google Benchmark dep.

#include "unittest/gtest.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include "rdb_protocol/fts_tokenizer.hpp"

namespace unittest {

namespace {

// ── Benchmark helpers ─────────────────────────────────────────────────

struct bench_stats_t {
    double min_us;
    double mean_us;
    double median_us;
    double max_us;
    size_t iterations;

    void print(const std::string &label) const {
        std::cout << "BENCH: " << label << "\n"
                  << "  iters=" << iterations
                  << "  min=" << std::fixed << std::setprecision(1) << min_us
                  << "us  mean=" << mean_us
                  << "us  median=" << median_us
                  << "us  max=" << max_us << "us\n";
    }
};

bench_stats_t compute_stats(std::vector<double> &durations_us,
                            size_t iterations) {
    std::sort(durations_us.begin(), durations_us.end());
    double sum = std::accumulate(durations_us.begin(), durations_us.end(), 0.0);
    return {
        durations_us.front(),
        sum / static_cast<double>(iterations),
        durations_us[iterations / 2],
        durations_us.back(),
        iterations,
    };
}

// ── Representative text samples ───────────────────────────────────────

const std::vector<std::string> kTextSamples = {
    "The quick brown fox jumps over the lazy dog",
    "RethinkDB is an open-source distributed NoSQL database designed for "
    "building realtime web applications",
    "Full-text search enables efficient retrieval of documents by matching "
    "query terms against indexed tokens",
    "machine learning artificial intelligence neural networks deep learning "
    "natural language processing computer vision",
    "PostgreSQL BRIN indexes provide efficient range-based filtering for "
    "large tables with naturally ordered data",
    "a b c d e f g h i j k l m n o p q r s t u v w x y z",
};

}  // namespace

// ── Tokenization: Single Word ─────────────────────────────────────────

TEST(BenchmarkFTS, TokenizeSingleWord) {
    constexpr size_t kIters = 20000;
    constexpr size_t kWarmup = 1000;

    ql::fts_tokenizer_t t;

    for (size_t i = 0; i < kWarmup; ++i) t.tokenize("hello");

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = t.tokenize("hello");
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_EQ(1u, result.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("FTS: tokenize single word");
}

// ── Tokenization: Short Phrase ────────────────────────────────────────

TEST(BenchmarkFTS, TokenizeShortPhrase) {
    constexpr size_t kIters = 10000;
    constexpr size_t kWarmup = 500;

    ql::fts_tokenizer_t t;
    const std::string text = "hello world foo bar";

    for (size_t i = 0; i < kWarmup; ++i) t.tokenize(text);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = t.tokenize(text);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_GE(result.size(), 2u);  // "hello", "world" (foo,bar may stem)
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("FTS: tokenize short phrase (4 words)");
}

// ── Tokenization: Long Paragraph ──────────────────────────────────────

TEST(BenchmarkFTS, TokenizeParagraph) {
    constexpr size_t kIters = 5000;
    constexpr size_t kWarmup = 200;

    ql::fts_tokenizer_t t;
    const std::string text =
        "Four score and seven years ago our fathers brought forth on this "
        "continent a new nation conceived in liberty and dedicated to the "
        "proposition that all men are created equal now we are engaged in a "
        "great civil war testing whether that nation or any nation so "
        "conceived and so dedicated can long endure";

    for (size_t i = 0; i < kWarmup; ++i) t.tokenize(text);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = t.tokenize(text);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_GE(result.size(), 5u);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("FTS: tokenize paragraph (~70 words)");
}

// ── Tokenization: Raw vs Stemmed ──────────────────────────────────────

TEST(BenchmarkFTS, TokenizeRawVsStemmed) {
    constexpr size_t kIters = 5000;
    constexpr size_t kWarmup = 200;

    ql::fts_tokenizer_t t;
    const std::string text =
        "running jumped computing writing thinking searched indexing "
        "walking talking processing analyzing optimizing deploying";

    // Benchmark stemmed tokenization
    for (size_t i = 0; i < kWarmup; ++i) t.tokenize(text);

    std::vector<double> stemmed_durations;
    stemmed_durations.reserve(kIters);
    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = t.tokenize(text);
        auto end = std::chrono::high_resolution_clock::now();
        stemmed_durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto stemmed_stats = compute_stats(stemmed_durations, kIters);
    stemmed_stats.print("FTS: tokenize with stemming");

    // Benchmark raw tokenization
    for (size_t i = 0; i < kWarmup; ++i) t.tokenize_raw(text);

    std::vector<double> raw_durations;
    raw_durations.reserve(kIters);
    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = t.tokenize_raw(text);
        auto end = std::chrono::high_resolution_clock::now();
        raw_durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto raw_stats = compute_stats(raw_durations, kIters);
    raw_stats.print("FTS: tokenize without stemming (raw)");

    double overhead_pct =
        ((stemmed_stats.mean_us - raw_stats.mean_us) / raw_stats.mean_us) *
        100.0;
    std::cout << "  stemming overhead=" << std::fixed << std::setprecision(1)
              << overhead_pct << "%\n";
}

// ── Stop-Word Filtering ───────────────────────────────────────────────

TEST(BenchmarkFTS, TokenizeWithStopWords) {
    constexpr size_t kIters = 5000;
    constexpr size_t kWarmup = 200;

    ql::fts_tokenizer_t t;
    // Text heavy with English stop words (the, is, a, it, and, to, of, ...)
    const std::string text =
        "it is the end of the beginning and the beginning of the end "
        "to be or not to be that is the question whether it is nobler "
        "in the mind to suffer the slings and arrows of outrageous fortune";

    for (size_t i = 0; i < kWarmup; ++i) t.tokenize(text);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto result = t.tokenize(text);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        // Most stop words filtered, only content words survive
        EXPECT_GE(result.size(), 3u);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("FTS: tokenize with heavy stop-word content");
}

// ── Match Query Simulation ────────────────────────────────────────────

TEST(BenchmarkFTS, MatchQuerySimulation) {
    // Simulates an FTS match query: tokenize query string, then check each
    // token against a pre-built document token set.
    constexpr size_t kIters = 2000;
    constexpr size_t kDocCount = 100;
    constexpr size_t kWarmup = 50;

    ql::fts_tokenizer_t t;

    // Build document token sets
    std::vector<std::unordered_set<std::string>> doc_tokens;
    doc_tokens.reserve(kDocCount);
    for (size_t i = 0; i < kDocCount; ++i) {
        auto tokens = t.tokenize(kTextSamples[i % kTextSamples.size()]);
        doc_tokens.emplace_back(tokens.begin(), tokens.end());
    }

    // Query strings
    std::vector<std::string> queries = {
        "quick fox", "distributed database", "full text search",
        "machine learning", "neural network", "range based index",
        "open source", "realtime application",
    };

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i) {
        auto q_tokens = t.tokenize(queries[i % queries.size()]);
        volatile size_t hits = 0;
        for (const auto &doc_set : doc_tokens) {
            for (const auto &qt : q_tokens) {
                if (doc_set.count(qt) > 0) ++hits;
            }
        }
        (void)hits;
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        // Tokenize query
        auto q_tokens = t.tokenize(queries[i % queries.size()]);

        // Match against all documents
        size_t matched_docs = 0;
        for (const auto &doc_set : doc_tokens) {
            bool matched = false;
            for (const auto &qt : q_tokens) {
                if (doc_set.count(qt) > 0) {
                    matched = true;
                    break;
                }
            }
            if (matched) ++matched_docs;
        }

        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_GE(matched_docs, 0u);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("FTS: match query simulation (tokenize + 100 doc scan)");

    double qps = (1.0 / stats.mean_us) * 1e6;
    std::cout << "  queries/sec=" << std::fixed << std::setprecision(0)
              << qps << "\n";
}

// ── Bulk Tokenization ─────────────────────────────────────────────────

TEST(BenchmarkFTS, BulkTokenization) {
    // Tokenize all sample texts in sequence — simulates indexing a batch
    // of documents.
    constexpr size_t kIters = 500;
    constexpr size_t kWarmup = 20;

    ql::fts_tokenizer_t t;

    for (size_t i = 0; i < kWarmup; ++i) {
        for (const auto &text : kTextSamples) t.tokenize(text);
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        size_t total_tokens = 0;
        for (const auto &text : kTextSamples) {
            total_tokens += t.tokenize(text).size();
        }
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_GT(total_tokens, 0u);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print(
        "FTS: bulk tokenization (6 sample texts, " +
        std::to_string(kTextSamples.size()) + " docs)");
}

}  // namespace unittest
