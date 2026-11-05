#pragma once
// Decoding is outside the NGS release fixture. No audio module is substituted
// in the AAC fixture, which links the pinned real FFmpeg prebuilt.
namespace ngs { struct Atrac9Module { static void free_swr_contexts(); }; }
