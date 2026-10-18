'use strict';

/* --------------------------------------------------------------------------
 * e_server front-end: single page, three tabs, no external libraries.
 * Talks to the reference (and embedded) C backend over relative /api paths.
 * ------------------------------------------------------------------------ */

async function getJSON(url, signal) {
  const r = await fetch(url, { cache: 'no-store', signal });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

async function postJSON(url, obj) {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(obj),
    cache: 'no-store'
  });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

function setStatus(id, text, cls) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = 'status' + (cls ? ' ' + cls : '');
}

/* ---------------------------------- tabs --------------------------------- */

const tabButtons = Array.from(document.querySelectorAll('.tab-btn'));

function showTab(name) {
  tabButtons.forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach(p => {
    p.classList.toggle('active', p.id === 'tab-' + name);
  });
  if (name === 'led') loadLeds();
  if (name === 'sensor') { startAdc(); redrawAll(); }
  else stopAdc();                    /* poll sensors only while the tab is open */
  if (name === 'camera') loadCamera();
  if (name === 'board') loadBoardInfo();
}
tabButtons.forEach(b => b.addEventListener('click', () => showTab(b.dataset.tab)));

/* ----------------------------- Tab 1: LEDs ------------------------------- */

const LED_NAMES = ['LED1 (P40)', 'LED2 (P41)'];
let ledStates = [0, 0];

function renderLeds() {
  const list = document.getElementById('led-list');
  list.innerHTML = '';
  ledStates.forEach((s, i) => {
    const label = document.createElement('label');
    label.className = 'led';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = !!s;
    cb.addEventListener('change', () => setLed(i, cb.checked));
    label.appendChild(cb);
    label.appendChild(document.createTextNode(LED_NAMES[i]));
    list.appendChild(label);
  });
}

async function loadLeds() {
  try {
    const j = await getJSON('/api/leds');
    ledStates = (j.leds && j.leds.length === 2) ? j.leds.slice() : [0, 0];
    renderLeds();
  } catch (e) {
    setStatus('led-status', 'failed to load LED state', 'err');
  }
}

let ledBusy = false;
let ledDirty = false;

async function setLed(i, on) {
  ledStates[i] = on ? 1 : 0;
  if (ledBusy) { ledDirty = true; return; }   /* coalesce clicks mid-flight */
  ledBusy = true;
  ledDirty = false;
  setStatus('led-status', 'sending…', '');
  try {
    do {
      const sent = ledStates.slice();
      const j = await postJSON('/api/leds', { leds: sent });
      ledStates = (j.leds && j.leds.length === 2) ? j.leds.slice() : ledStates;
      ledDirty = JSON.stringify(sent) !== JSON.stringify(ledStates);
    } while (ledDirty);
    renderLeds();
    setStatus('led-status', 'ok', 'ok');
  } catch (e) {
    setStatus('led-status', 'failed — ' + e.message, 'err');
    loadLeds();                   /* re-sync checkboxes with the server */
  } finally {
    ledBusy = false;
  }
}

/* ------------------------------ Tab 2: Sensors --------------------------- */

const MAX_SAMPLES = 60;

const plots = {
  temp: { canvasId: 'plot-temp', label: '°C', min: 0, max: 100, dec: 1, buf: [] }
};

let adcTimer = null;
let lastAdc = null;
let adcGen = 0;                  /* sampling-session generation; bumped on
                                    every start/stop so in-flight samples from
                                    an older session are discarded */
let adcInFlight = null;          /* AbortController of the pending request */

const intervalSel = document.getElementById('adc-interval');

function adcIntervalMs() { return parseInt(intervalSel.value, 10); }

function startAdc() {
  stopAdc();
  const gen = ++adcGen;
  adcTimer = setInterval(() => sampleAdc(gen), adcIntervalMs());
  sampleAdc(gen);
}

function stopAdc() {
  adcGen++;                      /* invalidate any in-flight sample */
  if (adcTimer) { clearInterval(adcTimer); adcTimer = null; }
  if (adcInFlight) { adcInFlight.abort(); adcInFlight = null; }
}

intervalSel.addEventListener('change', () => {
  try { localStorage.setItem('adc-interval', intervalSel.value); } catch (e) {}
  if (adcTimer) { startAdc(); }  /* restart the timer with the new interval */
});

function pushSample(key, val) {
  const p = plots[key];
  if (typeof val !== 'number' || !isFinite(val)) {
    val = p.buf.length ? p.buf[p.buf.length - 1] : p.min;   /* previous value */
  }
  p.buf.push(val);
  if (p.buf.length > MAX_SAMPLES) p.buf.shift();
}

