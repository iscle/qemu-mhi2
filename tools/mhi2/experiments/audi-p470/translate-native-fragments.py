#!/usr/bin/env python3
"""Exact simple K3346 fragment operations recovered from captured instruction streams.
Destination reads are replaced with the bridge's premultiplied source-over blend.
"""
from pathlib import Path
import json
from importlib.machinery import SourceFileLoader
R=Path(__file__).resolve().parent
m=SourceFileLoader('native_vertices',str(R/'translate-native-vertices.py')).load_module()

# These programs have no lighting, depth side attachments, or unknown ALU operations.
PROGRAMS={
 # Portable four-neighbour filtering for the smooth billboard. Native packed
 # coverage/depth attachments are not reproduced by this GLES bridge.
 'c9329494e13778d4':('varying mediump vec2 v_texCoord;','vec2 d=u_texOffsetForSmoothing;vec4 c=texture2D(s_texture0,v_texCoord);vec4 n=(texture2D(s_texture0,v_texCoord+vec2(d.x,d.y))+texture2D(s_texture0,v_texCoord+vec2(d.x,-d.y))+texture2D(s_texture0,v_texCoord+vec2(-d.x,d.y))+texture2D(s_texture0,v_texCoord-d))*0.25;c=mix(c,n,0.2998046875)*u_colorScale;c.rgb=clamp(c.rgb,0.0,1.0);if(c.a<=0.0)discard;gl_FragColor=c;',2),
 'c05313b981de9f9e':('varying mediump vec2 v_texCoord;','vec4 c=texture2D(s_texture0,v_texCoord);c.rgb=clamp(c.rgb*u_colorScale.rgb,0.0,1.0);c.a*=u_colorScale.w;if(c.a<=0.0)discard;gl_FragColor=c;',2),
 '14e87a9476a80d3c':('varying mediump vec2 v_texCoord;','vec4 c=texture2D(s_texture0,v_texCoord);c.rgb=clamp(c.rgb*u_colorScale.rgb,0.0,1.0);c.a=min(c.a,u_colorScale.w);if(c.a<=0.0)discard;gl_FragColor=c;',2),
 'ed3a1d9bc7560c05':('varying mediump vec2 v_texCoord;','vec4 c=texture2D(s_texture0,v_texCoord);if(c.a<=0.0)discard;gl_FragColor=c;',0),
 'bcac1d79ebe5e35b':('varying mediump float v_texCoord;','gl_FragColor=vec4(mix(u_skyColorBottom,u_skyColorTop,clamp(v_texCoord,0.0,1.0)),0.0);',0),
 '352ad410c772a062':('varying mediump float v_texCoord;','gl_FragColor=texture2D(s_texture0,vec2(v_texCoord,0.5));',1),
 '04fb3f2c9d9b8aaf':('','gl_FragColor=texture2D(s_texture0,gl_PointCoord);',1),
 '08e42ce15ee05876':('varying mediump vec2 v_texCoord;varying mediump float v_fogFactor;','float a=texture2D(s_texture0,v_texCoord).a;vec3 c=clamp(mix(u_fogColor,u_color.rgb,clamp(v_fogFactor,0.0,1.0)),0.0,1.0);gl_FragColor=vec4(c*a,a);',2),
 'f1f72e0c4d5c49fb':('varying mediump vec2 v_texCoord;varying mediump float v_fogFactor;','vec4 c=texture2D(s_texture0,v_texCoord);gl_FragColor=vec4(c.rgb,clamp(v_fogFactor,0.0,1.0)*c.a);',2),
 'd5e8bb34f94b434c':('varying mediump vec2 v_texCoord;','vec4 c=texture2D(s_texture0,v_texCoord);gl_FragColor=clamp(vec4(mix(u_color.rgb,c.rgb,c.a),u_color.a),0.0,1.0);',2),
 '1961f4a7ac29ba22':('varying mediump float v_brightness,v_elevation;','gl_FragColor=vec4(texture2D(s_texture0,vec2(v_elevation,u_colorMapTexCoord)).rgb*v_brightness,1.0);',0),
 'ce6cef4f81e6bd86':('varying mediump float v_brightness,v_elevation,v_fogFactor;','vec3 c=texture2D(s_texture0,vec2(v_elevation,u_colorMapTexCoord)).rgb*v_brightness;gl_FragColor=vec4(mix(u_fogColor,c,clamp(v_fogFactor,0.0,1.0)),1.0);',0),
 '36d5b5d1755989e9':('','gl_FragColor=clamp(u_color,0.0,1.0);',2),
 '730a52061eb97de5':('varying mediump vec4 v_color;','gl_FragColor=clamp(vec4(v_color.rgb,v_color.a*u_fadeout),0.0,1.0);',2),
 'fd12b5eaefc1ced2':('varying mediump vec2 v_texCoord;','gl_FragColor=texture2D(s_texture0,v_texCoord);',1),
 '1741c600471d8e8f':('varying mediump vec2 v_texCoord;','gl_FragColor=texture2D(s_texture0,v_texCoord);',1),
 '8e737d5293e50f53':('varying mediump float v_fogFactor;','gl_FragColor=vec4(clamp(u_color.rgb,0.0,1.0),clamp(v_fogFactor,0.0,1.0)*u_color.a);',2),
}
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('captures', type=Path, help='Directory containing unsupported-shaders.json')
 parser.add_argument('--output', type=Path, default=R/'shader-cache')
 args=parser.parse_args(); R=args.captures
 D=json.loads((R/'unsupported-shaders.json').read_text())
 out=args.output;out.mkdir(exist_ok=True, parents=True)
 for h,(vary,body,blend) in PROGRAMS.items():
  path,size=D[h];ss,attrs,vs,uniforms=m.metadata(Path(path).read_bytes())
  src='precision highp float;\n'+vary+'\n'
  for name,(kind,count),reg,packed in uniforms:src+=f'uniform {kind} {name};\n'
  src+='void main(){'+body+'}\n'
  (out/f'{h}-{size}.glsl').write_text(src)
  (out/f'{h}-{size}.blend').write_text(str(blend)+'\n')
  print(h)
