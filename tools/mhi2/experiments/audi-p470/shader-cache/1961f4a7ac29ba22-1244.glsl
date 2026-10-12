precision highp float;
varying mediump float v_brightness,v_elevation;
uniform sampler2D s_texture0;
uniform vec2 u_ambient;
uniform float u_colorMapTexCoord;
uniform vec3 u_lightVec;
uniform float u_maxHeight;
uniform float u_normalBendFactor;
uniform vec4 u_originWithInverseMagnitude;
uniform vec2 u_scaleH;
uniform float u_scaleXYZ;
void main(){gl_FragColor=vec4(texture2D(s_texture0,vec2(v_elevation,u_colorMapTexCoord)).rgb*v_brightness,1.0);}
