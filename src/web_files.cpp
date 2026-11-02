#include "web_files.h"
#include "tape_sd.h"
#include "wifi_settings.h"
#include "update_sd.h"

#include <circle/sched/scheduler.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/httpdaemon.h>
#include <circle/net/mdnsdaemon.h>
#include <circle/net/ipaddress.h>
#include <circle/netdevice.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>
#include <fatfs/ff.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <stdarg.h>
#include <string.h>

#define HOSTNAME            "xace"          // http://xace.local
#define MAX_CONTENT         (8 * 1024 * 1024)   // largest reply: a file or a .zip
#define MAX_UPLOAD          (32 * 1024 * 1024)  // largest upload, all files together
#define TIMEOUT_SECONDS     20
#define MAX_FILES           1024
#define NO_ADAPTER_MICROS   6000000         // no Ethernet adapter after this long: give up
#define JOIN_TROUBLE_MICROS 30000000        // not joined the Wi-Fi after this long: say so
#define WIFI_FIRMWARE_PATH  "SD:/firmware/"

// Added to hostap's wpa_supplicant/main_circle.cpp for xAce-baremetal
int wpa_supplicant_is_running(void);
void wpa_supplicant_reload(void);

static const char FromWeb[] = "web";

static CScheduler    *s_pScheduler = nullptr;
static CNetSubSystem *s_pNet = nullptr;
static CmDNSDaemon   *s_pmDNS = nullptr;
static boolean        s_bStarted = FALSE;
static boolean        s_bFailed = FALSE;
static u64            s_nStartMicros = 0;
static TWebNetwork    s_Network = WebCable;
static CBcm4343Device *s_pWLAN = nullptr;
static CWPASupplicant *s_pWPASupplicant = nullptr;
static TWebState      s_WiFiFailure = WebNotStarted;
static u64            s_nRunMicros = 0;         // time spent in WebFilesRun()
static u64            s_nJoinStartMicros = 0;   // s_nRunMicros when (re)joining began
static boolean        s_bHadAddress = FALSE;

static char     s_Activity[WEB_ACTIVITY_LINES][WEB_ACTIVITY_WIDTH + 1];
static unsigned s_nActivity = 0;

static void activity(const char *pFormat, ...)
{
    va_list Args;
    va_start(Args, pFormat);
    CString Line;
    Line.FormatV(pFormat, Args);
    va_end(Args);

    CLogger::Get()->Write(FromWeb, LogNotice, "%s", (const char *) Line);

    char *pLine = s_Activity[s_nActivity % WEB_ACTIVITY_LINES];
    strncpy(pLine, Line, WEB_ACTIVITY_WIDTH);
    pLine[WEB_ACTIVITY_WIDTH] = '\0';
    s_nActivity++;
}

unsigned WebFilesActivityCount(void)
{
    return s_nActivity;
}

const char *WebFilesActivityLine(unsigned nIndex)
{
    unsigned nFirst = s_nActivity > WEB_ACTIVITY_LINES ? s_nActivity - WEB_ACTIVITY_LINES : 0;
    if (nFirst + nIndex >= s_nActivity) {
        return "";
    }
    return s_Activity[(nFirst + nIndex) % WEB_ACTIVITY_LINES];
}

// ---------------------------------------------------------------------------
// The page

