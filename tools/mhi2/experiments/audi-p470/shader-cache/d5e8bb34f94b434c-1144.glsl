precision highp float;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform vec4 u_color;
uniform vec3 u_origin;
uniform float u_roadGeometryWidthHalf;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
uniform vec2 u_texCoordsScaleAndOffset;
uniform float u_widthScale;
void main(){vec4 c=texture2D(s_texture0,v_texCoord);gl_FragColor=clamp(vec4(mix(u_color.rgb,c.rgb,c.a),u_color.a),0.0,1.0);}
