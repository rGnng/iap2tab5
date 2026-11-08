/* Shared display helpers for the dashboard and diagnostics. */
window.Tab5 = (function () {
  'use strict';
  function escape(value) {
    return String(value == null ? '—' : value).replace(/[&<>"']/g, function (c) {
      return {'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c];
    });
  }
  function row(label, value, cls) {
    return '<div class="row"><span class="k">' + escape(label) + '</span>' +
      '<span class="v ' + (cls || '') + '">' + escape(value == null || value === '' ? '—' : value) + '</span></div>';
  }
  function chip(label, value, cls) {
    return '<span class="chip ' + (cls || '') + '"><span class="led"></span>' +
      escape(label) + ' <b>' + escape(value) + '</b></span>';
  }
  function bytes(n) {
    if (!Number.isFinite(n)) return '—';
    var u = ['B','KB','MB','GB','TB'], i = 0;
    while (n >= 1024 && i < 4) { n /= 1024; i++; }
    return n.toFixed(n >= 10 || i === 0 ? 0 : 1) + ' ' + u[i];
  }
  function time(sec) {
    sec = Math.max(0, Math.floor(sec || 0));
    return Math.floor(sec / 60) + ':' + String(sec % 60).padStart(2, '0');
  }
  function hex(n) { return Number.isFinite(n) ? '0x' + n.toString(16).toUpperCase().padStart(2, '0') : '—'; }
  function reset(n) {
    var names = {1:'Power on',2:'External',3:'Software',4:'Panic',5:'Interrupt watchdog',
      6:'Task watchdog',7:'Watchdog',8:'Deep sleep',9:'Brownout',10:'SDIO'};
    return (names[n] || 'Unknown') + ' (' + n + ')';
  }
  function usb(n) { return n === 2 ? 'USB-A + USB-C host' : 'USB-A host'; }
  function power(p, config) {
    p = p || {}; config = config || {};
    var valid = p.ina226_ok === true && Number.isFinite(p.bus_mv) && Number.isFinite(p.current_ma);
    var state = 'Unavailable', cls = 'bad', flow = 'Sensor unavailable';
    if (valid) {
      if (p.current_ma < -10) { state = 'Charging'; cls = 'ok'; flow = Math.abs(p.current_ma) + ' mA into battery'; }
      else if (p.current_ma > 10) { state = 'Discharging'; cls = 'warn'; flow = p.current_ma + ' mA from battery'; }
      else { state = p.chg_en ? 'Idle' : 'Disabled'; cls = ''; flow = '0 mA · no net battery current'; }
    }
    return {valid:valid, state:state, cls:cls, flow:flow,
      voltage:valid ? (p.bus_mv / 1000).toFixed(2) + ' V' : '—',
      current:valid ? p.current_ma + ' mA' : '—',
      enabled:p.chg_en === true, requested:config.charge_enabled === true};
  }
  function api(path, options) {
    var controller = new AbortController();
    var timer = setTimeout(function () { controller.abort(); }, 12000);
    return fetch(path, Object.assign({cache:'no-store'}, options || {}, {signal:controller.signal}))
      .then(function (response) {
        return response.text().then(function (body) {
          if (!response.ok) throw new Error(body || 'HTTP ' + response.status);
          return body;
        });
      }).finally(function () { clearTimeout(timer); });
  }
  function json(path) { return api(path).then(JSON.parse); }
  function key(name) { return api('/api/key?k=' + encodeURIComponent(name), {method:'POST'}); }
  function state(name, down) {
    return api('/api/keystate?k=' + encodeURIComponent(name) + '&down=' + (down ? 1 : 0), {method:'POST'});
  }
  function poll(onData, onError) {
    var busy = null, again = false, timer;
    function refresh() {
      if (busy) { again = true; return busy; }
      clearTimeout(timer);
      busy = Promise.allSettled([json('/api/status'), json('/api/config'), json('/api/power')])
        .then(function (results) {
          if (results[0].status !== 'fulfilled' || results[1].status !== 'fulfilled') {
            throw new Error('Device unreachable');
          }
          onData(results[0].value, results[1].value,
                 results[2].status === 'fulfilled' ? results[2].value : {});
        }).catch(onError).finally(function () {
          busy = null;
          if (again) { again = false; refresh(); }
          else if (!document.hidden) timer = setTimeout(refresh, 2000);
        });
      return busy;
    }
    document.addEventListener('visibilitychange', function () {
      clearTimeout(timer); if (!document.hidden) refresh();
    });
    refresh();
    return refresh;
  }
  return {escape:escape,row:row,chip:chip,bytes:bytes,time:time,hex:hex,reset:reset,
    usb:usb,power:power,api:api,json:json,key:key,state:state,poll:poll};
}());
