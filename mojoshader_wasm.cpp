/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 *
 *  Modified to run on Emscripten by ashduino101
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <cassert>

#include <emscripten.h>
#include <emscripten/bind.h>

#include "mojoshader.h"
#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"
#ifdef MOJOSHADER_HAS_SPIRV_TOOLS
#include "spirv-tools/libspirv.h"
#endif

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#if MOJOSHADER_DEBUG_MALLOC
static void *Malloc(int len)
{
    void *ptr = malloc(len + sizeof (int));
    int *store = (int *) ptr;
    printf("malloc() %d bytes (%p)\n", len, ptr);
    if (ptr == NULL) return NULL;
    *store = len;
    return (void *) (store + 1);
} // Malloc


static void Free(void *_ptr)
{
    int *ptr = (((int *) _ptr) - 1);
    int len = *ptr;
    printf("free() %d bytes (%p)\n", len, ptr);
    free(ptr);
} // Free
#else
#define Malloc NULL
#define Free NULL
#endif

static inline void do_indent(const unsigned int indent)
{
    unsigned int i;
    for (i = 0; i < indent; i++)
        printf("    ");
}

#define INDENT() do { if (indent) { do_indent(indent); } } while (0)


static const char *shader_type(const MOJOSHADER_shaderType s)
{
    switch (s)
    {
        case MOJOSHADER_TYPE_UNKNOWN: return "unknown";
        case MOJOSHADER_TYPE_PIXEL: return "pixel";
        case MOJOSHADER_TYPE_VERTEX: return "vertex";
        case MOJOSHADER_TYPE_GEOMETRY: return "geometry";
        default: return "(bogus value?)";
    } // switch

    return NULL;  // shouldn't hit this.
} // shader_type


static void print_typeinfo(const MOJOSHADER_symbolTypeInfo *info,
                           unsigned int indent)
{
    static const char *symclasses[] = {
            "scalar", "vector", "row-major matrix",
            "column-major matrix", "object", "struct"
    };

    static const char *symtypes[] = {
            "void", "bool", "int", "float", "string", "texture",
            "texture1d", "texture2d", "texture3d", "texturecube",
            "sampler", "sampler1d", "sampler2d", "sampler3d",
            "samplercube", "pixelshader", "vertexshader", "unsupported"
    };

    INDENT();
    printf("      symbol class %s\n", symclasses[info->parameter_class]);
    INDENT();
    printf("      symbol type %s\n", symtypes[info->parameter_type]);
    INDENT();
    printf("      rows %u\n", info->rows);
    INDENT();
    printf("      columns %u\n", info->columns);
    INDENT();
    printf("      elements %u\n", info->elements);

    if (info->member_count > 0)
    {
        int i;
        INDENT(); printf("      MEMBERS:\n");
        for (i = 0; i < info->member_count; i++)
        {
            const MOJOSHADER_symbolStructMember *member = &info->members[i];
            INDENT(); printf("      MEMBERS:\n");
            indent++;
            INDENT(); printf("      * %d: \"%s\"\n", i, member->name);
            print_typeinfo(&member->info, indent);
            indent--;
        } // for
    } // if
} // print_typeinfo


static void print_symbols(const MOJOSHADER_symbol *sym,
                          const unsigned int symbol_count,
                          const unsigned int indent)
{
    INDENT(); printf("SYMBOLS:");
    if (symbol_count == 0)
        printf(" (none.)\n");
    else
    {
        int i;
        printf("\n");
        for (i = 0; i < symbol_count; i++, sym++)
        {
            static const char *regsets[] = {
                    "bool", "int4", "float4", "sampler"
            };

            INDENT(); printf("    * %d: \"%s\"\n", i, sym->name.c_str());
            INDENT(); printf("      register set %s\n", regsets[sym->register_set]);
            INDENT(); printf("      register index %u\n", sym->register_index);
            INDENT(); printf("      register count %u\n", sym->register_count);
            print_typeinfo(&sym->info, indent);
        } // for
        printf("\n");
    } // else
} // print_symbols


