// DOOM for the browser: page glue around the WebAssembly engine.
// Everything game-related happens in C (src/); this file only moves
// pixels, sound, input and files between the browser and the engine.
'use strict';

const SCREENWIDTH = 320;
const SCREENHEIGHT = 200;

const $ = (id) => document.getElementById(id);
const canvas = $('canvas');
const startButton = $('start');
const startInfo = $('start-info');

const params = new URLSearchParams(location.search);
let running = false;
let audio = null;

// ---------------------------------------------------------------------------
// Video: the engine hands over 320x200 RGBA. It is scaled by an exact 5x6
// (so every DOOM pixel becomes the same block, with the 1.2 aspect of a
// 4:3 monitor) and the browser then smooths that large canvas to the window.

const ctx = canvas.getContext('2d', { alpha: false });
ctx.imageSmoothingEnabled = false;
const frame = document.createElement('canvas');
frame.width = SCREENWIDTH;
frame.height = SCREENHEIGHT;
const frameCtx = frame.getContext('2d');
const frameImage = frameCtx.createImageData(SCREENWIDTH, SCREENHEIGHT);

function doomBlit(pixels) {
  frameImage.data.set(pixels);
  frameCtx.putImageData(frameImage, 0, 0);
  ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
}

// ---------------------------------------------------------------------------
// Keyboard: physical key (event.code) to DOOM key codes (doomdef.h).
// Using the physical key keeps the original US layout, like DOS did.

const KEY_RIGHTARROW = 0xae, KEY_LEFTARROW = 0xac, KEY_UPARROW = 0xad,
  KEY_DOWNARROW = 0xaf, KEY_ESCAPE = 27, KEY_ENTER = 13, KEY_TAB = 9,
  KEY_BACKSPACE = 127, KEY_PAUSE = 0xff, KEY_EQUALS = 0x3d, KEY_MINUS = 0x2d,
  KEY_RSHIFT = 0x80 + 0x36, KEY_RCTRL = 0x80 + 0x1d, KEY_RALT = 0x80 + 0x38;

const KEYMAP = {
  ArrowRight: KEY_RIGHTARROW, ArrowLeft: KEY_LEFTARROW,
  ArrowUp: KEY_UPARROW, ArrowDown: KEY_DOWNARROW,
  Escape: KEY_ESCAPE, Enter: KEY_ENTER, NumpadEnter: KEY_ENTER, Tab: KEY_TAB,
  Backspace: KEY_BACKSPACE, Delete: KEY_BACKSPACE, Pause: KEY_PAUSE,
  Equal: KEY_EQUALS, NumpadAdd: KEY_EQUALS,
  Minus: KEY_MINUS, NumpadSubtract: KEY_MINUS,
  ShiftLeft: KEY_RSHIFT, ShiftRight: KEY_RSHIFT,
  ControlLeft: KEY_RCTRL, ControlRight: KEY_RCTRL,
  AltLeft: KEY_RALT, AltRight: KEY_RALT,
  Space: 32, Comma: 44, Period: 46, Slash: 47, Semicolon: 59, Quote: 39,
  BracketLeft: 91, BracketRight: 93, Backslash: 92, Backquote: 96,
  F11: 0x80 + 0x57, F12: 0x80 + 0x58,
};
for (let i = 1; i <= 10; i++) KEYMAP['F' + i] = 0x80 + 0x3a + i;
for (let i = 0; i <= 9; i++) KEYMAP['Digit' + i] = 48 + i;
for (let c = 65; c <= 90; c++) KEYMAP['Key' + String.fromCharCode(c)] = c + 32;

function onKey(e) {
  if (!running) return;
  const key = KEYMAP[e.code];
  if (key === undefined) return;
  e.preventDefault();
  Module._I_WebKey(e.type === 'keydown' ? 1 : 0, key);
}
window.addEventListener('keydown', onKey);
window.addEventListener('keyup', onKey);

// ---------------------------------------------------------------------------
// Mouse, through pointer lock. Buttons use the same bit order as the DOS
// mouse driver: 1 = left (fire), 2 = right (strafe), 4 = middle (forward).

const novert = $('opt-novert');
try { novert.checked = localStorage.getItem('doom-novert') !== '0'; } catch (e) {}
novert.addEventListener('change', () => {
  try { localStorage.setItem('doom-novert', novert.checked ? '1' : '0'); } catch (e) {}
  canvas.focus();
});

const locked = () => document.pointerLockElement === canvas;

canvas.addEventListener('click', () => {
  if (running && !locked()) canvas.requestPointerLock();
});
document.addEventListener('mousemove', (e) => {
  if (running && locked())
    Module._I_WebMouseMove(e.movementX, novert.checked ? 0 : e.movementY);
});
function onMouseButton(e) {
  if (running && locked()) {
    e.preventDefault();
    Module._I_WebMouseButtons(e.buttons & 7);
  }
}
document.addEventListener('mousedown', onMouseButton);
document.addEventListener('mouseup', onMouseButton);
canvas.addEventListener('contextmenu', (e) => e.preventDefault());

