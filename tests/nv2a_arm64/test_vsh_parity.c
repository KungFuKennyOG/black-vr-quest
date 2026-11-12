/*
 * nv2a-vsh-parity: CPU vertex shading, arm64 vs x86.
 *
 * Kelvin shades a draw's vertices on the CPU whenever the GPU backend has no compiled vertex
 * program for it yet (kgpu_vs_ready) -- on the Quest that is the first frames of every new
 * program, so CPU-shaded and GPU-shaded frames of the same draw must agree, and the CPU path
 * must give what the Windows build gives. Windows (MSVC /arch:AVX2) runs kvsh_run8_state
 * (nv2a_vsh_emulator8, AVX); arm64 runs kvsh_run4_state (nv2a_vsh_emulator4, SSE through
 * sse2neon); both are documented to equal the scalar emulator bit for bit.
 *
 * Random programs over every opcode (MAC: MOV MUL ADD MAD DP3 DPH DP4 DST MIN MAX SLT SGE ARL;
 * ILU: MOV RCP RCC RSQ, and EXP LOG LIT in the libm class), swizzles, negation, write masks,
 * a0-relative constant reads, r12 = oPos aliasing, paired MAC+ILU steps; inputs and constants
 * mix ordinary values with +-0, +-inf, NaN payloads, denormals and huge values.
 *
 * In process: emulator4 == scalar and (AVX2) emulator8 == scalar, bit for bit, every lane.
 * Across hosts (--dump on x86-64 AVX2, --check elsewhere): the scalar outputs and the finished
 * KVertex of the host's production path (run8 on AVX2, run4 otherwise).
 *   class A  no libm op, linear or no fog: must be bit-identical (NaN bits excepted).
 *   class B  ends in EXP/LOG/LIT (powf/log2f) and uses exp/exp2 fog (exp2f): up to 2 ulp.
 */
#include "parity_common.h"

#include "kelvin_render.h"
#include "nv2a_vsh_cpu/nv2a_vsh_emulator.h"
#include "nv2a_vsh_cpu/nv2a_vsh_emulator4.h"
#if defined(__AVX2__)
#include "nv2a_vsh_cpu/nv2a_vsh_emulator8.h"
#endif

#define NPROG  700
#define NVERT  8
#define MAXSTEP 12

static float s_const[NPROG][192 * 4];
static float s_in[NPROG][NVERT][NV2A_VERTEXSHADER_ATTRIBUTES][4];
static float s_raw[2][NPROG][NVERT][13 * 4];          /* scalar emulator, by class */
static KVertex s_vtx[2][NPROG][NVERT];               /* production path, by class */
static Nv2aVshStep s_steps[NPROG][MAXSTEP];
static int s_class[NPROG];

static unsigned long s_self_checked, s_self_bad;

static int nin_of(Nv2aVshOpcode op)
{
    switch (op) {
    case NV2AOP_MAD: return 3;
    case NV2AOP_MUL: case NV2AOP_ADD: case NV2AOP_DP3: case NV2AOP_DPH: case NV2AOP_DP4:
    case NV2AOP_DST: case NV2AOP_MIN: case NV2AOP_MAX: case NV2AOP_SLT: case NV2AOP_SGE: return 2;
    case NV2AOP_NOP: return 0;
    default: return 1;
    }
}

static void rnd_input(Nv2aVshInput *in)
{
    int k;
    switch (pr_n(3)) {
    case 0:
        in->type = NV2ART_TEMPORARY;
        in->index = pr_n(13);                          /* 12 reads o0 */
        break;
    case 1:
        in->type = NV2ART_INPUT;
        in->index = pr_n(16);
        break;
    default:
        in->type = NV2ART_CONTEXT;
        in->is_relative = pr_n(3) == 0;
        in->index = in->is_relative ? pr_n(185) : pr_n(192);   /* a0 is 0..7 */
        break;
    }
    for (k = 0; k < 4; k++) in->swizzle[k] = (uint8_t)pr_n(4);
    in->is_negated = pr_n(2);
}