async function sampleAdc(gen) {
  if (gen !== adcGen) return;    /* session stopped or restarted */
  if (adcInFlight) return;       /* previous request still pending (e.g. the
                                    server is offline): skip this tick instead
                                    of piling up requests that would all fire
                                    at once when the server comes back */
  const ctl = new AbortController();
  adcInFlight = ctl;
  let v;
  try {
    const to = setTimeout(() => ctl.abort(), adcIntervalMs() * 2);
    try {
      v = await getJSON('/api/adc', ctl.signal);
    } catch (e) {
      v = null;                  /* failed / aborted: no sample this tick */
    } finally {
      clearTimeout(to);
    }
  } finally {
    if (adcInFlight === ctl) adcInFlight = null;
  }
  if (gen !== adcGen) return;    /* stopped/restarted while awaiting */
  if (!v) return;
  lastAdc = v;
  /* VBAT is in volts (`vbat_v`); tolerate an old `vbat_mv` (millivolts). */
  const vbat = (v.vbat_v !== undefined) ? v.vbat_v
             : (v.vbat_mv !== undefined) ? v.vbat_mv / 1000 : undefined;
  pushSample('temp', v.temp_c);
  redrawAll();
}

function drawPlot(p) {
  const cv = document.getElementById(p.canvasId);
  const w = cv.clientWidth;
  const h = cv.clientHeight;
  if (w === 0 || h === 0) return;          /* panel hidden */
  cv.width = Math.round(w * devicePixelRatio);
  cv.height = Math.round(h * devicePixelRatio);
  const ctx = cv.getContext('2d');
  ctx.scale(devicePixelRatio, devicePixelRatio);
  ctx.clearRect(0, 0, w, h);

  const padL = 6, padR = 8, padT = 6, padB = 16;
  const pw = w - padL - padR, ph = h - padT - padB;

  /* Auto-scale the y-range to the buffered data (with padding) so a plot
   * always shows its line no matter the signal's range (e.g. the motion
   * channels can sit near 0 g or near ±1 g depending on how the board is
   * held). Falls back to the configured min/max while there is no data. */
  let lo = p.min, hi = p.max;
  const buf = p.buf, n = buf.length;
  if (n > 0) {
    let dlo = buf[0], dhi = buf[0];
    for (let i = 1; i < n; i++) {
      if (buf[i] < dlo) dlo = buf[i];
      if (buf[i] > dhi) dhi = buf[i];
    }
    let pad = (dhi - dlo) * 0.15;
    if (pad < 1e-6) pad = Math.max(Math.abs(dlo) * 0.1 + 1e-3, 1e-3); /* flat */
    lo = dlo - pad;
    hi = dhi + pad;
  }
  const span = hi - lo;

  /* grid */
  ctx.strokeStyle = '#232a37';
  ctx.lineWidth = 1;
  for (let g = 0; g < 4; g++) {
    const y = padT + (g / 3) * ph;
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(w - padR, y);
    ctx.stroke();
  }

  /* axis labels (dynamic min/max) */
  ctx.fillStyle = '#8b93a7';
  ctx.font = '10px monospace';
  ctx.textAlign = 'left';
  ctx.fillText(hi.toFixed(p.dec), padL, padT - 3);
  ctx.fillText(lo.toFixed(p.dec), padL, h - padB + 10);

  /* series */
  if (n >= 2) {
    ctx.strokeStyle = '#4ea1ff';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < n; i++) {
      const x = padL + (i / (MAX_SAMPLES - 1)) * pw;
      const y = padT + ph - ((buf[i] - lo) / span) * ph;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }

  /* latest value - centered horizontally so it never overlaps the gray
   * min/max axis labels drawn on the left */
  if (n > 0) {
    ctx.fillStyle = '#cfe3ff';
    ctx.textAlign = 'center';
    ctx.fillText(buf[n - 1].toFixed(p.dec) + ' ' + p.label, w / 2, h - 3);
  }
}

function redrawAll() {
  Object.keys(plots).forEach(k => drawPlot(plots[k]));
}

window.addEventListener('resize', redrawAll);

/* ---------------------------- Tab 3: Camera ------------------------------ */

const camImg = document.getElementById('camera-img');
const camPh = document.getElementById('camera-placeholder');
const camBtn = document.getElementById('cam-stream');
const camRot = document.getElementById('cam-rotation');
const camQ = document.getElementById('cam-quality');
const camQVal = document.getElementById('cam-quality-val');
const camApply = document.getElementById('cam-apply');
let camStreaming = false;
let camLastFrames = -1;
let camNoAdvance = 0;

function camShowImg(show) {
  camImg.style.display = show ? 'block' : 'none';
  camPh.style.display = show ? 'none' : 'block';
}

/* Apply rotation + quality on the sensor; the stream (if running) picks up
 * new frames automatically - restart it to force a clean boundary. */
async function camApplySettings() {
  try {
    const url = '/api/camera/control?rot=' + encodeURIComponent(camRot.value) +
                '&quality=' + encodeURIComponent(camQ.value);
    const j = await getJSON(url, null);
    setStatus('camera-status', 'applied (rot ' + (j.rot === undefined ? camRot.value : j.rot) +
              '°, quality ' + (j.quality === undefined ? camQ.value : j.quality) + ')', 'ok');
  } catch (e) {
    setStatus('camera-status', 'apply failed — camera not ready', 'err');
  }
}

