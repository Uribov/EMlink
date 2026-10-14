#pragma once
// ============================================================
// webui.h —— Web 管理页静态资源（仅供 ota.cpp 包含）
// 视觉规格：学 ELRS —— 深色单页 / 纯黑白灰 / 1px 发丝线 / 零圆角 /
//           等边三角网格标记（中心三角 + 由中心向四周渐隐）。
// 全部放 PROGMEM（Flash），不占 DRAM；页面由 ota.cpp 按段拼装发送，
// 动态值（节点/角色/射频参数）运行时注入。
// 设计稿：design/webui.html      规格：ota.md
// 硬约束：① 板子自己开热点、手机无外网 → 不许引用任何外部资源（CSS/JS 全内嵌）
//         ② Flash 每 KB 都要省 → 不引框架，纯手写 HTML + 少量原生 JS
//         ③ WebServer 单线程串行 → 页面越简单越稳
// ============================================================
#include <Arduino.h>

// 页脚版本号
#define WEBUI_VER "v1.0"

// ---- 段 1：页头（DOCTYPE + CSS + 几何标记 + 大标题；<header> 留给调用方闭合）----
static const char WUI_HEAD[] PROGMEM =
"<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<title>EMlink</title><style>"
"*{box-sizing:border-box}"
":root{--bg:#0d0d0d;--line:#2c2c2c;--line2:#232323;--text:#ededed;"
"--muted:#7d7d7d;--dim:#4a4a4a;--ok:#34d399;--err:#f87171}"
"html,body{margin:0;padding:0}"
"body{background:var(--bg);color:var(--text);font-size:15px;line-height:1.5;"
"font-family:system-ui,-apple-system,\"Segoe UI\",Roboto,Arial,sans-serif;"
"-webkit-font-smoothing:antialiased}"
".wrap{max-width:440px;margin:0 auto;padding:32px 20px 56px}"
"header{margin-bottom:36px}"
".mark{display:block;width:68px;height:68px;color:var(--text);margin-bottom:20px}"
"h1{margin:0;font-size:42px;line-height:1;font-weight:600;letter-spacing:-.035em}"
"h1 .dot{color:var(--dim)}"
".sub{margin-top:12px;font-size:10px;letter-spacing:.16em;text-transform:uppercase;"
"color:var(--muted);display:flex;flex-wrap:wrap;gap:6px 14px}"
".sub b{font-weight:500;color:var(--text);letter-spacing:.1em}"
".st{display:inline-flex;align-items:center;gap:6px}"
".st i{width:6px;height:6px;border-radius:50%;background:var(--ok);flex:none}"
".st.off i{background:var(--dim)}"
"section{border-top:1px solid var(--line);padding-top:20px;margin-top:28px}"
"h2{margin:0 0 18px;font-size:10px;font-weight:600;letter-spacing:.16em;"
"text-transform:uppercase;color:var(--muted);display:flex;align-items:center;gap:10px}"
"h2:after{content:\"\";flex:1;height:1px;background:var(--line2)}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:14px 12px}"
".grid .wide{grid-column:1/-1}"
"label{display:block;font-size:10px;letter-spacing:.12em;"
"color:var(--muted);margin-bottom:7px}"
"input[type=text],select{width:100%;padding:11px 12px;background:transparent;"
"border:1px solid var(--line);border-radius:0;color:var(--text);font-size:14px;"
"font-family:ui-monospace,Menlo,Consolas,monospace}"
"input:focus,select:focus{outline:none;border-color:var(--text)}"
"input::placeholder{color:var(--dim)}"
".file{position:relative;display:flex;align-items:center;gap:12px;"
"border:1px dashed var(--line);padding:16px;cursor:pointer}"
".file input{position:absolute;top:0;left:0;width:100%;height:100%;opacity:0;cursor:pointer}"
".file .fx{font-size:10px;letter-spacing:.14em;text-transform:uppercase;"
"color:var(--muted);border:1px solid var(--line);padding:6px 10px;flex:none}"
".file .fn{font-size:12px;color:var(--dim);font-family:ui-monospace,Menlo,Consolas,monospace;"
"overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".file .fn.set{color:var(--text)}"
".btn{display:block;width:100%;padding:13px 16px;margin-top:16px;background:transparent;"
"border:1px solid var(--line);border-radius:0;color:var(--text);font-size:11px;"
"font-weight:600;letter-spacing:.14em;text-transform:uppercase;font-family:inherit;cursor:pointer}"
".btn:hover{border-color:var(--text)}.btn:active{background:#1c1c1c}"
".btn.primary{background:var(--text);color:var(--bg);border-color:var(--text)}"
".btn.primary:hover{background:#fff}"
".btn.ghost{color:var(--muted)}.btn.ghost:hover{color:var(--text)}"
".btn.danger{color:var(--err);border-color:#3a2020}.btn.danger:hover{border-color:var(--err)}"
".row{display:flex;gap:12px}.row .btn{flex:1;width:auto}"
".note{margin-top:10px;font-size:11px;color:var(--dim)}"
".err{margin-top:10px;font-size:12px;color:var(--err)}"
".msg{font-size:14px;color:var(--muted)}"
".msg.bad{color:var(--err)}"
"footer{margin-top:44px;padding-top:18px;border-top:1px solid var(--line);display:flex;"
"justify-content:space-between;font-size:10px;letter-spacing:.14em;text-transform:uppercase;"
"color:var(--dim)}"
"@media(max-width:380px){.grid{grid-template-columns:1fr}}"
"</style></head><body><div class=\"wrap\"><header>"
// 几何标记：中心三角 + 由中心向四周渐隐的等边三角网格
"<svg class=\"mark\" viewBox=\"0 0 96 96\" fill=\"none\" aria-hidden=\"true\"><defs>"
"<radialGradient id=\"emFade\" gradientUnits=\"userSpaceOnUse\" cx=\"48\" cy=\"48\" r=\"50\">"
"<stop offset=\"0\" stop-color=\"#fff\" stop-opacity=\"1\"/>"
"<stop offset=\".35\" stop-color=\"#fff\" stop-opacity=\".8\"/>"
"<stop offset=\".7\" stop-color=\"#fff\" stop-opacity=\".3\"/>"
"<stop offset=\"1\" stop-color=\"#fff\" stop-opacity=\"0\"/></radialGradient>"
"<mask id=\"emMask\" maskUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"96\" height=\"96\">"
"<rect x=\"0\" y=\"0\" width=\"96\" height=\"96\" fill=\"url(#emFade)\"/></mask></defs>"
"<g mask=\"url(#emMask)\" stroke=\"currentColor\" stroke-width=\"1\" opacity=\".7\">"
"<path d=\"M-172 -150L268 -150M-172 -139L268 -139M-172 -128L268 -128M-172 -117L268 -117M-172 -106L268 -106M-172 -95L268 -95M-172 -84L268 -84M-172 -73L268 -73M-172 -62L268 -62M-172 -51L268 -51M-172 -40L268 -40M-172 -29L268 -29M-172 -18L268 -18M-172 -7L268 -7M-172 4L268 4M-172 15L268 15M-172 26L268 26M-172 37L268 37M-172 48L268 48M-172 59L268 59M-172 70L268 70M-172 81L268 81M-172 92L268 92M-172 103L268 103M-172 114L268 114M-172 125L268 125M-172 136L268 136M-172 147L268 147M-172 158L268 158M-172 169L268 169M-172 180L268 180M-172 191L268 191M-172 202L268 202M-172 213L268 213M-172 224L268 224M-172 235L268 235M-172 246L268 246M109 -242L329 140M100 -236L320 145M90 -231L310 151M81 -225L301 156M71 -220L291 162M62 -214L282 167M52 -209L272 173M43 -203L263 178M33 -198L253 184M24 -192L244 189M14 -187L234 195M5 -181L225 200M-5 -176L215 206M-14 -170L206 211M-24 -165L196 217M-33 -159L187 222M-43 -154L177 228M-52 -148L168 233M-62 -143L158 239M-72 -137L148 244M-81 -132L139 250M-91 -126L129 255M-100 -121L120 261M-110 -115L110 266M-119 -110L101 272M-129 -104L91 277M-138 -99L82 283M-148 -93L72 288M-157 -88L63 294M-167 -82L53 299M-176 -77L44 305M-186 -71L34 310M-195 -66L25 316M-205 -60L15 321M-214 -55L6 327M-224 -49L-4 332M-233 -44L-13 338M329 -44L109 338M320 -49L100 332M310 -55L90 327M301 -60L81 321M291 -66L71 316M282 -71L62 310M272 -77L52 305M263 -82L43 299M253 -88L33 294M244 -93L24 288M234 -99L14 283M225 -104L5 277M215 -110L-5 272M206 -115L-14 266M196 -121L-24 261M187 -126L-33 255M177 -132L-43 250M168 -137L-52 244M158 -143L-62 239M148 -148L-72 233M139 -154L-81 228M129 -159L-91 222M120 -165L-100 217M110 -170L-110 211M101 -176L-119 206M91 -181L-129 200M82 -187L-138 195M72 -192L-148 189M63 -198L-157 184M53 -203L-167 178M44 -209L-176 173M34 -214L-186 167M25 -220L-195 162M15 -225L-205 156M6 -231L-214 151M-4 -236L-224 145M-13 -242L-233 140\"/>"
"</g>"
"<path d=\"M48 22L70.5 61L25.5 61Z\" stroke=\"currentColor\" stroke-width=\"2.6\" stroke-linejoin=\"miter\"/>"
"<circle cx=\"48\" cy=\"22\" r=\"5.2\" fill=\"currentColor\"/>"
"<circle cx=\"70.5\" cy=\"61\" r=\"5.2\" fill=\"currentColor\"/>"
"<circle cx=\"25.5\" cy=\"61\" r=\"5.2\" fill=\"currentColor\"/>"
"</svg>"
"<h1>EMlink<span class=\"dot\">.</span></h1>";