static void print_preshader_operand(const MOJOSHADER_preshader *preshader,
                                    const int instidx, const int opidx)
{
    static char mask[] = { 'x', 'y', 'z', 'w' };
    const MOJOSHADER_preshaderInstruction *inst = &preshader->instructions[instidx];
    const MOJOSHADER_preshaderOperand *operand = &inst->operands[opidx];
    const int elems = inst->element_count;
    const int isscalarop = (inst->opcode >= MOJOSHADER_PRESHADEROP_SCALAR_OPS);
    const int isscalar = ((isscalarop) && (opidx == 0)); // probably wrong.
    int i;

    switch (operand->type)
    {
        case MOJOSHADER_PRESHADEROPERAND_LITERAL:
        {
            const double *lit = &preshader->literals[operand->index];
            printf("(");
            if (isscalar)
            {
                const double val = *lit;
                for (i = 0; i < elems-1; i++)
                    printf("%g, ", val);
                printf("%g)", val);
            } // if
            else
            {
                for (i = 0; i < elems-1; i++, lit++)
                    printf("%g, ", *lit);
                printf("%g)", *lit);
            } // else
            break;
        } // case

        case MOJOSHADER_PRESHADEROPERAND_INPUT:
        case MOJOSHADER_PRESHADEROPERAND_OUTPUT:
        case MOJOSHADER_PRESHADEROPERAND_TEMP:
        {
            int idx = operand->index % 4;
            char regch = 'c';
            if (operand->type == MOJOSHADER_PRESHADEROPERAND_TEMP)
                regch = 'r';

            if (operand->array_register_count > 0)
            {
                for (i = operand->array_register_count - 1; i >= 0; i--)
                    printf("c%d[", operand->array_registers[i]);
                printf("%c%d.%c", regch, operand->index / 4, mask[idx]);
                for (i = 0; i < operand->array_register_count; i++)
                    printf("]");
                break;
            } // if
            printf("%c%d", regch, operand->index / 4);
            if (isscalar)
                printf(".%c", mask[idx]);
            else if (elems != 4)
            {
                printf(".");
                for (i = 0; i < elems; i++)
                    printf("%c", mask[idx++]);
            } // else if
            break;
        } // case

        default:
            printf("[???{%d, %u}???]", (int) operand->type, operand->index);
            break;
    } // switch
} // print_preshader_operand


static void print_preshader(const MOJOSHADER_preshader *preshader,
                            const int indent)
{
    MOJOSHADER_preshaderInstruction *inst = preshader->instructions;
    int i, j;

    static const char *opcodestr[] = {
            "nop", "mov", "neg", "rcp", "frc", "exp", "log", "rsq", "sin", "cos",
            "asin", "acos", "atan", "min", "max", "lt", "ge", "add", "mul",
            "atan2", "div", "cmp", "movc", "dot", "noise", "min", "max", "lt",
            "ge", "add", "mul", "atan2", "div", "dot", "noise"
    };

    INDENT(); printf("PRESHADER:\n");

    print_symbols(preshader->symbols, preshader->symbol_count, indent + 1);

    for (i = 0; i < preshader->instruction_count; i++, inst++)
    {
        INDENT(); printf("    %s ", opcodestr[inst->opcode]);

        // print dest register first...
        print_preshader_operand(preshader, i, inst->operand_count - 1);

        // ...then the source registers.
        for (j = 0; j < inst->operand_count - 1; j++)
        {
            printf(", ");
            print_preshader_operand(preshader, i, j);
        } // for

        printf("\n");
    } // for

    printf("\n");
} // print_preshader


static void print_attrs(const char *category, const int count,
                        const MOJOSHADER_attribute *attributes,
                        const int indent)
{
    INDENT(); printf("%s:", category);
    if (count == 0)
        printf(" (none.)\n");
    else
    {
        int i;
        printf("\n");
        for (i = 0; i < count; i++)
        {
            static const char *usagenames[] = {
                    "<unknown>",
                    "position", "blendweight", "blendindices", "normal",
                    "psize", "texcoord", "tangent", "binormal", "tessfactor",
                    "positiont", "color", "fog", "depth", "sample"
            };
            const MOJOSHADER_attribute *a = &attributes[i];
            char numstr[16] = { 0 };
            if (a->index != 0)
                snprintf(numstr, sizeof (numstr), "%d", a->index);
            INDENT();
            printf("    * %s%s", usagenames[1 + (int) a->usage], numstr);
            if (!a->name.empty())
                printf(" (\"%s\")", a->name.c_str());
            printf("\n");
        } // for
    } // else
} // print_attrs