static void rnd_output(Nv2aVshOutput *o, int allow_none)
{
    int r = (int)pr_n(allow_none ? 3 : 2);
    if (r == 2) { o->type = NV2ART_NONE; return; }
    if (r == 0) { o->type = NV2ART_TEMPORARY; o->index = pr_n(12); }   /* r12 is read-only */
    else        { o->type = NV2ART_OUTPUT;    o->index = pr_n(13); }
    o->writemask = (Nv2aVshWritemask)(1 + pr_n(15));
}

static void rnd_op(Nv2aVshOperation *op, Nv2aVshOpcode code)
{
    int i;
    memset(op, 0, sizeof(*op));
    op->opcode = code;
    if (code == NV2AOP_NOP) return;
    if (code == NV2AOP_ARL) {
        /* a0 from v15.x, which holds small integers, so relative reads stay in c0..c191. */
        op->outputs[0].type = NV2ART_ADDRESS;
        op->inputs[0].type = NV2ART_INPUT;
        op->inputs[0].index = 15;
        return;
    }
    for (i = 0; i < nin_of(code); i++) rnd_input(&op->inputs[i]);
    rnd_output(&op->outputs[0], 0);
    rnd_output(&op->outputs[1], 1);
}

static void make_program(int p, int libm_class)
{
    static const Nv2aVshOpcode mac[] = {
        NV2AOP_MOV, NV2AOP_MUL, NV2AOP_ADD, NV2AOP_MAD, NV2AOP_DP3, NV2AOP_DPH, NV2AOP_DP4,
        NV2AOP_DST, NV2AOP_MIN, NV2AOP_MAX, NV2AOP_SLT, NV2AOP_SGE, NV2AOP_ARL, NV2AOP_NOP };
    static const Nv2aVshOpcode ilu[] = { NV2AOP_NOP, NV2AOP_MOV, NV2AOP_RCP, NV2AOP_RCC, NV2AOP_RSQ };
    static const Nv2aVshOpcode libm[] = { NV2AOP_EXP, NV2AOP_LOG, NV2AOP_LIT };
    int n = 1 + (int)pr_n(MAXSTEP - 1), i;
    for (i = 0; i < n; i++) {
        Nv2aVshStep *st = &s_steps[p][i];
        memset(st, 0, sizeof(*st));
        rnd_op(&st->mac, mac[pr_n(sizeof(mac) / sizeof(mac[0]))]);
        rnd_op(&st->ilu, ilu[pr_n(sizeof(ilu) / sizeof(ilu[0]))]);
        if (st->mac.opcode == NV2AOP_ARL) {
            /* the hardware refuses ARL beside an ILU op that writes a0's users */
            memset(&st->ilu, 0, sizeof(st->ilu));
        }
        if (libm_class && i == n - 1) {
            /* class B: the last step's ILU op is libm, straight to an output, so a
             * one-ulp library difference is not amplified by later arithmetic. */
            rnd_op(&st->ilu, libm[pr_n(3)]);
            st->ilu.outputs[0].type = NV2ART_OUTPUT;
            st->ilu.outputs[0].index = pr_n(13);
            st->ilu.outputs[1].type = NV2ART_NONE;
            memset(&st->mac, 0, sizeof(st->mac));
        }
        st->is_final = i == n - 1;
    }
}

static unsigned long s_self_nan;

/* Bit-identical, or both NaN with other bits (counted apart: the SIMD emulators produce the
 * default NaN where the scalar one propagates an input payload -- on x86 too, pre-existing). */
static int same_bits(const float *a, const float *b, int n)
{
    int i, nan_only = 0;
    for (i = 0; i < n; i++) {
        if (pr_ubits(a[i]) == pr_ubits(b[i])) continue;
        if (isnan(a[i]) && isnan(b[i])) { nan_only = 1; continue; }
        return 0;
    }
    s_self_nan += nan_only;
    return 1;
}

