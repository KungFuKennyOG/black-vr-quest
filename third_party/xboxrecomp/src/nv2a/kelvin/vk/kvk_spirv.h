/*
 * kvk_spirv.h - GLSL -> SPIR-V at run time for the Kelvin Vulkan backend (glslang, linked into the app).
 *
 * Why at run time: the NV2A vertex programs and combiner configurations are only known when the title uses them
 * (hundreds of each), so their shaders are generated per program like the D3D11 backend's HLSL. glslang (built
 * from source with the NDK, without SPIRV-Tools/optimizer and without HLSL) turns the generated GLSL into SPIR-V in
 * about a millisecond for a combiner, a few for a long vertex program on a desktop core; the driver's pipeline
 * compile that follows (Adreno: tens of ms) costs more than glslang and is the same whichever way the SPIR-V is
 * made. Both are hidden the same way as on D3D11: compiles run on worker threads, draws fall back to the CPU vertex
 * path / ubershader until ready, SPIR-V is cached on disk keyed by the source hash, VkPipelineCache is persisted,
 * and the vertex-program manifest lets a later run compile everything before the game starts.
 *
 * Thread-safe after kvk_spirv_init.
 */
#ifndef KVK_SPIRV_H
#define KVK_SPIRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { KVK_STAGE_VERTEX = 0, KVK_STAGE_FRAGMENT = 1 };

typedef struct KvkSpirv {
    uint32_t *words;        /* malloc'd */
    size_t count;           /* in 32-bit words */
    char *log;              /* malloc'd compiler messages on failure (or warnings), may be NULL */
} KvkSpirv;

bool kvk_spirv_init(void);
void kvk_spirv_shutdown(void);
/* Compile one GLSL #version 450 shader for Vulkan 1.1 / SPIR-V 1.3. False on error (out->log says why). */
bool kvk_spirv_compile(int stage, const char *glsl, KvkSpirv *out);
void kvk_spirv_free(KvkSpirv *s);

/* 64-bit FNV-1a of a string: the on-disk SPIR-V cache key. */
uint64_t kvk_hash64(const void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