function camStart() {
  camBtn.textContent = 'Stop stream';
  camLastFrames = -1;
  camNoAdvance = 0;
  /* The MJPEG stream auto-updates: pointing src at /stream keeps it live. */
  camImg.src = '/stream?' + Date.now();
  camStreaming = true;
  setStatus('camera-status', 'connecting…', '');
  camWatch();
}

function camStop() {
  camImg.removeAttribute('src');
  camBtn.textContent = 'Start stream';
  camStreaming = false;
  camShowImg(false);
  setStatus('camera-status', 'stream stopped', '');
}

/* While streaming, check /api/camera every 2 s: the frame counter must
 * advance, otherwise the sensor isn't feeding frames (the stream header
 * alone never draws an image). */
async function camWatch() {
  if (!camStreaming) return;
  let j;
  try {
    j = await getJSON('/api/camera');
  } catch (e) {
    setTimeout(camWatch, 2000);
    return;
  }
  if (j.frames !== undefined) {
    if (camLastFrames >= 0 && j.frames === camLastFrames) {
      if (++camNoAdvance >= 3) {
        setStatus('camera-status', 'no frames arriving — restarting stream', 'err');
        camStop();
        setTimeout(camStart, 1000);
        return;
      }
    } else {
      camNoAdvance = 0;
    }
    camLastFrames = j.frames;
  }
  setTimeout(camWatch, 2000);
}

/* 'load' fires when the browser has decoded the first frame (for the live
 * stream this is the first MJPEG part; for a snapshot the single frame). */
camImg.addEventListener('load', () => {
  camShowImg(true);
  if (camStreaming) setStatus('camera-status', 'streaming', 'ok');
});

/* 'error' fires when the request fails (503 etc.) or the data can't be
 * decoded - surface it instead of leaving a silent blank box. */
camImg.addEventListener('error', () => {
  camShowImg(false);
  if (camStreaming) {
    camBtn.textContent = 'Start stream';
    camStreaming = false;
    setStatus('camera-status', 'stream failed — camera not feeding frames', 'err');
  } else {
    setStatus('camera-status', 'snapshot failed — no frame', 'err');
  }
});

document.getElementById('cam-stream').addEventListener('click', () => {
  if (camStreaming) { camStop(); } else { camStart(); }
});
document.getElementById('cam-snapshot').addEventListener('click', () => {
  if (camStreaming) return;          /* don't fight the live stream */
  camShowImg(false);                 /* show the placeholder while fetching */
  camImg.src = '/capture?' + Date.now();
  setStatus('camera-status', 'snapshot…', '');
});

/* Rotation/quality controls: the Apply button pushes both to the sensor. */
camApply.addEventListener('click', camApplySettings);
camQ.addEventListener('input', () => { camQVal.textContent = camQ.value; });

async function loadCamera() {
  setStatus('camera-status', 'querying camera…', '');
  try {
    const j = await getJSON('/api/camera');
    if (j.ready) {
      setStatus('camera-status',
                'GC2145 ready (' + (j.w || '?') + 'x' + (j.h || '?') +
                (j.frames !== undefined ? ' · ' + j.frames + ' frames' : '') +
                ')', 'ok');
    } else {
      setStatus('camera-status', 'GC2145 not ready', 'err');
    }
  } catch (e) {
    setStatus('camera-status', 'camera backend unavailable', 'err');
  }
}

/* ---------------------------- Tab 4: Board info -------------------------- */

async function loadBoardInfo() {
  const set = (id, v) => {
    document.getElementById(id).textContent =
      (v === null || v === undefined || v === '') ? 'N/A' : v;
  };
  set('info-arch', '…');
  set('info-lan', '…');
  set('info-pub', '…');
  set('info-geo', '…');
  set('info-weather', '…');

  let j;
  try {
    j = await getJSON('/api/info');
  } catch (e) {
    set('info-arch', 'N/A');
    set('info-lan', 'N/A');
    set('info-pub', 'N/A');
    set('info-geo', 'N/A');
    set('info-weather', 'N/A');
    return;
  }

  set('info-arch', j.arch);
  set('info-lan', j.lan_ip);
  set('info-pub', j.public_ip);
  if (j.geo && (j.geo.city || j.geo.country)) {
    set('info-geo', [j.geo.city, j.geo.country].filter(Boolean).join(', '));
  } else {
    set('info-geo', 'N/A');
  }
  if (j.weather && j.weather.temp_c !== null && j.weather.temp_c !== undefined) {
    const parts = [j.weather.temp_c + ' °C'];
    if (j.weather.desc) parts.push(j.weather.desc);
    set('info-weather', parts.join(' · '));
  } else {
    set('info-weather', 'N/A');
  }
}

document.getElementById('info-refresh').addEventListener('click', loadBoardInfo);

/* --------------------------------- init ---------------------------------- */

(function init() {
  /* restore the persisted sample interval, default 1 s */
  try {
    const saved = localStorage.getItem('adc-interval');
    if (saved && ['1000', '2000', '4000'].includes(saved)) intervalSel.value = saved;
  } catch (e) {}
  showTab('led');
})();
