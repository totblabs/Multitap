/* Multitap build glue, not part of x264. Equivalent of configure + version.sh
 * output for the pinned x264 stable commit
 * b35605ace3ddf7c1a5d67a2eb553f034aef41d55 (git rev-list --count HEAD = 3222),
 * --bit-depth=8 --chroma-format=all. x264.cmake refuses any other checkout. */
#define X264_GPL           1
#define X264_INTERLACED    1
#define X264_BIT_DEPTH     8
#define X264_CHROMA_FORMAT 0
#define X264_REV 3222
#define X264_REV_DIFF 0
#define X264_VERSION " r3222 b35605a"
#define X264_POINTVER "0.165.3222 b35605a"