static const char s_Page[] = R"PAGE(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jupiter Ace files</title>
<style>
:root{--bg:#fff;--fg:#111;--mut:#666;--line:#ddd;--acc:#1a5fb4;--err:#b00020;--hi:#eef4fc}
@media (prefers-color-scheme:dark){:root{--bg:#121212;--fg:#eee;--mut:#999;--line:#333;--acc:#78aeed;--err:#ff6b81;--hi:#1c2733}}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0 auto;max-width:860px;padding:16px}
h1{font-size:22px;margin:0 0 4px}#tape{color:var(--mut);margin:0 0 12px}
#crumbs{font-size:17px;margin:0 0 12px;overflow-wrap:anywhere}#crumbs a{text-decoration:none}
#drop{border:2px dashed var(--line);border-radius:8px;padding:16px;margin-bottom:12px}
#drop.over{border-color:var(--acc);background:var(--hi)}#drop p{margin:0 0 8px}
#drop .pick{display:inline-block;margin:0 16px 4px 0}#drop label{display:block;margin-top:8px}
#msg{white-space:pre-line;margin:8px 0;min-height:1.4em;max-height:14em;overflow:auto}#msg.err{color:var(--err)}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}#count{color:var(--mut)}
table{width:100%;border-collapse:collapse}td,th{padding:6px 4px;border-bottom:1px solid var(--line);text-align:left}
th{color:var(--mut);font-weight:normal}.r{text-align:right;white-space:nowrap}a{color:var(--acc)}
td:nth-child(2){overflow-wrap:anywhere}.dir a{font-weight:600}
@media (max-width:560px){td:nth-child(3),th:nth-child(3){display:none}}button{font:inherit;padding:3px 10px}progress{width:100%}
</style></head><body>
<h1>Jupiter Ace files</h1>
<p id="tape"></p>
<div id="crumbs"></div>
<section id="drop">
<p>Drop <b>.tap .tzx .ace .fth .fs .f .zip</b> files or whole folders here to put them in this folder, or choose them:</p>
<span class="pick">Files: <input type="file" id="pick" multiple accept=".tap,.tzx,.ace,.fth,.fs,.f,.zip"></span>
<span class="pick">A folder: <input type="file" id="pickdir" webkitdirectory></span>
<label><input type="checkbox" id="replace"> Replace files with the same name</label>
<progress id="prog" max="1" hidden></progress>
</section>
<div id="msg"></div>
<div class="bar"><label><input type="checkbox" id="all"> All</label>
<button id="mkdir">New folder</button><button id="move">Move selected</button>
<button id="zip">Download selected (.zip)</button><button id="del">Delete selected</button>
<button id="ref">Refresh</button><span id="count"></span></div>
<table><thead><tr><th></th><th>Name</th><th>Type</th><th class="r">Size</th><th></th></tr></thead>
<tbody id="list"></tbody></table>
<p style="margin-top:24px"><a href="/system">System update</a></p>
<script>
const $=i=>document.getElementById(i);let items=[],dir=hashDir(),note='';
const ACE=/\.(tap|tzx|ace|fth|fs|f|zip)$/i,BATCH=30*1024*1024;
function hashDir(){try{return decodeURIComponent(location.hash.slice(1));}catch(e){return '';}}
function join(a,b){return a?a+'/'+b:b;}
function msg(t,e){const m=$('msg');m.textContent=t;m.className=e?'err':'';}
function size(n){return n<1024?n+' B':(n/1024).toFixed(n<10240?1:0)+' KB';}
function go(d){location.hash=encodeURIComponent(d);}
window.onhashchange=()=>{dir=hashDir();msg(note,!!note);note='';load();};
async function load(){try{const r=await fetch('/api/list?dir='+encodeURIComponent(dir));
if(r.status==404&&dir){note='Folder '+dir+' not found';go('');return;}const d=await r.json();
items=d.folders.map(n=>({n,p:join(dir,n),f:1})).concat(d.files.map(f=>({n:f.n,p:join(dir,f.n),s:f.s,t:f.t})));
$('tape').textContent=d.tape?'Tape in the player: '+d.tape:'No tape in the player';render();}
catch(e){msg('The Ace is not answering: press F11 on the Ace to open the file exchange, then Refresh.',1);}}
function crumbs(){const c=$('crumbs');c.textContent='';const parts=dir?dir.split('/'):[];
const a=document.createElement('a');a.textContent='programs';a.href='#';c.appendChild(a);
parts.forEach((p,i)=>{c.appendChild(document.createTextNode(' / '));const l=document.createElement('a');l.textContent=p;
l.href='#'+encodeURIComponent(parts.slice(0,i+1).join('/'));c.appendChild(l);});}
function render(){crumbs();const tb=$('list');tb.innerHTML='';
if(dir){const tr=document.createElement('tr');tr.className='dir';tr.innerHTML='<td></td><td><a href="#">.. (up)</a></td><td></td><td></td><td></td>';
tr.querySelector('a').href='#'+encodeURIComponent(dir.split('/').slice(0,-1).join('/'));tb.appendChild(tr);}
for(const it of items){const tr=document.createElement('tr');if(it.f)tr.className='dir';
tr.innerHTML='<td><input type="checkbox"></td><td><a></a></td><td></td><td class="r"></td><td class="r"><button>Rename</button> <button>Delete</button></td>';
const c=tr.querySelector('input');c.dataset.i=items.indexOf(it);c.onchange=upd;const a=tr.querySelector('a');
if(it.f){a.textContent=it.n+'/';a.href='#'+encodeURIComponent(it.p);tr.children[2].textContent='folder';}
else{a.textContent=it.n;a.href='/file?p='+encodeURIComponent(it.p);a.download=it.n;tr.children[2].textContent=it.t;tr.children[3].textContent=size(it.s);}
const b=tr.querySelectorAll('button');b[0].onclick=()=>ren(it);b[1].onclick=()=>del([it]);tb.appendChild(tr);}
if(!items.length){const tr=document.createElement('tr');tr.innerHTML='<td></td><td colspan="4">Empty folder</td>';tb.appendChild(tr);}
$('all').checked=false;upd();}
function sel(){return[...document.querySelectorAll('#list input:checked')].map(c=>items[c.dataset.i]);}
function upd(){const s=sel(),nd=items.filter(i=>i.f).length;
$('count').textContent=(nd?nd+' folders, ':'')+(items.length-nd)+' files'+(s.length?', '+s.length+' selected':'');
$('zip').disabled=$('del').disabled=$('move').disabled=!s.length;}
$('all').onchange=e=>{document.querySelectorAll('#list input').forEach(c=>c.checked=e.target.checked);upd();};
async function post(u,fd){const r=await fetch(u,{method:'POST',body:fd});if(!r.ok)throw new Error(r.status+' '+r.statusText);return r;}
async function del(l){const nd=l.filter(i=>i.f).length;let q;
if(l.length==1)q=l[0].f?'Delete the folder '+l[0].n+' and everything in it (files and subfolders)?':'Delete '+l[0].n+'?';
else q='Delete '+l.length+' items'+(nd?', including '+nd+' folder'+(nd>1?'s':'')+' with everything in them':'')+'?';
if(!confirm(q+'\nThis cannot be undone.'))return;
const fd=new FormData();l.forEach(i=>fd.append('name',i.p));try{msg(await(await post('/api/delete',fd)).text());}
catch(e){msg('Delete failed: '+e.message,1);}load();}
async function ren(it){const t=prompt('New name for '+it.n,it.n);if(!t||t==it.n)return;
if(t.includes('/')){msg('A name cannot have a / (use Move selected to move it)',1);return;}
const fd=new FormData();fd.append('from',it.p);fd.append('to',join(dir,t));
try{msg(await(await post('/api/rename',fd)).text());}catch(e){msg('Rename failed: '+e.message,1);}load();}
$('mkdir').onclick=async()=>{const t=prompt('Name of the new folder in '+(dir?'programs/'+dir:'programs'));if(!t)return;
const fd=new FormData();fd.append('dir',dir);fd.append('name',t);
try{msg(await(await post('/api/mkdir',fd)).text());}catch(e){msg('New folder failed: '+e.message,1);}load();};
$('move').onclick=async()=>{const l=sel();const t=prompt('Move '+(l.length==1?l[0].n:l.length+' items')+' to the folder (its path in programs, e.g. games/arcade; empty for programs itself):',dir);
if(t===null)return;const to=t.replace(/^\/+|\/+$/g,'');if(to==dir)return;
const fd=new FormData();fd.append('to',to);l.forEach(i=>fd.append('name',i.p));
try{msg(await(await post('/api/move',fd)).text());}catch(e){msg('Move failed: '+e.message,1);}load();};
$('zip').onclick=async()=>{const l=sel();const fd=new FormData();fd.append('dir',dir);l.forEach(i=>fd.append('name',i.p));
const zn=(l.length==1&&l[0].f?l[0].n:'jupiter-ace')+'.zip';
try{const b=await(await post('/api/zip',fd)).blob();const a=document.createElement('a');a.href=URL.createObjectURL(b);
a.download=zn;document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),10000);
msg('Downloaded '+zn);}catch(e){msg('Download failed: '+(e.message.startsWith('413')?'too much at once (8 MB or 1024 files at most)':e.message),1);}};
$('del').onclick=()=>del(sel());$('ref').onclick=()=>{msg('');load();};
function send(batch,done,total){return new Promise(res=>{const fd=new FormData();fd.append('dir',dir);
fd.append('replace',$('replace').checked?'1':'0');batch.forEach(u=>{fd.append('path',u.p);fd.append('file',u.f,u.f.name);});
const x=new XMLHttpRequest(),p=$('prog');x.upload.onprogress=e=>{if(e.lengthComputable)p.value=(done+e.loaded*batch.size/e.total)/total;};
x.onload=()=>res(x.status==200?x.responseText:'Upload failed: '+x.status+'\n');
x.onerror=()=>res('Upload failed: the Ace is not answering (is F11 open?)\n');x.open('POST','/api/upload');x.send(fd);});}
async function upload(l){let skipped=0,big=[];const ok=[];
for(const u of l){if(u.p.split('/').some(n=>!n||n[0]=='.')||!ACE.test(u.p)){skipped++;continue;}if(u.f.size>BATCH){big.push(u.p);continue;}ok.push(u);}
let out='';if(skipped)out+=skipped+' file'+(skipped>1?'s':'')+' left out: not .tap, .tzx, .ace, .fth, .fs, .f or .zip\n';
big.forEach(p=>out+=p+': too big (30 MB at most)\n');
if(ok.length){const p=$('prog');p.hidden=false;p.value=0;const total=ok.reduce((t,u)=>t+u.f.size,0)||1;let done=0;
for(let i=0;i<ok.length;){const b=[];b.size=0;while(i<ok.length&&(b.size+ok[i].f.size<=BATCH||!b.length)){b.push(ok[i]);b.size+=ok[i].f.size;i++;}
out+=await send(b,done,total);done+=b.size;}p.hidden=true;}
msg(out||'Nothing to upload',/failed|not saved/.test(out));load();}
$('pick').onchange=e=>{upload([...e.target.files].map(f=>({f,p:f.name})));e.target.value='';};
$('pickdir').onchange=e=>{upload([...e.target.files].map(f=>({f,p:f.webkitRelativePath||f.name})));e.target.value='';};
function readAll(r){return new Promise(res=>{const all=[];const next=()=>r.readEntries(es=>{if(!es.length)res(all);else{all.push(...es);next();}},()=>res(all));next();});}
async function walk(entry,out){if(entry.isFile){await new Promise(res=>entry.file(f=>{out.push({f,p:entry.fullPath.replace(/^\/+/,'')});res();},res));}
else if(entry.isDirectory){for(const e of await readAll(entry.createReader()))await walk(e,out);}}
const d=$('drop');['dragenter','dragover'].forEach(t=>d.addEventListener(t,e=>{e.preventDefault();d.classList.add('over');}));
['dragleave','drop'].forEach(t=>d.addEventListener(t,e=>{e.preventDefault();d.classList.remove('over');}));
d.addEventListener('drop',async e=>{const its=e.dataTransfer.items;
if(its&&its.length&&its[0].webkitGetAsEntry){const es=[...its].map(i=>i.webkitGetAsEntry()).filter(x=>x);const out=[];
msg('Reading the dropped files...');for(const en of es)await walk(en,out);upload(out);}
else upload([...e.dataTransfer.files].map(f=>({f,p:f.name})));});load();
</script></body></html>
)PAGE";