static void shade_program(int p)
{
    Nv2aVshProgram prog = { s_steps[p] };
    int c = s_class[p], v, lane, r, k;
    KVshState vs;
    KVertex *outs[NVERT];

    memset(&vs, 0, sizeof(vs));
    vs.program = &prog;
    vs.constants = s_const[p];
    vs.fog_enable = pr_n(2);
    vs.fog_mode = c ? (int[]){ FOG_MODE_EXP, FOG_MODE_EXP_ABS, FOG_MODE_EXP2, FOG_MODE_EXP2_ABS }[pr_n(4)]
                    : (int[]){ FOG_MODE_LINEAR, FOG_MODE_LINEAR_ABS }[pr_n(2)];
    vs.fog_param[0] = pr_value(16);
    vs.fog_param[1] = pr_value(16) * 0.01f;
    vs.specular_enable = pr_n(2);
    vs.ignore_specular_alpha = pr_n(2);
    vs.point_size = pr_value(16);

    /* the scalar emulator, as kvsh_run_state's run_program drives it */
    for (v = 0; v < NVERT; v++) {
        float temps[12 * 4], addr[4] = { 0, 0, 0, 0 }, *o = s_raw[c][p][v];
        Nv2aVshExecutionState st;
        memset(temps, 0, sizeof(temps));
        for (r = 0; r < 13; r++) { o[4 * r] = o[4 * r + 1] = o[4 * r + 2] = 0.0f; o[4 * r + 3] = 1.0f; }
        st.input_regs = &s_in[p][v][0][0];
        st.output_regs = o;
        st.temp_regs = temps;
        st.context_regs = s_const[p];
        st.address_reg = addr;
        st.context_dirty = NULL;
        nv2a_vsh_emu_execute(&st, &prog);
    }

    /* emulator4 == scalar, every lane */
    for (v = 0; v < NVERT; v += 4) {
        Nv2aVshState4 st;
        float lanes[4][4];
        int a;
        nv2a_vsh_emu4_reset(&st);
        st.context = s_const[p];
        for (a = 0; a < 16; a++) {
            for (k = 0; k < 4; k++) {
                st.input[a].c[k] = _mm_setr_ps(s_in[p][v][a][k], s_in[p][v + 1][a][k],
                                               s_in[p][v + 2][a][k], s_in[p][v + 3][a][k]);
            }
        }
        nv2a_vsh_emu4_execute(&st, &prog);
        for (r = 0; r < 13; r++) {
            for (k = 0; k < 4; k++) _mm_storeu_ps(lanes[k], st.output[r].c[k]);
            for (lane = 0; lane < 4; lane++) {
                float got[4] = { lanes[0][lane], lanes[1][lane], lanes[2][lane], lanes[3][lane] };
                s_self_checked++;
                if (!same_bits(got, &s_raw[c][p][v + lane][4 * r], 4) && s_self_bad++ < 5) {
                    printf("  emu4 != scalar: program %d vertex %d o%d\n", p, v + lane, r);
#ifdef VSH_DEBUG
                    {
                        int q, m;
                        for (m = 0; m < 4; m++) printf("    c%d emu4 %08X scalar %08X\n", m, pr_ubits(got[m]), pr_ubits(s_raw[c][p][v + lane][4 * r + m]));
                        for (q = 0; q < MAXSTEP; q++) {
                            const Nv2aVshStep *st = &s_steps[p][q];
                            printf("    step %d mac op%d out(%d,%u,%d)(%d,%u,%d) in", q, st->mac.opcode, st->mac.outputs[0].type, st->mac.outputs[0].index, st->mac.outputs[0].writemask, st->mac.outputs[1].type, st->mac.outputs[1].index, st->mac.outputs[1].writemask);
                            for (m = 0; m < 3; m++) printf(" (%d,%u%s%s %d%d%d%d)", st->mac.inputs[m].type, st->mac.inputs[m].index, st->mac.inputs[m].is_relative ? "+a0" : "", st->mac.inputs[m].is_negated ? " neg" : "", st->mac.inputs[m].swizzle[0], st->mac.inputs[m].swizzle[1], st->mac.inputs[m].swizzle[2], st->mac.inputs[m].swizzle[3]);
                            printf(" | ilu op%d out(%d,%u,%d)(%d,%u,%d) in (%d,%u%s)\n", st->ilu.opcode, st->ilu.outputs[0].type, st->ilu.outputs[0].index, st->ilu.outputs[0].writemask, st->ilu.outputs[1].type, st->ilu.outputs[1].index, st->ilu.outputs[1].writemask, st->ilu.inputs[0].type, st->ilu.inputs[0].index, st->ilu.inputs[0].is_relative ? "+a0" : "");
                            if (st->is_final) break;
                        }
                    }
#endif
                }
            }
        }
    }
#if defined(__AVX2__)
    {
        Nv2aVshState8 st;
        float lanes[4][8];
        int a;
        nv2a_vsh_emu8_reset(&st);
        st.context = s_const[p];
        for (a = 0; a < 16; a++) {
            for (k = 0; k < 4; k++) {
                float t[8];
                for (lane = 0; lane < 8; lane++) t[lane] = s_in[p][lane][a][k];
                st.input[a].c[k] = _mm256_loadu_ps(t);
            }
        }
        nv2a_vsh_emu8_execute(&st, &prog);
        for (r = 0; r < 13; r++) {
            for (k = 0; k < 4; k++) _mm256_storeu_ps(lanes[k], st.output[r].c[k]);
            for (lane = 0; lane < 8; lane++) {
                float got[4] = { lanes[0][lane], lanes[1][lane], lanes[2][lane], lanes[3][lane] };
                s_self_checked++;
                if (!same_bits(got, &s_raw[c][p][lane][4 * r], 4) && s_self_bad++ < 5) {
                    printf("  emu8 != scalar: program %d vertex %d o%d\n", p, lane, r);
                }
            }
        }
    }
#endif

    /* the production path: what the engine runs for a batch on this host */
    for (v = 0; v < NVERT; v++) outs[v] = &s_vtx[c][p][v];
#if defined(__AVX2__)
    kvsh_run8_state(&vs, (const float (*)[NV2A_VERTEXSHADER_ATTRIBUTES][4])s_in[p], outs, NVERT);
#else
    kvsh_run4_state(&vs, (const float (*)[NV2A_VERTEXSHADER_ATTRIBUTES][4])s_in[p], outs, 4);
    kvsh_run4_state(&vs, (const float (*)[NV2A_VERTEXSHADER_ATTRIBUTES][4])s_in[p] + 4, outs + 4, 4);
#endif
    /* ... and it equals the one-vertex path */
    for (v = 0; v < NVERT; v++) {
        KVertex one;
        kvsh_run_state(&vs, (const float (*)[4])s_in[p][v], &one);
        s_self_checked++;
        if (!same_bits((const float *)&one, (const float *)&s_vtx[c][p][v], (int)(sizeof(one) / 4)) && s_self_bad++ < 5) {
            printf("  batch != kvsh_run_state: program %d vertex %d\n", p, v);
        }
    }
}

