import gzip

with open('web_extracted.html', 'r', encoding='utf-8') as f:
    html = f.read()

# 1. Update card header
old_card = '''  <h2>📝 Запис CSV-лога (окремий файл для кожної АКБ)</h2>
  <div style="display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px">
    <div>Файлів у записі: <b id="logpath">—</b> · Записано рядків всього: <span id="logrows">0</span></div>
    <button id="logbtn" onclick="toggleLog()">Старт запису (усі АКБ)</button>
  </div>'''

new_card = '''  <h2>💾 Запис логів та сховище MicroSD</h2>
  <div style="display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;margin-bottom:12px">
    <div>Стан SD-карти модуля: <b id="sd_status">перевірка...</b> · Записано рядків: <span id="logrows">0</span></div>
    <div style="display:flex;gap:8px;flex-wrap:wrap">
      <button id="logbtn" onclick="toggleLog()">Старт запису (усі АКБ)</button>
      <button id="web_rec_btn" onclick="toggleWebRecord()" style="background:#238636;color:#fff">💻 Запис у проект (Браузер)</button>
      <button onclick="refreshSdFiles()" style="background:#30363d;color:#fff">🔄 Оновити список SD</button>
    </div>
  </div>'''

if old_card in html:
    html = html.replace(old_card, new_card)
    print("Replaced old_card")
else:
    print("old_card not found or already replaced")

# 2. Update logfiles table header & rendering
old_lf = '''    const lf = document.getElementById('logfiles');
    if (lf) {
      const files = d.log_files || {};
      const keys = Object.keys(files).sort();
      lf.innerHTML = keys.length ? keys.map(k => {
        const f = files[k];
        const state = f.on ? '<span class="pill ok">запис</span>' : '<span class="pill">стоп</span>';
        return `<tr><td>${escapeHtml(k)}</td><td>${escapeHtml(f.path || '—')}</td><td>${f.rows || 0} · ${escapeHtml(f.last_ts || '—')}</td><td>${state}</td><td style="color:var(--dim);font-size:12px">A ${f.alarm_rows || 0} / W ${f.work_rows || 0}</td></tr>`;
      }).join('') : '<tr><td>—</td><td>лог ще не створювався</td><td>—</td><td>—</td><td>—</td></tr>';
    }'''

new_lf = '''    const sdEl = document.getElementById('sd_status');
    if (sdEl) {
      sdEl.innerHTML = d.sd_card ? '<span class="pill ok">Вставлена (Ready)</span>' : '<span class="pill bad">Відсутня</span>';
    }
    const lf = document.getElementById('logfiles');
    if (lf && window.sdFilesList) {
      if (window.sdFilesList.length === 0) {
        lf.innerHTML = '<tr><td colspan="4" style="text-align:center;color:var(--dim);padding:10px">На MicroSD карті поки немає збережених CSV логів</td></tr>';
      } else {
        lf.innerHTML = window.sdFilesList.map(f => {
          const szKb = (f.size / 1024).toFixed(1) + ' KB';
          return `<tr><td>📄 <b>${escapeHtml(f.name)}</b></td><td>${szKb}</td><td><a href="/download?file=${encodeURIComponent(f.name)}" download class="tab-badge" style="background:#238636;color:#fff;text-decoration:none;padding:5px 12px;border-radius:6px;font-weight:600;display:inline-block">📥 Завантажити з SD</a></td><td><button onclick="analyzeSdFile(\'${encodeURIComponent(f.name)}\')" style="background:#30363d;color:#58a6ff;border:1px solid #30363d;padding:4px 10px;border-radius:6px;cursor:pointer">📊 Відкрити в аналізаторі</button></td></tr>`;
        }).join('');
      }
    }'''

if old_lf in html:
    html = html.replace(old_lf, new_lf)
    print("Replaced old_lf")
else:
    print("old_lf not found or already replaced")