// The system update page (update_sd.h)
static const char s_SystemPage[] = R"PAGE(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jupiter Ace system update</title>
<style>
:root{--bg:#fff;--fg:#111;--mut:#666;--line:#ddd;--acc:#1a5fb4;--err:#b00020;--warn:#8a5a00;--hi:#eef4fc}
@media (prefers-color-scheme:dark){:root{--bg:#121212;--fg:#eee;--mut:#999;--line:#333;--acc:#78aeed;--err:#ff6b81;--warn:#e0b050;--hi:#1c2733}}
body{background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif;margin:0 auto;max-width:860px;padding:16px}
h1{font-size:22px;margin:0 0 4px}a{color:var(--acc)}.mut{color:var(--mut)}
section{border:1px solid var(--line);border-radius:8px;padding:16px;margin:12px 0}
#warn{color:var(--warn)}#msg{white-space:pre-line;margin:8px 0;max-height:20em;overflow:auto}#msg.err{color:var(--err)}
#state{font-weight:600}button{font:inherit;padding:3px 10px}progress{width:100%}
</style></head><body>
<h1>System update</h1>
<p><a href="/">Back to the files</a></p>
<section><div id="now" class="mut">...</div></section>
<section>
<p>Choose a release of xAce-baremetal for this Raspberry Pi: the <b>.zip</b> for it from the releases
(<span id="zipname">xAce-baremetal-&lt;version&gt;-pi&lt;model&gt;.zip</span>). Only the files that differ from those on the card are
changed (kernel, boot firmware, Wi-Fi firmware); config.txt, cmdline.txt, wpa_supplicant.conf and your programs are kept.</p>
<p id="warn"></p>
<input type="file" id="pick" accept=".zip"> <progress id="prog" max="1" hidden></progress>
</section>
<p id="state"></p><div id="msg"></div>
<p><button id="cancel" hidden>Discard the update</button></p>
<script>
const $=i=>document.getElementById(i);let st=null,timer=0;
function msg(t,e){const m=$('msg');m.textContent=t;m.className=e?'err':'';}
async function status(){try{const r=await fetch('/api/system');st=await r.json();
$('now').textContent='This is xAce-baremetal '+st.version+' for the Raspberry Pi '+st.model+' ('+st.kernel+'), built '+st.build+'.';
$('zipname').textContent='xAce-baremetal-<version>-pi'+st.model+'.zip';
$('warn').textContent=st.tryboot?'The Pi restarts to try the new version first: if it does not start, switching the Pi off and on brings this version back.'
:'Warning: this Pi cannot try a new version first. After installing, switch the Pi off and on. If it then does not start, copy the files in the card\'s update-backup folder back to its root on a computer.';
if(st.pending){$('state').textContent='Version '+st.pending.version+' (built '+st.pending.build+', '+st.pending.files+' files to change) is waiting: press Y on the Ace (F11 screen) to install it.';$('cancel').hidden=false;}
else{$('state').textContent=st.last;$('cancel').hidden=true;}}
catch(e){$('state').textContent='The Ace is not answering (it may be restarting): press F11 on the Ace, then reload this page.';}}
function poll(){clearInterval(timer);timer=setInterval(status,2000);}
$('pick').onchange=e=>{const f=e.target.files[0];e.target.value='';if(!f)return;
const fd=new FormData();fd.append('file',f,f.name);const x=new XMLHttpRequest(),p=$('prog');p.hidden=false;p.value=0;msg('Sending '+f.name+'...');
x.upload.onprogress=ev=>{if(ev.lengthComputable)p.value=ev.loaded/ev.total;};
x.onload=()=>{p.hidden=true;msg(x.status==200?x.responseText:'Upload failed: '+x.status,x.status!=200||!/install it/.test(x.responseText));status();poll();};
x.onerror=()=>{p.hidden=true;msg('Upload failed: the Ace is not answering (is F11 open?)',1);};x.open('POST','/api/system/upload');x.send(fd);};
$('cancel').onclick=async()=>{await fetch('/api/system/cancel',{method:'POST'});msg('');status();};
status();poll();
</script></body></html>
)PAGE";

// ---------------------------------------------------------------------------
// Helpers

// The value of attribute pAttr ("name" or "filename") in a multipart part header
static boolean part_attribute(const char *pHeader, const char *pAttr, char *pValue, unsigned nSize)
{
    CString Key;
    Key.Format("; %s=\"", pAttr);
    const char *p = strstr(pHeader, Key);
    if (p == nullptr) {
        return FALSE;
    }
    p += Key.GetLength();
    unsigned n = 0;
    while (*p != '\0' && *p != '"' && *p != '\r' && *p != '\n') {
        if (n + 1 < nSize) {
            pValue[n++] = *p;
        }
        p++;
    }
    pValue[n] = '\0';
    return TRUE;
}

// A short text part's value (names in the delete / rename / zip forms)
static void part_text(const u8 *pData, unsigned nLength, char *pValue, unsigned nSize)
{
    unsigned n = nLength < nSize - 1 ? nLength : nSize - 1;
    memcpy(pValue, pData, n);
    pValue[n] = '\0';
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX decoding of a URL path
static void url_decode(const char *pIn, char *pOut, unsigned nSize)
{
    unsigned n = 0;
    while (*pIn != '\0' && n + 1 < nSize) {
        int h, l;
        if (pIn[0] == '%' && (h = hex_digit(pIn[1])) >= 0 && (l = hex_digit(pIn[2])) >= 0) {
            pOut[n++] = (char) (h * 16 + l);
            pIn += 3;
        } else {
            pOut[n++] = *pIn++;
        }
    }
    pOut[n] = '\0';
}

// The value of pKey in URL parameters "a=1&b=2", decoded ("" if missing)
static void url_param(const char *pParams, const char *pKey, char *pValue, unsigned nSize)
{
    pValue[0] = '\0';
    unsigned nKey = strlen(pKey);
    for (const char *p = pParams; p != nullptr && *p != '\0'; ) {
        const char *pEnd = strchr(p, '&');
        unsigned nLen = pEnd != nullptr ? (unsigned) (pEnd - p) : strlen(p);
        if (nLen > nKey && strncmp(p, pKey, nKey) == 0 && p[nKey] == '=') {
            char Raw[3 * TAPE_PATH_SIZE];
            unsigned n = nLen - nKey - 1 < sizeof(Raw) - 1 ? nLen - nKey - 1 : sizeof(Raw) - 1;
            memcpy(Raw, p + nKey + 1, n);
            Raw[n] = '\0';
            for (char *q = Raw; *q != '\0'; q++) {
                if (*q == '+') {
                    *q = ' ';
                }
            }
            url_decode(Raw, pValue, nSize);
            return;
        }
        p = pEnd != nullptr ? pEnd + 1 : nullptr;
    }
}

// Plain ASCII: names the page can handle
static boolean is_ascii(const char *p)
{
    for (; *p != '\0'; p++) {
        if ((unsigned char) *p >= 0x7F) {
            return FALSE;
        }
    }
    return TRUE;
}

static void json_string(CString *pOut, const char *p)
{
    pOut->Append("\"");
    for (; *p != '\0'; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') {
            char s[3] = {'\\', (char) c, '\0'};
            pOut->Append(s);
        } else if (c < 0x20 || c >= 0x7F) {
            CString s;
            s.Format("\\u%04x", c);
            pOut->Append(s);
        } else {
            char s[2] = {(char) c, '\0'};
            pOut->Append(s);
        }
    }
    pOut->Append("\"");
}

static const char *result_text(TTapeFileResult Result)
{
    switch (Result) {
    case TapeFileOK:            return "OK";
    case TapeFileNotFound:      return "not found";
    case TapeFileInvalidName:   return "not a .tap, .tzx, .ace, .fth, .fs, .f or .zip file with plain names";
    case TapeFileReadOnly:      return "files in a .zip cannot be changed";
    case TapeFileZipUnsupported: return "the .zip cannot be read";
    case TapeFileNoCard:        return "no SD card";
    case TapeFileExists:        return "a file with that name already exists";
    case TapeFileTooBig:        return "too big";
    default:                    return "could not be written";
    }
}

static u32 crc32(const u8 *pData, unsigned nLength)
{
    static u32 Table[256];
    static boolean bTable = FALSE;
    if (!bTable) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            }
            Table[i] = c;
        }
        bTable = TRUE;
    }
    u32 c = 0xFFFFFFFF;
    for (unsigned i = 0; i < nLength; i++) {
        c = Table[(c ^ pData[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFF;
}

static void put16(u8 *p, unsigned v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

// ---------------------------------------------------------------------------
// The web server

class CWebFiles : public CHTTPDaemon
{
public:
    CWebFiles(CNetSubSystem *pNet, CSocket *pSocket = nullptr)
    :   CHTTPDaemon(pNet, pSocket, pSocket != nullptr ? MAX_CONTENT : 0, HTTP_PORT,
                    MAX_UPLOAD, TIMEOUT_SECONDS)
    {
    }

    CHTTPDaemon *CreateWorker(CNetSubSystem *pNet, CSocket *pSocket)
    {
        return new CWebFiles(pNet, pSocket);
    }

    THTTPStatus GetContent(const char *pPath, const char *pParams, const char *pFormData,
                           u8 *pBuffer, unsigned *pLength, const char **ppContentType);

private:
    THTTPStatus List(const char *pDir, u8 *pBuffer, unsigned *pLength);
    THTTPStatus Download(const char *pPath, u8 *pBuffer, unsigned *pLength);
    THTTPStatus Upload(CString *pResult);
    THTTPStatus Delete(CString *pResult);
    THTTPStatus Rename(CString *pResult);
    THTTPStatus Move(CString *pResult);
    THTTPStatus MakeFolder(CString *pResult);
    THTTPStatus SystemUpload(CString *pResult);
    THTTPStatus Zip(u8 *pBuffer, unsigned *pLength);
};

THTTPStatus CWebFiles::GetContent(const char *pPath, const char *pParams, const char *pFormData,
                                  u8 *pBuffer, unsigned *pLength, const char **ppContentType)
{
    if (strcmp(pPath, "/") == 0 || strcmp(pPath, "/index.html") == 0) {
        unsigned n = sizeof(s_Page) - 1;
        if (n > *pLength) {
            return HTTPInternalServerError;
        }
        memcpy(pBuffer, s_Page, n);
        *pLength = n;
        *ppContentType = "text/html; charset=utf-8";
        return HTTPOK;
    }

    if (strcmp(pPath, "/system") == 0) {
        unsigned n = sizeof(s_SystemPage) - 1;
        memcpy(pBuffer, s_SystemPage, n);
        *pLength = n;
        *ppContentType = "text/html; charset=utf-8";
        return HTTPOK;
    }

    if (strcmp(pPath, "/api/system") == 0) {
        CString Json;
        UpdateStatusJson(&Json);
        memcpy(pBuffer, (const char *) Json, Json.GetLength());
        *pLength = Json.GetLength();
        *ppContentType = "application/json";
        return HTTPOK;
    }

    if (strcmp(pPath, "/api/list") == 0) {
        char Dir[TAPE_PATH_SIZE];
        url_param(pParams, "dir", Dir, sizeof(Dir));
        *ppContentType = "application/json";
        return List(Dir, pBuffer, pLength);
    }

    if (strcmp(pPath, "/file") == 0) {
        char Path[TAPE_PATH_SIZE];
        url_param(pParams, "p", Path, sizeof(Path));
        *ppContentType = "application/octet-stream";
        return Download(Path, pBuffer, pLength);
    }

    if (strcmp(pPath, "/api/zip") == 0) {
        *ppContentType = "application/zip";
        return Zip(pBuffer, pLength);
    }

    CString Result;
    THTTPStatus Status;
    if (strcmp(pPath, "/api/upload") == 0) {
        Status = Upload(&Result);
    } else if (strcmp(pPath, "/api/delete") == 0) {
        Status = Delete(&Result);
    } else if (strcmp(pPath, "/api/rename") == 0) {
        Status = Rename(&Result);
    } else if (strcmp(pPath, "/api/move") == 0) {
        Status = Move(&Result);
    } else if (strcmp(pPath, "/api/mkdir") == 0) {
        Status = MakeFolder(&Result);
    } else if (strcmp(pPath, "/api/system/upload") == 0) {
        Status = SystemUpload(&Result);
    } else if (strcmp(pPath, "/api/system/cancel") == 0) {
        UpdateDiscard();
        Result = "Discarded\n";
        Status = HTTPOK;
    } else {
        return HTTPNotFound;
    }
    if (Status != HTTPOK) {
        return Status;
    }

    unsigned n = Result.GetLength();
    if (n > *pLength) {
        n = *pLength;
    }
    memcpy(pBuffer, (const char *) Result, n);
    *pLength = n;
    *ppContentType = "text/plain; charset=utf-8";
    return HTTPOK;
}

// {"dir":"path","tape":"path","folders":["name",...],"files":[{"n":"name","s":size,"t":"tape"},...]}
THTTPStatus CWebFiles::List(const char *pDir, u8 *pBuffer, unsigned *pLength)
{
    // From the heap on first use: kept out of the kernel's fixed 2 MB (as are the
    // other big buffers of the page)
    static TTapeListEntry *Entries = nullptr;
    if (Entries == nullptr && (Entries = new TTapeListEntry[MAX_FILES]) == nullptr) {
        return HTTPInternalServerError;
    }
    if (!TapeIsValidPath(pDir, TRUE, TRUE)) {
        return HTTPNotFound;
    }
    int nFiles = TapeListFiles(pDir, Entries, MAX_FILES, TRUE);
    if (nFiles < 0) {
        return HTTPNotFound;
    }

    CString Json("{\"dir\":");
    json_string(&Json, pDir);
    Json.Append(",\"tape\":");
    json_string(&Json, TapeGetInserted());
    Json.Append(",\"folders\":[");
    boolean bFirst = TRUE;
    for (int i = 0; i < nFiles; i++) {
        if (Entries[i].bFolder && !Entries[i].bZip && is_ascii(Entries[i].Name)) {
            if (!bFirst) {
                Json.Append(",");
            }
            bFirst = FALSE;
            json_string(&Json, Entries[i].Name);
        }
    }
    Json.Append("],\"files\":[");
    bFirst = TRUE;
    for (int i = 0; i < nFiles; i++) {
        if ((Entries[i].bFolder && !Entries[i].bZip) || !is_ascii(Entries[i].Name)) {
            continue;
        }
        if (!bFirst) {
            Json.Append(",");
        }
        bFirst = FALSE;
        Json.Append("{\"n\":");
        json_string(&Json, Entries[i].Name);
        CString Rest;
        Rest.Format(",\"s\":%u,\"t\":\"%s\"}", Entries[i].nSize,
                    Entries[i].bZip ? "zip archive" : Entries[i].bSnapshot ? "snapshot" :
                    Entries[i].bSource ? "Forth source" : "tape");
        Json.Append(Rest);
    }
    Json.Append("]}");

    if (Json.GetLength() > *pLength) {
        return HTTPInternalServerError;
    }
    memcpy(pBuffer, (const char *) Json, Json.GetLength());
    *pLength = Json.GetLength();
    return HTTPOK;
}

THTTPStatus CWebFiles::Download(const char *pPath, u8 *pBuffer, unsigned *pLength)
{
    unsigned nLength;
    TTapeFileResult Result = TapeReadFile(pPath, pBuffer, *pLength, &nLength);
    if (Result != TapeFileOK) {
        return Result == TapeFileTooBig ? HTTPRequestEntityTooLarge :
               Result == TapeFileError ? HTTPInternalServerError : HTTPNotFound;
    }
    *pLength = nLength;
    activity("Sent %s", TapeBaseName(pPath));
    return HTTPOK;
}

// Parts: "dir" (the folder shown), "replace" ("1" to overwrite), then for each file a
// "path" (its path in "dir": its name, or folder/.../name for a folder upload) and a
// "file". The folders in the paths are made as needed.
THTTPStatus CWebFiles::Upload(CString *pResult)
{
    boolean bReplace = FALSE;
    unsigned nFiles = 0, nSaved = 0;
    char Dir[TAPE_PATH_SIZE] = "", Path[TAPE_PATH_SIZE] = "";

    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field))) {
            continue;
        }
        if (strcmp(Field, "dir") == 0) {
            part_text(pData, nLength, Dir, sizeof(Dir));
            continue;
        }
        if (strcmp(Field, "replace") == 0) {
            bReplace = nLength > 0 && pData[0] == '1';
            continue;
        }
        if (strcmp(Field, "path") == 0) {
            part_text(pData, nLength, Path, sizeof(Path));
            continue;
        }
        char Name[TAPE_PATH_SIZE];
        if (strcmp(Field, "file") != 0 || !part_attribute(pHeader, "filename", Name, sizeof(Name))) {
            continue;
        }
        if (Path[0] == '\0') {
            // No "path" part: the file name, without any folders (/ or \) before it
            const char *pName = Name;
            for (const char *p = Name; *p != '\0'; p++) {
                if (*p == '/' || *p == '\\') {
                    pName = p + 1;
                }
            }
            strcpy(Path, pName);
        }

        nFiles++;
        char Target[TAPE_PATH_SIZE], Folder[TAPE_PATH_SIZE];
        TTapeFileResult Result = TapeFileInvalidName;
        if (   TapeIsValidPath(Dir, TRUE, TRUE) && TapeJoinPath(Target, Dir, Path)
            && TapeIsAceFileName(Target)) {
            TapeParentFolder(Target, Folder);
            Result = TapeMakeFolders(Folder);
            if (Result == TapeFileOK) {
                Result = TapeWriteFile(Target, pData, nLength, bReplace);
            }
        }
        CString Line;
        if (Result == TapeFileOK) {
            nSaved++;
            Line.Format("%s: saved (%u bytes)\n", Path, nLength);
            activity("Got %s", TapeBaseName(Path));
        } else {
            Line.Format("%s: not saved, %s%s\n", Path, result_text(Result),
                        Result == TapeFileExists ? " (tick Replace)" : "");
        }
        pResult->Append(Line);
        Path[0] = '\0';
    }

    if (nFiles == 0) {
        pResult->Append("No files received\n");
    } else if (nFiles > 1) {
        CString Line;
        Line.Format("%u of %u files saved\n", nSaved, nFiles);
        pResult->Append(Line);
    }
    return HTTPOK;
}

