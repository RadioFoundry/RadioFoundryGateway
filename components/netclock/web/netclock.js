// netclock's page section. A page that serves this (netclock_http_register)
// puts <div data-netclock></div><script src="/netclock.js" defer></script>
// where the section goes. Options on the div:
//   data-base="/api"     where the JSON API is (CONFIG_NETCLOCK_HTTP_BASE)
//   data-auto-time="0"   don't send this browser's clock while the device has no NTP
//   data-refresh="5"     seconds between refreshes while visible
// Colors come from the page's --fg, --mute, --line, --ok, --bad, --accent, --warn if it has them.
// Localization: the page may define window.rdofT(s) (returns the Chinese string, or s when English /
// unknown) and window.rdofApplyLang() to retranslate static text; this file re-renders on 'rdof-langchange'.
(() => {
  const css = `.nc{--nc-mute:var(--mute,#6b6b70);--nc-line:var(--line,#d8d8dc);--nc-ok:var(--ok,#1a7f37);--nc-bad:var(--bad,#c62828);--nc-warn:var(--warn,#b26a00);--nc-acc:var(--accent,#0b5cad)}
.nc table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}.nc td,.nc th{padding:3px 6px;border-bottom:1px solid var(--nc-line);text-align:right;white-space:nowrap}
.nc th{color:var(--nc-mute);font-weight:500}.nc .l{text-align:left}.nc .m{color:var(--nc-mute)}.nc .wrap{overflow-x:auto}
.nc .kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:2px 16px;margin:6px 0}.nc .kv div{display:flex;justify-content:space-between;gap:8px;border-bottom:1px solid var(--nc-line);padding:2px 0}
.nc .kv span{color:var(--nc-mute)}.nc .now{font-size:1.15em;font-variant-numeric:tabular-nums}.nc input{font:inherit;padding:4px 6px;max-width:100%;box-sizing:border-box}
.nc form{display:flex;flex-wrap:wrap;gap:8px;align-items:end;margin:8px 0}.nc label{display:flex;flex-direction:column;font-size:13px;color:var(--nc-mute)}
.nc button{padding:6px 12px;border:0;border-radius:4px;background:var(--nc-acc);color:#fff;font:inherit;cursor:pointer}.nc svg{width:100%;height:auto;max-height:220px}`;
  const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const sgn = (v, d = 3) => (v >= 0 ? '+' : '−') + Math.abs(v).toFixed(d);
  const iso = ms => new Date(ms).toISOString().replace('T', ' ').slice(0, 23);
  const dur = s => { s = Math.round(s); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60); return h ? `${h}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`; };
  const markColor = {'*': 'var(--nc-ok)', '+': 'var(--nc-acc)', 'x': 'var(--nc-bad)', '~': 'var(--nc-warn)'};
  const L = s => (window.rdofT ? window.rdofT(s) : s);

  function mount(el) {
    if (el.dataset.ncMounted) return;
    el.dataset.ncMounted = 1;
    if (!document.getElementById('nc-css')) {
      const st = document.createElement('style'); st.id = 'nc-css'; st.textContent = css; document.head.appendChild(st);
    }
    const base = el.dataset.base || '/api', auto = el.dataset.autoTime !== '0', every = (+el.dataset.refresh || 5) * 1000;
    el.classList.add('nc');
    el.innerHTML = `<div class="now" data-r="now">--</div>
<div class="kv" data-r="track"></div>
<div class="wrap"><table><thead><tr><th class="l">S</th><th class="l">Server</th><th>Stratum</th><th>Refid</th><th>Reach</th><th>Last rx</th>
<th>Offset</th><th>Delay</th><th>Jitter</th><th>Root dist</th><th>NP</th></tr></thead><tbody data-r="src"></tbody></table></div>
<p class="m" style="font-size:13px">* selected, + combined, x falseticker (outside the majority), ~ delay spike (skipped this poll), ? no usable reply,
- unresolved or a duplicate address. Offset: last sample minus this clock (+ = behind). Reach: the last 8 polls, octal.</p>
<form data-r="set"><label>NTP servers (space separated, up to 6; pool.ntp.org counts 3)<input name="servers" size="40"></label>
<label>Poll (s)<input name="poll_s" type="number" min="16" max="3600" style="width:7em"></label><button>Save</button>
<button type="button" data-r="browser">Set from this browser</button><span class="m" data-r="msg"></span></form>
<details><summary>References</summary><svg data-r="plot" viewBox="0 0 640 200"></svg>
<div class="wrap"><table><thead><tr><th class="l">Time (UTC)</th><th class="l">Source</th><th>Result</th><th>Offset</th><th>Residual</th><th>In fit</th>
<th>Rate</th><th>Die</th></tr></thead><tbody data-r="log"></tbody></table></div></details>`;
    const $ = r => el.querySelector(`[data-r="${r}"]`);
    let clk = null, busy = false, lastPush = 0, formLoaded = false;
    const msg = (t, bad) => { $('msg').textContent = t; $('msg').style.color = bad ? 'var(--nc-bad)' : ''; };

    async function load() {
      try {
        const r = await fetch(base + '/clock', {cache: 'no-store'});
        if (!r.ok) throw new Error(r.status); /* a server without the clock: nothing to show */
        const c = await r.json();
        clk = {dev: c.utc_ms, at: performance.now(), c};
        render(c);
        if (auto && c.source !== 'ntp' && c.source !== 'pps' && c.source !== 'gnss' && (c.source === 'none' || (Date.now() - lastPush > 600000 && c.since_sync_s > 600))) push(false);
      } catch (e) { /* next tick */ }
      if (!formLoaded) {
        try {
          const r = await fetch(base + '/clock/settings', {cache: 'no-store'});
          if (!r.ok) throw new Error(r.status);
          const s = await r.json();
          const f = $('set'); f.servers.value = s.servers; f.poll_s.value = s.poll_s; formLoaded = true;
        } catch (e) { /* next tick */ }
      }
    }

    // This browser's clock: the quickest of 3 round trips, sent as our time plus half of it.
    async function push(manual) {
      if (busy) return;
      busy = true;
      try {
        let best = 1e9;
        for (let i = 0; i < 3; i++) {
          const t0 = performance.now();
          await fetch(base + '/clock/settings', {cache: 'no-store'});
          best = Math.min(best, performance.now() - t0);
        }
        const r = await (await fetch(base + '/time', {method: 'POST', headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({utc_ms: Date.now() + best / 2})})).json();
        lastPush = Date.now();
        if (r.applied) { msg(`${L('Set from this browser (±')}${Math.ceil(best / 2)}${L(' ms)')}`); load(); }
        else if (manual) msg(L('Not applied: the clock follows NTP'));
      } catch (e) { if (manual) msg(L('Failed: ') + e, true); }
      busy = false;
    }

    function render(c) {
      const src = c.sources || [], best = src.find(x => x.mark === '*');
      const used = src.filter(x => x.mark === '*' || x.mark === '+').length;
      const ref = c.source === 'ntp' ? (best ? `NTP ${esc(best.name)}` + (used > 1 ? L(' + ') + (used - 1) + L(' more') : '') : L('NTP (no server selected)'))
        : c.source === 'pps' ? L('GNSS pulse per second') : c.source === 'gnss' ? L('GNSS NMEA time (no pulse)')
        : c.source === 'browser' ? L('browser clock')
        : '<span style="color:var(--nc-bad)">' + L('not set') + '</span>';
      const pps = c.pps_gpio >= 0 ? {[`PPS (GPIO ${c.pps_gpio})`]: c.pps_last_s < 0 ? L('no pulses')
        : `${c.pps_edges}${L(' pulses, last ')}${dur(c.pps_last_s)}${L(' ago; ')}${c.pps_bad}${L(' out of step, ')}${c.pps_unlabeled}${L(' unnumbered')}`
          + (c.pps_by_gnss ? `${L(', ')}${c.pps_by_gnss}${L(' numbered by GNSS')}` : '')} : {};
      const gnss = c.gnss_last_s >= 0 ? {'GNSS NMEA': `${L('last ')}${dur(c.gnss_last_s)}${L(' ago')}`
        + (c.gnss_lag_ms != null ? `${L('; ')}${c.gnss_lag_ms}${L(' ms after the pulse')}` : '') + (c.gnss_refs ? `${L('; ')}${c.gnss_refs}${L(' references')}` : '')} : {};
      const fit = c.rate_src === 'learned';
      $('track').innerHTML = Object.entries({
        [L('Reference')]: ref,
        ...pps,
        ...gnss,
        [L('Since last reference')]: c.since_sync_s >= 0 ? dur(c.since_sync_s) : '--',
        [L('Slewing')]: `${sgn(c.slew_left_ms)} ms`,
        [L('Last offset')]: `${sgn(c.last_err_ms)} ms`,
        [L('RMS offset')]: fit ? `${c.resid_rms_ms.toFixed(3)} ms` : '--',
        [L('Frequency')]: `${Math.abs(c.rate_ppm).toFixed(3)} ppm ${c.rate_ppm >= 0 ? L('slow') : L('fast')} (${esc(c.rate_src)})`,
        [L('Skew')]: fit ? `±${c.skew_ppm.toFixed(3)} ppm` : '--',
        [L('Holdover, 1 h')]: fit ? `±${(c.skew_ppm * 3.6).toFixed(2)} ms` : '--',
        [L('Stored frequency')]: c.stored_ppm ? `${sgn(c.stored_ppm)} ppm` : L('none'),
        [L('References / span')]: `${c.refs} / ${dur(c.span_s)}`,
        [L('Poll interval')]: c.poll_s ? `${c.poll_s} s` : '--',
        [L('Stratum')]: best ? best.stratum + 1 : '--',
        [L('Root delay')]: best ? `${(best.root_delay_ms + best.delay_ms).toFixed(3)} ms` : '--',
        [L('Root dispersion')]: best ? `${(best.root_disp_ms + best.jitter_ms).toFixed(3)} ms` : '--',
        [L('Polls / no majority')]: `${c.polls} / ${c.no_majority}`,
        [L('Outliers ignored')]: c.outliers,
        [L('Die temperature')]: c.die_c !== undefined ? `${c.die_c} °C` : '--',
        [L('Temperature correction')]: c.base_die_c !== undefined ? `${sgn(c.temp_comp_ppm)} ppm (fitted at ${c.base_die_c} °C)` : '--',
        [L('Learned at')]: c.tcal && c.tcal.length ? c.tcal.map(e => `${e.c} °C ${sgn(e.ppm)}${e.n > 1 ? ` (${e.n})` : ''}`).join(', ') : L('none yet'),
      }).map(([k, v]) => `<div><span>${k}</span>${v}</div>`).join('');
      $('src').innerHTML = src.map(x => `<tr><td class="l" style="font-weight:600;color:${markColor[x.mark] || 'var(--nc-mute)'}">${esc(x.mark)}</td>` +
        `<td class="l">${esc(x.name)}<br><span class="m">${esc(x.ip || L('unresolved'))}</span></td><td>${x.stratum || '--'}</td>` +
        `<td>${esc(x.refid || '--')}${x.kod ? ' (KoD)' : ''}</td><td>${x.reach.toString(8)}</td>` +
        `<td>${x.last_rx_s >= 0 ? dur(x.last_rx_s) : '--'}</td><td>${x.np ? sgn(x.offset_ms) + ' ms' : '--'}</td>` +
        `<td>${x.last_rx_s >= 0 ? x.delay_ms.toFixed(1) + ` <span class="m">${L('(min ')}${x.min_delay_ms.toFixed(1)})</span> ms` : '--'}</td>` +
        `<td>${x.np > 1 ? x.jitter_ms.toFixed(2) + ' ms' : '--'}</td><td>${x.last_rx_s >= 0 ? '±' + x.lambda_ms.toFixed(2) + ' ms' : '--'}</td>` +
        `<td>${x.np}</td></tr>`).join('') || `<tr><td colspan="11" class="l m">${L('No NTP servers yet (no network, or none set)')}</td></tr>`;
      const log = c.log || [];
      $('log').innerHTML = log.slice().reverse().map(e => `<tr><td class="l">${iso(e.utc_ms)}</td>` +
        `<td class="l">${esc(e.src)} <span class="m">${esc(e.peer)}</span></td>` +
        `<td style="color:${e.result === 'outlier' ? 'var(--nc-bad)' : e.result === 'step' ? 'var(--nc-warn)' : 'inherit'}">${L(e.result)}</td>` +
        `<td>${e.result === 'step' ? '--' : sgn(e.err_ms) + ' ms'}</td><td>${e.resid_ms !== undefined ? sgn(e.resid_ms) + ' ms' : '--'}</td>` +
        `<td>${e.in_fit ? '✓' : ''}</td><td>${sgn(e.rate_ppm)} ppm</td><td>${e.die_c !== undefined ? e.die_c + ' °C' : '--'}</td></tr>`).join('') ||
        `<tr><td colspan="8" class="l m">${L('No references yet')}</td></tr>`;
      plot(log);
    }

    // Residuals about the fitted rate: filled = in the fit, open = not, x = outlier (clamped). Dashed: die temperature.
    function plot(log) {
      const svg = $('plot'), W = 640, H = 200, LM = 46, R = 40, T = 10, B = 24;
      const pts = log.filter(e => e.resid_ms !== undefined || e.result === 'outlier');
      if (pts.length < 2) {
        svg.innerHTML = `<text x="${W / 2}" y="${H / 2}" text-anchor="middle" style="fill:var(--nc-mute);font-size:12px">${L('Residuals show once the rate is fitted (about 10 min of references)')}</text>`;
        return;
      }
      const t0 = pts[0].utc_ms, t1 = Math.max(pts[pts.length - 1].utc_ms, t0 + 1);
      const ymax = Math.max(1, ...pts.filter(e => e.result !== 'outlier' && e.resid_ms !== undefined).map(e => Math.abs(e.resid_ms))) * 1.2;
      const temps = log.filter(e => e.die_c !== undefined && e.utc_ms >= t0);
      const tmin = temps.length ? Math.floor(Math.min(...temps.map(e => e.die_c)) - 1) : 0, tmax = temps.length ? Math.ceil(Math.max(...temps.map(e => e.die_c)) + 1) : 1;
      const X = ms => LM + (ms - t0) / (t1 - t0) * (W - LM - R);
      const Y = v => T + (1 - (Math.max(-ymax, Math.min(ymax, v)) + ymax) / (2 * ymax)) * (H - T - B);
      const YT = c => T + (1 - (c - tmin) / (tmax - tmin)) * (H - T - B);
      const txt = (x, y, s, a = 'end', col = 'var(--nc-mute)') => `<text x="${x}" y="${y}" text-anchor="${a}" style="fill:${col};font-size:10px">${s}</text>`;
      let g = `<line x1="${LM}" x2="${W - R}" y1="${Y(0)}" y2="${Y(0)}" style="stroke:var(--nc-line)"/>` + txt(LM - 4, Y(0) + 4, '0');
      for (const v of [ymax / 1.2, -ymax / 1.2]) g += `<line x1="${LM}" x2="${W - R}" y1="${Y(v)}" y2="${Y(v)}" style="stroke:var(--nc-line);stroke-dasharray:2 4"/>` + txt(LM - 4, Y(v) + 4, sgn(v, 1) + L(' ms'));
      g += txt(LM, H - 6, iso(t0).slice(11, 19), 'start') + txt(W - R, H - 6, iso(t1).slice(11, 19) + ' UTC');
      if (temps.length) {
        g += `<polyline points="${temps.map(e => X(e.utc_ms).toFixed(1) + ',' + YT(e.die_c).toFixed(1)).join(' ')}" style="fill:none;stroke:var(--nc-warn);stroke-dasharray:4 3"/>` +
          txt(W - R + 4, T + 10, tmax + '°', 'start', 'var(--nc-warn)') + txt(W - R + 4, H - B, tmin + '°', 'start', 'var(--nc-warn)');
      }
      for (const e of pts) {
        const x = X(e.utc_ms);
        if (e.result === 'outlier') {
          const y = Y(e.err_ms);
          g += `<path d="M${x - 4},${y - 4}L${x + 4},${y + 4}M${x - 4},${y + 4}L${x + 4},${y - 4}" style="stroke:var(--nc-bad);stroke-width:1.5"><title>${iso(e.utc_ms)}${L(' outlier ')}${sgn(e.err_ms)}${L(' ms')}</title></path>`;
        } else {
          g += `<circle cx="${x.toFixed(1)}" cy="${Y(e.resid_ms).toFixed(1)}" r="3.5" style="fill:${e.in_fit ? 'var(--nc-acc)' : 'none'};stroke:var(--nc-acc)"><title>${iso(e.utc_ms)} ${sgn(e.resid_ms)}${L(' ms')}</title></circle>`;
        }
      }
      svg.innerHTML = g;
    }

    $('set').onsubmit = async ev => {
      ev.preventDefault();
      const f = $('set');
      const r = await fetch(base + '/clock/settings', {method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({servers: f.servers.value.trim(), poll_s: +f.poll_s.value})});
      msg(r.ok ? L('Saved') : await r.text(), !r.ok);
      if (r.ok) load();
    };
    $('browser').onclick = () => push(true);
    setInterval(() => {
      if (!clk) return;
      const c = clk.c, ms = clk.dev + performance.now() - clk.at;
      $('now').innerHTML = `${iso(ms)} UTC <span class="m">${c.source === 'ntp' ? 'NTP' : c.source === 'pps' ? L('GNSS PPS') : c.source === 'gnss' ? L('GNSS NMEA') : c.source === 'browser' ? L('from a browser') : L('not set')}</span>`;
    }, 100);
    document.addEventListener('rdof-langchange', () => { if (clk) render(clk.c); });
    load();
    if (window.rdofApplyLang) window.rdofApplyLang();
    setInterval(() => { if (!document.hidden && el.offsetParent !== null) load(); }, every);
  }

  const all = () => document.querySelectorAll('[data-netclock]').forEach(mount);
  window.netclock = {mount};
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', all); else all();
})();