// Esc releases the pointer lock before the page can see the key, so
// treat losing the lock as pressing Esc: the menu opens, as expected.
document.addEventListener('pointerlockchange', () => {
  if (running && !locked() && !Module._I_WebMenuActive()) {
    Module._I_WebKey(1, KEY_ESCAPE);
    Module._I_WebKey(0, KEY_ESCAPE);
  }
});

$('btn-fullscreen').addEventListener('click', async () => {
  const screen = $('screen');
  try {
    await screen.requestFullscreen();
    // Chrome: keep Esc and friends inside the game while fullscreen.
    if (navigator.keyboard && navigator.keyboard.lock)
      navigator.keyboard.lock().catch(() => {});
  } catch (e) {}
  canvas.focus();
});

// ---------------------------------------------------------------------------
// Sound: the engine mixes effects and OPL music itself (i_sound.c,
// i_music.c); Web Audio only pulls finished stereo samples from it.

function startAudio() {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  const actx = new AC();
  const FRAMES = 1024;
  const node = actx.createScriptProcessor(FRAMES, 0, 2);
  const buffer = Module._malloc(FRAMES * 2 * 4);
  node.onaudioprocess = (e) => {
    const left = e.outputBuffer.getChannelData(0);
    const right = e.outputBuffer.getChannelData(1);
    const n = left.length;
    Module._I_WebMixAudio(buffer, n);
    const heap = Module.HEAPF32;
    let p = buffer >> 2;
    for (let i = 0; i < n; i++) {
      left[i] = heap[p++];
      right[i] = heap[p++];
    }
  };
  node.connect(actx.destination);
  actx.resume();
  audio = { actx, node };
}

// ---------------------------------------------------------------------------
// Files: the IWAD goes to /wad, savegames and .doomrc to /save, which is
// kept in IndexedDB so they survive reloads.

function openStore() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open('doom-html', 1);
    req.onupgradeneeded = () => req.result.createObjectStore('wads');
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
async function storeOp(mode, fn) {
  const db = await openStore();
  return new Promise((resolve, reject) => {
    const tx = db.transaction('wads', mode);
    const req = fn(tx.objectStore('wads'));
    tx.oncomplete = () => resolve(req && req.result);
    tx.onerror = () => reject(tx.error);
  });
}
const getUserWad = () => storeOp('readonly', (s) => s.get('iwad')).catch(() => null);
const setUserWad = (wad) => storeOp('readwrite', (s) => s.put(wad, 'iwad'));
const clearUserWad = () => storeOp('readwrite', (s) => s.delete('iwad'));

// linuxdoom finds its IWAD by file name, so give the file the name
// that matches its contents.
function identifyWad(bytes, filename) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = String.fromCharCode(...bytes.subarray(0, 4));
  if (magic !== 'IWAD') return { error: magic === 'PWAD'
    ? 'Eso es un PWAD (un mod). De momento solo se admiten IWADs: DOOM.WAD, DOOM2.WAD, etc.'
    : 'No parece un archivo WAD.' };
  const count = view.getInt32(4, true);
  const dir = view.getInt32(8, true);
  const names = new Set();
  for (let i = 0; i < count; i++) {
    const at = dir + i * 16 + 8;
    if (at + 8 > bytes.length) break;
    let name = '';
    for (let j = 0; j < 8 && bytes[at + j]; j++) name += String.fromCharCode(bytes[at + j]);
    names.add(name.toUpperCase());
  }
  const lower = filename.toLowerCase();
  if (names.has('MAP01')) {
    if (lower.includes('tnt')) return { file: 'tnt.wad', label: 'Final Doom: TNT Evilution' };
    if (lower.includes('plutonia')) return { file: 'plutonia.wad', label: 'Final Doom: The Plutonia Experiment' };
    return { file: 'doom2.wad', label: 'DOOM II' };
  }
  if (names.has('E4M1')) return { file: 'doomu.wad', label: 'The Ultimate DOOM' };
  if (names.has('E2M1')) return { file: 'doom.wad', label: 'DOOM registrado' };
  return { file: 'doom1.wad', label: 'DOOM shareware' };
}

$('wad-file').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  const bytes = new Uint8Array(await file.arrayBuffer());
  const id = identifyWad(bytes, file.name);
  if (id.error) { alert(id.error); return; }
  await setUserWad({ name: file.name, data: bytes });
  location.reload();
});
$('btn-shareware').addEventListener('click', async () => {
  await clearUserWad();
  location.reload();
});

let syncing = false;
function syncFS() {
  if (syncing || !Module.FS) return;
  syncing = true;
  Module.FS.syncfs(false, () => { syncing = false; });
}
setInterval(() => { if (running) syncFS(); }, 5000);
document.addEventListener('visibilitychange', () => {
  if (running && document.visibilityState === 'hidden') {
    Module._I_WebSaveDefaults();
    syncFS();
  }
});

