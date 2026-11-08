'use strict';
var T = window.Tab5, S = {}, C = {}, dirty = {}, pending = {};
var $ = function (id) { return document.getElementById(id); };
var configFields = ['volume','brightness','screen_off_minutes','theme','sound_mode',
  'debug_log','iap2_mode','usb_mode','screen_flip','wifi_mode','ap_ssid'];
function toast(msg, bad) {
  $('toast').textContent = msg; $('toast').className = 'on' + (bad ? ' bad' : '');
  clearTimeout(toast.timer); toast.timer = setTimeout(function () { $('toast').className = ''; }, 4000);
}
function value(id) { var e = $(id); return e.type === 'checkbox' ? e.checked : e.value; }
function same(a,b) { return String(a) === String(b); }
function setVal(id,v) {
  if (pending[id] !== undefined) {
    if (!same(v,pending[id])) return;
    delete pending[id];
  }
  if (dirty[id] || document.activeElement === $(id)) return;
  if ($(id).type === 'checkbox') $(id).checked = !!v; else $(id).value = v;
}
function markConfigDirty(id) {
  if (id==='brightness' && +value(id)<=0) { $('brightness').value=5; $('bri_v').textContent=5; }
  dirty[id] = true;
}
configFields.concat(['charge_en','qc_high']).forEach(function (id) {
  $(id).addEventListener('input', function () { markConfigDirty(id); });
  $(id).addEventListener('change', function () { markConfigDirty(id); });
});
['volume','brightness'].forEach(function (id) {
  $(id).addEventListener('input', function () { $(id === 'volume' ? 'vol_v' : 'bri_v').textContent = value(id); });
});
function saved(values) {
  Object.keys(values).forEach(function (id) {
    if (same(value(id), values[id])) { delete dirty[id]; pending[id] = values[id]; }
  });
}
function key(k) {
  return T.key(k).then(function () { setTimeout(refresh,350); })
    .catch(function (e) { toast(e.message,true); });
}
function render(s,c,p) {
  S=s; C=c;
  $('chips').innerHTML = T.chip('Device',s.iap2_mode ? (s.iap2_identified ? 'Linked' : 'Waiting') : 'Library Sync',s.iap2_identified ? 'ok' : '') +
    T.chip('SD',s.sd_ready ? 'Ready' : 'Missing',s.sd_ready ? 'ok' : 'bad') + T.chip('WiFi',s.ip || '—','ok');
  $('np_title').textContent=s.title || 'No track'; $('np_artist').textContent=s.artist || '—'; $('np_album').textContent=s.album || '';
  $('play_state').textContent=['STOPPED','PLAYING','PAUSED'][s.play_status] || '—';
  $('btn_play').textContent=s.play_status===1 ? 'PAUSE' : 'PLAY';
  $('btn_star').textContent=s.starred ? '★' : '☆';
  [['btn_star',s.starred],['btn_shuffle',s.shuffle],['btn_repeat',s.repeat>0]].forEach(function (a) {
    $(a[0]).classList.toggle('active',!!a[1]); $(a[0]).setAttribute('aria-pressed',!!a[1]);
  });
  $('btn_repeat').textContent=s.repeat===2 ? 'REP 1' : 'REP';
  var cur=(s.elapsed_ms || 0)/1000, dur=(s.duration_ms || 0)/1000;
  $('np_cur').textContent=T.time(cur); $('np_dur').textContent=T.time(dur);
  $('np_bar').style.width=(dur ? Math.min(100,cur/dur*100) : 0)+'%';
  var battery=Number.isFinite(s.device_battery) && s.device_battery>=0 && s.device_battery<=100 ? s.device_battery+'%' : 'Unavailable';
  if (s.device_charging_state===2) battery+=' · Charged'; else if(s.device_charging) battery+=' · Charging';
  $('device_rows').innerHTML=T.row('Device',s.device_name)+T.row('iPod battery',battery)+T.row('Connection',s.iap2_mode ? s.iap2 : s.usbmux)+
    T.row('WiFi',s.wifi)+T.row('IP',s.ip)+T.row('SD card',s.sd_ready ? T.bytes(s.sd_used)+' / '+T.bytes(s.sd_total) : 'Missing')+
    T.row('Uptime',T.time(s.uptime_s))+T.row('Free heap',T.bytes(s.free_heap))+T.row('Free PSRAM',T.bytes(s.free_psram))+T.row('Last reset',T.reset(s.reset_reason));
  var power=T.power(p,c);
  $('charge_state').textContent=power.state; $('charge_state').className='micro '+power.cls;
  $('power_rows').innerHTML=T.row('Battery voltage',power.voltage)+T.row('Current flow',power.flow,power.cls)+
    T.row('Charger output',p.chg_en===undefined ? 'Unavailable' : power.enabled ? 'Enabled' : 'Disabled')+
    T.row('USB 5 V output',p.usb5v_en===undefined ? 'Unavailable' : p.usb5v_en ? 'On' : 'Off');
  setVal('charge_en',c.charge_enabled); setVal('qc_high',c.qc_high ? 1 : 0);
  var sync=s.afc_sync || {}, busy=!!sync.active || !!s.apple_pair_busy;
  $('btn_sync').disabled=s.iap2_mode || !s.apple_trusted || !s.sd_ready || busy;
  $('btn_trust').disabled=s.iap2_mode || busy;
  $('btn_mode').disabled=busy; $('btn_mode').textContent=s.iap2_mode ? 'Enter Library Sync' : 'Exit';
  $('library_rows').innerHTML=T.row('Mode',s.iap2_mode ? 'Player' : 'Library Sync')+T.row('Device',s.iap2_mode ? s.iap2 : s.lockdown);
  $('sync_hint').textContent=sync.active ? 'Syncing · '+(sync.cached || 0)+' covers cached · '+(sync.attempted || 0)+' / '+(sync.total || 0)+' checked' :
    s.iap2_mode ? 'Enter Library Sync to update the library and artwork. The device will restart.' : 'Sync updates the library and caches missing artwork.';
  $('apple_trust').textContent=s.iap2_mode || s.apple_trusted ? '' : s.apple_pair_status || 'Unlock the device and choose Pair / Trust.';
  $('fw_rows').innerHTML=T.row('Firmware',s.firmware)+T.row('Build',s.build)+T.row('OTA slot',s.ota_partition)+T.row('USB mode',T.usb(s.usb_mode));
  $('clock_hint').textContent=(s.local_time || '—')+(s.rtc ? ' · RTC' : '');
  $('screen_state').textContent=s.screen_on===undefined ? '—' : s.screen_on ? 'Screen on' : 'Screen off';
  configFields.forEach(function (id) { setVal(id,c[id] === undefined ? (id==='screen_off_minutes' ? 0 : '') : c[id]); });
  $('vol_v').textContent=value('volume'); $('bri_v').textContent=value('brightness');
}
var refresh=T.poll(render,function (e) { $('chips').innerHTML=T.chip('Device',e.message,'bad'); });
function postConfig(body) { return T.api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}); }
function saveConfig() {
  var body={}, snapshot={};
  configFields.forEach(function (id) {
    if (!dirty[id]) return;
    snapshot[id]=value(id); body[id]=$(id).type==='checkbox' ? value(id) : id==='ap_ssid' ? value(id) : +value(id);
  });
  if (!Object.keys(body).length) { toast('No changes'); return; }
  postConfig(body).then(function (t) { saved(snapshot); toast(t); refresh(); }).catch(function(e) { toast(e.message,true); });
}
function saveCharge() {
  var snapshot={charge_en:value('charge_en'),qc_high:value('qc_high')};
  T.api('/api/charge?en='+(snapshot.charge_en ? 1 : 0)+'&qc='+snapshot.qc_high,{method:'POST'})
    .then(function () { saved(snapshot); toast('Charging settings saved'); refresh(); }).catch(function(e) { toast(e.message,true); });
}
function syncTime() {
  T.api('/api/time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({epoch:Math.floor(Date.now()/1000)})})
    .then(function(t) { toast(t); refresh(); }).catch(function(e) { toast(e.message,true); });
}
function reboot() {
  T.api('/api/reboot',{method:'POST'}).then(function() { toast('Restarting…'); }).catch(function(e) { toast(e.message,true); });
}
function switchMode() { postConfig({iap2_mode:!S.iap2_mode}).then(reboot).catch(function(e) { toast(e.message,true); }); }
function sync_media() { T.api('/api/sync',{method:'POST'}).then(function(t) { toast(t); refresh(); }).catch(function(e) { toast(e.message,true); }); }
function pairApple() { T.api('/api/trust',{method:'POST'}).then(function(t) { toast(t); refresh(); }).catch(function(e) { toast(e.message,true); }); }
function upload(path,file,prog) {
  if (!file) { toast('Choose a file first',true); return; }
  var x=new XMLHttpRequest(); x.open('POST',path); x.setRequestHeader('Content-Type','application/octet-stream');
  prog.style.display='block'; prog.firstElementChild.style.width='0%';
  x.upload.onprogress=function(e) { if(e.lengthComputable) prog.firstElementChild.style.width=(e.loaded/e.total*100)+'%'; };
  x.onload=function() { toast(x.responseText || 'Upload complete',x.status<200 || x.status>=300); setTimeout(refresh,5000); };
  x.onerror=function() { toast('Upload failed',true); }; x.send(file);
}
function uploadLibrary() { upload('/api/library',$('lib_file').files[0],$('lib_prog')); }
function uploadFirmware() { upload('/api/ota',$('fw_file').files[0],$('fw_prog')); }
function uploadPair() {
  var f=$('pair_file').files[0]; if(!f) { toast('Choose a pair record first',true); return; }
  T.api('/api/pair',{method:'POST',body:f}).then(function(t) { toast(t); }).catch(function(e) { toast(e.message,true); });
}
async function loadLog() {
  var cursor=0,text='';
  try {
    for(var i=0;i<64;i++) {
      var r=await fetch('/api/log?since='+cursor,{cache:'no-store'});
      if(!r.ok) throw new Error('HTTP '+r.status);
      var chunk=await r.text(), next=Number(r.headers.get('X-Log-Seq'));
      text=(text+chunk).slice(-30000);
      if(!chunk || !next || next===cursor) break;
      cursor=next;
    }
    $('log').textContent=text || 'No log entries';
  } catch(e) { $('log').textContent=e.message; }
}
