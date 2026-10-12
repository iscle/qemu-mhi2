precision highp float;
varying mediump float v_brightness,v_elevation,v_fogFactor;
uniform sampler2D s_texture0;
uniform vec2 u_ambient;
uniform float u_colorMapTexCoord;
uniform vec3 u_fogColor;
uniform float u_fogEnd;
uniform float u_fogStart;
uniform vec3 u_lightVec;
uniform float u_maxHeight;
uniform float u_normalBendFactor;
uniform vec4 u_originWithInverseMagnitude;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
void main(){vec3 c=texture2D(s_texture0,vec2(v_elevation,u_colorMapTexCoord)).rgb*v_brightness;gl_FragColor=vec4(mix(u_fogColor,c,clamp(v_fogFactor,0.0,1.0)),1.0);}
