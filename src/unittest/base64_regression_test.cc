// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include <string>
#include "crypto/base64.hpp"

namespace unittest {

TEST(CryptoBase64Test, EncodeEmpty) {
    EXPECT_EQ("", crypto::detail::base64_encode(nullptr, 0));
}

TEST(CryptoBase64Test, EncodeSingleByte) {
    unsigned char data[] = {0x00};
    EXPECT_EQ("AA==", crypto::detail::base64_encode(data, 1));
}

TEST(CryptoBase64Test, EncodeTwoBytes) {
    unsigned char data[] = {0x00, 0x00};
    EXPECT_EQ("AAA=", crypto::detail::base64_encode(data, 2));
}

TEST(CryptoBase64Test, EncodeThreeBytes) {
    // REGRESSION: was broken by 'size > 3' off-by-one — crashed
    // with unreachable() on exactly-3-byte input during SCRAM auth.
    unsigned char data[] = {0x00, 0x00, 0x00};
    EXPECT_EQ("AAAA", crypto::detail::base64_encode(data, 3));
}

TEST(CryptoBase64Test, EncodeFourBytes) {
    unsigned char data[] = {0x00, 0x00, 0x00, 0x00};
    auto result = crypto::detail::base64_encode(data, 4);
    // "AAAA" + 1 extra byte → "AAAA" + 2-char base64 of last byte + padding
    EXPECT_EQ(8u, result.size());
}

TEST(CryptoBase64Test, EncodeThreeBytesKnownVector) {
    // "Man" in ASCII = {0x4d, 0x61, 0x6e} → "TWFu"
    // REGRESSION: exact 3-byte input was the crash trigger
    unsigned char data[] = {0x4d, 0x61, 0x6e};
    EXPECT_EQ("TWFu", crypto::detail::base64_encode(data, 3));
}

TEST(CryptoBase64Test, EncodeScramHandshakeSize) {
    // SCRAM auth sends a 3-byte nonce → this was the exact input that
    // triggered the unreachable() crash during live integration testing.
    unsigned char nonce[] = {0xa1, 0xb2, 0xc3};
    auto result = crypto::detail::base64_encode(nonce, 3);
    EXPECT_EQ(4u, result.size());
    EXPECT_NE("", result);
}

TEST(CryptoBase64Test, DecodeRoundtrip) {
    unsigned char original[] = {0x4d, 0x61, 0x6e};
    auto encoded = crypto::detail::base64_encode(original, 3);
    auto decoded = crypto::base64_decode(encoded);
    EXPECT_EQ(std::string("Man"), decoded);
}

TEST(CryptoBase64Test, DecodeRoundtripVariableSizes) {
    for (size_t len = 1; len <= 64; len++) {
        std::vector<unsigned char> data(len);
        for (size_t i = 0; i < len; i++) data[i] = (unsigned char)(i * 7 + 1);
        auto encoded = crypto::detail::base64_encode(data.data(), len);
        auto decoded = crypto::base64_decode(encoded);
        EXPECT_EQ(len, decoded.size());
        for (size_t i = 0; i < len; i++)
            EXPECT_EQ(data[i], (unsigned char)decoded[i]);
    }
}

}  // namespace unittest