static void print_shader(const char *fname, const MOJOSHADER_parseData *pd,
                         unsigned int indent)
{
    INDENT(); printf("PROFILE: %s\n", pd->profile.c_str());
    if (pd->error_count > 0)
    {
        int i;
        for (i = 0; i < pd->error_count; i++)
        {
            const MOJOSHADER_error *err = &pd->errors[i];
            INDENT();
            printf("%s:%d: ERROR: %s\n",
                   err->filename.empty() ? fname : err->filename.c_str(),
                   err->error_position, err->error.c_str());
        } // for
    } // if
    else
    {
        INDENT(); printf("SHADER TYPE: %s\n", shader_type(pd->shader_type));
        INDENT(); printf("VERSION: %d.%d\n", pd->major_ver, pd->minor_ver);
        INDENT(); printf("INSTRUCTION COUNT: %d\n", (int) pd->instruction_count);
        INDENT(); printf("MAIN FUNCTION: %s\n", pd->mainfn.c_str());
        print_attrs("INPUTS", pd->input_count, pd->inputs, indent);
        print_attrs("OUTPUTS", pd->output_count, pd->outputs, indent);

        INDENT(); printf("CONSTANTS:");
        if (pd->constant_count == 0)
            printf(" (none.)\n");
        else
        {
            int i;
            printf("\n");
            for (i = 0; i < pd->constant_count; i++)
            {
                static const char *typenames[] = { "float", "int", "bool" };
                const MOJOSHADER_constant *c = &pd->constants[i];
                INDENT();
                printf("    * %d: %s (", c->index, typenames[(int) c->type]);
                if (c->type == MOJOSHADER_UNIFORM_FLOAT)
                {
                    printf("%f %f %f %f", c->value.f[0], c->value.f[1],
                           c->value.f[2], c->value.f[3]);
                } // if
                else if (c->type == MOJOSHADER_UNIFORM_INT)
                {
                    printf("%d %d %d %d", c->value.i[0], c->value.i[1],
                           c->value.i[2], c->value.i[3]);
                } // else if
                else if (c->type == MOJOSHADER_UNIFORM_BOOL)
                {
                    printf("%s", c->value.b ? "true" : "false");
                } // else if
                else
                {
                    printf("???");
                } // else
                printf(")\n");
            } // for
        } // else

        INDENT(); printf("UNIFORMS:");
        if (pd->uniform_count == 0)
            printf(" (none.)\n");
        else
        {
            int i;
            printf("\n");
            for (i = 0; i < pd->uniform_count; i++)
            {
                static const char *typenames[] = { "float", "int", "bool" };
                const MOJOSHADER_uniform *u = &pd->uniforms[i];
                const char *arrayof = "";
                const char *constant = u->constant ? "const " : "";
                char arrayrange[64] = { '\0' };
                if (u->array_count > 0)
                {
                    arrayof = "array[";
                    snprintf(arrayrange, sizeof (arrayrange), "%d] ",
                             u->array_count);
                } // if

                INDENT();
                printf("    * %d: %s%s%s%s", u->index, constant, arrayof,
                       arrayrange, typenames[(int) u->type]);
                if (!u->name.empty())
                    printf(" (\"%s\")", u->name.c_str());
                printf("\n");
            } // for
        } // else

        INDENT(); printf("SAMPLERS:");
        if (pd->sampler_count == 0)
            printf(" (none.)\n");
        else
        {
            int i;
            printf("\n");
            for (i = 0; i < pd->sampler_count; i++)
            {
                static const char *typenames[] = { "2d", "cube", "volume" };
                const MOJOSHADER_sampler *s = &pd->samplers[i];
                INDENT();
                printf("    * %d: %s", s->index, typenames[(int) s->type]);
                if (!s->name.empty())
                    printf(" (\"%s\")", s->name.c_str());
                if (s->texbem)
                    printf(" [TEXBEM]");
                printf("\n");
            } // for
        } // else

        print_symbols(pd->symbols, pd->symbol_count, indent);

        if (pd->preshader != NULL)
            print_preshader(pd->preshader, indent);

        if (!pd->output.empty())
        {
            const char *output;
            int output_len;
            int i;

            if (strcmp(pd->profile.c_str(), "spirv") == 0)
            {
#if SUPPORT_PROFILE_SPIRV && defined(MOJOSHADER_HAS_SPIRV_TOOLS)
                int binary_len = pd->output_len - sizeof(SpirvPatchTable);

                uint32_t *words = (uint32_t *) pd->output;
                size_t word_count = binary_len / 4;

                spv_text text;
                spv_diagnostic diagnostic;
                spv_context ctx = spvContextCreate(SPV_ENV_UNIVERSAL_1_0);
                int options = /*SPV_BINARY_TO_TEXT_OPTION_COLOR |*/ SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES;
                spv_result_t disResult = spvBinaryToText(ctx, words, word_count, options, &text, &diagnostic);
                if (disResult == SPV_SUCCESS)
                {
                    output = text->str;
                    output_len = text->length;
                } // if
                else
                {
                    fprintf(stderr, "\nERROR DIAGNOSTIC: %s\n\n", diagnostic->error);
                } // else

                spv_result_t validateResult = spvValidateBinary(ctx, words, word_count, &diagnostic);
                if (validateResult != SPV_SUCCESS)
                {
                    fprintf(stderr, "\nVALIDATION FAILURE: %s\n\n", diagnostic->error);
                } // if

                if (disResult != SPV_SUCCESS || validateResult != SPV_SUCCESS)
                {
                    exit(EXIT_FAILURE);
                } // if

                // FIXME: we're currently just leaking this disassembly...
#else
                output = pd->output.c_str();
                output_len = pd->output_len;
#endif
            } // if
            else
            {
                output = pd->output.c_str();
                output_len = pd->output_len;
            } // else

            INDENT();
            printf("OUTPUT:\n");
            indent++;
            INDENT();
            for (i = 0; i < output_len; i++)
            {
                putchar((int) output[i]);
                if (output[i] == '\n')
                    INDENT();
            } // for
            printf("\n");
            indent--;
        } // if
    } // else

    printf("\n\n");
} // print_shader

