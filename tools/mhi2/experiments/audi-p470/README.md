# Experimental Audi P470 map shader cache

These text GLSL translations are keyed by the native shader FNV hash and
binary length consumed by the existing graphics bridge. They cover known
captured programs, not a general Tegra decoder. The billboard smoothing
translation approximates coverage/depth behavior with four-neighbour filtering
and weight 0.2998046875; unknown programs remain unsupported.

To test rendering without distributing firmware shader binaries:

```sh
bash tools/mhi2/glforward/build_host.sh /tmp/mhi2-a4-glhost
python3 tools/mhi2/experiments/audi-p470/check-native-shaders.py \
  --host /tmp/mhi2-a4-glhost --captures /path/to/your/captured/shaders
```

The check uses a temporary output directory, compiles 42 programs, links 17
pairs and compares road/billboard source-over pixels and transparent texels.
To enable for a prepared local map session, copy the matching reviewed HMI
shader cache into a new combined directory, overlay these .glsl/.blend files,
and select it with MHI2_SHADER_CACHE. A media manifest may override that
variable: use its shader_cache path for the separate combined cache.

The translation tools accept a captures directory containing
unsupported-shaders.json (hash -> [binary path, length]) and, for vertices,
decoded hash.asm files. Binary paths must point to locally held inputs:

```sh
python3 tools/mhi2/experiments/audi-p470/translate-native-vertices.py /path/to/captures --output /tmp/map-translations
python3 tools/mhi2/experiments/audi-p470/translate-native-fragments.py /path/to/captures --output /tmp/map-translations
```

These tools do not include the source binaries or a disassembler. The cache
contains the translations already tested locally. Factory map styles, fonts,
icons and databases are not included or changed. Complete labels/POIs/3D
coverage, perspective artifacts and cold-start reliability remain unaccepted.