// Parts: "name", one per file or folder (folders go with everything in them)
THTTPStatus CWebFiles::Delete(CString *pResult)
{
    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field)) || strcmp(Field, "name") != 0) {
            continue;
        }
        char Path[TAPE_PATH_SIZE];
        part_text(pData, nLength, Path, sizeof(Path));

        boolean bFolder = FALSE;
        TTapeFileResult Result;
        if (!TapeIsValidPath(Path, FALSE, TRUE)) {
            Result = TapeFileInvalidName;
        } else if (!TapeExists(Path, &bFolder)) {
            Result = TapeFileNotFound;
        } else if (bFolder) {
            Result = TapeDeleteFolder(Path);
        } else {
            Result = TapeIsAceFileName(Path) ? TapeDeleteFile(Path) : TapeFileInvalidName;
        }
        CString Line;
        if (Result == TapeFileOK) {
            Line.Format(bFolder ? "%s: folder deleted, with everything in it\n" : "%s: deleted\n", Path);
            activity("Deleted %s%s", TapeBaseName(Path), bFolder ? "/" : "");
        } else {
            Line.Format("%s: not deleted, %s\n", Path, Result == TapeFileError ? "could not delete it"
                                                                             : result_text(Result));
        }
        pResult->Append(Line);
    }
    return HTTPOK;
}