static void print_value(const MOJOSHADER_effectValue *value,
                        const unsigned int indent)
{
    int i, r, c;

    INDENT();
    printf("VALUE: %s -> %s\n", value->name, value->semantic);

    static const char *classes[] =
    {
        "SCALAR",
        "VECTOR",
        "ROW-MAJOR MATRIX",
        "COLUMN-MAJOR MATRIX",
        "OBJECT",
        "STRUCT"
    };
    static const char *types[] =
    {
        "VOID",
        "BOOL",
        "INT",
        "FLOAT",
        "STRING",
        "TEXTURE",
        "TEXTURE1D",
        "TEXTURE2D",
        "TEXTURE3D",
        "TEXTURECUBE",
        "SAMPLER",
        "SAMPLER1D",
        "SAMPLER2D",
        "SAMPLER3D",
        "SAMPLERCUBE",
        "PIXELSHADER",
        "VERTEXSHADER",
        "UNSUPPORTED"
    };
    do_indent(indent + 1);
    printf("CLASS: %s\n", classes[value->type.parameter_class]);
    do_indent(indent + 1);
    printf("TYPE: %s\n", types[value->type.parameter_type]);

    do_indent(indent + 1);
    printf("ROWS/COLUMNS/ELEMENTS: %d, %d, %d\n",
           value->type.rows, value->type.columns, value->type.elements);
    do_indent(indent + 1);
    printf("TOTAL VALUES: %d\n", value->value_count);

    if (value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER
     || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER1D
     || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER2D
     || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER3D
     || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLERCUBE)
    {
        do_indent(indent + 1);
        printf("SAMPLER VALUES:\n");
        for (i = 0; i < value->value_count; i++)
        {
            MOJOSHADER_effectSamplerState *state = &value->valuesSS[i];

            static const char *samplerstatetypes[] =
            {
                "UNKNOWN0",
                "UNKNOWN1",
                "UNKNOWN2",
                "UNKNOWN3",
                "TEXTURE",
                "ADDRESSU",
                "ADDRESSV",
                "ADDRESSW",
                "BORDERCOLOR",
                "MAGFILTER",
                "MINFILTER",
                "MIPFILTER",
                "MIPMAPLODBIAS",
                "MAXMIPLEVEL",
                "MAXANISOTROPY",
                "SRGBTEXTURE",
                "ELEMENTINDEX",
                "DMAPOFFSET",
            };
            do_indent(indent + 2);
            printf("TYPE: %s -> ", samplerstatetypes[state->type]);

            /* Assuming only one value per state! */
            if (state->type == MOJOSHADER_SAMP_MIPMAPLODBIAS)
            {
                /* float types */
                printf("%.2f\n", *state->value.valuesF);
            } // if
            else
            {
                /* int/enum types */
                printf("%d\n", *state->value.valuesI);
            } // else
        } // for
    } // if
    else
    {
        do_indent(indent + 1);
        printf("%s VALUES:\n", types[value->type.parameter_type]);
        i = 0;
        do
        {
            static const char *prints[] =
            {
                "%X ",
                "%d ",
                "%d ",
                "%.2f ",
                "%d ",
                "%d ",
                "%d ",
                "%d ",
                "%d ",
                "%d ",
                "SAMPLER?! ",
                "SAMPLER?! ",
                "SAMPLER?! ",
                "SAMPLER?! ",
                "SAMPLER?! ",
                "%d ",
                "%d ",
                "%X "
            };
            for (r = 0; r < value->type.rows; r++)
            {
                do_indent(indent + 2);
                for (c = 0; c < value->type.columns; c++)
                {
                    const int offset = (i * value->type.rows * 4) + (r * 4) + c;
                    if (value->type.parameter_type == MOJOSHADER_SYMTYPE_FLOAT)
                        printf(prints[value->type.parameter_type], value->valuesF[offset]);
                    else
                        printf(prints[value->type.parameter_type], value->valuesI[offset]);
                } // for
                printf("\n");
            } // for
        } while (++i < value->type.elements);
    } // else
} // print_value


