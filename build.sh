emcmake cmake .
em++ mojoshader.cpp mojoshader_common.cpp mojoshader_opengl.cpp mojoshader_metal.cpp profiles/mojoshader_profile_arb1.cpp profiles/mojoshader_profile_bytecode.cpp profiles/mojoshader_profile_common.cpp profiles/mojoshader_profile_d3d.cpp profiles/mojoshader_profile_glsl.cpp profiles/mojoshader_profile_hlsl.cpp profiles/mojoshader_profile_metal.cpp  \
mojoshader_effects.cpp mojoshader_wasm.cpp \
-std=c++11 \
-ferror-limit=400 \
-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DMOJOSHADER_EFFECT_SUPPORT \
-s "EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8" \
-s "EXPORTED_FUNCTIONS=_malloc,_free" \
--no-entry -O2 \
-lembind --bind \
-s ALLOW_MEMORY_GROWTH=1 \
-s EXPORT_NAME=MojoShader \
-o ./mojoshader.js
