// Spectrogram shaders (M07). One textured quad per visible tile (or one full-width quad for the
// overview strip), sampling an R8 atlas/overview texture and mapping through a 256x1 RGBA colour
// map LUT — floor/gain/gamma are uniforms applied here, so changing them is instant with no tile
// regeneration (M07 "Colour maps": "All applied in the shader, so they are free and instant").

export const kSpectrogramVertexShader = /* glsl */ `#version 300 es
precision highp float;

in vec2 aPosition; // clip-space xy
in vec2 aUV;        // texture-space uv into whichever texture is bound this draw call

out vec2 vUV;

void main() {
  vUV = aUV;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
`;

export const kSpectrogramFragmentShader = /* glsl */ `#version 300 es
precision highp float;

in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uSource;    // R8 atlas or overview texture
uniform sampler2D uColorMap;  // 256x1 RGBA LUT

// The tile's own quantisation range (M07 "Tiles": dB baked in at generation time) — recovers the
// real dB value from the normalised byte before applying the *display* adjustments below, which
// are independent of it and never require regenerating a tile.
uniform float uFloorDb;
uniform float uCeilDb;

// Display-only (shader) parameters — M07 "Interactive controls: dynamic range (floor dB), gain/
// brightness, gamma", all instant.
uniform float uDisplayFloorDb;
uniform float uGainDb;
uniform float uGamma;

void main() {
  float raw = texture(uSource, vUV).r;
  float db = uFloorDb + raw * (uCeilDb - uFloorDb);

  float range = max(-uDisplayFloorDb, 1e-3);
  float adjusted = clamp((db + uGainDb - uDisplayFloorDb) / range, 0.0, 1.0);
  adjusted = pow(adjusted, 1.0 / max(uGamma, 0.01));

  fragColor = texture(uColorMap, vec2(adjusted, 0.5));
}
`;
