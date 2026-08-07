// Waveform shaders. Per-vertex clip-space position + a per-vertex "isRms" flag so the min/max
// envelope and the RMS body share one draw call (one buffer upload, one drawArrays), rather than
// two — see waveformProgram.ts for how the vertex buffer is built.

export const kWaveformVertexShader = /* glsl */ `#version 300 es
precision highp float;

in vec2 aPosition;   // clip-space xy
in float aIsRms;      // 0 = min/max envelope colour, 1 = rms colour

uniform vec4 uEnvelopeColor;
uniform vec4 uRmsColor;

out vec4 vColor;

void main() {
  vColor = mix(uEnvelopeColor, uRmsColor, aIsRms);
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
`;

export const kWaveformFragmentShader = /* glsl */ `#version 300 es
precision highp float;
in vec4 vColor;
out vec4 fragColor;
void main() {
  fragColor = vColor;
}
`;