// ---- 段 2：状态行（节点 / 角色 / 加密）——动态值插在 SUB_OPEN 与 SUB_END 之间 ----
static const char WUI_SUB_OPEN[] PROGMEM = "<div class=\"sub\">";
static const char WUI_SUB_END[]  PROGMEM = "</div></header>";

// ---- 段 3：射频参数区开头（动态字段插在 A 与 B 之间）----
static const char WUI_RADIO_A[] PROGMEM =
"<section><h2>射频参数</h2><form action=\"/set\" method=\"get\"><div class=\"grid\">";

static const char WUI_RADIO_B[] PROGMEM =
"</div><button class=\"btn primary\" type=\"submit\">保存参数</button></form>"
"<div class=\"note\">写入后重启生效</div></section>";

// ---- 段 4：加密密钥区（整段静态；密钥永不回显，只留占位符）----
static const char WUI_CRYPTO[] PROGMEM =
"<section><h2>加密密钥</h2><form id=\"cf\" action=\"/crypto\" method=\"get\">"
"<label for=\"k\">32 位十六进制</label>"
"<input id=\"k\" name=\"key\" type=\"text\" maxlength=\"32\" autocomplete=\"off\" "
"spellcheck=\"false\" placeholder=\"0123456789abcdef0123456789abcdef\">"
"<div class=\"err\" id=\"kerr\" style=\"display:none\"></div>"
"<div class=\"row\">"
"<button class=\"btn primary\" type=\"submit\">设置并启用</button>"
"<button class=\"btn ghost\" id=\"koff\" type=\"submit\" name=\"off\" value=\"1\">关闭加密</button>"
"</div></form>"
"<div class=\"note\">十六进制字符 0-9 a-f，共 32 位；设置后立即生效</div></section>";

