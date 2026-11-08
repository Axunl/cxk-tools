#pragma once

// ---------------------------------------------------------------------------
// 浏览器端页面：一个自包含的单页应用（HTML + CSS + JS 全部内联，无外部依赖）。
//
// 为什么内联而不是放 .qrc 资源：这是插件模块，qrc 要额外配 AUTORCC 与资源路径；
// 而且页面需要跟 C++ 侧的协议（路由、JSON 字段）一起改，放在同一个目录里更不容易
// 出现"页面改了、服务端没改"的错位。代价是编译期多一个几十 KB 的字符串常量。
//
// 前后端接口约定（改任何一条都要同时改 ShareServer 的路由）：
//   GET    /                本页面
//   GET    /api/state       文件 + 文本 + 可用地址（JSON）
//   GET    /f/<id>          下载共享文件
//   GET    /t/<id>          查看单条文本（纯文本）
//   POST   /api/upload      body = 文件原始字节，文件名放 X-File-Name（percent-encoded）
//   POST   /api/text        body = {"text": "..."}
//   DELETE /api/file/<id>   从共享列表移除（本服务上传的副本会连磁盘文件一起删）
//   DELETE /api/text/<id>
// ---------------------------------------------------------------------------

namespace share {

/// 页面正文（UTF-8 字节）。返回 QByteArray 而不是 const char*，
/// 方便调用方直接算 Content-Length。
inline const char *indexHtmlData()
{
    return R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>共享 · 局域网文件与文本</title>
<style>
*{box-sizing:border-box}
body{margin:0;min-height:100vh;padding:26px 18px 34px;
  background:linear-gradient(160deg,#a5b4fc 0%,#c4b5fd 45%,#fbcfe8 100%);
  font-family:"Microsoft YaHei UI","PingFang SC","Noto Sans CJK SC",system-ui,sans-serif;color:#0f172a}
.wrap{max-width:1020px;margin:0 auto}
header{text-align:center;margin-bottom:20px}
h1{margin:0 0 6px;font:700 30px/1.25 "Microsoft YaHei UI","PingFang SC","Noto Sans CJK SC",system-ui,sans-serif;
  letter-spacing:.14em;padding-left:.14em;color:#fff;text-shadow:0 2px 14px rgba(79,107,246,.45)}
.sub{margin:0;color:rgba(255,255,255,.94);font-size:13px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media (max-width:800px){.grid{grid-template-columns:1fr}}
.card{background:rgba(255,255,255,.93);border-radius:14px;padding:16px;
  box-shadow:0 14px 34px rgba(76,86,160,.2);display:flex;flex-direction:column;min-height:430px}
.cardHead{display:flex;align-items:center;gap:8px;margin-bottom:10px}
h2{margin:0;font-size:14px;color:#334155}
.count{margin-left:auto;font-size:12px;color:#94a3b8}
.btn{border:1px solid #d7dee9;background:#fff;color:#334155;border-radius:8px;padding:5px 12px;
  font-size:12px;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;
  gap:4px;transition:.15s;font-family:inherit}
.btn:hover{background:#f1f5ff;border-color:#c3cedd}
.btn.primary{background:#4f6bf6;border-color:#4f6bf6;color:#fff;font-weight:600}
.btn.primary:hover{background:#4360ee}
.btn.ghost{color:#64748b}
.btn.ghost:hover{color:#dc2626;border-color:#f3c9c9;background:#fef2f2}
.btn.small{padding:3px 9px;font-size:11px}
.drop{border:2px dashed #c7d2fe;border-radius:10px;padding:14px;text-align:center;color:#94a3b8;
  font-size:12px;margin-bottom:10px;transition:.15s;cursor:pointer}
.drop.over{border-color:#4f6bf6;background:#eef2ff;color:#4f6bf6}
.list{list-style:none;margin:0;padding:0;overflow-y:auto;flex:1;min-height:0}
.row{display:flex;align-items:center;gap:8px;padding:7px 4px;border-bottom:1px solid #f1f5f9}
.row:last-child{border-bottom:none}
.name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:13px}
.name.missing{color:#b6c0cc;text-decoration:line-through}
.size{font-size:11px;color:#94a3b8;flex:none}
.empty{color:#b6c0cc;font-size:12px;text-align:center;padding:28px 0}
.item{padding:8px 4px;border-bottom:1px solid #f1f5f9}
.item:last-child{border-bottom:none}
.meta{display:flex;align-items:center;gap:6px;font-size:11px;color:#94a3b8;margin-bottom:5px}
.meta .ip{color:#4f6bf6}
.body{font:12px/1.6 "Cascadia Mono",Consolas,"Menlo",monospace;color:#334155;white-space:pre-wrap;
  word-break:break-word;background:#f8fafc;border:1px solid #eef2f7;border-radius:7px;
  padding:6px 9px;max-height:88px;overflow:hidden;cursor:pointer}
.body.open{max-height:none}
.foot{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:11px;color:#94a3b8}
.send{display:flex;gap:8px;margin-top:10px}
textarea{flex:1;resize:none;height:38px;border:1px solid #d7dee9;border-radius:8px;padding:8px 10px;
  font:12px/1.5 inherit;font-family:inherit;color:#0f172a;outline:none}
textarea:focus{border-color:#4f6bf6}
.progress{height:4px;background:#eef2f6;border-radius:2px;overflow:hidden;margin-top:8px;display:none}
.progress.on{display:block}
.progress i{display:block;height:100%;width:0;background:#4f6bf6;transition:width .15s}
.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(14px);
  background:#0f172a;color:#f8fafc;font-size:12px;padding:7px 14px;border-radius:8px;opacity:0;
  transition:.2s;pointer-events:none;max-width:80vw}
.toast.on{opacity:1;transform:translateX(-50%) translateY(0)}
.dot{width:7px;height:7px;border-radius:50%;background:#10b981;flex:none}
.dot.off{background:#ef4444}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>共享</h1>
    <p class="sub">局域网文件与文本共享 · 无需登录，打开即用</p>
  </header>
  <div class="grid">
    <section class="card">
      <div class="cardHead">
        <h2>共享文件</h2>
        <span class="count" id="fileCount">共 0 个</span>
      </div>
      <label class="drop" id="drop" for="picker">拖文件到这里上传，或点击选择文件</label>
      <input type="file" id="picker" multiple hidden>
      <div class="progress" id="progress"><i></i></div>
      <ul class="list" id="files"></ul>
      <div class="foot"><span class="dot" id="dot"></span><span id="fileFoot">桌面端添加的文件会出现在这里</span></div>
    </section>
    <section class="card">
      <div class="cardHead">
        <h2>共享文本</h2>
        <span class="count" id="textCount">共 0 条</span>
      </div>
      <ul class="list" id="texts"></ul>
      <div class="send">
        <textarea id="textInput" placeholder="输入文本，Ctrl+Enter 发送（16 KB 以内）"></textarea>
        <button class="btn primary" id="sendBtn">发送</button>
      </div>
      <div class="foot"><span>发出去的文本会同时出现在桌面端的「共享文本」里</span></div>
    </section>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
var lastJson = "";
var lastTexts = [];

function $(id) { return document.getElementById(id); }

function toast(msg) {
  var el = $('toast');
  el.textContent = msg;
  el.classList.add('on');
  clearTimeout(el._t);
  el._t = setTimeout(function () { el.classList.remove('on'); }, 2200);
}

function render(s) {
  // 文件
  var ul = $('files');
  ul.innerHTML = '';
  if (!s.files.length) {
    var empty = document.createElement('li');
    empty.className = 'empty';
    empty.textContent = '还没有共享文件';
    ul.appendChild(empty);
  }
  s.files.forEach(function (f) {
    var li = document.createElement('li');
    li.className = 'row';
    var nm = document.createElement('span');
    nm.className = 'name' + (f.exists ? '' : ' missing');
    nm.textContent = f.name;
    nm.title = f.exists ? f.name : f.name + '（文件已不在桌面端磁盘上）';
    var sz = document.createElement('span');
    sz.className = 'size';
    sz.textContent = f.sizeText;
    li.appendChild(nm);
    li.appendChild(sz);
    if (f.exists) {
      var a = document.createElement('a');
      a.className = 'btn small';
      a.href = '/f/' + f.id;
      a.textContent = '下载';
      li.appendChild(a);
    }
    var del = document.createElement('button');
    del.className = 'btn small ghost';
    del.textContent = '删除';
    del.onclick = function () { removeItem('/api/file/' + f.id, '已删除文件'); };
    li.appendChild(del);
    ul.appendChild(li);
  });
  $('fileCount').textContent = '共 ' + s.files.length + ' 个';
  $('fileFoot').textContent = s.files.length ? '点击「下载」保存到本机' : '桌面端添加的文件会出现在这里';

  // 文本
  var tl = $('texts');
  tl.innerHTML = '';
  lastTexts = s.texts;
  if (!s.texts.length) {
    var e2 = document.createElement('li');
    e2.className = 'empty';
    e2.textContent = '还没有共享文本';
    tl.appendChild(e2);
  }
  s.texts.slice().reverse().forEach(function (t) {
    var li = document.createElement('li');
    li.className = 'item';
    var meta = document.createElement('div');
    meta.className = 'meta';
    var ip = document.createElement('span');
    ip.className = 'ip';
    ip.textContent = t.from;
    var tm = document.createElement('span');
    tm.textContent = t.time;
    meta.appendChild(ip);
    meta.appendChild(tm);
    var cp = document.createElement('button');
    cp.className = 'btn small ghost';
    cp.textContent = '复制';
    cp.style.marginLeft = 'auto';
    cp.onclick = function () {
      navigator.clipboard.writeText(t.text).then(function () { toast('已复制到剪贴板'); },
        function () { toast('复制失败，请手动选中'); });
    };
    var del2 = document.createElement('button');
    del2.className = 'btn small ghost';
    del2.textContent = '删除';
    del2.onclick = function () { removeItem('/api/text/' + t.id, '已删除文本'); };
    meta.appendChild(cp);
    meta.appendChild(del2);
    var body = document.createElement('div');
    body.className = 'body';
    body.textContent = t.text;
    body.onclick = function () { body.classList.toggle('open'); };
    li.appendChild(meta);
    li.appendChild(body);
    tl.appendChild(li);
  });
  $('textCount').textContent = '共 ' + s.texts.length + ' 条';
}

function setOnline(ok) {
  var d = $('dot');
  if (d) d.className = ok ? 'dot' : 'dot off';
  if (!ok) $('fileFoot').textContent = '连接已断开，正在重试…';
}

async function refresh() {
  if (document.hidden) return;
  try {
    var r = await fetch('/api/state', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    var s = await r.json();
    setOnline(true);
    var stamp = JSON.stringify(s.files) + JSON.stringify(s.texts);
    if (stamp !== lastJson) {
      lastJson = stamp;
      render(s);
    }
  } catch (e) {
    setOnline(false);
  }
}

async function removeItem(url, okMsg) {
  try {
    var r = await fetch(url, { method: 'DELETE' });
    if (!r.ok) throw new Error((await r.json()).error || r.status);
    lastJson = '';
    toast(okMsg);
    refresh();
  } catch (e) {
    toast('操作失败：' + e.message);
  }
}

// ---- 上传：串行队列 + XHR（fetch 拿不到上传进度）-------------------------
var queue = [];
var uploading = false;

function enqueue(files) {
  for (var i = 0; i < files.length; i++) queue.push(files[i]);
  if (!uploading) uploadNext();
}

function uploadNext() {
  if (!queue.length) {
    uploading = false;
    $('progress').classList.remove('on');
    return;
  }
  uploading = true;
  var file = queue.shift();
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/upload');
  // 文件名不能直接进 header（非 ASCII 会被浏览器拒绝），统一 percent-encode；
  // 服务端用 QUrl::fromPercentEncoding 还原，UTF-8 中文名不会乱码。
  xhr.setRequestHeader('X-File-Name', encodeURIComponent(file.name));
  var bar = $('progress');
  bar.classList.add('on');
  bar.firstElementChild.style.width = '0%';
  xhr.upload.onprogress = function (e) {
    if (e.lengthComputable) {
      bar.firstElementChild.style.width = Math.round(e.loaded / e.total * 100) + '%';
    }
  };
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      toast('已上传 ' + file.name);
      lastJson = '';
      refresh();
    } else {
      var msg = '上传失败';
      try { msg = JSON.parse(xhr.responseText).error || msg; } catch (e2) {}
      toast(msg);
    }
    uploadNext();
  };
  xhr.onerror = function () { toast('上传失败：连接中断'); uploadNext(); };
  xhr.send(file);
}

$('picker').addEventListener('change', function (e) {
  enqueue(e.target.files);
  e.target.value = '';
});

var drop = $('drop');
['dragenter', 'dragover'].forEach(function (ev) {
  drop.addEventListener(ev, function (e) { e.preventDefault(); drop.classList.add('over'); });
});
['dragleave', 'drop'].forEach(function (ev) {
  drop.addEventListener(ev, function (e) { e.preventDefault(); drop.classList.remove('over'); });
});
drop.addEventListener('drop', function (e) {
  if (e.dataTransfer && e.dataTransfer.files.length) enqueue(e.dataTransfer.files);
});

// ---- 发送文本 -----------------------------------------------------------
async function sendText() {
  var input = $('textInput');
  var text = input.value;
  if (!text.trim()) { toast('先输入点内容'); return; }
  try {
    var r = await fetch('/api/text', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: text })
    });
    if (!r.ok) throw new Error((await r.json()).error || r.status);
    input.value = '';
    lastJson = '';
    toast('已发送');
    refresh();
  } catch (e) {
    toast('发送失败：' + e.message);
  }
}
$('sendBtn').onclick = sendText;
$('textInput').addEventListener('keydown', function (e) {
  if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); sendText(); }
});

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)HTML";
}

} // namespace share
