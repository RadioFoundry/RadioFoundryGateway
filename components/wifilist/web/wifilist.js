// wifilist's page section. A page that serves this (wifilist_http_register)
// puts <div data-wifilist></div><script src="/wifilist.js" defer></script>
// where the section goes. Options on the div:
//   data-base="/api"     where the JSON API is (CONFIG_WIFILIST_HTTP_BASE)
//   data-refresh="5"     seconds between refreshes while visible
// Colors come from the page's --mute, --line, --ok, --bad, --accent if it has them.
// Passwords never come back from the board: a stored one shows as "set".
(() => {
  const css = `.wl{--wl-mute:var(--mute,#6b6b70);--wl-line:var(--line,#d8d8dc);--wl-ok:var(--ok,#1a7f37);--wl-bad:var(--bad,#c62828);--wl-acc:var(--accent,#0b5cad)}
.wl table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}.wl td,.wl th{padding:3px 6px;border-bottom:1px solid var(--wl-line);text-align:left;white-space:nowrap}
.wl th{color:var(--wl-mute);font-weight:500}.wl .m{color:var(--wl-mute)}.wl .wrap{overflow-x:auto}.wl .ok{color:var(--wl-ok)}.wl .bad{color:var(--wl-bad)}
.wl form{display:flex;flex-wrap:wrap;gap:8px;align-items:end;margin:8px 0}.wl label{display:flex;flex-direction:column;font-size:13px;color:var(--wl-mute)}
.wl input{font:inherit;padding:4px 6px;max-width:100%;box-sizing:border-box}.wl label.cb{flex-direction:row;align-items:center;gap:4px}
.wl button{padding:6px 12px;border:0;border-radius:4px;background:var(--wl-acc);color:#fff;font:inherit;cursor:pointer}
.wl td button{padding:2px 8px;margin-right:4px;background:none;color:var(--wl-acc);border:1px solid var(--wl-line)}.wl .now{margin:4px 0 8px}`;
  const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const dur = s => { const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60); return h ? `${h}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`; };
  const bars = r => r === undefined ? '<span class="m">not seen</span>' : `${r} dBm`;

  function mount(el) {
    if (el.dataset.wlMounted) return;
    el.dataset.wlMounted = 1;
    if (!document.getElementById('wl-css')) {
      const st = document.createElement('style'); st.id = 'wl-css'; st.textContent = css; document.head.appendChild(st);
    }
    const base = el.dataset.base || '/api', every = (+el.dataset.refresh || 5) * 1000;
    el.classList.add('wl');
    el.innerHTML = `<div class="now" data-r="now">--</div>
<div class="wrap"><table><thead><tr><th>Network</th><th>Priority</th><th>Signal</th><th>Password</th><th></th></tr></thead><tbody data-r="nets"></tbody></table></div>
<form data-r="form" autocomplete="off"><label>Network name<input name="ssid" list="wl-seen" maxlength="32" required size="18"></label>
<label>Password<input name="password" type="password" maxlength="64" size="16" placeholder=""></label>
<label>Priority<input name="priority" type="number" min="-100" max="100" value="0" style="width:5em"></label>
<label class="cb"><input name="hidden" type="checkbox">hidden</label><button data-r="add">Add</button>
<button type="button" data-r="cancel" hidden>Cancel</button><span class="m" data-r="msg"></span></form><datalist id="wl-seen"></datalist>
<p class="m" style="font-size:13px">The board joins the highest priority network in sight (the stronger signal among equals) and falls back down the list.
Moving a network renumbers the priorities in the new order. A blank password keeps the stored one when editing, and means an open network when adding.</p>
<details><summary>In sight and recent failures</summary><p><button type="button" data-r="scan">Scan</button></p>
<div class="wrap"><table><thead><tr><th>Network</th><th>Signal</th><th>Channel</th><th>Security</th><th></th></tr></thead><tbody data-r="seen"></tbody></table></div>
<div class="wrap"><table><thead><tr><th>When</th><th>Network</th><th>What</th><th>Signal</th></tr></thead><tbody data-r="fails"></tbody></table></div></details>`;
    const $ = r => el.querySelector(`[data-r="${r}"]`);
    const f = $('form');
    let st = null, editing = null;
    const msg = (t, bad) => { $('msg').textContent = t; $('msg').style.color = bad ? 'var(--wl-bad)' : ''; };

    async function load() {
      try { st = await (await fetch(base + '/wifi', {cache: 'no-store'})).json(); render(); } catch (e) { /* next tick */ }
    }

    // The whole list goes back; entries without "password" keep theirs (by "was", the name they had).
    async function save(list, done) {
      const r = await fetch(base + '/wifi/networks', {method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({networks: list})});
      if (!r.ok) { msg(await r.text(), true); return false; }
      st = await r.json(); render(); msg(done); return true;
    }
    const current = () => st.networks.map(n => ({ssid: n.ssid, priority: n.priority, hidden: n.hidden, was: n.ssid}));

    function render() {
      const s = st;
      $('now').innerHTML = s.connected
        ? `<span class="ok">Connected</span> to <b>${esc(s.ssid)}</b>, ${s.rssi} dBm, channel ${s.channel}, ${esc(s.ip)} <span class="m">for ${dur(s.connected_s)} (${esc(s.bssid)})</span>`
        : `<span class="bad">Not connected</span> <span class="m">(${esc(s.state)})</span>`;
      if (s.ap_up) $('now').innerHTML += `<br><span class="m">Setup access point ${esc(s.ap_ssid)} is on (http://192.168.4.1/)</span>`;
      $('nets').innerHTML = s.networks.map((n, i) => `<tr><td>${esc(n.ssid)}${n.hidden ? ' <span class="m">(hidden)</span>' : ''}` +
        `${s.connected && n.ssid === s.ssid ? ' <span class="ok">●</span>' : ''}</td><td>${n.priority}</td><td>${bars(n.rssi)}</td>` +
        `<td>${n.password_set ? 'set' : '<span class="m">open</span>'}</td><td><button type="button" data-a="up" data-i="${i}"${i ? '' : ' disabled'}>↑</button>` +
        `<button type="button" data-a="down" data-i="${i}"${i < s.networks.length - 1 ? '' : ' disabled'}>↓</button>` +
        `<button type="button" data-a="edit" data-i="${i}">Edit</button><button type="button" data-a="del" data-i="${i}">Delete</button></td></tr>`).join('') ||
        '<tr><td colspan="5" class="m">No networks yet: add one below</td></tr>';
      $('seen').innerHTML = s.seen.map(a => `<tr><td>${esc(a.ssid)}</td><td>${a.rssi} dBm</td><td>${a.channel}</td><td>${a.open ? 'open' : 'secured'}</td>` +
        `<td>${s.networks.some(n => n.ssid === a.ssid) ? '<span class="m">in the list</span>' : `<button type="button" data-a="pick" data-s="${esc(a.ssid)}">Add</button>`}</td></tr>`).join('') ||
        `<tr><td colspan="5" class="m">${s.scans ? 'Nothing in sight' : 'No scan yet'}</td></tr>`;
      document.getElementById('wl-seen').innerHTML = s.seen.map(a => `<option value="${esc(a.ssid)}">`).join('');
      $('fails').innerHTML = s.fails.slice().reverse().map(x => `<tr><td>${dur(Math.max(0, s.uptime_s - x.t_s))} ago</td><td>${esc(x.ssid)}</td>` +
        `<td>${x.lost ? 'lost: ' : ''}${esc(x.why)} <span class="m">(${x.reason})</span></td><td>${x.rssi ? x.rssi + ' dBm' : '--'}</td></tr>`).join('') ||
        '<tr><td colspan="4" class="m">None</td></tr>';
    }

    function resetForm() {
      editing = null; f.reset(); f.priority.value = 0; $('add').textContent = 'Add'; $('cancel').hidden = true;
      f.password.placeholder = '';
    }

    el.addEventListener('click', async ev => {
      const b = ev.target.closest('button[data-a]');
      if (!b || !st) return;
      const i = +b.dataset.i, list = current();
      if (b.dataset.a === 'pick') { resetForm(); f.ssid.value = b.dataset.s; f.password.focus(); return; }
      if (b.dataset.a === 'edit') {
        const n = st.networks[i];
        editing = n.ssid; f.ssid.value = n.ssid; f.password.value = ''; f.priority.value = n.priority; f.hidden.checked = n.hidden;
        f.password.placeholder = n.password_set ? 'set (blank keeps it)' : 'open (blank)';
        $('add').textContent = 'Save'; $('cancel').hidden = false; f.ssid.focus(); return;
      }
      if (b.dataset.a === 'del') {
        if (!confirm(`Delete ${st.networks[i].ssid}?`)) return;
        list.splice(i, 1); await save(list, 'Deleted'); return;
      }
      const j = b.dataset.a === 'up' ? i - 1 : i + 1;
      [list[i], list[j]] = [list[j], list[i]];
      list.forEach((n, k) => { n.priority = 10 * (list.length - k); });
      await save(list, 'Moved');
    });

    f.onsubmit = async ev => {
      ev.preventDefault();
      const list = current();
      const e = {ssid: f.ssid.value, priority: +f.priority.value, hidden: f.hidden.checked};
      if (f.password.value) e.password = f.password.value;
      if (editing !== null) {
        const k = list.findIndex(n => n.was === editing);
        e.was = editing;
        list[k] = e;
      } else {
        if (!e.password) e.password = '';
        list.push(e);
      }
      if (await save(list, editing !== null ? 'Saved' : 'Added')) resetForm();
    };
    $('cancel').onclick = resetForm;
    $('scan').onclick = async () => { await fetch(base + '/wifi/scan', {method: 'POST'}); msg('Scanning...'); setTimeout(load, 4000); };
    load();
    setInterval(() => { if (!document.hidden && el.offsetParent !== null) load(); }, every);
  }

  const all = () => document.querySelectorAll('[data-wifilist]').forEach(mount);
  window.wifilist = {mount};
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', all); else all();
})();
