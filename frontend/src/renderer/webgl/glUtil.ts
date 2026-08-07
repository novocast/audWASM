// WebGL2 context setup + shader program management + context-loss handling (M17 tasks list,
// "WebGL context loss (common on mobile/laptop GPU switches)" risk).

export function detectWebgl2Support(): boolean {
  try {
    const canvas = document.createElement('canvas');
    const gl = canvas.getContext('webgl2');
    return gl !== null;
  } catch {
    return false;
  }
}

export function compileShader(gl: WebGL2RenderingContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type);
  if (!shader) throw new Error('createShader failed');
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new Error(`shader compile failed: ${log ?? '(no log)'}`);
  }
  return shader;
}

export function createProgram(gl: WebGL2RenderingContext, vertexSource: string, fragmentSource: string): WebGLProgram {
  const vs = compileShader(gl, gl.VERTEX_SHADER, vertexSource);
  const fs = compileShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
  const program = gl.createProgram();
  if (!program) throw new Error('createProgram failed');
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program);
    gl.deleteProgram(program);
    throw new Error(`program link failed: ${log ?? '(no log)'}`);
  }
  return program;
}

export interface ContextLossHandlers {
  onLost: () => void;
  onRestored: () => void;
}

/** Registers the `webglcontextlost`/`webglcontextrestored` listeners the M17 risk table calls
 *  for. `onLost` should drop all GL object references (they're already invalid); `onRestored`
 *  should rebuild programs/buffers/textures from scratch. Returns an unsubscribe function. */
export function watchContextLoss(canvas: HTMLCanvasElement, handlers: ContextLossHandlers): () => void {
  const onLost = (e: Event): void => {
    e.preventDefault(); // required to allow restoration rather than a permanent loss
    handlers.onLost();
  };
  const onRestored = (): void => handlers.onRestored();
  canvas.addEventListener('webglcontextlost', onLost);
  canvas.addEventListener('webglcontextrestored', onRestored);
  return () => {
    canvas.removeEventListener('webglcontextlost', onLost);
    canvas.removeEventListener('webglcontextrestored', onRestored);
  };
}
