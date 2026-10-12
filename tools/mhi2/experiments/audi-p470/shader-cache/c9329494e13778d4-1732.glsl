precision highp float;
varying mediump vec2 v_texCoord;
uniform sampler2D s_texture0;
uniform vec4 u_colorScale;
uniform vec2 u_texCoordScaleXY;
uniform vec2 u_texOffsetForSmoothing;
void main(){vec2 d=u_texOffsetForSmoothing;vec4 c=texture2D(s_texture0,v_texCoord);vec4 n=(texture2D(s_texture0,v_texCoord+vec2(d.x,d.y))+texture2D(s_texture0,v_texCoord+vec2(d.x,-d.y))+texture2D(s_texture0,v_texCoord+vec2(-d.x,d.y))+texture2D(s_texture0,v_texCoord-d))*0.25;c=mix(c,n,0.2998046875)*u_colorScale;c.rgb=clamp(c.rgb,0.0,1.0);if(c.a<=0.0)discard;gl_FragColor=c;}
