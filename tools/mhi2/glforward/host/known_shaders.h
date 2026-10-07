/* Experimental translations of the captured K3342 compositor shaders.
 * Vertex arithmetic is transcribed from Grate disassembly; fragment blending
 * is reconstructed from the ALU/texture operations. FX10 quantization is not
 * reproduced. Unknown binaries are rejected rather than replaced.
 * blend=1 emits premultiplied source and requests source-over fixed-function
 * blending; blend=2 additionally preserves destination alpha. These replace
 * Tegra's baked destination reads without requiring framebuffer-fetch GLSL.
 */
struct ShaderTranslation {
    uint64_t hash; size_t size; const char *source; unsigned blend;
    const char *firmware; /* NULL is the historical Porsche/VW fallback. */
};
static const struct ShaderTranslation shaders[] = {
#include "audi_shaders.h"
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
/* Porsche libhybrid variants. Native framebuffer reads are implemented
 * with premultiplied source-over. FX10 rounding remains host precision. */
{ UINT64_C(0x546ecb628e4429af), 1308, "attribute vec4 a_vertex,a_vertexColor;attribute vec2 a_texCoords;uniform mat4 u_modelviewMatrix,u_projectionMatrix;uniform vec4 u_modulateColor;uniform float u_useVertexColor,u_premultiplied;varying mediump vec4 v_color;varying mediump vec2 v_texCoords;void main(){vec4 c=u_modulateColor;if(u_useVertexColor>0.0)c.rgb*=a_vertexColor.rgb;c.a*=a_vertexColor.a;if(u_premultiplied>0.0)c.rgb*=c.a;v_color=c;v_texCoords=a_texCoords;gl_Position=u_projectionMatrix*u_modelviewMatrix*vec4(a_vertex.xyz,1.0);}", 0 },
{ UINT64_C(0xb372986db48cc454), 1412, "attribute vec2 position;uniform vec4 uSource,uTarget;uniform float uAngle,maskAngle;uniform vec2 uResolution,maskOffset;varying mediump vec2 texout,maskout;void main(){vec2 inv=1.0/uResolution;vec2 center=uTarget.xy*inv*2.0-1.0;center-=step(vec2(0.0001),fract(uTarget.zw*0.5))*inv;mat2 r=mat2(cos(uAngle),sin(uAngle),-sin(uAngle),cos(uAngle));gl_Position=vec4(r*position*uTarget.zw*inv+center,0.0,1.0);texout=(position+0.5)*uSource.zw+uSource.xy;mat2 m=mat2(cos(maskAngle),sin(maskAngle),-sin(maskAngle),cos(maskAngle));maskout=m*(texout-maskOffset*inv);}", 0 },
{ UINT64_C(0xa1d1062962f918ff), 952, "precision mediump float;varying vec2 texout,maskout;uniform sampler2D tex,mask;uniform float opacity;void main(){vec4 s=texture2D(tex,texout);float a=s.a*(1.0-texture2D(mask,maskout).r)*opacity;gl_FragColor=vec4(s.rgb*a,a);}", 1 },
{ UINT64_C(0x172ca8f2c75a0855), 864, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;s.rgb*=s.a;gl_FragColor=s;}", 1 },
{ UINT64_C(0x88ba2d41a03c77af), 900, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;gl_FragColor=s;}", 1 },
{ UINT64_C(0x0e0678e414c74fb3), 900, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb*(t.a*c.a),t.a*c.a);gl_FragColor=s;}", 1 },
{ UINT64_C(0x637074c20868dabd), 900, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb,t.a*c.a);gl_FragColor=s;}", 1 },
{ UINT64_C(0x556f5665c07a2e0f), 1064, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;s.rgb*=s.a;if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0x8aabe5e28f88af23), 1064, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0x89f164b96af99cd0), 1064, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb*(t.a*c.a),t.a*c.a);if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0xf622efe69c148d34), 1064, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb,t.a*c.a);if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0xc82ecc9d125ef397), 972, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;s.rgb*=s.a;s*=texture2D(u_maskSampler,v_texCoords).r;gl_FragColor=s;}", 1 },
{ UINT64_C(0xb1b143f735f37064), 1016, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;s*=texture2D(u_maskSampler,v_texCoords).r;gl_FragColor=s;}", 1 },
{ UINT64_C(0x3c41557a5ad688ca), 1016, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb*(t.a*c.a),t.a*c.a);s*=texture2D(u_maskSampler,v_texCoords).r;gl_FragColor=s;}", 1 },
{ UINT64_C(0x28949bfba0bb4ceb), 1036, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb,t.a*c.a);s*=texture2D(u_maskSampler,v_texCoords).r;gl_FragColor=s;}", 1 },
{ UINT64_C(0x2489caa29fb252f4), 1124, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;s.rgb*=s.a;s*=texture2D(u_maskSampler,v_texCoords).r;if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0x233caf056dfa4547), 1124, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=t*c;s*=texture2D(u_maskSampler,v_texCoords).r;if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0x8622e0452d0632e0), 1124, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb*(t.a*c.a),t.a*c.a);s*=texture2D(u_maskSampler,v_texCoords).r;if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0xb401e6bf40f5eaf2), 1124, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform sampler2D u_maskSampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;vec4 s=vec4(c.rgb,t.a*c.a);s*=texture2D(u_maskSampler,v_texCoords).r;if(s.a<=0.0)discard;gl_FragColor=s;}", 1 },
{ UINT64_C(0x63aa8149a8885415), 920, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;void main(){vec4 t=texture2D(u_sampler,v_texCoords);vec4 c=v_color;c*=c;vec4 s=t*c;s.rgb*=s.a;gl_FragColor=s;}", 1 },
{ UINT64_C(0x33ea5406668ed38a), 1700, "precision mediump float;varying vec4 v_color;varying vec2 v_texCoords;uniform sampler2D u_sampler;uniform vec3 u_texCoordOffsets;void main(){vec4 s=vec4(0.0);for(int y=-1;y<=1;y++){for(int x=-1;x<=1;x++){s+=texture2D(u_sampler,v_texCoords+vec2(float(x)*u_texCoordOffsets.x,float(y)*u_texCoordOffsets.y))*u_texCoordOffsets.z;}}gl_FragColor=s;}", 1 },
/* Audi P5089 masked compositor, recovered from captured Tegra programs. */
{ UINT64_C(0xb372986db48cc454), 1412, "attribute vec2 position; uniform vec4 uSource,uTarget; uniform vec2 uResolution,maskOffset; uniform float uAngle,maskAngle; varying mediump vec2 texout,maskout; void main(){ vec2 inv=1.0/uResolution; vec2 rotated=vec2(cos(uAngle)*position.x-sin(uAngle)*position.y,sin(uAngle)*position.x+cos(uAngle)*position.y); vec2 origin=uTarget.xy*inv*2.0-1.0; origin-=vec2(fract(uTarget.z*.5)>0.0?inv.x:0.0,fract(uTarget.w*.5)>0.0?inv.y:0.0); gl_Position=vec4(rotated*uTarget.zw*inv+origin,0.0,1.0); texout=uSource.xy+(position+1.0)*uSource.zw*.5; vec2 m=texout-maskOffset*inv; maskout=vec2(cos(maskAngle)*m.x-sin(maskAngle)*m.y,sin(maskAngle)*m.x+cos(maskAngle)*m.y); }", 0, "audi-a3" },
{ UINT64_C(0xa1d1062962f918ff), 952, "precision mediump float; varying vec2 texout,maskout; uniform sampler2D tex,mask; uniform float opacity; void main(){ gl_FragColor=texture2D(tex,texout)*(texture2D(mask,maskout).r*opacity); }", 1, "audi-a3" },
};

/* The two branches translated the same masked-compositor binaries differently.
 * Keep each profile's established rendering until both can be compared against
 * captures on the same hardware; table order must not silently select one. */
static const struct ShaderTranslation *find_shader(uint64_t hash, size_t size)
{
    const char *firmware = getenv("MHI2_FIRMWARE");
    const struct ShaderTranslation *fallback = NULL;
    for (size_t i = 0; i < sizeof(shaders) / sizeof(shaders[0]); i++) {
        if (shaders[i].hash != hash || shaders[i].size != size) continue;
        if (!shaders[i].firmware) fallback = &shaders[i];
        else if (firmware && !strcmp(firmware, shaders[i].firmware)) return &shaders[i];
    }
    return fallback;
}
