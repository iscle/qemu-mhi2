precision highp float;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform vec4 u_colorScale;
uniform vec2 u_texCoordScaleXY;
void main(){vec4 c=texture2D(s_texture0,v_texCoord);c.rgb=clamp(c.rgb*u_colorScale.rgb,0.0,1.0);c.a*=u_colorScale.w;if(c.a<=0.0)discard;gl_FragColor=c;}
