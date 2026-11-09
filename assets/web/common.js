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
  function bluetoothPanel(root, notify) {
    var latest=null, busy=false, signature='';
    root.innerHTML='<h2>Bluetooth controllers</h2><div class="body">'+
      '<div id="bt_status" class="hint" role="status" aria-live="polite">Loading…</div>'+
      '<div class="actions"><button id="bt_toggle" data-bt-action="enable" disabled>Enable Bluetooth</button>'+
      '<button id="bt_scan" data-bt-action="scan" disabled>Scan for controllers</button></div>'+
      '<h3>Paired controllers</h3><div id="bt_paired"></div>'+
      '<h3>Nearby controllers</h3><div id="bt_discovered"></div>'+
      '<div class="hint">Put the controller in pairing mode, scan, then choose Pair. Delete forgets its saved pairing.</div></div>';
    function find(id) { return root.querySelector('#'+id); }
    function button(parent, text, action, dev, disabled) {
      var b=document.createElement('button');
      b.textContent=text; b.dataset.btAction=action; b.dataset.addr=dev.addr;
      b.setAttribute('aria-label',text+' '+(dev.name || dev.addr));
      b.disabled=disabled; if(action==='forget') b.className='danger';
      parent.appendChild(b);
    }
    function list(id, devices, paired, bt) {
      var container=find(id); container.replaceChildren();
      if(!devices.length) {
        var hint=document.createElement('div'); hint.className='hint';
        hint.textContent=paired ? 'No paired controllers.' : !bt.enabled ? 'Enable Bluetooth to search.' :
          bt.scanning ? 'Searching… Put the controller in pairing mode.' : 'Scan to find controllers.';
        container.appendChild(hint); return;
      }
      devices.forEach(function(dev) {
        var card=document.createElement('div'); card.className='bt-device';
        var head=document.createElement('div'); head.className='bt-device-head';
        var name=document.createElement('strong'); name.textContent=dev.name || dev.addr; head.appendChild(name);
        if(paired) {
          var battery=document.createElement('span'); battery.className='bt-battery';
          var valid=Number.isFinite(dev.battery) && dev.battery>=0 && dev.battery<=100;
          battery.textContent=!dev.connected ? 'disconnect' : 'BATT '+(valid ? dev.battery : '--')+'%'+
            (valid && dev.charging ? ' CHG' : '');
          if(dev.connected && dev.charging) battery.classList.add('ok');
          head.appendChild(battery);
        }
        card.appendChild(head);
        var meta=document.createElement('div'); meta.className='bt-meta';
        meta.textContent=dev.addr+(paired ? (dev.connected ? ' · Sleep: '+(dev.sleep_disabled ? 'disabled' : 'checking…') : '') :
          Number.isFinite(dev.rssi) ? ' · '+dev.rssi+' dBm' : '');
        card.appendChild(meta);
        var actions=document.createElement('div'); actions.className='actions';
        button(actions,paired ? 'Connect' : 'Pair',paired ? 'connect' : 'pair',dev,!bt.enabled || !!dev.connected);
        if(paired) button(actions,'Delete','forget',dev,false);
        card.appendChild(actions); container.appendChild(card);
      });
    }
    function draw() {
      var bt=latest, toggle=find('bt_toggle'), scan=find('bt_scan');
      find('bt_status').textContent=bt ? bt.status || (bt.enabled ? 'Bluetooth on' : 'Bluetooth off') : 'Bluetooth unavailable';
      toggle.textContent=bt && bt.enabled ? 'Disable Bluetooth' : 'Enable Bluetooth';
      toggle.disabled=busy || !bt;
      scan.textContent=bt && bt.scanning ? 'Stop scanning' : 'Scan for controllers';
      scan.dataset.btAction=bt && bt.scanning ? 'stop_scan' : 'scan';
      scan.disabled=busy || !bt || !bt.enabled;
      if(!bt) return;
      var devices=Array.isArray(bt.devices) ? bt.devices : [];
      var addresses=new Set(devices.map(function(d) { return d.addr.toLowerCase(); }));
      var discovered=(bt.enabled && Array.isArray(bt.discovered) ? bt.discovered : []).filter(function(d) {
        return !addresses.has(d.addr.toLowerCase());
      });
      var next=JSON.stringify([bt.enabled,bt.scanning,devices,discovered]);
      if(!busy && next!==signature) {
        list('bt_paired',devices,true,bt); list('bt_discovered',discovered,false,bt); signature=next;
      }
      root.querySelectorAll('.bt-device button').forEach(function(b) {
        b.disabled=busy || (b.dataset.btAction!=='forget' && (!bt.enabled ||
          devices.some(function(d) { return d.addr===b.dataset.addr && d.connected; })));
      });
    }
    function render(bt) {
      if(bt && latest && Number.isFinite(bt.revision) && Number.isFinite(latest.revision) &&
         bt.uptime_s>=latest.uptime_s && bt.revision<latest.revision) return;
      latest=bt || null; draw();
    }
    root.addEventListener('click',function(event) {
      var b=event.target.closest('button[data-bt-action]');
      if(!b || b.disabled || busy || !latest) return;
      var body={action:b.dataset.btAction};
      if(body.action==='enable') body.enabled=!latest.enabled;
      if(b.dataset.addr) body.addr=b.dataset.addr;
      busy=true; draw();
      api('/api/bluetooth',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
        .then(JSON.parse).then(function(bt) { render(bt); notify(bt.message || 'Bluetooth updated'); })
        .catch(function(e) { notify(e.message,true); })
        .finally(function() { busy=false; draw(); });
    });
    return {render:render};
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
    usb:usb,power:power,api:api,json:json,key:key,state:state,poll:poll,bluetoothPanel:bluetoothPanel};
}());
