// WebGL2 drawer for batches produced by runtime/gfx.c.
const VS = `#version 300 es
layout(location=0) in vec4 aPos; layout(location=1) in vec4 aCol; layout(location=2) in vec4 aUV;
out vec4 vCol; out vec2 vUV0; out vec2 vUV1;
void main(){ gl_Position = aPos; vCol = aCol; vUV0 = aUV.xy; vUV1 = aUV.zw; }`;

const FS = `#version 300 es
precision highp float; precision highp int;
in vec4 vCol; in vec2 vUV0; in vec2 vUV1; out vec4 outColor;
uniform sampler2D uT0, uT1; uniform int uHas0, uHas1;
uniform ivec4 uC1, uA1, uC2, uA2;  // combiner selectors a,b,c,d per cycle
uniform int uTwoCycle, uMode;      // uMode: 0 3D/rect combiner, 1 fill (vertex color)
uniform vec4 uPrim, uEnv, uFog; uniform float uPrimLod;
uniform int uAlphaTest; uniform float uAlphaRef; uniform int uFogOn;
uniform vec4 uClampS, uClampT;     // per texture: clamp flags (unused when GL wraps)
vec3 cA(int s, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){ if(s==0)return cmb.rgb; if(s==1)return t0.rgb; if(s==2)return t1.rgb; if(s==3)return uPrim.rgb; if(s==4)return sh.rgb; if(s==5)return uEnv.rgb; if(s==6)return vec3(1.0); return vec3(0.0);}
vec3 cB(int s, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){ if(s==0)return cmb.rgb; if(s==1)return t0.rgb; if(s==2)return t1.rgb; if(s==3)return uPrim.rgb; if(s==4)return sh.rgb; if(s==5)return uEnv.rgb; return vec3(0.0);}
vec3 cC(int s, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){ if(s==0)return cmb.rgb; if(s==1)return t0.rgb; if(s==2)return t1.rgb; if(s==3)return uPrim.rgb; if(s==4)return sh.rgb; if(s==5)return uEnv.rgb;
  if(s==7)return vec3(cmb.a); if(s==8)return vec3(t0.a); if(s==9)return vec3(t1.a); if(s==10)return vec3(uPrim.a); if(s==11)return vec3(sh.a); if(s==12)return vec3(uEnv.a); if(s==14)return vec3(uPrimLod); return vec3(0.0);}
vec3 cD(int s, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){ if(s==0)return cmb.rgb; if(s==1)return t0.rgb; if(s==2)return t1.rgb; if(s==3)return uPrim.rgb; if(s==4)return sh.rgb; if(s==5)return uEnv.rgb; if(s==6)return vec3(1.0); return vec3(0.0);}
float aABD(int s, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){ if(s==0)return cmb.a; if(s==1)return t0.a; if(s==2)return t1.a; if(s==3)return uPrim.a; if(s==4)return sh.a; if(s==5)return uEnv.a; if(s==6)return 1.0; return 0.0;}
float aC(int s, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){ if(s==1)return t0.a; if(s==2)return t1.a; if(s==3)return uPrim.a; if(s==4)return sh.a; if(s==5)return uEnv.a; if(s==6)return uPrimLod; return 0.0;}
vec4 cycle(ivec4 c, ivec4 a, vec4 cmb, vec4 t0, vec4 t1, vec4 sh){
  vec3 rgb = (cA(c.x,cmb,t0,t1,sh) - cB(c.y,cmb,t0,t1,sh)) * cC(c.z,cmb,t0,t1,sh) + cD(c.w,cmb,t0,t1,sh);
  float al = (aABD(a.x,cmb,t0,t1,sh) - aABD(a.y,cmb,t0,t1,sh)) * aC(a.z,cmb,t0,t1,sh) + aABD(a.w,cmb,t0,t1,sh);
  return vec4(rgb, al);
}
void main(){
  vec4 sh = vCol;
  if (uMode == 1) { outColor = vec4(sh.rgb, 1.0); return; }
  vec4 t0 = uHas0 == 1 ? texture(uT0, vUV0) : vec4(1.0);
  vec4 t1 = uHas1 == 1 ? texture(uT1, vUV1) : t0;
  vec4 shade = uFogOn == 1 ? vec4(sh.rgb, 1.0) : sh;
  vec4 c = cycle(uC1, uA1, vec4(0.0), t0, t1, shade);
  if (uTwoCycle == 1) c = cycle(uC2, uA2, c, t0, t1, shade);
  c = clamp(c, 0.0, 1.0);
  if (uAlphaTest == 1 && c.a < uAlphaRef) discard;
  if (uFogOn == 1) c.rgb = mix(c.rgb, uFog.rgb, sh.a);
  outColor = c;
}`;

