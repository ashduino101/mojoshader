emcc mojoshader.c mojoshader_common.c mojoshader_opengl.c mojoshader_metal.c profiles/mojoshader_profile_arb1.c profiles/mojoshader_profile_bytecode.c profiles/mojoshader_profile_common.c profiles/mojoshader_profile_d3d.c profiles/mojoshader_profile_glsl.c profiles/mojoshader_profile_hlsl.c profiles/mojoshader_profile_metal.c  \
mojoshader_effects.c dxdisassemble.c \
-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DMOJOSHADER_EFFECT_SUPPORT \
-s "EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8" \
-s "EXPORTED_FUNCTIONS=_malloc,_free,_do_parse" \
--no-entry -O2 \
-o ./dxdisassemble.js