static void print_effect(const char *fname, const MOJOSHADER_effect *effect,
                         const unsigned int indent)
{
    INDENT();
    printf("\n");
    if (effect->error_count > 0)
    {
        int i;
        for (i = 0; i < effect->error_count; i++)
        {
            const MOJOSHADER_error *err = &effect->errors[i];
            INDENT();
            printf("%s:%d: ERROR: %s\n",
                    err->filename.empty() ? fname : err->filename.c_str(),
                    err->error_position, err->error.c_str());
        } // for
    } // if
    else
    {
        int i, j, k;
        const MOJOSHADER_effectTechnique *technique = effect->techniques;
        const MOJOSHADER_effectObject *object = effect->objects;
        const MOJOSHADER_effectParam *param = effect->params;

        for (i = 0; i < effect->param_count; i++, param++)
        {
            INDENT();
            printf("PARAM #%d\n", i);
            print_value(&param->value, indent + 1);

            if (param->annotation_count > 0)
            {
                do_indent(indent + 1);
                printf("ANNOTATIONS:\n");
            } // if
            for (j = 0; j < param->annotation_count; j++)
            {
                print_value(&param->annotations[j], indent + 2);
            } // for
        } // for
        printf("\n");

        for (i = 0; i < effect->technique_count; i++, technique++)
        {
            const MOJOSHADER_effectPass *pass = technique->passes;
            INDENT();
            printf("TECHNIQUE #%d ('%s'):\n", i, technique->name);
            for (j = 0; j < technique->pass_count; j++, pass++)
            {
                const MOJOSHADER_effectState *state = pass->states;
                do_indent(indent + 1);
                printf("PASS #%d ('%s'):\n", j, pass->name);
                for (k = 0; k < pass->state_count; k++, state++)
                {
                    do_indent(indent + 2);
                    printf("STATE %d:\n", state->type);
                    print_value(&state->value, indent + 3);
                } // for
            } // for
        } // for
        printf("\n");

        /* Start at index 1, 0 is always empty (thanks Microsoft!) */
        object++;
        for (i = 1; i < effect->object_count; i++, object++)
        {
            INDENT();
            if (object->type == MOJOSHADER_SYMTYPE_PIXELSHADER
             || object->type == MOJOSHADER_SYMTYPE_VERTEXSHADER)
            {
                if (object->shader.is_preshader)
                {
                    printf("OBJECT #%d: PRESHADER, technique %u, pass %u, param %s\n", i,
                           object->shader.technique, object->shader.pass,
                           effect->params[object->shader.params[0]].value.name);
                    print_preshader(object->shader.preshader, indent + 1);
                } // if
                else
                {
                    printf("OBJECT #%d: SHADER, technique %u, pass %u\n", i,
                           object->shader.technique, object->shader.pass);
                    print_shader(fname,
                                 (MOJOSHADER_parseData*) object->shader.shader,
                                 indent + 1);
                } // else
            } // if
            else if (object->type == MOJOSHADER_SYMTYPE_STRING)
                printf("OBJECT #%d: STRING, '%s'\n", i,
                       object->string.string);
            else if (object->type == MOJOSHADER_SYMTYPE_SAMPLER
                  || object->type == MOJOSHADER_SYMTYPE_SAMPLER1D
                  || object->type == MOJOSHADER_SYMTYPE_SAMPLER2D
                  || object->type == MOJOSHADER_SYMTYPE_SAMPLER3D
                  || object->type == MOJOSHADER_SYMTYPE_SAMPLERCUBE)
                printf("OBJECT #%d: MAPPING, '%s'\n", i,
                       object->mapping.name);
            else if (object->type == MOJOSHADER_SYMTYPE_TEXTURE
                  || object->type == MOJOSHADER_SYMTYPE_TEXTURE1D
                  || object->type == MOJOSHADER_SYMTYPE_TEXTURE2D
                  || object->type == MOJOSHADER_SYMTYPE_TEXTURE3D
                  || object->type == MOJOSHADER_SYMTYPE_TEXTURECUBE)
                printf("OBJECT #%d: TEXTURE\n", i);
            else
                printf("UNKNOWN OBJECT: #%d\n", i);
        } // for
    } // else
} // print_effect