// ---- 段 5：固件升级区 ----
static const char WUI_OTA[] PROGMEM =
"<section><h2>固件升级</h2>"
"<form action=\"/update\" method=\"post\" enctype=\"multipart/form-data\">"
"<label class=\"file\">"
"<input type=\"file\" name=\"firmware\" id=\"fw\" accept=\".bin\">"
"<span class=\"fx\">选择文件</span><span class=\"fn\" id=\"fwname\">未选择 .bin</span>"
"</label>"
"<button class=\"btn primary\" type=\"submit\">上传刷写</button></form>"
"<div class=\"note\">刷写成功后自动重启并切换到新分区</div></section>";

// ---- 段 6：维护模式 + 页脚 + 收尾 JS（含密钥格式校验）----
static const char WUI_TAIL[] PROGMEM =
"<section><h2>维护模式</h2>"
"<form action=\"/exit\" method=\"get\">"
"<button class=\"btn danger\" type=\"submit\">退出维护模式</button></form>"
"<div class=\"note\">退出后 WiFi 关闭，LoRa 数传恢复</div></section>"
"<footer><span>EMlink WebUI</span><span>" WEBUI_VER "</span></footer>"
"</div><script>"
"(function(){"
"var f=document.getElementById('cf'),k=document.getElementById('k'),"
"e=document.getElementById('kerr'),off=document.getElementById('koff'),"
"w=document.getElementById('fw'),n=document.getElementById('fwname'),skip=false;"
"off.addEventListener('click',function(){skip=true;});"
"f.addEventListener('submit',function(ev){"
"if(skip)return;"
"var v=k.value.replace(/\\s/g,'');"
"if(!/^[0-9a-fA-F]{32}$/.test(v)){"
"ev.preventDefault();e.textContent='请输入正确的格式';e.style.display='block';k.focus();}"
"else{k.value=v;}"
"});"
"k.addEventListener('input',function(){e.style.display='none';});"
"w.addEventListener('change',function(){"
"if(w.files&&w.files[0]){n.textContent=w.files[0].name;n.className='fn set';}"
"else{n.textContent='未选择 .bin';n.className='fn';}});"
"})();"
"</script></body></html>";

// ---- 段 7：回执页（/set、/crypto、/exit、/update 的结果页）----
// 结构：WUI_HEAD + WUI_SUB_OPEN + 状态行 + WUI_SUB_END + WUI_DONE_A + 提示 + WUI_DONE_B
static const char WUI_DONE_A[] PROGMEM = "<section><h2>操作结果</h2>";

static const char WUI_DONE_B[] PROGMEM =
"<form action=\"/\" method=\"get\">"
"<button class=\"btn primary\" type=\"submit\">返回</button></form></section>"
"<footer><span>EMlink WebUI</span><span>" WEBUI_VER "</span></footer>"
"</div></body></html>";
