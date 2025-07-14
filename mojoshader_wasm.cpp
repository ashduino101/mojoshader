#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <cassert>

#include <emscripten/emscripten.h>
#include <emscripten/console.h>

#include "mojoshader.h"

#define __MOJOSHADER_INTERNAL__ 1

#include "mojoshader_internal.h"

#include "json.hpp"

using namespace nlohmann;

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#define Malloc NULL
#define Free NULL

static const char *shader_type(const MOJOSHADER_shaderType s) {
    switch (s) {
        case MOJOSHADER_TYPE_UNKNOWN:
            return "unknown";
        case MOJOSHADER_TYPE_PIXEL:
            return "pixel";
        case MOJOSHADER_TYPE_VERTEX:
            return "vertex";
        case MOJOSHADER_TYPE_GEOMETRY:
            return "geometry";
        default:
            return "unknown";
    } // switch
} // shader_type


json serialize_typeinfo(const MOJOSHADER_symbolTypeInfo *info) {
    static const char *symclasses[] = {
            "scalar", "vector", "rm_matrix",
            "cm_matrix", "object", "struct"
    };

    static const char *symtypes[] = {
            "void", "bool", "int", "float", "string", "texture",
            "texture1d", "texture2d", "texture3d", "texturecube",
            "sampler", "sampler1d", "sampler2d", "sampler3d",
            "samplercube", "pixelshader", "vertexshader", "<unsupported>"
    };

    json symjson;

    symjson["param_class"] = symclasses[info->parameter_class];
    symjson["param_type"] = symtypes[info->parameter_type];
    symjson["rows"] = info->rows;
    symjson["columns"] = info->columns;
    symjson["elements"] = info->elements;
    symjson["members"] = json::array();

    if (info->member_count > 0) {
        for (int i = 0; i < info->member_count; i++) {
            const MOJOSHADER_symbolStructMember *member = &info->members[i];
            json mj;
            mj["name"] = member->name;
            mj["info"] = serialize_typeinfo(&member->info);
            symjson["members"].push_back(mj);
        }
    }

    return symjson;
}


json serialize_symbol(const MOJOSHADER_symbol sym) {
    json symjson;
    static const char *regsets[] = {
            "bool", "int4", "float4", "sampler"
    };

    symjson["name"] = sym.name;
    symjson["register_set"] = regsets[sym.register_set];
    symjson["register_index"] = sym.register_index;
    symjson["register_count"] = sym.register_count;

    symjson["type_info"] = serialize_typeinfo(&sym.info);

    return symjson;
}


json serialize_preshader_operand(const MOJOSHADER_preshader *preshader,
                                    const int instidx, const int opidx) {
    static char mask[] = {'x', 'y', 'z', 'w'};
    const MOJOSHADER_preshaderInstruction *inst = &preshader->instructions[instidx];
    const MOJOSHADER_preshaderOperand *operand = &inst->operands[opidx];
    const int elems = inst->element_count;
    const int isscalarop = (inst->opcode >= MOJOSHADER_PRESHADEROP_SCALAR_OPS);
    const int isscalar = ((isscalarop) && (opidx == 0)); // probably wrong.
    int i;

    json oj;

    switch (operand->type) {
        case MOJOSHADER_PRESHADEROPERAND_LITERAL: {
            oj["type"] = "literal";
            const double *lit = &preshader->literals[operand->index];
            oj["value"] = json::array();
            if (isscalar) {
                const double val = *lit;
                for (i = 0; i < elems; i++)
                    oj["value"].push_back(val);
            }
            else {
                for (i = 0; i < elems; i++, lit++)
                    oj["value"].push_back(*lit);
            }
            break;
        }

        case MOJOSHADER_PRESHADEROPERAND_INPUT:
        case MOJOSHADER_PRESHADEROPERAND_OUTPUT:
        case MOJOSHADER_PRESHADEROPERAND_TEMP: {
            switch (operand->type) {
                case MOJOSHADER_PRESHADEROPERAND_INPUT:
                    oj["type"] = "input";
                    break;
                case MOJOSHADER_PRESHADEROPERAND_OUTPUT:
                    oj["type"] = "output";
                    break;
                case MOJOSHADER_PRESHADEROPERAND_TEMP:
                    oj["type"] = "temp";
                    break;
                default:
                    break;
            }
            int idx = operand->index % 4;
            oj["index"] = idx;
            oj["array_registers"] = json::array();
            for (i = 0; i < operand->array_register_count; i++) {
                oj["array_registers"].push_back(operand->array_registers[i]);
            }
            break;
        }
    }

    return oj;
}