static const char *effect_profile = NULL;


static void* MOJOSHADERCALL effect_compile_shader(
    const void *ctx,
    const char *mainfn,
    const unsigned char *tokenbuf,
    const unsigned int bufsize,
    const MOJOSHADER_swizzle *swiz,
    const unsigned int swizcount,
    const MOJOSHADER_samplerMap *smap,
    const unsigned int smapcount
) {
    return (MOJOSHADER_parseData*) MOJOSHADER_parse(effect_profile, mainfn,
                                                    tokenbuf, bufsize,
                                                    swiz, swizcount,
                                                    smap, smapcount,
                                                    Malloc, Free, NULL);
} // effect_compile_shader


static void MOJOSHADERCALL effect_delete_shader(const void *ctx, void *shader)
{

} // effect_delete_shader


static MOJOSHADER_parseData* MOJOSHADERCALL effect_get_parse_data(void *shader)
{
    return (MOJOSHADER_parseData*) shader;
} // effect_get_parse_data

class ParseResult {
public:
    ParseResult() = default;
    explicit ParseResult(std::vector<MOJOSHADER_parseData*> shaders) {
        this->shaders = std::move(shaders);
    }

    MOJOSHADER_parseData get_shader(int idx) {
        return *this->shaders[idx];
    }

    [[nodiscard]] int num_shaders() const {
        return (int) this->shaders.size();
    }

    [[nodiscard]] int parse(const uintptr_t buf_, const int len, std::string prof)
    {
        const unsigned char *buf = reinterpret_cast<unsigned char*>(buf_);
        // all of this is so stupid
        if (((buf[0] == 0x01) && (buf[1] == 0x09) &&
             (buf[2] == 0xFF) && (buf[3] == 0xFE)) ||
            ((buf[0] == 0xCF) && (buf[1] == 0x0B) &&
             (buf[2] == 0xF0) && (buf[3] == 0xBC))) {
            const MOJOSHADER_effect *effect;
            const MOJOSHADER_effectShaderContext ctx =
                    {
                            effect_compile_shader,
                            NULL, /* Meh! */
                            effect_delete_shader,
                            effect_get_parse_data,
                            /* Meh! */
                            NULL,
                            NULL,
                            NULL,
                            NULL
                    };
            effect_profile = prof.c_str();
            effect = MOJOSHADER_compileEffect(buf, len, NULL, 0, NULL, 0, &ctx);
            int num_shaders = effect->object_count;
            std::vector<MOJOSHADER_parseData *> shaders;
            for (int i = 0; i < effect->object_count; i++) {
                MOJOSHADER_effectObject *object = &effect->objects[i];
                switch (object->type) {
                    case MOJOSHADER_SYMTYPE_VERTEXSHADER:
                    case MOJOSHADER_SYMTYPE_PIXELSHADER:
                        if (!object->shader.is_preshader) {
                            const MOJOSHADER_parseData *shader =
                                    ctx.getParseData(object->shader.shader);
                            if (shader) {
                                MOJOSHADER_parseData *pd = ((MOJOSHADER_parseData *) object->shader.shader);
                                shaders.push_back(pd);
                            }
                        }
                        break;
                    default:
                        break;
                }
            }

            this->shaders = shaders;
            return 0;
        }
        return -1;
    }

