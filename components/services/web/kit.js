/* appkit dashboard runtime: pairing, API calls, header, views, toasts, charts.
 * The page calls kit.start({onReady}) and uses kit.api('/api/...'). */
(function () {
  'use strict';
  const KEY = 'appkit-token-' + location.host;
  const kit = window.kit = {};
  let token = '';
  try { token = localStorage.getItem(KEY) || ''; } catch (e) { /* private mode */ }

  /* ---------------------------------------------------------------- dom */
  kit.$ = (s, r) => (r || document).querySelector(s);
  kit.$$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  kit.el = function (tag, attrs, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v == null || v === false) continue;
      if (k === 'class') e.className = v;
      else if (k === 'style' && typeof v === 'object') Object.assign(e.style, v);
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else if (k === 'html') e.innerHTML = v;
      else e.setAttribute(k, v === true ? '' : v);
    }
    for (const c of children.flat()) if (c != null && c !== false) e.append(c.nodeType ? c : String(c));
    return e;
  };
  kit.esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

  /* ------------------------------------------------------------- format */
  kit.num = (v, d = 0) => v == null || isNaN(v) ? '–' : Number(v).toLocaleString('it-IT', {minimumFractionDigits: d, maximumFractionDigits: d});
  kit.euro = (v, d = 2) => v == null || isNaN(v) ? '–' : kit.num(v, d) + ' €';
  kit.ago = function (seconds) {
    if (seconds == null || seconds < 0) return '–';
    if (seconds < 60) return 'adesso';
    if (seconds < 3600) return Math.round(seconds / 60) + ' min fa';
    if (seconds < 86400) return Math.round(seconds / 3600) + ' h fa';
    const d = Math.round(seconds / 86400);
    return d === 1 ? 'ieri' : d + ' giorni fa';
  };
  kit.date = (t, opt) => new Date(t * 1000).toLocaleDateString('it-IT', opt || {day: 'numeric', month: 'short'});
  kit.time = t => new Date(t * 1000).toLocaleTimeString('it-IT', {hour: '2-digit', minute: '2-digit'});

  /* -------------------------------------------------------------- toast */
  kit.toast = function (text, kind) {
    let box = kit.$('.kit-toasts');
    if (!box) document.body.append(box = kit.el('div', {class: 'kit-toasts'}));
    const t = kit.el('div', {class: 'kit-toast ' + (kind || '')}, text);
    box.append(t);
    setTimeout(() => t.remove(), 4200);
  };

  /* ---------------------------------------------------------------- api */
  /* An internal app's page lives in a folder of the OS server ("/pieno/"):
   * its "/api/..." calls go to "/pieno/api/...". Pairing stays at the root. */
  kit.base = location.pathname.replace(/\/[^/]*$/, '');
  kit.api = async function (path, opt) {
    opt = opt || {};
    if (kit.base && path.startsWith('/api/') && path !== '/api/pair') path = kit.base + path;
    const init = {method: opt.method || (opt.body !== undefined ? 'POST' : 'GET'), headers: {'X-Key': token}};
    if (opt.body !== undefined) {
      if (opt.body instanceof Blob || typeof opt.body === 'string') init.body = opt.body;
      else { init.body = JSON.stringify(opt.body); init.headers['Content-Type'] = 'application/json'; }
    }
    let r;
    try { r = await fetch(path, init); }
    catch (e) { setOnline(false); throw new Error('Orologio non raggiungibile'); }
    setOnline(true);
    if (r.status === 401) { showPair(); throw new Error('Abbina la dashboard'); }
    if (r.status === 404 && kit.base && !document.getElementById('kit-closed')) {
      /* An internal app closed on the watch: reloading the page opens it again. */
      const bar = kit.el('div', {id: 'kit-closed', style: {position: 'fixed', left: 0, right: 0, bottom: 0, zIndex: 60,
        padding: '14px', background: 'var(--panel)', borderTop: '1px solid var(--line)', textAlign: 'center'}},
        "L'app è chiusa sull'orologio. ", kit.el('button', {class: 'btn primary', onclick: () => location.reload()}, 'Riapri'));
      document.body.append(bar);
    }
    if (!r.ok) throw new Error((await r.text()) || ('Errore ' + r.status));
    if (opt.raw) return r;
    const type = r.headers.get('Content-Type') || '';
    return type.includes('json') ? r.json() : r.text();
  };
  kit.key = () => token;   /* links and uploads that cannot set headers use ?k= */
  kit.try = async function (fn, okText) {
    try { const v = await fn(); if (okText) kit.toast(okText, 'good'); return v; }
    catch (e) { kit.toast(e.message, 'bad'); return undefined; }
  };

  function setOnline(on) {
    const dot = kit.$('.kit-dot');
    if (dot) dot.classList.toggle('off', !on);
  }

  /* -------------------------------------------------------------- pair */
  let pairing = null, readyCb = null;
  function showPair(prefill) {
    if (pairing) return;
    pairing = kit.el('div', {class: 'kit-pair'});
    const inputs = [...Array(6)].map((_, i) => kit.el('input', {inputmode: 'numeric', maxlength: 1, 'aria-label': 'Cifra ' + (i + 1)}));
    const msg = kit.el('p', {class: 'dim'}, "Sull'orologio apri la pagina Dashboard e leggi il codice a 6 cifre.");
    const submit = async () => {
      const code = inputs.map(i => i.value).join('');
      if (code.length !== 6) return;
      try {
        const r = await fetch('/api/pair', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({code})});
        if (!r.ok) throw new Error(await r.text());
        token = (await r.json()).token;
        try { localStorage.setItem(KEY, token); } catch (e) { /* session only */ }
        pairing.remove(); pairing = null;
        history.replaceState(null, '', location.pathname);
        boot();
      } catch (e) {
        msg.textContent = e.message; msg.className = 'bad';
        inputs.forEach(i => i.value = ''); inputs[0].focus();
      }
    };
    inputs.forEach((input, i) => {
      input.addEventListener('input', () => {
        input.value = input.value.replace(/\D/g, '').slice(-1);
        if (input.value && i < 5) inputs[i + 1].focus();
        submit();
      });
      input.addEventListener('keydown', e => { if (e.key === 'Backspace' && !input.value && i) inputs[i - 1].focus(); });
      input.addEventListener('paste', e => {
        const d = (e.clipboardData.getData('text') || '').replace(/\D/g, '').slice(0, 6);
        if (d.length === 6) { e.preventDefault(); d.split('').forEach((c, k) => inputs[k].value = c); submit(); }
      });
    });
    const name = document.title.split('·')[0].trim();
    pairing.append(kit.el('div', {class: 'box'},
      kit.el('div', {class: 'kit-watch'}, name.slice(0, 5)),
      kit.el('h1', {}, 'Abbina ' + name),
      msg, kit.el('div', {class: 'digits'}, inputs),
      kit.el('p', {class: 'faint'}, 'Il browser resta abbinato anche dopo un riavvio dell\'app.')));
    document.body.append(pairing);
    if (prefill && /^\d{6}$/.test(prefill)) { prefill.split('').forEach((c, k) => inputs[k].value = c); submit(); }
    else inputs[0].focus();
  }

  /* ------------------------------------------------------------- header */
  kit.info = null;
  async function header() {
    try {
      const info = kit.info = await kit.api('/api/kit');
      const dev = kit.$('.kit-dev');
      if (dev) {
        const batt = info.battery >= 0 ? (info.charging ? '⚡ ' : '') + info.battery + '%' : '';
        dev.innerHTML = '';
        dev.append(kit.el('span', {class: 'kit-dot'}), kit.el('span', {class: 'hide-s'}, info.board),
                   batt ? kit.el('span', {}, batt) : '', themeButton());
      }
      return info;
    } catch (e) { return null; }
  }
  function themeButton() {
    const b = kit.el('button', {class: 'kit-icon', title: 'Tema chiaro/scuro'}, '◐');
    b.onclick = () => {
      const r = document.documentElement;
      const dark = r.dataset.theme ? r.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
      r.dataset.theme = dark ? 'light' : 'dark';
      try { localStorage.setItem('appkit-theme', r.dataset.theme); } catch (e) { /* ignore */ }
      window.dispatchEvent(new Event('kit-theme'));
    };
    return b;
  }
  try { const t = localStorage.getItem('appkit-theme'); if (t) document.documentElement.dataset.theme = t; } catch (e) { /* ignore */ }

  /* -------------------------------------------------------------- views */
  kit.views = function (onShow) {
    const nav = kit.$('.kit-nav');
    if (!nav) return;
    const show = id => {
      kit.$$('.kit-nav button').forEach(b => b.classList.toggle('on', b.dataset.view === id));
      kit.$$('.view').forEach(v => v.classList.toggle('on', v.id === id));
      try { sessionStorage.setItem('appkit-view', id); } catch (e) { /* ignore */ }
      if (onShow) onShow(id);
    };
    nav.addEventListener('click', e => { const b = e.target.closest('button[data-view]'); if (b) show(b.dataset.view); });
    let first = nav.querySelector('button[data-view]').dataset.view;
    try { const s = sessionStorage.getItem('appkit-view'); if (s && kit.$('#' + s)) first = s; } catch (e) { /* ignore */ }
    const linked = location.hash.match(/#(v-[\w-]+)/);   /* links like http://pieno.local/#v-map */
    if (linked && kit.$('#' + linked[1])) first = linked[1];
    show(first);
    kit.show = show;
  };

  /* -------------------------------------------------------------- poll */
  kit.poll = function (fn, ms) {
    let timer = null;
    const run = async () => {
      clearTimeout(timer);
      if (!document.hidden && !pairing) { try { await fn(); } catch (e) { /* shown by api */ } }
      timer = setTimeout(run, ms);
    };
    document.addEventListener('visibilitychange', () => { if (!document.hidden) run(); });
    run();
    return () => clearTimeout(timer);
  };

  /* ------------------------------------------------------------- tables */
  kit.sortable = function (table, rows, render) {
    let key = null, dir = 1;
    const draw = () => {
      const list = rows().slice();
      if (key) list.sort((a, b) => {
        const x = a[key], y = b[key];
        return (x == null) - (y == null) || (x < y ? -dir : x > y ? dir : 0);
      });
      render(list);
    };
    kit.$$('th[data-sort]', table).forEach(th => th.onclick = () => {
      dir = key === th.dataset.sort ? -dir : 1; key = th.dataset.sort;
      kit.$$('th[data-sort]', table).forEach(t => t.textContent = t.textContent.replace(/ [▲▼]$/, ''));
      th.textContent += dir > 0 ? ' ▲' : ' ▼';
      draw();
    });
    return draw;
  };

  /* ------------------------------------------------------------ download */
  kit.download = function (name, text, type) {
    const a = kit.el('a', {href: URL.createObjectURL(new Blob([text], {type: type || 'text/plain'})), download: name});
    document.body.append(a); a.click(); a.remove();
  };

  /* ------------------------------------------------------------- charts */
  /* series: [{points:[[x,y],...], color, fill, label, dash}], opt: {height, yfmt, xfmt, ymin, ymax, band:[lo,hi]} */
  kit.chart = function (host, series, opt) {
    opt = opt || {};
    const W = Math.max(280, host.clientWidth || 600), H = opt.height || 220, P = {l: 46, r: 12, t: 12, b: 26};
    const pts = series.flatMap(s => s.points);
    host.innerHTML = '';
    if (!pts.length) { host.append(kit.el('div', {class: 'empty'}, opt.empty || 'Ancora nessun dato')); return; }
    let x0 = Math.min(...pts.map(p => p[0])), x1 = Math.max(...pts.map(p => p[0]));
    let y0 = opt.ymin != null ? opt.ymin : Math.min(...pts.map(p => p[1]));
    let y1 = opt.ymax != null ? opt.ymax : Math.max(...pts.map(p => p[1]));
    if (x1 === x0) { x0 -= 1; x1 += 1; }
    if (y1 === y0) { y0 -= 1; y1 += 1; }
    const pad = (y1 - y0) * 0.08; if (opt.ymin == null) y0 -= pad; if (opt.ymax == null) y1 += pad;
    const X = x => P.l + (x - x0) / (x1 - x0) * (W - P.l - P.r), Y = y => H - P.b - (y - y0) / (y1 - y0) * (H - P.t - P.b);
    const ns = 'http://www.w3.org/2000/svg', mk = (t, a) => { const e = document.createElementNS(ns, t); for (const k in a) e.setAttribute(k, a[k]); return e; };
    const svg = mk('svg', {class: 'chart', viewBox: `0 0 ${W} ${H}`, width: '100%', height: H});
    const g = mk('g', {class: 'grid'});
    for (let i = 0; i <= 4; i++) {
      const v = y0 + (y1 - y0) * i / 4, y = Y(v);
      g.append(mk('line', {x1: P.l, x2: W - P.r, y1: y, y2: y}));
      const t = mk('text', {x: P.l - 6, y: y + 4, 'text-anchor': 'end'}); t.textContent = (opt.yfmt || (v => kit.num(v, 2)))(v); g.append(t);
    }
    const ticks = Math.min(6, Math.max(2, Math.floor(W / 120)));
    for (let i = 0; i <= ticks; i++) {
      const v = x0 + (x1 - x0) * i / ticks;
      const t = mk('text', {x: X(v), y: H - 6, 'text-anchor': i === 0 ? 'start' : i === ticks ? 'end' : 'middle'});
      t.textContent = (opt.xfmt || (v => kit.date(v)))(v); g.append(t);
    }
    svg.append(g);
    if (opt.band) svg.append(mk('rect', {x: P.l, width: W - P.l - P.r, y: Y(opt.band[1]), height: Math.max(0, Y(opt.band[0]) - Y(opt.band[1])), fill: 'var(--good)', opacity: .08}));
    for (const s of series) {
      if (!s.points.length) continue;
      const d = s.points.map((p, i) => (i ? 'L' : 'M') + X(p[0]).toFixed(1) + ' ' + Y(p[1]).toFixed(1)).join('');
      if (s.fill) {
        const id = 'f' + Math.random().toString(36).slice(2);
        const grad = mk('linearGradient', {id, x1: 0, x2: 0, y1: 0, y2: 1});
        grad.append(mk('stop', {offset: 0, 'stop-color': s.color, 'stop-opacity': .35}), mk('stop', {offset: 1, 'stop-color': s.color, 'stop-opacity': 0}));
        const defs = mk('defs', {}); defs.append(grad); svg.append(defs);
        svg.append(mk('path', {d: d + `L${X(s.points[s.points.length - 1][0])} ${H - P.b}L${X(s.points[0][0])} ${H - P.b}Z`, fill: `url(#${id})`}));
      }
      svg.append(mk('path', {d, fill: 'none', stroke: s.color, 'stroke-width': s.width || 2.2, 'stroke-linejoin': 'round', 'stroke-dasharray': s.dash || ''}));
      if (s.points.length < 40) for (const p of s.points) svg.append(mk('circle', {cx: X(p[0]), cy: Y(p[1]), r: 2.6, fill: s.color}));
    }
    /* hover readout */
    const cursor = mk('line', {y1: P.t, y2: H - P.b, stroke: 'var(--faint)', 'stroke-dasharray': '3 3', opacity: 0});
    const tip = kit.el('div', {class: 'card', style: {position: 'absolute', padding: '7px 10px', pointerEvents: 'none', display: 'none', fontSize: '13px', zIndex: 5}});
    svg.append(cursor);
    host.style.position = 'relative';
    svg.addEventListener('mousemove', e => {
      const r = svg.getBoundingClientRect(), x = x0 + ((e.clientX - r.left) * W / r.width - P.l) / (W - P.l - P.r) * (x1 - x0);
      let best = null;
      for (const s of series) for (const p of s.points) if (!best || Math.abs(p[0] - x) < Math.abs(best[0] - x)) best = p;
      if (!best) return;
      cursor.setAttribute('x1', X(best[0])); cursor.setAttribute('x2', X(best[0])); cursor.setAttribute('opacity', 1);
      const lines = series.map(s => { const p = s.points.find(q => q[0] === best[0]); return p ? `<b style="color:${s.color}">${kit.esc(s.label || '')}</b> ${(opt.yfmt || (v => kit.num(v, 3)))(p[1])}` : ''; }).filter(Boolean);
      tip.innerHTML = `<div class="faint">${(opt.xfmt || (v => kit.date(v)))(best[0])}</div>` + lines.join('<br>');
      tip.style.display = 'block';
      tip.style.left = Math.min(r.width - 160, (e.clientX - r.left) + 12) + 'px'; tip.style.top = '8px';
    });
    svg.addEventListener('mouseleave', () => { cursor.setAttribute('opacity', 0); tip.style.display = 'none'; });
    host.append(svg, tip);
  };

  /* ------------------------------------------------------------- start */
  function boot() {
    header().then(info => { if (info && readyCb) { const cb = readyCb; readyCb = null; cb(info); } });
  }
  kit.start = function (opt) {
    readyCb = opt && opt.onReady;
    const m = location.hash.match(/code=(\d{6})/);
    if (m) { showPair(m[1]); return; }
    if (!token) { showPair(); return; }
    boot();
    setInterval(() => { if (!pairing && !document.hidden) header(); }, 30000);
  };
})();