json serialize_preshader(const MOJOSHADER_preshader *preshader) {
    json psj;

    MOJOSHADER_preshaderInstruction *inst = preshader->instructions;
    int i, j;

    static const char *opcodestr[] = {
            "nop", "mov", "neg", "rcp", "frc", "exp", "log", "rsq", "sin", "cos",
            "asin", "acos", "atan", "min", "max", "lt", "ge", "add", "mul",
            "atan2", "div", "cmp", "movc", "dot", "noise", "min", "max", "lt",
            "ge", "add", "mul", "atan2", "div", "dot", "noise"
    };

    psj["symbols"] = json::array();
    for (i = 0; i < preshader->symbol_count; i++) {
        psj["symbols"].push_back(serialize_symbol(preshader->symbols[i]));
    }

    psj["instructions"] = json::array();
    for (i = 0; i < preshader->instruction_count; i++, inst++) {
        json instj;
        instj["opcode"] = opcodestr[inst->opcode];
        instj["operands"] = json::array();
        for (j = 0; j < inst->operand_count; j++) {
            instj["operands"].push_back(serialize_preshader_operand(preshader, i, j));
        }
        psj["instructions"].push_back(instj);
    }

    return psj;
}


json serialize_attr(const MOJOSHADER_attribute &attribute) {
    static const char *usagenames[] = {
            "<unknown>",
            "position", "blendweight", "blendindices", "normal",
            "psize", "texcoord", "tangent", "binormal", "tessfactor",
            "positiont", "color", "fog", "depth", "sample"
    };

    json aj;
    aj["name"] = attribute.name;
    aj["index"] = attribute.index;
    aj["usage"] = usagenames[1 + (int) attribute.usage];

    return aj;
}


json serialize_shader(const MOJOSHADER_parseData *pd) {
    json sj;

    sj["profile"] = pd->profile;

    if (pd->error_count > 0) {
        sj["errors"] = json::array();
        for (int i = 0; i < pd->error_count; i++) {
            const MOJOSHADER_error *err = &pd->errors[i];
            json ej;
            ej["error"] = err->error;
            ej["position"] = err->error_position;
            ej["filename"] = err->filename;
            sj["errors"].push_back(ej);
        }
    } else {
        sj["type"] = shader_type(pd->shader_type);
        sj["version"] = {pd->major_ver, pd->minor_ver};
        sj["instruction_count"] = pd->instruction_count;
        sj["main_function"] = pd->mainfn;
        sj["inputs"] = json::array();
        for (int i = 0; i < pd->input_count; i++) {
            sj["inputs"].push_back(serialize_attr(pd->inputs[i]));
        }
        sj["outputs"] = json::array();
        for (int i = 0; i < pd->output_count; i++) {
            sj["outputs"].push_back(serialize_attr(pd->outputs[i]));
        }

        static const char *typenames[] = {"float", "int", "bool"};

        sj["constants"] = json::array();
        for (int i = 0; i < pd->constant_count; i++) {
            json cj;
            const MOJOSHADER_constant *c = &pd->constants[i];
            cj["index"] = c->index;
            cj["type"] = typenames[(int) c->type];
            if (c->type == MOJOSHADER_UNIFORM_FLOAT) {
                cj["value"] = {c->value.f[0], c->value.f[1],
                               c->value.f[2], c->value.f[3]};
            } // if
            else if (c->type == MOJOSHADER_UNIFORM_INT) {
                cj["value"] = {c->value.i[0], c->value.i[1],
                               c->value.i[2], c->value.i[3]};
            } // else if
            else if (c->type == MOJOSHADER_UNIFORM_BOOL) {
                cj["value"] = c->value.b;
            } // else if
            else {
                cj["value"] = nullptr;
            } // else
            sj["constants"].push_back(cj);
        } // for

        sj["uniforms"] = json::array();
        for (int i = 0; i < pd->uniform_count; i++) {
            json uj;
            const MOJOSHADER_uniform *u = &pd->uniforms[i];
            uj["name"] = u->name;
            uj["type"] = typenames[(int) u->type];
            uj["constant"] = u->constant != 0;
            uj["index"] = u->index;
            uj["array_count"] = u->array_count;

            sj["uniforms"].push_back(uj);
        }

        sj["samplers"] = json::array();
        for (int i = 0; i < pd->sampler_count; i++) {
            json sampj;
            static const char *sampler_typenames[] = {"2d", "cube", "volume"};
            const MOJOSHADER_sampler *s = &pd->samplers[i];
            sampj["index"] = s->index;
            sampj["name"] = s->name;
            sampj["type"] = sampler_typenames[(int) s->type];
            sampj["texbem"] = s->texbem != 0;
            sj["samplers"].push_back(sampj);
        }
    }

    sj["symbols"] = json::array();
    for (int i = 0; i < pd->symbol_count; i++) {
        sj["symbols"].push_back(serialize_symbol(pd->symbols[i]));
    }

    if (pd->preshader != nullptr)
        sj["preshader"] = serialize_preshader(pd->preshader);

    if (!pd->output.empty()) {
        sj["output"] = pd->output;
    }

    return sj;
}