// Rename or move a file or folder: TapeRenameFile() / TapeRenameFolder()
static TTapeFileResult rename_path(const char *pFrom, const char *pTo, boolean *pbFolder)
{
    *pbFolder = FALSE;
    if (!TapeIsValidPath(pFrom, FALSE, TRUE) || !TapeIsValidPath(pTo, FALSE, TRUE)) {
        return TapeFileInvalidName;
    }
    if (!TapeExists(pFrom, pbFolder)) {
        return TapeFileNotFound;
    }
    boolean bTargetFolder;
    if (strcasecmp(pFrom, pTo) != 0 && TapeExists(pTo, &bTargetFolder)) {
        return TapeFileExists;
    }
    return *pbFolder ? TapeRenameFolder(pFrom, pTo) : TapeRenameFile(pFrom, pTo);
}

// Parts: "from", "to" (paths)
THTTPStatus CWebFiles::Rename(CString *pResult)
{
    char From[TAPE_PATH_SIZE] = "", To[TAPE_PATH_SIZE] = "";

    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field))) {
            continue;
        }
        if (strcmp(Field, "from") == 0) {
            part_text(pData, nLength, From, sizeof(From));
        } else if (strcmp(Field, "to") == 0) {
            part_text(pData, nLength, To, sizeof(To));
        }
    }

    boolean bFolder;
    TTapeFileResult Result = rename_path(From, To, &bFolder);
    CString Line;
    if (Result == TapeFileOK) {
        Line.Format("%s renamed to %s\n", From, TapeBaseName(To));
        activity("Renamed %s%s", TapeBaseName(To), bFolder ? "/" : "");
    } else {
        Line.Format("%s not renamed: %s\n", From, Result == TapeFileError ? "could not rename it" :
                                               Result == TapeFileInvalidName && bFolder ? "not a plain name"
                                                                                        : result_text(Result));
    }
    pResult->Append(Line);
    return HTTPOK;
}

