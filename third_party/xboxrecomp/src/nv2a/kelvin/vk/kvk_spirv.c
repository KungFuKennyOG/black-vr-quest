/*
 * kvk_spirv.c - GLSL -> SPIR-V through glslang's C interface. See kvk_spirv.h.
 */
#include "kvk_spirv.h"

#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

static int s_init;

bool kvk_spirv_init(void)
{
    if (!s_init) s_init = glslang_initialize_process() ? 1 : -1;
    return s_init > 0;
}

void kvk_spirv_shutdown(void)
{
    if (s_init > 0) glslang_finalize_process();
    s_init = 0;
}

static char *dup_str(const char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
    char *r = (char *)malloc(la + lb + 2);
    if (!r) return NULL;
    if (la) memcpy(r, a, la);
    r[la] = '\n';
    if (lb) memcpy(r + la + 1, b, lb);
    r[la + 1 + lb] = 0;
    return r;
}

bool kvk_spirv_compile(int stage, const char *glsl, KvkSpirv *out)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *prog = NULL;
    bool ok = false;
    memset(out, 0, sizeof(*out));
    if (!glsl || !kvk_spirv_init()) return false;
    memset(&in, 0, sizeof(in));
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage == KVK_STAGE_VERTEX ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = glsl;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = (glslang_messages_t)(GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!sh) return false;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        out->log = dup_str(glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
        goto done;
    }
    prog = glslang_program_create();
    if (!prog) goto done;
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, (int)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))) {
        out->log = dup_str(glslang_program_get_info_log(prog), glslang_program_get_info_debug_log(prog));
        goto done;
    }
    glslang_program_SPIRV_generate(prog, in.stage);
    out->count = glslang_program_SPIRV_get_size(prog);
    if (!out->count) goto done;
    out->words = (uint32_t *)malloc(out->count * sizeof(uint32_t));
    if (!out->words) { out->count = 0; goto done; }
    glslang_program_SPIRV_get(prog, out->words);
    {
        const char *m = glslang_program_SPIRV_get_messages(prog);
        if (m && *m) out->log = dup_str(m, NULL);
    }
    ok = true;
done:
    if (prog) glslang_program_delete(prog);
    glslang_shader_delete(sh);
    return ok;
}

void kvk_spirv_free(KvkSpirv *s)
{
    free(s->words);
    free(s->log);
    memset(s, 0, sizeof(*s));
}

uint64_t kvk_hash64(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
