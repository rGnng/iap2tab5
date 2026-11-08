'use strict';
var T=window.Tab5, $=function(id) { return document.getElementById(id); };
var held={knob:false,topright:false}, gestureBusy=false, source, lastId=0, lines=[], paused=false, uptime=0;
function toast(msg,bad) {
  $('toast').textContent=msg; $('toast').className='on'+(bad ? ' bad' : '');
  clearTimeout(toast.timer); toast.timer=setTimeout(function() { $('toast').className=''; },4000);
}
function buttons(id,items) {
  items.forEach(function(item) { var b=document.createElement('button'); b.textContent=item[0]; b.dataset.key=item[1]; $(id).appendChild(b); });
}
buttons('pages',[['Now Playing','nowplaying'],['Albums','albums'],['Artists','artists'],['Songs','songs'],['Playlists','playlists'],['Settings','settings']]);
buttons('navigation',[['Up','up'],['Down','down'],['Enter','enter'],['Back','back'],['Tab −','tabprev'],['Tab +','tabnext'],['Context','context'],['Up Next','queue']]);
buttons('media',[['Previous','prev'],['Play / Pause','play'],['Next','next'],['Star','star'],['Shuffle','shuffle'],['Repeat','repeat'],['Volume −','voldown'],['Volume +','volup']]);
document.querySelectorAll('[data-key]').forEach(function(b) {
  b.addEventListener('click',function() { T.key(b.dataset.key).then(function() { setTimeout(refresh,300); }).catch(function(e) { toast(e.message,true); }); });
});
function holdLabels() {
  Object.keys(held).forEach(function(k) {
    var b=$(k+'_hold'); b.classList.toggle('active',held[k]); b.setAttribute('aria-pressed',held[k]);
    b.textContent=(k==='knob' ? 'Knob' : 'Top-right')+(held[k] ? ' up' : ' down');
  });
}
async function toggleHold(k) {
  if(gestureBusy) return; gestureBusy=true;
  try { await T.state(k,!held[k]); held[k]=!held[k]; holdLabels(); }
  catch(e) { toast(e.message,true); } finally { gestureBusy=false; }
}
function gesture(k) {
  if(!held.knob) { toast('Press Knob down first',true); return; }
  T.state(k,true).catch(function(e) { toast(e.message,true); });
}
function cancelGestures() {
  return T.state('cancel',false).then(function() { held.knob=held.topright=false; holdLabels(); }).catch(function(e) { toast(e.message,true); });
}
function leave() {
  if(source) source.close();
  if(held.knob || held.topright) fetch('/api/keystate?k=cancel&down=0',{method:'POST',keepalive:true}).catch(function() {});
  held.knob=held.topright=false; holdLabels();
}
window.addEventListener('pagehide',leave);
document.addEventListener('visibilitychange',function() {
  if(document.hidden && (held.knob || held.topright)) cancelGestures();
});
async function captureScreen() {
  $('capture').disabled=true;
  try {
    var r=await fetch('/api/screenshot.bmp',{cache:'no-store'}); if(!r.ok) throw new Error('Capture failed: '+r.status);
    var url=URL.createObjectURL(await r.blob()), old=$('preview').src;
    $('preview').src=url; $('preview').hidden=false; $('preview_hint').hidden=true;
    if(old.startsWith('blob:')) URL.revokeObjectURL(old);
  } catch(e) { toast(e.message,true); } finally { $('capture').disabled=false; }
}
function renderLog() {
  var filter=$('log_filter').value.toLowerCase();
  $('live_log').textContent=lines.filter(function(s) { return !filter || s.toLowerCase().includes(filter); }).join('\n');
  if($('autoscroll').checked) $('live_log').scrollTop=$('live_log').scrollHeight;
}
$('log_filter').addEventListener('input',renderLog);
function connectLog() {
  if(source) source.close();
  source=new EventSource('/api/log/stream?since='+lastId);
  source.onopen=function() { $('stream_state').textContent='Live'; };
  source.onmessage=function(e) {
    var id=Number(e.lastEventId); if(id && id<=lastId) return;
    if(id) lastId=id;
    lines.push(e.data); if(lines.length>1800) lines.splice(0,lines.length-1800);
    if(!renderLog.timer) renderLog.timer=setTimeout(function() { renderLog.timer=null; renderLog(); },120);
  };
  source.onerror=function() { $('stream_state').textContent='Reconnecting…'; };
}
function toggleLog() {
  paused=!paused; $('log_pause').textContent=paused ? 'Resume' : 'Pause';
  if(paused) { source.close(); $('stream_state').textContent='Paused'; } else connectLog();
}
function clearLog() { lines=[]; renderLog(); }
function downloadLog() {
  var a=document.createElement('a'); a.href=URL.createObjectURL(new Blob([$('live_log').textContent],{type:'text/plain'}));
  a.download='tab5-debug.log'; a.click(); setTimeout(function() { URL.revokeObjectURL(a.href); },1000);
}
function setLevel() {
  T.api('/api/loglevel?tag='+encodeURIComponent($('tag').value)+'&level='+$('level').value,{method:'POST'})
    .then(function(t) { toast(t); }).catch(function(e) { toast(e.message,true); });
}
async function markFlicker() {
  $('mark_flicker').disabled=true;
  try {
    var d=JSON.parse(await T.api('/api/display/mark',{method:'POST'}));
    toast('Marked #'+d.marker+' at '+(d.marker_uptime_ms/1000).toFixed(1)+' s; saving trace');
    var a=document.createElement('a'); a.href='/api/display/trace'; a.download='tab5-display.csv'; a.click();
    refresh();
  } catch(e) { toast(e.message,true); }
  finally { $('mark_flicker').disabled=false; }
}
function render(s,c,p) {
  if(s.uptime_s<uptime) {
    lastId=0; lines.push('— Device restarted —'); if(!paused) connectLog();
  }
  uptime=s.uptime_s;
  $('chips').innerHTML=T.chip('Firmware',s.firmware,'')+T.chip('Device',s.iap2_identified ? 'Linked' : s.iap2_mode ? 'Waiting' : 'Library Sync',s.iap2_identified ? 'ok' : '');
  $('screen_state').textContent=s.screen_on===undefined ? '—' : s.screen_on ? 'On' : 'Off';
  var library=s.library_available===undefined ? !!(s.sd_ready && s.iap2_mode && s.iap2_identified) : s.library_available;
  ['albums','artists','songs','playlists'].forEach(function(k) {
    document.querySelector('[data-key="'+k+'"]').disabled=!library;
  });
  var d=s.display || {};
  $('display_rows').innerHTML=d.available ?
    T.row('Backlight PWM',d.pwm_duty+' / '+(d.pwm_period || 1024)+' · '+d.pwm_hz+' Hz')+
    T.row('Sampled PWM changes',d.pwm_duty_changes+' duty · '+d.pwm_freq_changes+' frequency')+
    T.row('VSYNC callbacks',d.vsync_gap_us ? (1000000/d.vsync_gap_us).toFixed(1)+' Hz' : 'Waiting')+
    T.row('VSYNC max gap',(d.vsync_max_gap_us/1000).toFixed(2)+' ms')+
    T.row('Gaps over 25 ms',d.vsync_gaps_over_25ms)+
    T.row('Render last / max',(d.render_last_us/1000).toFixed(2)+' / '+(d.render_max_us/1000).toFixed(2)+' ms')+
    T.row('DMA wait timeouts',d.dma_wait_timeouts)+T.row('Draw / rotation errors',d.draw_errors+' / '+d.rotation_errors)+
    T.row('Internal free / largest',T.bytes(d.internal_free)+' / '+T.bytes(d.internal_largest))+
    T.row('Last marker',d.marker ? '#'+d.marker+' · '+(d.marker_uptime_ms/1000).toFixed(1)+' s uptime' : 'None') : T.row('Recorder','Unavailable');
  var pwr=T.power(p,c);
  $('battery_metrics').innerHTML='<div class="metric"><span>Battery</span><strong class="'+pwr.cls+'">'+T.escape(pwr.state)+'</strong></div>'+ 
    '<div class="metric"><span>Voltage</span><strong>'+T.escape(pwr.voltage)+'</strong></div>';
  $('power_rows').innerHTML=T.row('Current flow',pwr.flow,pwr.cls)+T.row('Charger requested',c.charge_enabled ? 'Enabled' : 'Disabled')+
    T.row('Charger output',p.chg_en===undefined ? 'Unavailable' : pwr.enabled ? 'Enabled' : 'Disabled')+T.row('Charge current',c.qc_high ? 'Reduced' : 'Standard');
  $('power_raw').innerHTML=T.row('INA226',pwr.valid ? 'Available' : 'Unavailable')+T.row('Signed current',pwr.current)+
    T.row('LCD enable',p.lcd_enable_ok ? (p.lcd_enabled ? 'High' : 'Low') : 'Unavailable')+
    T.row('CHG_STAT',p.chg_stat)+T.row('nCHG_QC_EN',p.nchg_qc_en)+T.row('QC push-pull',p.qc_driven)+
    T.row('Expander output',T.hex(p.expander_output))+T.row('Direction',T.hex(p.expander_direction))+T.row('High-Z',T.hex(p.expander_highz));
  $('system_rows').innerHTML=T.row('Uptime',T.time(s.uptime_s))+T.row('Last reset',T.reset(s.reset_reason))+T.row('Free heap',T.bytes(s.free_heap))+
    T.row('Free PSRAM',T.bytes(s.free_psram))+T.row('Screen Off',c.screen_off_minutes ? c.screen_off_minutes+' min' : 'Off')+
    T.row('SD card',s.sd_ready ? T.bytes(s.sd_used)+' / '+T.bytes(s.sd_total) : 'Missing')+T.row('SD log',s.debug_log_sd_open ? 'Open' : 'Closed')+
    T.row('SD log lines',s.debug_log_sd_lines)+T.row('SD log errors',s.debug_log_sd_errors);
  $('connection_rows').innerHTML=T.row('USB',T.usb(s.usb_mode))+T.row('WiFi',s.wifi)+T.row('IP',s.ip)+T.row('iAP2',s.iap2)+T.row('USB mux',s.usbmux)+T.row('Lockdown',s.lockdown)+T.row('AFC',s.afc);
  var link=s.iap2_link || {};
  $('link_rows').innerHTML=Object.keys(link).map(function(k) { return T.row(k.replace(/_/g,' '),link[k]); }).join('') || T.row('Status','Unavailable');
}
var refresh=T.poll(render,function(e) { $('chips').innerHTML=T.chip('Device',e.message,'bad'); });
connectLog();