// Parts: "to" (a folder, "" for programs itself), then "name", one per file or folder
THTTPStatus CWebFiles::Move(CString *pResult)
{
    char To[TAPE_PATH_SIZE] = "";
    boolean bTo = FALSE;

    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field))) {
            continue;
        }
        if (strcmp(Field, "to") == 0) {
            part_text(pData, nLength, To, sizeof(To));
            boolean bFolder = FALSE;
            bTo = TapeIsValidPath(To, TRUE, TRUE) && TapeExists(To, &bFolder) && bFolder;
            if (!bTo) {
                CString Line;
                Line.Format("No folder %s: nothing moved\n", To[0] != '\0' ? To : "programs");
                pResult->Append(Line);
                return HTTPOK;
            }
            continue;
        }
        if (strcmp(Field, "name") != 0 || !bTo) {
            continue;
        }
        char From[TAPE_PATH_SIZE], Target[TAPE_PATH_SIZE];
        part_text(pData, nLength, From, sizeof(From));

        boolean bFolder = FALSE;
        TTapeFileResult Result = TapeJoinPath(Target, To, TapeBaseName(From))
                                 ? rename_path(From, Target, &bFolder) : TapeFileInvalidName;
        CString Line;
        if (Result == TapeFileOK) {
            Line.Format("%s moved to %s\n", From, To[0] != '\0' ? To : "programs");
            activity("Moved %s%s", TapeBaseName(From), bFolder ? "/" : "");
        } else {
            Line.Format("%s not moved: %s\n", From,
                        Result == TapeFileError ? "could not move it" :
                        Result == TapeFileInvalidName && bFolder ? "not into itself"
                                                                 : result_text(Result));
        }
        pResult->Append(Line);
    }
    return HTTPOK;
}

// Parts: "dir", "name"
THTTPStatus CWebFiles::MakeFolder(CString *pResult)
{
    char Dir[TAPE_PATH_SIZE] = "", Name[TAPE_LIST_NAME_SIZE] = "";

    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field))) {
            continue;
        }
        if (strcmp(Field, "dir") == 0) {
            part_text(pData, nLength, Dir, sizeof(Dir));
        } else if (strcmp(Field, "name") == 0) {
            part_text(pData, nLength, Name, sizeof(Name));
        }
    }

    char Path[TAPE_PATH_SIZE];
    TTapeFileResult Result = strchr(Name, '/') == nullptr && TapeIsValidPath(Dir, TRUE, TRUE)
                             && TapeJoinPath(Path, Dir, Name) ? TapeMakeFolder(Path)
                                                              : TapeFileInvalidName;
    CString Line;
    if (Result == TapeFileOK) {
        Line.Format("Folder %s made\n", Name);
        activity("New folder %s", Name);
    } else {
        Line.Format("Folder %s not made: %s\n", Name,
                    Result == TapeFileInvalidName ? "not a plain name (ASCII letters, digits, ...)" :
                    Result == TapeFileExists ? "that name is already taken" :
                    Result == TapeFileNotFound ? "no such folder" : "could not make it");
    }
    pResult->Append(Line);
    return HTTPOK;
}