int main(int argc, char **argv)
{
    int p, v, a, k;
    ParitySection sec[4];

    pr_seed(0x5EED0001u);
    memset(s_raw, 0, sizeof(s_raw));
    memset(s_vtx, 0, sizeof(s_vtx));
    for (p = 0; p < NPROG; p++) {
        s_class[p] = p >= NPROG * 3 / 4;
        for (k = 0; k < 192 * 4; k++) s_const[p][k] = pr_value(24);
        for (v = 0; v < NVERT; v++) {
            for (a = 0; a < 16; a++) {
                for (k = 0; k < 4; k++) s_in[p][v][a][k] = pr_value(24);
            }
            s_in[p][v][15][0] = (float)pr_n(8);      /* a0 source */
        }
        make_program(p, s_class[p]);
    }
    for (p = 0; p < NPROG; p++) shade_program(p);

    printf("[%s] in-process: %lu lane/vertex results checked against the scalar emulator: %lu differ, %lu NaN-bits only\n",
           PARITY_HOST, s_self_checked, s_self_bad, s_self_nan);

    sec[0] = (ParitySection){ "A scalar emulator", s_raw[0], sizeof(s_raw[0]), 1, 0 };
    sec[1] = (ParitySection){ "A KVertex (production)", s_vtx[0], sizeof(s_vtx[0]), 1, 0 };
    sec[2] = (ParitySection){ "B scalar emulator (libm)", s_raw[1], sizeof(s_raw[1]), 1, 2 };
    sec[3] = (ParitySection){ "B KVertex (libm fog)", s_vtx[1], sizeof(s_vtx[1]), 1, 2 };
    {
        int rc = parity_finish(argc, argv, sec, 4);
        return s_self_bad ? 1 : rc;
    }
}