json serialize_value(const MOJOSHADER_effectValue *value) {
    int i, r, c;
    json vj;

    vj["name"] = value->name;
    vj["semantic"] = value->semantic;

    static const char *classes[] =
            {
                    "scalar",
                    "vector",
                    "rm_matrix",
                    "cm_matrix",
                    "object",
                    "struct"
            };
    static const char *types[] =
            {
                    "void",
                    "bool",
                    "int",
                    "float",
                    "string",
                    "texture",
                    "texture1d",
                    "texture2d",
                    "texture3d",
                    "texturecube",
                    "sampler",
                    "sampler1d",
                    "sampler2d",
                    "sampler3d",
                    "samplercube",
                    "pixelshader",
                    "vertexshader",
                    "<unsupported>"
            };

    vj["type"] = serialize_typeinfo(&value->type);
    vj["num_values"] = value->value_count;

    vj["values"] = json::array();
    if (value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER
        || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER1D
        || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER2D
        || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLER3D
        || value->type.parameter_type == MOJOSHADER_SYMTYPE_SAMPLERCUBE) {
        for (i = 0; i < value->value_count; i++) {
            MOJOSHADER_effectSamplerState *state = &value->valuesSS[i];

            static const char *samplerstatetypes[] =
                    {
                            "unknown0",
                            "unknown1",
                            "unknown2",
                            "unknown3",
                            "texture",
                            "addressU",
                            "addressV",
                            "addressW",
                            "bordercolor",
                            "magfilter",
                            "minfilter",
                            "mipfilter",
                            "mipmaplodbias",
                            "maxmiplevel",
                            "maxanisotropy",
                            "srgbtexture",
                            "elementindex",
                            "dmapoffset",
                    };
            json val;
            val["type"] = samplerstatetypes[state->type];

            /* Assuming only one value per state! */
            if (state->type == MOJOSHADER_SAMP_MIPMAPLODBIAS) {
                /* float types */
                val["value"] = *state->value.valuesF;
            } else {
                /* int/enum types */
                val["value"] = *state->value.valuesI;
            }
            vj["values"].push_back(val);
        }
    } else {
        i = 0;
        do {
            for (r = 0; r < value->type.rows; r++) {
                for (c = 0; c < value->type.columns; c++) {
                    const int offset = (i * value->type.rows * 4) + (r * 4) + c;
                    if (value->type.parameter_type == MOJOSHADER_SYMTYPE_FLOAT)
                        vj["values"].push_back(value->valuesF[offset]);
                    else
                        vj["values"].push_back(value->valuesI[offset]);
                }
            }
        } while (++i < value->type.elements);
    }

    return vj;
}


