#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Hauptseite: Zähleranzeige + Reset
// ---------------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Personenzähler</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f5f7;color:#222;margin:0;padding:16px}
.card{max-width:420px;margin:0 auto 16px;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.1)}
h1{font-size:1.2em;margin:0 0 12px}
.big{font-size:5em;font-weight:700;text-align:center;line-height:1.1}
.lbl{text-align:center;color:#666}
.row{display:flex;justify-content:space-around;margin-top:16px;text-align:center;color:#666}
.row b{display:block;font-size:1.6em;color:#222}
button{width:100%;padding:12px;font-size:1em;border:0;border-radius:8px;background:#d33;color:#fff;margin-top:16px;cursor:pointer}
.sens{display:flex;gap:8px}
.sens div{flex:1;padding:10px;border-radius:8px;background:#eee;text-align:center}
.sens .on{background:#2a2;color:#fff}
small{color:#888;line-height:1.6}
a{color:#06c}
</style></head><body>
<div class="card">
  <h1>Personenzähler</h1>
  <div class="big" id="inside">–</div>
  <div class="lbl">Personen im Raum</div>
  <div class="row">
    <div><b id="cin">–</b>Eintritte</div>
    <div><b id="cout">–</b>Austritte</div>
  </div>
  <button onclick="doReset()">Zähler zurücksetzen</button>
</div>
<div class="card">
  <h1>Sensoren</h1>
  <div class="sens"><div id="sa">A (aussen)<br>–</div><div id="sb">B (innen)<br>–</div></div>
  <p><small id="info"></small></p>
  <a href="/config">&#9881; Konfiguration</a> &nbsp;&middot;&nbsp; <a href="/log">&#128196; Log</a>
</div>
<script>
const $=id=>document.getElementById(id);
function fmt(d){return d<0?'frei':d+' mm';}
async function upd(){
  try{
    const d=await (await fetch('/api/status')).json();
    $('inside').textContent=d.inside;
    $('cin').textContent=d.in;
    $('cout').textContent=d.out;
    $('sa').innerHTML='A (aussen)<br>'+fmt(d.distA);
    $('sb').innerHTML='B (innen)<br>'+fmt(d.distB);
    $('sa').className=d.occA?'on':'';
    $('sb').className=d.occB?'on':'';
    $('info').innerHTML=(d.sensors?'':'<b style="color:#d33">Sensorfehler!</b><br>')+
      'Modus: '+d.mode+' &middot; IP: '+d.ip+'<br>MQTT: '+d.mqtt+' &middot; Laufzeit: '+Math.floor(d.uptime/60)+' min';
  }catch(e){}
}
function doReset(){
  if(confirm('Zähler wirklich zurücksetzen?'))
    fetch('/api/reset',{method:'POST'}).then(upd);
}
setInterval(upd,1000);upd();
</script>
</body></html>
)rawliteral";

// ---------------------------------------------------------------------------
// Konfigurationsseite: WLAN, MQTT, Sensor
// ---------------------------------------------------------------------------
const char CONFIG_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Konfiguration</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f5f7;color:#222;margin:0;padding:16px}
.card{max-width:420px;margin:0 auto 16px;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.1)}
h1{font-size:1.2em;margin:0 0 12px}
label{display:block;margin:10px 0 4px;font-size:.9em;color:#555}
input[type=text],input[type=password],input[type=number]{width:100%;box-sizing:border-box;padding:9px;border:1px solid #ccc;border-radius:6px;font-size:1em}
.chk{display:flex;align-items:center;gap:8px;margin-top:12px}
button{width:100%;padding:12px;font-size:1em;border:0;border-radius:8px;background:#06c;color:#fff;margin-top:16px;cursor:pointer}
button.sec{background:#888;padding:8px;margin-top:8px}
small{color:#888}
a{color:#06c}
</style></head><body>
<form method="POST" action="/config">
<div class="card">
  <h1>WLAN</h1>
  <label>SSID</label>
  <input type="text" name="ssid" list="nets" autocomplete="off">
  <datalist id="nets"></datalist>
  <button type="button" class="sec" id="scanbtn" onclick="scan()">Netzwerke suchen</button>
  <label>Passwort <small>(leer = unverändert)</small></label>
  <input type="password" name="pass">
  <label>Hostname</label>
  <input type="text" name="hostname">
</div>
<div class="card">
  <h1>MQTT</h1>
  <label>Broker <small>(leer = MQTT deaktiviert)</small></label>
  <input type="text" name="mqtt_host">
  <label>Port</label>
  <input type="number" name="mqtt_port" min="1" max="65535">
  <label>Benutzer</label>
  <input type="text" name="mqtt_user">
  <label>Passwort <small>(leer = unverändert)</small></label>
  <input type="password" name="mqtt_pass">
  <label>Basis-Topic</label>
  <input type="text" name="mqtt_topic">
</div>
<div class="card">
  <h1>Sensor</h1>
  <label>Auslöseschwelle in mm <small>(Objekt näher als dieser Wert = belegt)</small></label>
  <input type="number" name="threshold" min="50" max="4000">
  <div class="chk"><input type="checkbox" name="invert" id="invert"><label for="invert" style="margin:0">Richtung umkehren (A = innen, B = aussen)</label></div>
  <div class="chk"><input type="checkbox" name="long_range" id="long_range"><label for="long_range" style="margin:0">Grosse Reichweite (bis ~2.9 m statt ~1.3 m, aber langsamer: 30 statt 50 Messungen/s)</label></div>
  <button type="submit">Speichern &amp; Neustart</button>
</div>
</form>
<div class="card"><a href="/">&larr; Zurück zum Zähler</a></div>
<script>
fetch('/api/config').then(r=>r.json()).then(c=>{
  for(const k in c){
    const e=document.getElementsByName(k)[0];
    if(!e)continue;
    if(e.type=='checkbox')e.checked=!!c[k];else e.value=c[k];
  }
});
function scan(){
  const b=document.getElementById('scanbtn');
  b.textContent='Suche läuft…';
  fetch('/api/scan').then(r=>r.json()).then(list=>{
    const d=document.getElementById('nets');d.innerHTML='';
    list.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.label=n.ssid+' ('+n.rssi+' dBm)';d.appendChild(o);});
    b.textContent=list.length+' Netzwerke gefunden – ins SSID-Feld klicken';
  }).catch(()=>b.textContent='Suche fehlgeschlagen');
}
</script>
</body></html>
)rawliteral";

// ---------------------------------------------------------------------------
// Log-Seite: Konsolenausgabe live im Browser
// ---------------------------------------------------------------------------
const char LOG_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Log</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f5f7;color:#222;margin:0;padding:16px}
.card{max-width:900px;margin:0 auto 16px;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.1)}
h1{font-size:1.2em;margin:0 0 12px}
pre{background:#111;color:#0d0;padding:12px;border-radius:8px;height:60vh;overflow:auto;font-size:.85em;margin:0;white-space:pre-wrap}
.bar{display:flex;flex-wrap:wrap;gap:16px;align-items:center;margin-bottom:12px}
button{padding:6px 14px;border:0;border-radius:6px;background:#888;color:#fff;cursor:pointer}
a{color:#06c}
</style></head><body>
<div class="card">
  <h1>Konsole</h1>
  <div class="bar">
    <label><input type="checkbox" id="dbg" onchange="setDebug()"> Debugmodus (Sensor-Details)</label>
    <label><input type="checkbox" id="pause"> Pause</label>
    <button onclick="document.getElementById('log').textContent=''">Leeren</button>
  </div>
  <pre id="log"></pre>
</div>
<div class="card"><a href="/">&larr; Zurück zum Zähler</a></div>
<script>
let seq=0;
const log=document.getElementById('log');
async function poll(){
  if(document.getElementById('pause').checked)return;
  try{
    const d=await (await fetch('/api/log?since='+seq)).json();
    if(d.seq<seq)log.textContent+='--- Gerät neu gestartet ---\n';
    seq=d.seq;
    document.getElementById('dbg').checked=d.debug;
    if(d.lines.length){
      const atBottom=log.scrollTop+log.clientHeight>=log.scrollHeight-20;
      log.textContent+=d.lines.join('\n')+'\n';
      if(atBottom)log.scrollTop=log.scrollHeight;
    }
  }catch(e){}
}
function setDebug(){
  fetch('/api/debug?on='+(document.getElementById('dbg').checked?1:0),{method:'POST'});
}
setInterval(poll,1000);poll();
</script>
</body></html>
)rawliteral";

const char SAVED_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="15;url=/">
<title>Gespeichert</title>
<style>body{font-family:system-ui,sans-serif;background:#f4f5f7;padding:16px}
.card{max-width:420px;margin:0 auto;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.1)}</style>
</head><body><div class="card">
<h2>Gespeichert</h2>
<p>Das Gerät startet neu und verbindet sich mit dem konfigurierten WLAN.</p>
<p>Danach ist es unter der neuen IP-Adresse bzw. unter <b>http://&lt;hostname&gt;.local</b> erreichbar.
Schlägt die Verbindung fehl, öffnet es wieder den Access Point.</p>
</div></body></html>
)rawliteral";
