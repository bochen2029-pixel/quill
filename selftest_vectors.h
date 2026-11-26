// quill · selftest_vectors.h — known answers for --selftest, made once outside quill.
// Provenance: zlib 1.3.1 (the deflate streams), numpy 2.4.2 (the compressed .npz, the weight-file hashes,
// the 8-bit rounding), and a separate Python transcription of splitmix64 and xoshiro256**.
// The build does not regenerate this file and quill does not need Python: these are fixed data.
#pragma once

namespace quill::vectors {

// xoshiro256** seeded through splitmix64: the first four outputs for seeds 0, 1 and 101
inline constexpr unsigned long long kRng0[4] = { 0x99EC5F36CB75F2B4ull, 0xBF6E1F784956452Aull, 0x1A5F849D4933E6E0ull, 0x6AA594F1262D2D2Cull };
inline constexpr unsigned long long kRng1[4] = { 0xB3F2AF6D0FC710C5ull, 0x853B559647364CEAull, 0x92F89756082A4514ull, 0x642E1C7BC266A3A7ull };
inline constexpr unsigned long long kRng101[4] = { 0x05BFD51F21ED53E3ull, 0xC93C8EC8FFF46ECCull, 0x8D2DFE5D32776DB0ull, 0xDFB06248F87B63A8ull };

// raw deflate streams (RFC 1951) of three texts: one stored block, one fixed-code block, one dynamic-code block
struct Deflate {
    const char* name;
    int block_type;
    unsigned size;          // of the data once inflated
    const char* sha256;     // of that data
    const char* hex;        // the stream
};
inline constexpr Deflate kDeflate[3] = {
    { "stored", 0, 102, "2623ba0e754185c5d6d0b5c280e378e9db14dd877f38106528321622f8dcc19d",
      "01660099ff7175696c6c2073746f726573207468697320626c6f636b2061732069742069732e207175696c6c2073746f726573207468697320626c6f636b2061732069742069732e207175696c6c2073746f726573207468697320626c6f636b2061732069742069732e20" },
    { "fixed", 1, 66, "4acae10647428889b4520991514acf378cf82f4b6ec3552275834256f31f7364",
      "4b4c2a4a4c4e4c4904520a89d8d93a0a2519a90a699915a9290ac9f929a90a99c50aa979f9a5e9190a19a945a900" },
    { "dynamic", 2, 882, "11cbb897fac8aa37bbab06250e3e7c1b92fc701039e1ae618786c7711c2c15ea",
      "e5d1f75fcc011cc7f12814425654f4a5d25d7464641e4e8806d1656644571747babba4cbcec80849c348292b1a2aab525665678fec95868cc8c84fde9f3efe0bbf7d7ef8fcf07cbcde7a362e81bb725fb61b346bed919bb53d5c97c4e4bd321a3c3bfce8ad6f166e41b1f9af9bf79eb32eb5f47b9751eab8736f5af4f15a7fec765d5727657cc1db960e53c28edff961393a605be13be3be53971fb8fb5318b360fbc9f7adfa4d5b917ce557b7b10b779c7adabafff49529573f757756459d7ed664c08c5507af7dee346ed1ce33cf9bf69cb9fad0f52f9d872f8e3efba259af496b0edff86a36c2171c43fbc921e0988f546c0647e2b92c011c99df9613e0c843f75f06c77f6bd6237074894555e044663f6e044e5271757b70729e34b605a7e4638721e094e98bdcc1a9e938742e3806e2091a704c87f96c00c76ea2763738d2791bd3c0f108de731e9cf911e9f7c059baf74239389b32eeff0667dfc50f6dc0c97c506f05cea50a1347701efeb11e0f4e65db81dee0e8a12538d4121c6a090eb504875a82432dc1a196e0504b70a82538d4121c6a090eb504875a82432dc1a196e0504b70a82538d4121c6a090eb504875af2b4863cad394f2be169653cad9ca7f5e769753c6d244f9bc4d3e6f0b4253c6d194f5bc3d31af0b4a63cad1d4f2b6d98562668956a854619a8f215023482d62758a4130b5241ad50355cf6ff2e1c0e6289f01fbdff05" },
};

// numpy.savez_compressed(a = arange(12).reshape(3, 4) / 8 as float64, b = [1.5, -2.25, 0] as float32): 416 bytes
inline constexpr const char* kCompressedNpzHex =
    "504b03042d000000080000002100e5168311ffffffffffffffff05001400612e6e707901001000e00000000000000067000000000000009bec17ea1b"
    "10c9c850c650ad9e925a9c5ca46ea5a06e9366a1aea3a09e965f54529498179f5f94920a12774bcc294e058a17672416a402f91ac63a0a269a3a0ab5"
    "0a64032e061470c01e425f80d237a0f40328fd044abf80d26fa0f40728fd094a7f81d2dfec01504b03042d0000000800000021004b1d6f7bffffffff"
    "ffffffff05001400622e6e7079010010008c000000000000004f000000000000009bec17ea1b10c9c850c650ad9e925a9c5ca46ea5a06e9366a2aea3"
    "a09e965f54529498179f5f94920a12774bcc294e058a17672416a402f91ac63a9a3a0ab50a14002e068603f60c0c020718800000504b01022d002d00"
    "0000080000002100e516831167000000e0000000050000000000000000000000800100000000612e6e7079504b01022d002d0000000800000021004b"
    "1d6f7b4f0000008c00000005000000000000000000000080019e000000622e6e7079504b0506000000000200020066000000240100000000";

// sha256 of weights.npz for the kit's own recurrent articles, built axis-aligned by the kit's code and
// saved by numpy.savez. quill build specs/<name>.spec must write the same bytes.
struct KitNpz {
    const char* name;
    const char* sha256;
};
inline constexpr KitNpz kKitNpz[5] = {
    { "R1_plain", "de8a29d1b4a19100a115b3343a47d3794ec3496332453054ed72d06e5b6bc791" },
    { "R2_quotient", "6e91137ad70a0816f1dfc1cc784b00de56cfabe49cdc0e7c579babe5e134d5ff" },
    { "R3_overcomplete", "c383b60015169e3738a5eb8dc9f4335272cd6d3157a0ae4319a5fefaec348f70" },
    { "R4_two_bits", "9f6239e21988c8fc4692e47425b67924c8f49ba28264cb69ffca95df07f67bc7" },
    { "R5_redundant", "c53e6a519ea8f84d73180556ce244a893b87dbe2c1914259b19f27b53d2972f9" },
};

// The kit's q8 (kit/build.py) of the float32 row (-1)^k * (k + 1) / 7 for k = 0 .. 39: a block of 32 and a block of 8
inline constexpr double kQ8[40] = {
    0x1.26e00a142850ap-3, -0x1.26e00a142850ap-2, 0x1.ba500f1e3c78fp-2, -0x1.26e00a142850ap-1,
    0x1.70980c993264cp-1, -0x1.ba500f1e3c78fp-1, 0x1.020408d1a3469p+0, -0x1.26e00a142850ap+0,
    0x1.4bbc0b56ad5abp+0, -0x1.70980c993264cp+0, 0x1.95740ddbb76eep+0, -0x1.ba500f1e3c78fp+0,
    0x1.df2c1060c1830p+0, -0x1.020408d1a3469p+1, 0x1.14720972e5cb9p+1, -0x1.26e00a142850ap+1,
    0x1.34b28a8d1a346p+1, -0x1.47208b2e5cb97p+1, 0x1.598e8bcf9f3e8p+1, -0x1.6bfc8c70e1c38p+1,
    0x1.7e6a8d1224489p+1, -0x1.90d88db366cdap+1, 0x1.a3468e54a952ap+1, -0x1.b5b48ef5ebd7bp+1,
    0x1.c8228f972e5cbp+1, -0x1.da90903870e1cp+1, 0x1.ecfe90d9b366dp+1, -0x1.ff6c917af5ebdp+1,
    0x1.08ed490e1c387p+2, -0x1.1224495ebd7afp+2, 0x1.1b5b49af5ebd8p+2, -0x1.24924a0000000p+2,
    0x1.2e5cb9ebd7af6p+2, -0x1.37004a3468d1ap+2, 0x1.3fa3da7cf9f3ep+2, -0x1.48476ac58b163p+2,
    0x1.50eafb0e1c387p+2, -0x1.5c6fbb6eddbb7p+2, 0x1.65134bb76eddcp+2, -0x1.6db6dc0000000p+2,
};

}  // namespace quill::vectors
