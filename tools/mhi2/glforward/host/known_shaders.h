/* Experimental translations of the captured K3342 compositor shaders.
 * Vertex arithmetic is transcribed from Grate disassembly; fragment blending
 * is reconstructed from the ALU/texture operations. FX10 quantization is not
 * reproduced. Unknown binaries are rejected rather than replaced.
 * blend=1 emits premultiplied source and requests source-over fixed-function
 * blending; blend=2 additionally preserves destination alpha. These replace
 * Tegra's baked destination reads without requiring framebuffer-fetch GLSL.
 */
static const struct { uint64_t hash; size_t size; const char *source; unsigned blend; } shaders[] = {
{ UINT64_C(0x841571f264507820), 708, "attribute vec2 position;uniform vec2 uGlobalOffset;uniform vec4 uSource,uTarget;varying mediump vec2 texout;void main(){texout=position*uSource.zw+uSource.xy;gl_Position=vec4(position*uTarget.zw+uTarget.xy+uGlobalOffset,0.0,1.0);}" },
{ UINT64_C(0x713bf0146ddfca27), 744, "attribute vec2 position;uniform vec4 uSourceBottom,uSourceTop,uTarget;varying mediump vec2 texBottom,texTop;void main(){texBottom=position*uSourceBottom.zw+uSourceBottom.xy;texTop=position*uSourceTop.zw+uSourceTop.xy;gl_Position=vec4(position*uTarget.zw+uTarget.xy,0.0,1.0);}" },
{ UINT64_C(0xf5f1d8bd3514e58c), 520, "attribute vec2 attrTex,attrVertex;varying mediump vec2 var_tex;void main(){var_tex=attrTex;gl_Position=vec4(attrVertex,0.0,1.0);}" },
{ UINT64_C(0xb768a6623dd44225), 892, "precision mediump float;varying vec2 texout;uniform sampler2D uTex;uniform float uOpacity;void main(){vec4 s=texture2D(uTex,texout)*uOpacity;gl_FragColor=s;}", 1 },
{ UINT64_C(0xf2ec23266c4bf76d), 968, "precision mediump float;varying vec2 texBottom,texTop;uniform sampler2D uTexBottom,uTexTop;uniform vec2 uOpacity;void main(){vec4 b=texture2D(uTexBottom,texBottom);vec4 t=texture2D(uTexTop,texTop);gl_FragColor=vec4(t.rgb*uOpacity.x+b.rgb*uOpacity.y*(1.0-t.a*uOpacity.x),1.0);}" },
{ UINT64_C(0xc06fb2c806459895), 748, "precision mediump float;varying vec2 texout;uniform sampler2D uTex;uniform float uOpacity;void main(){gl_FragColor=vec4(texture2D(uTex,texout).rgb*uOpacity,1.0);}" },
{ UINT64_C(0xc3e140e23568c0d7), 704, "precision mediump float;varying vec2 var_tex;uniform sampler2D texture1;void main(){gl_FragColor=vec4(texture2D(texture1,var_tex).rgb,1.0);}" },
/* Native navigation solid-fill and vertex-color programs.
 * Sources transcribed from captured Tegra instruction streams (22/23,70/71).
 * Solid-fill depth is abs(clip.w)*0.99, as encoded by constant c4.x. */
{ UINT64_C(0x6d8ae622f9287c38), 644, "attribute vec4 a_position; uniform mat4 u_mvpMatrix; void main(){vec4 p=u_mvpMatrix*vec4(a_position.xyz,1.0); gl_Position=vec4(p.xy,abs(p.w)*0.99,abs(p.w));}" },
{ UINT64_C(0xeb046d4f9ac3f444), 644, "precision mediump float; uniform vec4 u_color; void main(){gl_FragColor=vec4(u_color.rgb,1.0);}" },
{ UINT64_C(0x4a68cca47b2cc035), 588, "attribute vec4 a_position; attribute vec4 a_color; uniform mat4 u_mvpMatrix; varying mediump vec4 v_color; void main(){gl_Position=u_mvpMatrix*vec4(a_position.xyz,1.0);v_color=a_color;}" },
{ UINT64_C(0xaee443ef82817947), 644, "precision mediump float; varying vec4 v_color; void main(){gl_FragColor=vec4(v_color.rgb,1.0);}" },
/* Native textured navigation pass (68/69): source-over blending is baked
 * into the fragment binary. ALU operand 0x7a uses both fixed10-minus-one
 * and negate, giving (1 - source alpha), not just -source alpha. */
{ UINT64_C(0xc45514502dc3bb9b), 644, "attribute vec4 a_position;attribute vec2 a_texCoord;uniform mat4 u_mvpMatrix;varying mediump vec2 v_texCoord;void main(){gl_Position=u_mvpMatrix*vec4(a_position.xyz,1.0);v_texCoord=a_texCoord;}" },
{ UINT64_C(0x456f4843de8e81e), 708, "precision mediump float;uniform sampler2D s_texture0;varying vec2 v_texCoord;void main(){vec4 s=texture2D(s_texture0,v_texCoord);gl_FragColor=s;}", 1 },
/* Navigation linear-fog fill and extruded road programs (14/15,58/59).
 * Reflection register bindings put raw u_fogStart/u_fogEnd in c4/c5
 * (road: c9/c10). They already contain the application-computed slope
 * and offset. Fragment ALU1 SEND adds fogColor to ALU0; MFU clamps fog.
 * The enabled fourth MAD operand scales color by fog (and road RGB by
 * alpha); the textual Grate dump omits that operand. */
{ UINT64_C(0xb056fb13a7b807b3), 700, "attribute vec4 a_position;uniform mat4 u_mvpMatrix;uniform float u_fogStart,u_fogEnd;varying mediump float v_fogFactor;void main(){gl_Position=u_mvpMatrix*vec4(a_position.xyz,1.0);v_fogFactor=gl_Position.w*u_fogStart+u_fogEnd;}" },
{ UINT64_C(0x4b859a92ee435632), 904, "precision mediump float;uniform vec4 u_color;uniform vec3 u_fogColor;varying float v_fogFactor;void main(){gl_FragColor=vec4(mix(u_fogColor,u_color.rgb,clamp(v_fogFactor,0.0,1.0)),1.0);}" },
{ UINT64_C(0x798d05d4c458a967), 1104, "attribute vec4 a_position;attribute vec4 a_custom;uniform mat4 u_mvpMatrix;uniform vec3 u_origin;uniform vec2 u_scaleH;uniform float u_scaleXYZ,u_widthScale,u_roadGeometryWidthHalf,u_fogStart,u_fogEnd;varying mediump float v_fogFactor;void main(){vec3 p=a_position.xyz*u_scaleXYZ;p+=(u_origin+p)*(a_position.w*u_scaleH.x+u_scaleH.y);p+=a_custom.xyz*(u_widthScale*u_roadGeometryWidthHalf);gl_Position=u_mvpMatrix*vec4(p,1.0);v_fogFactor=gl_Position.w*u_fogStart+u_fogEnd;}" },
{ UINT64_C(0x7d495213a2eb81e3), 1228, "precision mediump float;uniform vec4 u_color;uniform vec3 u_fogColor;varying float v_fogFactor;void main(){vec3 rgb=clamp(mix(u_fogColor,u_color.rgb,clamp(v_fogFactor,0.0,1.0)),0.0,1.0);float a=clamp(u_color.a,0.0,1.0);gl_FragColor=vec4(rgb*a,a);}", 2 },
};