// Parts: "file", a release .zip of xAce-baremetal (update_sd.h)
THTTPStatus CWebFiles::SystemUpload(CString *pResult)
{
    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field)) || strcmp(Field, "file") != 0) {
            continue;
        }
        if (UpdateReceive(pData, nLength, pResult)) {
            activity("Update %s received", UpdatePendingVersion());
        }
        return HTTPOK;
    }
    pResult->Append("No file received\n");
    return HTTPOK;
}

// The files of a .zip: paths in programs, and how many characters of each to leave out
// of its name in the .zip (the folder shown on the page)
struct TZipList {
    char     Paths[MAX_FILES][TAPE_PATH_SIZE];
    unsigned nFiles;
    const char *pFolder;                // folder being walked
    boolean  bOverflow;
};

static boolean zip_add(TZipList *pList, const char *pPath)
{
    if (pList->nFiles >= MAX_FILES) {
        pList->bOverflow = TRUE;
        return FALSE;
    }
    strcpy(pList->Paths[pList->nFiles++], pPath);
    return TRUE;
}

static boolean zip_tree_handler(const char *pPath, unsigned nSize, void *pParam)
{
    TZipList *pList = (TZipList *) pParam;
    char Path[TAPE_PATH_SIZE];
    if (!TapeJoinPath(Path, pList->pFolder, pPath)) {
        return TRUE;                    // too long: left out
    }
    return zip_add(pList, Path);
}

// A file's name in the .zip: its path in the folder shown (pDir), else its whole path
static const char *zip_name(const char *pPath, const char *pDir)
{
    unsigned n = strlen(pDir);
    return n > 0 && strncasecmp(pPath, pDir, n) == 0 && pPath[n] == '/' ? pPath + n + 1 : pPath;
}

// Parts: "dir" (the folder shown), then "name", one per file or folder (with everything
// in it). A .zip with the files stored (not compressed), named by their path in "dir".
THTTPStatus CWebFiles::Zip(u8 *pBuffer, unsigned *pLength)
{
    struct TZipFiles {
        TZipList List;
        u32 Offsets[MAX_FILES], Sizes[MAX_FILES], Crcs[MAX_FILES];
        u16 Dates[MAX_FILES], Times[MAX_FILES];
    };
    static TZipFiles *pZip = nullptr;   // from the heap on first use, see List()
    if (pZip == nullptr && (pZip = new TZipFiles) == nullptr) {
        return HTTPInternalServerError;
    }
    TZipList &List = pZip->List;
    auto &Offsets = pZip->Offsets;
    auto &Sizes = pZip->Sizes;
    auto &Crcs = pZip->Crcs;
    auto &Dates = pZip->Dates;
    auto &Times = pZip->Times;

    // The files first
    char Dir[TAPE_PATH_SIZE] = "";
    List.nFiles = 0;
    List.bOverflow = FALSE;
    const char *pHeader;
    const u8 *pData;
    unsigned nLength;
    while (GetMultipartFormPart(&pHeader, &pData, &nLength)) {
        char Field[32];
        if (!part_attribute(pHeader, "name", Field, sizeof(Field))) {
            continue;
        }
        if (strcmp(Field, "dir") == 0) {
            part_text(pData, nLength, Dir, sizeof(Dir));
            continue;
        }
        if (strcmp(Field, "name") != 0) {
            continue;
        }
        char Path[TAPE_PATH_SIZE];
        part_text(pData, nLength, Path, sizeof(Path));
        boolean bFolder = FALSE;
        if (!TapeIsValidPath(Path, FALSE, TRUE) || !TapeExists(Path, &bFolder)) {
            return HTTPNotFound;
        }
        if (bFolder) {
            List.pFolder = Path;
            TapeWalkFolder(Path, zip_tree_handler, &List);
        } else {
            zip_add(&List, Path);
        }
        if (List.bOverflow) {
            return HTTPRequestEntityTooLarge;
        }
    }
    if (List.nFiles == 0) {
        return HTTPBadRequest;
    }

    const unsigned nMax = *pLength;
    unsigned nPos = 0, nCentral = 0;
    for (unsigned i = 0; i < List.nFiles; i++) {
        const char *pPath = List.Paths[i];
        const char *pName = zip_name(pPath, Dir);
        unsigned nName = strlen(pName);

        // Local header, then the data, leaving room for the central directory and its end
        unsigned nData = nPos + 30 + nName;
        unsigned nReserve = nCentral + 46 + nName + 22;
        if (nData + nReserve > nMax) {
            return HTTPRequestEntityTooLarge;
        }
        unsigned nSize;
        TTapeFileResult Result = TapeReadFile(pPath, pBuffer + nData, nMax - nData - nReserve,
                                              &nSize, &Dates[i], &Times[i]);
        if (Result != TapeFileOK) {
            return Result == TapeFileTooBig ? HTTPRequestEntityTooLarge :
                   Result == TapeFileError ? HTTPInternalServerError : HTTPNotFound;
        }

        u8 *p = pBuffer + nPos;
        Offsets[i] = nPos;
        Sizes[i] = nSize;
        Crcs[i] = crc32(pBuffer + nData, nSize);
        put32(p, 0x04034B50);           // local file header
        put16(p + 4, 10);               // version needed: 1.0
        put16(p + 6, 0);                // flags
        put16(p + 8, 0);                // stored
        put16(p + 10, Times[i]);
        put16(p + 12, Dates[i]);
        put32(p + 14, Crcs[i]);
        put32(p + 18, nSize);
        put32(p + 22, nSize);
        put16(p + 26, nName);
        put16(p + 28, 0);
        memcpy(p + 30, pName, nName);

        nPos = nData + nSize;
        nCentral += 46 + nName;
    }

    unsigned nCentralStart = nPos;
    for (unsigned i = 0; i < List.nFiles; i++) {
        const char *pPath = List.Paths[i];
        const char *pName = zip_name(pPath, Dir);
        unsigned nName = strlen(pName);
        u8 *p = pBuffer + nPos;
        memset(p, 0, 46);
        put32(p, 0x02014B50);           // central directory header
        put16(p + 4, 20);               // made by: MS-DOS, 2.0
        put16(p + 6, 10);
        put16(p + 12, Times[i]);
        put16(p + 14, Dates[i]);
        put32(p + 16, Crcs[i]);
        put32(p + 20, Sizes[i]);
        put32(p + 24, Sizes[i]);
        put16(p + 28, nName);
        put32(p + 42, Offsets[i]);
        memcpy(p + 46, pName, nName);
        nPos += 46 + nName;
    }

    u8 *p = pBuffer + nPos;
    memset(p, 0, 22);
    put32(p, 0x06054B50);               // end of central directory
    put16(p + 8, List.nFiles);
    put16(p + 10, List.nFiles);
    put32(p + 12, nPos - nCentralStart);
    put32(p + 16, nCentralStart);
    nPos += 22;

    *pLength = nPos;
    activity("Sent %u files (.zip)", List.nFiles);
    return HTTPOK;
}