    std::vector<MOJOSHADER_parseData*> shaders;
};

EMSCRIPTEN_BINDINGS(MojoShader) {
        emscripten::class_<ParseResult>("EffectParser")
                .constructor<>()
                .function("get_shader", &ParseResult::get_shader)
                .function("num_shaders", &ParseResult::num_shaders)
                .function("parse", &ParseResult::parse, emscripten::allow_raw_pointers());

        emscripten::class_<MOJOSHADER_parseData>("ParseData")
                .constructor<>()
                .function("get_error", &MOJOSHADER_parseData::get_error)
                .function("get_constant", &MOJOSHADER_parseData::get_constant)
                .function("get_input", &MOJOSHADER_parseData::get_input)
                .function("get_output", &MOJOSHADER_parseData::get_output)
                .function("get_swizzle", &MOJOSHADER_parseData::get_swizzle)
                .function("get_sampler", &MOJOSHADER_parseData::get_sampler)
                .function("get_uniform", &MOJOSHADER_parseData::get_uniform)
                .function("get_symbol", &MOJOSHADER_parseData::get_symbol)
                .function("get_preshader", &MOJOSHADER_parseData::get_preshader)
                .property("error_count", &MOJOSHADER_parseData::error_count)
                .property("profile", &MOJOSHADER_parseData::profile)
                .property("output", &MOJOSHADER_parseData::output)
                .property("instruction_count", &MOJOSHADER_parseData::instruction_count)
                .property("shader_type", &MOJOSHADER_parseData::shader_type)
                .property("mainfn", &MOJOSHADER_parseData::mainfn)
                .property("constant_count", &MOJOSHADER_parseData::constant_count)
                .property("input_count", &MOJOSHADER_parseData::input_count)
                .property("output_count", &MOJOSHADER_parseData::output_count)
                .property("swizzle_count", &MOJOSHADER_parseData::swizzle_count)
                .property("symbol_count", &MOJOSHADER_parseData::symbol_count)
                .property("uniform_count", &MOJOSHADER_parseData::uniform_count);

        emscripten::value_object<MOJOSHADER_attribute>("Attribute")
                .field("usage", &MOJOSHADER_attribute::usage)
                .field("index", &MOJOSHADER_attribute::index)
                .field("name", &MOJOSHADER_attribute::name);

        emscripten::enum_<MOJOSHADER_usage>("Usage")
                .value("Unknown", MOJOSHADER_USAGE_UNKNOWN)
                .value("Position", MOJOSHADER_USAGE_POSITION)
                .value("BlendWeight", MOJOSHADER_USAGE_BLENDWEIGHT)
                .value("BlendIndices", MOJOSHADER_USAGE_BLENDINDICES)
                .value("Normal", MOJOSHADER_USAGE_NORMAL)
                .value("PointSize", MOJOSHADER_USAGE_POINTSIZE)
                .value("TexCoord", MOJOSHADER_USAGE_TEXCOORD)
                .value("Tangent", MOJOSHADER_USAGE_TANGENT)
                .value("Binormal", MOJOSHADER_USAGE_BINORMAL)
                .value("TessFactor", MOJOSHADER_USAGE_TESSFACTOR)
                .value("PositionT", MOJOSHADER_USAGE_POSITIONT)
                .value("Color", MOJOSHADER_USAGE_COLOR)
                .value("Fog", MOJOSHADER_USAGE_FOG)
                .value("Depth", MOJOSHADER_USAGE_DEPTH)
                .value("Sample", MOJOSHADER_USAGE_SAMPLE);

        emscripten::value_object<MOJOSHADER_uniform>("Uniform")
                .field("type", &MOJOSHADER_uniform::type)
                .field("index", &MOJOSHADER_uniform::index)
                .field("array_count", &MOJOSHADER_uniform::array_count)
                .field("constant", &MOJOSHADER_uniform::constant)
                .field("name", &MOJOSHADER_uniform::name);

        emscripten::enum_<MOJOSHADER_uniformType>("UniformType")
                .value("Unknown", MOJOSHADER_UNIFORM_UNKNOWN)
                .value("Float", MOJOSHADER_UNIFORM_FLOAT)
                .value("Int", MOJOSHADER_UNIFORM_INT)
                .value("Bool", MOJOSHADER_UNIFORM_BOOL);

        emscripten::register_vector<int>("VectorInt");
        emscripten::register_vector<float>("VectorFloat");

        emscripten::class_<MOJOSHADER_constant>("Constant")
                .property("type", &MOJOSHADER_constant::type)
                .property("index", &MOJOSHADER_constant::index)
                .function("get_float4", &MOJOSHADER_constant::get_float4)
                .function("get_int4", &MOJOSHADER_constant::get_int4)
                .function("get_bool", &MOJOSHADER_constant::get_bool);

        emscripten::value_object<MOJOSHADER_symbol>("Symbol")
                .field("name", &MOJOSHADER_symbol::name)
                .field("register_set", &MOJOSHADER_symbol::name)
                .field("register_index", &MOJOSHADER_symbol::register_index)
                .field("register_count", &MOJOSHADER_symbol::register_count)
                .field("info", &MOJOSHADER_symbol::info);

        emscripten::enum_<MOJOSHADER_symbolRegisterSet>("SymbolRegisterSet")
                .value("Bool", MOJOSHADER_SYMREGSET_BOOL)
                .value("Int4", MOJOSHADER_SYMREGSET_INT4)
                .value("Float4", MOJOSHADER_SYMREGSET_FLOAT4)
                .value("Sampler", MOJOSHADER_SYMREGSET_SAMPLER);

        emscripten::value_object<MOJOSHADER_symbolTypeInfo>("SymbolTypeInfo")
                .field("parameter_class", &MOJOSHADER_symbolTypeInfo::parameter_class)
                .field("parameter_type", &MOJOSHADER_symbolTypeInfo::parameter_type)
                .field("rows", &MOJOSHADER_symbolTypeInfo::rows)
                .field("columns", &MOJOSHADER_symbolTypeInfo::columns)
                .field("elements", &MOJOSHADER_symbolTypeInfo::elements)
                .field("member_count", &MOJOSHADER_symbolTypeInfo::member_count);

        emscripten::enum_<MOJOSHADER_symbolClass>("SymbolClass")
                .value("Scalar", MOJOSHADER_SYMCLASS_SCALAR)
                .value("Vector", MOJOSHADER_SYMCLASS_VECTOR)
                .value("MatrixRows", MOJOSHADER_SYMCLASS_MATRIX_ROWS)
                .value("MatrixColumns", MOJOSHADER_SYMCLASS_MATRIX_COLUMNS)
                .value("Object", MOJOSHADER_SYMCLASS_OBJECT)
                .value("Struct", MOJOSHADER_SYMCLASS_STRUCT);

        emscripten::enum_<MOJOSHADER_symbolType>("SymbolType")
                .value("Void", MOJOSHADER_SYMTYPE_VOID)
                .value("Bool", MOJOSHADER_SYMTYPE_BOOL)
                .value("Int", MOJOSHADER_SYMTYPE_INT)
                .value("Float", MOJOSHADER_SYMTYPE_FLOAT)
                .value("String", MOJOSHADER_SYMTYPE_STRING)
                .value("Texture", MOJOSHADER_SYMTYPE_TEXTURE)
                .value("Texture1D", MOJOSHADER_SYMTYPE_TEXTURE1D)
                .value("Texture2D", MOJOSHADER_SYMTYPE_TEXTURE2D)
                .value("Texture3D", MOJOSHADER_SYMTYPE_TEXTURE3D)
                .value("TextureCube", MOJOSHADER_SYMTYPE_TEXTURECUBE)
                .value("Sampler", MOJOSHADER_SYMTYPE_SAMPLER)
                .value("Sampler1D", MOJOSHADER_SYMTYPE_SAMPLER1D)
                .value("Sampler2D", MOJOSHADER_SYMTYPE_SAMPLER2D)
                .value("Sampler3D", MOJOSHADER_SYMTYPE_SAMPLER3D)
                .value("SamplerCube", MOJOSHADER_SYMTYPE_SAMPLERCUBE)
                .value("PixelShader", MOJOSHADER_SYMTYPE_PIXELSHADER)
                .value("VertexShader", MOJOSHADER_SYMTYPE_VERTEXSHADER)
                .value("PixelFragment", MOJOSHADER_SYMTYPE_PIXELFRAGMENT)
                .value("VertexFragment", MOJOSHADER_SYMTYPE_VERTEXFRAGMENT)
                .value("Unsupported", MOJOSHADER_SYMTYPE_UNSUPPORTED);
};