json serialize_effect(const MOJOSHADER_effect *effect) {
    json ej;

    ej["errors"] = json::array();
    ej["params"] = json::array();
    ej["techniques"] = json::array();
    ej["objects"] = json::array();

    if (effect->error_count > 0) {
        int i;
        for (i = 0; i < effect->error_count; i++) {
            const MOJOSHADER_error *err = &effect->errors[i];
            json errj;
            errj["error"] = err->error;
            errj["position"] = err->error_position;
            errj["filename"] = err->filename;
            errj["errors"].push_back(ej);
        } // for
    } // if
    else {
        int i, j, k;
        const MOJOSHADER_effectTechnique *technique = effect->techniques;
        const MOJOSHADER_effectObject *object = effect->objects;
        const MOJOSHADER_effectParam *param = effect->params;

        for (i = 0; i < effect->param_count; i++, param++) {
            json pj;
            pj["value"] = serialize_value(&param->value);
            pj["annotations"] = json::array();
            for (j = 0; j < param->annotation_count; j++) {
                pj["annotations"].push_back(serialize_value(&param->annotations[j]));
            }
            ej["params"].push_back(pj);
        }

        for (i = 0; i < effect->technique_count; i++, technique++) {
            const MOJOSHADER_effectPass *pass = technique->passes;
            json tj;
            tj["name"] = technique->name;
            tj["passes"] = json::array();
            for (j = 0; j < technique->pass_count; j++, pass++) {
                const MOJOSHADER_effectState *state = pass->states;
                json pj;
                pj["name"] = pass->name;
                pj["states"] = json::array();
                for (k = 0; k < pass->state_count; k++, state++) {
                    json sj;
                    sj["type"] = state->type;
                    sj["value"] = serialize_value(&state->value);
                    pj["states"].push_back(sj);
                }
                pj["annotations"] = json::array();
                for (k = 0; k < pass->annotation_count; k++) {
                    pj["annotations"].push_back(serialize_value(&pass->annotations[k]));
                }

                tj["passes"].push_back(pj);
            }
            tj["annotations"] = json::array();
            for (j = 0; j < technique->annotation_count; j++) {
                tj["annotations"].push_back(serialize_value(&technique->annotations[j]));
            }

            ej["techniques"].push_back(tj);
        }

        /* Start at index 1, 0 is always empty (thanks Microsoft!) */
        object++;
        for (i = 1; i < effect->object_count; i++, object++) {
            json oj;
            static const char *types[] =
                    {
                            "void",
                            "bool",
                            "int",
                            "float",
                            "string",
                            "texture",
                            "texture1d",
                            "texture2d",
                            "texture3d",
                            "texturecube",
                            "sampler",
                            "sampler1d",
                            "sampler2d",
                            "sampler3d",
                            "samplercube",
                            "pixelshader",
                            "vertexshader",
                            "<unsupported>"
                    };
            oj["type"] = types[(int) object->type];
            if (object->type == MOJOSHADER_SYMTYPE_PIXELSHADER
                || object->type == MOJOSHADER_SYMTYPE_VERTEXSHADER) {
                json vj;
                vj["is_preshader"] = object->shader.is_preshader;
                vj["technique"] = object->shader.technique;
                vj["pass"] = object->shader.pass;
                if (object->shader.is_preshader) {
                    vj["preshader"] = serialize_preshader(object->shader.preshader);
                } else {
                    vj["shader"] = serialize_shader((MOJOSHADER_parseData *) object->shader.shader);
                }
                oj["value"] = vj;
            } else if (object->type == MOJOSHADER_SYMTYPE_STRING)
                oj["value"] = object->string.string;
            else if (object->type == MOJOSHADER_SYMTYPE_SAMPLER
                     || object->type == MOJOSHADER_SYMTYPE_SAMPLER1D
                     || object->type == MOJOSHADER_SYMTYPE_SAMPLER2D
                     || object->type == MOJOSHADER_SYMTYPE_SAMPLER3D
                     || object->type == MOJOSHADER_SYMTYPE_SAMPLERCUBE)
                oj["value"] = object->mapping.name;
            else if (object->type == MOJOSHADER_SYMTYPE_TEXTURE
                     || object->type == MOJOSHADER_SYMTYPE_TEXTURE1D
                     || object->type == MOJOSHADER_SYMTYPE_TEXTURE2D
                     || object->type == MOJOSHADER_SYMTYPE_TEXTURE3D
                     || object->type == MOJOSHADER_SYMTYPE_TEXTURECUBE)
                oj["value"] = nullptr;

            ej["objects"].push_back(oj);
        }
    }
    return ej;
}


static const char *effect_profile = nullptr;


static void *MOJOSHADERCALL effect_compile_shader(
        const void *ctx,
        const char *mainfn,
        const unsigned char *tokenbuf,
        const unsigned int bufsize,
        const MOJOSHADER_swizzle *swiz,
        const unsigned int swizcount,
        const MOJOSHADER_samplerMap *smap,
        const unsigned int smapcount
) {
    return (MOJOSHADER_parseData *) MOJOSHADER_parse(effect_profile, mainfn,
                                                     tokenbuf, bufsize,
                                                     swiz, swizcount,
                                                     smap, smapcount,
                                                     Malloc, Free, NULL);
} // effect_compile_shader


static void MOJOSHADERCALL effect_delete_shader(const void *ctx, void *shader) {

} // effect_delete_shader


static MOJOSHADER_parseData *MOJOSHADERCALL effect_get_parse_data(void *shader) {
    return (MOJOSHADER_parseData *) shader;
} // effect_get_parse_data

char *_parse(const uintptr_t buf_, const int len, const char *prof) {
    const unsigned char *buf = reinterpret_cast<unsigned char *>(buf_);
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
        effect_profile = prof;
        effect = MOJOSHADER_compileEffect(buf, len, NULL, 0, NULL, 0, &ctx);

        json j = serialize_effect(effect);

        std::string resp = j.dump();
        char *heap = (char *) malloc(resp.length() + 1);
        memset(heap, 0, resp.length() + 1);
        memcpy(heap, resp.c_str(), resp.length());

        return heap;
    }
    return nullptr;
}

extern "C" {
    char *EMSCRIPTEN_KEEPALIVE parse(const uintptr_t buf_, const int len, char *prof) {
        return _parse(buf_, len, prof);
    }
}