export class Renderer {
  constructor(canvas) {
    const gl = canvas.getContext('webgl2', { antialias: true, depth: true, alpha: false, preserveDrawingBuffer: false });
    if (!gl) throw new Error('WebGL2 is not available in this browser.');
    this.gl = gl; this.canvas = canvas;
    const sh = (t, s) => { const o = gl.createShader(t); gl.shaderSource(o, s); gl.compileShader(o); if (!gl.getShaderParameter(o, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(o)); return o; };
    const p = gl.createProgram();
    gl.attachShader(p, sh(gl.VERTEX_SHADER, VS)); gl.attachShader(p, sh(gl.FRAGMENT_SHADER, FS)); gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    this.prog = p; gl.useProgram(p);
    this.u = {};
    for (const n of ['uT0', 'uT1', 'uHas0', 'uHas1', 'uC1', 'uA1', 'uC2', 'uA2', 'uTwoCycle', 'uMode', 'uPrim', 'uEnv', 'uFog', 'uPrimLod', 'uAlphaTest', 'uAlphaRef', 'uFogOn'])
      this.u[n] = gl.getUniformLocation(p, n);
    gl.uniform1i(this.u.uT0, 0); gl.uniform1i(this.u.uT1, 1);
    this.vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 48, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 48, 16);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 48, 32);
    this.tex = new Map(); this.gen = -1;
  }

  // game area inside the canvas, keeping 4:3
  layout() {
    const c = this.canvas, dpr = window.devicePixelRatio || 1;
    const W = Math.round(c.clientWidth * dpr), H = Math.round(c.clientHeight * dpr);
    if (c.width !== W || c.height !== H) { c.width = W; c.height = H; }
    let gw = W, gh = Math.round(W * 3 / 4);
    if (gh > H) { gh = H; gw = Math.round(H * 4 / 3); }
    this.area = { x: Math.floor((W - gw) / 2), y: Math.floor((H - gh) / 2), w: gw, h: gh, sx: gw / 320, sy: gh / 240 };
  }

  texture(id, gen, info, pool, flags) {
    const gl = this.gl;
    if (gen !== this.gen) { for (const t of this.tex.values()) gl.deleteTexture(t); this.tex.clear(); this.gen = gen; }
    let t = this.tex.get(id);
    if (!t) {
      const off = info[id * 3], w = info[id * 3 + 1], h = info[id * 3 + 2];
      t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);
      const px = new Uint8Array(w * h * 4); px.set(new Uint8Array(pool.buffer, pool.byteOffset + off * 4, w * h * 4));
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
      this.tex.set(id, t);
    } else gl.bindTexture(gl.TEXTURE_2D, t);
    const wrap = m => m === 2 ? gl.CLAMP_TO_EDGE : m === 1 ? gl.MIRRORED_REPEAT : gl.REPEAT;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap(flags & 3));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap((flags >> 2) & 3));
    const f = (flags & 16) ? gl.LINEAR : gl.NEAREST;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, f); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, f);
  }

  draw(verts, nv, batches, nb, texGen, texInfo, texPool) {
    const gl = this.gl, u = this.u;
    this.layout();
    const A = this.area;
    gl.disable(gl.SCISSOR_TEST);
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.clearColor(0, 0, 0, 1); gl.clearDepth(1); gl.depthMask(true);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, verts.subarray(0, nv * 12), gl.STREAM_DRAW);
    const rgba = v => [(v >>> 24) / 255, ((v >>> 16) & 255) / 255, ((v >>> 8) & 255) / 255, (v & 255) / 255];
    for (let i = 0; i < nb; i++) {
      const b = batches.subarray(i * 32, i * 32 + 32);
      const kind = b[22];
      if (kind >= 16) {  // special: depth clear
        gl.scissor(A.x, A.y, A.w, A.h); gl.depthMask(true); gl.clear(gl.DEPTH_BUFFER_BIT); continue;
      }
      if (b[1] === 0) continue;
      const oh = b[8] >>> 0, ol = b[9] >>> 0, geom = b[10] >>> 0;
      // viewport & scissor (N64 coords, y down)
      gl.viewport(A.x + b[14] * A.sx, A.y + (240 - b[15] - b[17]) * A.sy, b[16] * A.sx, b[17] * A.sy);
      const sx0 = Math.max(0, b[18]), sy0 = Math.max(0, b[19]), sx1 = Math.min(320, b[20]), sy1 = Math.min(240, b[21]);
      gl.scissor(Math.round(A.x + sx0 * A.sx), Math.round(A.y + (240 - sy1) * A.sy), Math.max(0, Math.round((sx1 - sx0) * A.sx)), Math.max(0, Math.round((sy1 - sy0) * A.sy)));
      // combiner
      const h = b[6] >>> 0, l = b[7] >>> 0;
      gl.uniform4i(u.uC1, (h >> 20) & 15, (l >>> 28) & 15, (h >> 15) & 31, (l >> 15) & 7);
      gl.uniform4i(u.uA1, (h >> 12) & 7, (l >> 12) & 7, (h >> 9) & 7, (l >> 9) & 7);
      gl.uniform4i(u.uC2, (h >> 5) & 15, (l >> 24) & 15, h & 31, (l >> 6) & 7);
      gl.uniform4i(u.uA2, (l >> 21) & 7, (l >> 3) & 7, (l >> 18) & 7, l & 7);
      const cyc = (oh >> 20) & 3;
      gl.uniform1i(u.uTwoCycle, cyc === 1 ? 1 : 0);
      gl.uniform1i(u.uMode, kind === 2 ? 1 : 0);
      gl.uniform4fv(u.uPrim, rgba(b[11] >>> 0)); gl.uniform4fv(u.uEnv, rgba(b[12] >>> 0)); gl.uniform4fv(u.uFog, rgba(b[13] >>> 0));
      gl.uniform1f(u.uPrimLod, (b[24] & 255) / 255);
      // textures (copy mode uses texel0 directly)
      if (cyc === 2) { gl.uniform4i(u.uC1, 1, 8, 6, 7); gl.uniform4i(u.uA1, 7, 7, 7, 1); gl.uniform1i(u.uTwoCycle, 0); }
      gl.activeTexture(gl.TEXTURE0);
      if (b[2] >= 0 && kind !== 2) { this.texture(b[2], texGen, texInfo, texPool, b[4]); gl.uniform1i(u.uHas0, 1); } else gl.uniform1i(u.uHas0, 0);
      gl.activeTexture(gl.TEXTURE1);
      if (b[3] >= 0 && kind !== 2) { this.texture(b[3], texGen, texInfo, texPool, b[5]); gl.uniform1i(u.uHas1, 1); } else gl.uniform1i(u.uHas1, 0);
      // alpha test
      const ac = ol & 3;
      if (cyc === 2) { gl.uniform1i(u.uAlphaTest, (ol & 1) ? 1 : 0); gl.uniform1f(u.uAlphaRef, 0.5); }
      else if (ac === 1) { gl.uniform1i(u.uAlphaTest, 1); gl.uniform1f(u.uAlphaRef, Math.max(1 / 255, (b[23] & 255) / 255)); }
      else if (ol & 0x1000) { gl.uniform1i(u.uAlphaTest, 1); gl.uniform1f(u.uAlphaRef, 0.5); }
      else gl.uniform1i(u.uAlphaTest, 0);
      // blending: blender "B" input = 1-A means translucent
      const bsel = cyc === 1 ? (ol >>> 16) & 3 : (ol >>> 18) & 3;
      const forceBl = (ol & 0x4000) !== 0;
      if (kind !== 2 && cyc < 2 && bsel === 0 && forceBl) { gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA); }
      else gl.disable(gl.BLEND);
      // fog: blender mixes in the fog color using shade alpha
      const fogOn = (geom & 0x10000) && ((ol >>> 30) & 3) === 3 && cyc === 1;
      gl.uniform1i(u.uFogOn, fogOn ? 1 : 0);
      // depth
      if (kind === 0 && (geom & 1) && (ol & 0x10)) { gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); } else gl.disable(gl.DEPTH_TEST);
      gl.depthMask(kind === 0 && (geom & 1) && (ol & 0x20) ? true : false);
      if (((ol >> 10) & 3) === 3) { gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(-1, -2); } else gl.disable(gl.POLYGON_OFFSET_FILL);
      gl.drawArrays(gl.TRIANGLES, b[0], b[1]);
    }
  }
}