# 3. Add JS helper functions
extra_js = '''
// --- MicroSD Files List & Browser Project Recorder ---
window.sdFilesList = [];
async function refreshSdFiles() {
  try {
    const res = await fetch('/api/logs');
    if (res.ok) {
      const data = await res.json();
      window.sdFilesList = data.files || [];
      const lf = document.getElementById('logfiles');
      if (lf) {
        if (window.sdFilesList.length === 0) {
          lf.innerHTML = '<tr><td colspan="4" style="text-align:center;color:var(--dim);padding:10px">На MicroSD карті немає збережених логів</td></tr>';
        } else {
          lf.innerHTML = window.sdFilesList.map(f => {
            const szKb = (f.size / 1024).toFixed(1) + ' KB';
            return `<tr><td>📄 <b>${escapeHtml(f.name)}</b></td><td>${szKb}</td><td><a href="/download?file=${encodeURIComponent(f.name)}" download class="tab-badge" style="background:#238636;color:#fff;text-decoration:none;padding:5px 12px;border-radius:6px;font-weight:600;display:inline-block">📥 Завантажити з SD</a></td><td><button onclick="analyzeSdFile(\'${encodeURIComponent(f.name)}\')" style="background:#30363d;color:#58a6ff;border:1px solid #30363d;padding:4px 10px;border-radius:6px;cursor:pointer">📊 Відкрити в аналізаторі</button></td></tr>`;
          }).join('');
        }
      }
    }
  } catch(e) { console.error('Error fetching /api/logs', e); }
}

async function analyzeSdFile(filename) {
  try {
    const res = await fetch('/download?file=' + filename);
    if (!res.ok) throw new Error('File download failed');
    const text = await res.text();
    selectTab('logviewer');
    parseAndAnalyzeCSV(decodeURIComponent(filename), text);
  } catch(e) {
    alert('Помилка завантаження логу: ' + e);
  }
}

// Browser Project Recording (saves CSV on user computer)
let isWebRecording = false;
let webLogRows = [];

function toggleWebRecord() {
  const btn = document.getElementById('web_rec_btn');
  if (!isWebRecording) {
    isWebRecording = true;
    webLogRows = [
      'Timestamp_ISO,Avg_Voltage_V,Total_Current_A,Total_Power_kW,Avg_SOC,Online_Packs,Bank_Delta_mV,Min_Cell_mV,Max_Cell_mV'
    ];
    if (btn) {
      btn.style.background = '#da3633';
      btn.textContent = '⏹ Зупинити та зберегти CSV (0)';
    }
  } else {
    isWebRecording = false;
    if (btn) {
      btn.style.background = '#238636';
      btn.textContent = '💻 Запис у проект (Браузер)';
    }
    if (webLogRows.length > 1) {
      const blob = new Blob([webLogRows.join('\\n')], { type: 'text/csv;charset=utf-8;' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      const now = new Date();
      const pad = n => String(n).padStart(2, '0');
      const dtStr = now.getFullYear() + pad(now.getMonth()+1) + pad(now.getDate()) + '_' + pad(now.getHours()) + pad(now.getMinutes()) + pad(now.getSeconds());
      a.href = url;
      a.download = 'bms_web_session_' + dtStr + '.csv';
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    }
  }
}

// Hook into tick to capture data when web recording is active
const originalTick = tick;
tick = async function() {
  await originalTick();
  if (isWebRecording && latestData && latestData.summary) {
    const s = latestData.summary;
    const nowIso = new Date().toISOString();
    const row = `${nowIso},${s.avg_voltage},${s.total_current},${s.total_power_kw},${s.avg_soc},${s.online_packs},${s.bank_diff_mv},${s.min_cell_mv},${s.max_cell_mv}`;
    webLogRows.push(row);
    const btn = document.getElementById('web_rec_btn');
    if (btn) {
      btn.textContent = `⏹ Зупинити та зберегти CSV (${webLogRows.length - 1})`;
    }
  }
};

setTimeout(refreshSdFiles, 1500);
setInterval(refreshSdFiles, 10000);
'''

if 'refreshSdFiles' not in html:
    html = html.replace('</script></body></html>', extra_js + '\n</script></body></html>')
    print("Injected extra_js")

with open('web_extracted.html', 'w', encoding='utf-8') as f:
    f.write(html)

# Generate index_html_gz.h
data = html.encode('utf-8')
compressed = gzip.compress(data, 9)

with open('src/index_html_gz.h', 'w') as f:
    f.write('#ifndef INDEX_HTML_GZ_H\n#define INDEX_HTML_GZ_H\n\n')
    f.write(f'// Exact COMBOX Dashboard HTML with SD Downloader & Web Recorder ({len(data)} bytes -> {len(compressed)} bytes)\n')
    f.write(f'const uint32_t INDEX_HTML_GZ_LEN = {len(compressed)};\n')
    f.write('const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n')
    for i in range(0, len(compressed), 16):
        chunk = compressed[i:i+16]
        hex_str = ', '.join([f'0x{b:02x}' for b in chunk])
        f.write(f'    {hex_str},\n')
    f.write('};\n\n#endif // INDEX_HTML_GZ_H\n')

print(f"Generated src/index_html_gz.h with size {len(compressed)} bytes")

