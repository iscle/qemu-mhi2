precision highp float;
varying mediump vec2 v_texCoord;varying mediump float v_fogFactor;
uniform sampler2D s_texture0;
uniform float u_fogEnd;
uniform float u_fogStart;
uniform vec3 u_origin;
uniform float u_roadGeometryWidthHalf;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
uniform vec2 u_texCoordsScaleAndOffset;
uniform float u_widthScale;
void main(){vec4 c=texture2D(s_texture0,v_texCoord);gl_FragColor=vec4(c.rgb,clamp(v_fogFactor,0.0,1.0)*c.a);}
