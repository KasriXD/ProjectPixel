/* Single compilation unit for stb_vorbis (Ogg Vorbis decoding).
 * Decoding from memory only: sound bytes come through the engine's file API.
 * Users include stb_vorbis.c with STB_VORBIS_HEADER_ONLY and the same
 * settings. Untrusted files are decoded only in the sandboxed package check
 * (docs/audio.md#published-games). */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