// ---------------------------------------------------------------------------
// ENDOOM: the text screen DOS showed after quitting (80x25 text mode).

const CP437 = 'ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■ ';
const CGA = ['#000000', '#0000aa', '#00aa00', '#00aaaa', '#aa0000', '#aa00aa', '#aa5500', '#aaaaaa',
  '#555555', '#5555ff', '#55ff55', '#55ffff', '#ff5555', '#ff55ff', '#ffff55', '#ffffff'];

function showEndoom(data) {
  const box = $('endoom');
  const c = $('endoom-canvas');
  const g = c.getContext('2d');
  box.classList.remove('hidden');
  if (!data) { box.querySelector('canvas').classList.add('hidden'); return; }
  const draw = (blinkOn) => {
    g.textBaseline = 'top';
    g.font = '16px "Courier New", monospace';
    for (let i = 0; i < 2000; i++) {
      const ch = data[i * 2], attr = data[i * 2 + 1];
      const x = (i % 80) * 8, y = Math.floor(i / 80) * 16;
      const fg = CGA[attr & 15], bg = CGA[(attr >> 4) & 7];
      const visible = !(attr & 0x80) || blinkOn;
      g.fillStyle = bg;
      g.fillRect(x, y, 8, 16);
      if (!visible || ch === 0 || ch === 32) continue;
      g.fillStyle = fg;
      // Block characters drawn as shapes so they tile without gaps.
      switch (ch) {
        case 219: g.fillRect(x, y, 8, 16); break;
        case 220: g.fillRect(x, y + 8, 8, 8); break;
        case 223: g.fillRect(x, y, 8, 8); break;
        case 221: g.fillRect(x, y, 4, 16); break;
        case 222: g.fillRect(x + 4, y, 4, 16); break;
        case 176: case 177: case 178:
          g.globalAlpha = (ch - 175) * 0.25;
          g.fillRect(x, y, 8, 16);
          g.globalAlpha = 1;
          break;
        default: {
          const s = ch < 128 ? String.fromCharCode(ch) : CP437[ch - 128];
          g.fillText(s, x, y + 1, 8);
        }
      }
    }
  };
  let blink = true;
  draw(blink);
  setInterval(() => draw(blink = !blink), 280);
}
$('restart').addEventListener('click', () => location.reload());

// ---------------------------------------------------------------------------
// Emscripten module.

function stopGame() {
  running = false;
  if (document.pointerLockElement) document.exitPointerLock();
  if (document.fullscreenElement) document.exitFullscreen();
  if (audio) audio.actx.close();
}

var Module = {
  canvas,
  print: (s) => console.log(s),
  printErr: (s) => console.log(s),

  doomBlit,
  audioRate: () => (audio ? audio.actx.sampleRate : 44100),
  syncFS,
  onDoomQuit(endoom) {
    stopGame();
    showEndoom(endoom);
  },
  onDoomError(msg) {
    stopGame();
    $('error-text').textContent = msg;
    $('error').classList.remove('hidden');
  },

  preRun: [() => {
    const FS = Module.FS;
    FS.mkdir('/wad');
    FS.mkdir('/save');
    FS.mount(Module.IDBFS, {}, '/save');
    Module.ENV.HOME = '/save';
    Module.ENV.DOOMWADDIR = '/wad';

    Module.addRunDependency('syncfs');
    FS.syncfs(true, () => Module.removeRunDependency('syncfs'));

    Module.addRunDependency('wad');
    loadWad().then(() => Module.removeRunDependency('wad'), (err) => {
      startInfo.textContent = 'No se pudo cargar el WAD: ' + err.message;
    });
  }],

  onRuntimeInitialized() {
    startButton.disabled = false;
    startInfo.textContent = 'Necesita teclado. El ratón se captura al hacer clic.';
  },
};

async function loadWad() {
  const user = await getUserWad();
  let bytes, id;
  if (user) {
    bytes = new Uint8Array(user.data);
    id = identifyWad(bytes, user.name);
    $('wad-info').textContent = `Jugando con tu ${user.name} (${id.label}).`;
    $('btn-shareware').classList.remove('hidden');
  } else {
    startInfo.textContent = 'Descargando DOOM1.WAD (4 MB)…';
    const res = await fetch('wad/doom1.wad');
    if (!res.ok) throw new Error('HTTP ' + res.status);
    bytes = new Uint8Array(await res.arrayBuffer());
    id = { file: 'doom1.wad' };
    $('wad-info').textContent = 'DOOM shareware v1.9: episodio 1, Knee-Deep in the Dead.';
  }
  Module.FS.writeFile('/wad/' + id.file, bytes);
}

startButton.disabled = true;
startButton.addEventListener('click', () => {
  startButton.classList.add('hidden');
  startAudio();
  running = true;
  canvas.focus();
  canvas.requestPointerLock();
  Module.FS.chdir('/save');
  // Extra command line parameters, e.g. ?args=-warp 1 3 -skill 4
  const args = (params.get('args') || '').split(' ').filter(Boolean);
  Module.callMain(args);
});