// ---------------------------------------------------------------------------

static void start_scheduler(void)
{
    // Circle's network needs the scheduler; created only now, so nothing yields
    // to other tasks unless WebFilesRun() is called
    if (s_pScheduler != nullptr) {
        return;
    }
    if (CScheduler::IsActive()) {
        s_pScheduler = CScheduler::Get();
    } else {
        s_pScheduler = new CScheduler;
    }
}

// Is there a firmware file of the Wi-Fi chips in SD:/firmware/?
static boolean wifi_firmware_present(void)
{
    DIR Dir;
    FILINFO Info;
    if (f_findfirst(&Dir, &Info, "SD:/firmware", "brcmfmac*.bin") != FR_OK) {
        return FALSE;
    }
    boolean bFound = Info.fname[0] != '\0';
    f_closedir(&Dir);
    return bFound;
}

// Bring up the Wi-Fi device; FALSE (with s_WiFiFailure set) if it cannot be used
static boolean start_wifi_device(void)
{
    if (s_WiFiFailure != WebNotStarted) {
        return FALSE;                       // tried before: one device object only
    }

    s_pWLAN = new CBcm4343Device(WIFI_FIRMWARE_PATH);
    if (s_pWLAN == nullptr || !s_pWLAN->Initialize()) {
        CLogger::Get()->Write(FromWeb, LogError, "Cannot start the Wi-Fi device");
        s_WiFiFailure = WebNoWiFi;
        return FALSE;
    }
    return TRUE;
}

boolean WebFilesStart(TWebNetwork Network, TWebState *pFailure)
{
    *pFailure = WebNotStarted;
    if (s_bStarted) {
        if (s_bFailed) {
            *pFailure = WebFailed;
        }
        return !s_bFailed;
    }

    start_scheduler();

    if (Network == WebWiFi) {
        if (s_WiFiFailure == WebNotStarted && !wifi_firmware_present()) {
            CLogger::Get()->Write(FromWeb, LogError, "No Wi-Fi firmware in " WIFI_FIRMWARE_PATH);
            *pFailure = WebNoWiFiFirmware;  // can be tried again once the files are there
            return FALSE;
        }
        if (!start_wifi_device()) {
            *pFailure = s_WiFiFailure;
            return FALSE;
        }
    }

    s_bStarted = TRUE;
    s_Network = Network;
    s_nStartMicros = CTimer::GetClockTicks64();
    s_nJoinStartMicros = s_nRunMicros;

    s_pNet = new CNetSubSystem(0, 0, 0, 0, HOSTNAME,
                               Network == WebWiFi ? NetDeviceTypeWLAN : NetDeviceTypeEthernet);
    if (s_pNet == nullptr || !s_pNet->Initialize(FALSE)) {
        CLogger::Get()->Write(FromWeb, LogError, "Cannot start the network");
        s_bFailed = TRUE;
        *pFailure = WebFailed;
        return FALSE;
    }

    if (Network == WebWiFi) {
        s_pWPASupplicant = new CWPASupplicant(WIFI_SETTINGS_FILE);
        if (s_pWPASupplicant == nullptr || !s_pWPASupplicant->Initialize()) {
            CLogger::Get()->Write(FromWeb, LogError, "Cannot start the WPA supplicant");
            s_bFailed = TRUE;
            *pFailure = WebFailed;
            return FALSE;
        }
    }

    CLogger::Get()->Write(FromWeb, LogNotice, "Network started (%s)",
                          Network == WebWiFi ? "Wi-Fi" : "cable");
    return TRUE;
}

boolean WebFilesStarted(TWebNetwork *pNetwork)
{
    *pNetwork = s_Network;
    return s_bStarted && !s_bFailed;
}

TWebState WebFilesWiFiFailure(void)
{
    return s_WiFiFailure;
}

boolean WebFilesWiFiReload(void)
{
    if (!s_bStarted || s_bFailed || s_Network != WebWiFi
        || s_bHadAddress || !wpa_supplicant_is_running()) {
        return FALSE;
    }
    wpa_supplicant_reload();
    s_nJoinStartMicros = s_nRunMicros;
    return TRUE;
}

void WebFilesRun(unsigned nMicros)
{
    if (!s_bStarted || s_bFailed) {
        return;
    }
    u64 nStart = CTimer::GetClockTicks64();
    u64 nEnd = nStart + nMicros;
    u64 nNow;
    do {
        s_pScheduler->Yield();
        nNow = CTimer::GetClockTicks64();
    } while (nNow < nEnd);
    s_nRunMicros += nNow - nStart;
}

TWebState WebFilesGetState(CString *pAddress, CString *pName)
{
    *pAddress = "";
    *pName = "";
    if (!s_bStarted) {
        return WebNotStarted;
    }
    if (s_bFailed) {
        return WebFailed;
    }

    if (s_Network == WebWiFi) {
        // The WPA supplicant starts a moment after the network; it stops at once if it
        // cannot read its file
        if (!wpa_supplicant_is_running() && s_nRunMicros - s_nJoinStartMicros >= 3000000) {
            return WebWiFiBadSettings;
        }
        if (!CWPASupplicant::IsConnected()) {
            return s_nRunMicros - s_nJoinStartMicros >= JOIN_TROUBLE_MICROS ? WebJoinTrouble
                                                                            : WebJoining;
        }
        s_nJoinStartMicros = s_nRunMicros;  // joined: start again if it drops
    } else {
        CNetDevice *pDevice = CNetDevice::GetNetDevice(NetDeviceTypeEthernet);
        if (pDevice == nullptr) {
            return CTimer::GetClockTicks64() - s_nStartMicros >= NO_ADAPTER_MICROS ? WebNoAdapter
                                                                                  : WebWaitingAddress;
        }
        if (!pDevice->IsLinkUp()) {
            return WebNoCable;
        }
    }
    if (!s_pNet->IsRunning()) {
        return WebWaitingAddress;
    }
    s_bHadAddress = TRUE;

    // The web server, once there is an address: Circle's TCP listening sockets take
    // the Pi's address when they are made, and check incoming connections against it
    // (made before DHCP, they hold 0.0.0.0 and refuse every connection)
    static boolean bListening = FALSE;
    if (!bListening) {
        bListening = TRUE;
        new CWebFiles(s_pNet);          // the listener: starts a worker per connection
    }

    CString IP;
    s_pNet->GetConfig()->GetIPAddress()->Format(&IP);
    pAddress->Format("http://%s", (const char *) IP);

    // xace.local, once there is an address to publish
    if (s_pmDNS == nullptr) {
        s_pmDNS = new CmDNSDaemon(s_pNet);
    }
    if (s_pmDNS->IsRunning()) {
        pName->Format("http://%s.local", (const char *) s_pmDNS->GetHostname());
    }

    return WebReady;
}
