/* ManyCore UI — frontend logic */
"use strict";

// ── State ─────────────────────────────────────────────────────────────────
const S = {
  runId:           null,
  runStatus:       null,
  pollTimer:       null,
  classes:         [],
  classFilter:     "",
  selectedClass:   null,
  callsites:       [],        // callsite summaries for selected class
  selectedIdx:     null,      // callsite idx
  activeTab:       "targets", // "targets" | "bytecode"
  userPrefixes:    [],        // from run config, used for APP badge + diag classification
};

// ── API helpers ───────────────────────────────────────────────────────────
async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  if (!r.ok) {
    const t = await r.text().catch(() => "");
    throw new Error(`${r.status} ${r.statusText}: ${t.slice(0,200)}`);
  }
  return r.json();
}

async function getText(path) {
  const r = await fetch(path);
  return r.ok ? r.text() : "";
}

// ── Boot ──────────────────────────────────────────────────────────────────
document.addEventListener("DOMContentLoaded", () => {
  wireButtons();
  wireRunForm();
  wireTabs();
  wireClassFilter();
});

function wireButtons() {
  document.getElementById("btn-new-run").addEventListener("click", openRunModal);
  document.getElementById("btn-validate").addEventListener("click", openValidate);
  document.getElementById("btn-output").addEventListener("click", openOutput);
  document.getElementById("btn-diags").addEventListener("click", openDiags);
  document.getElementById("run-selector").addEventListener("change", e => selectRun(e.target.value));
  document.getElementById("run-modal-cancel").addEventListener("click", closeRunModal);
  document.getElementById("validate-modal-close").addEventListener("click", () => hide("validate-modal"));
  document.getElementById("output-modal-close").addEventListener("click", () => hide("output-modal"));
  document.getElementById("diag-modal-close").addEventListener("click", () => hide("diag-modal"));

  // Export dropdown
  document.getElementById("btn-export").addEventListener("click", e => {
    e.stopPropagation();
    document.getElementById("export-menu").classList.toggle("hidden");
  });
  document.getElementById("btn-dl-jsonl").addEventListener("click", () => {
    hide("export-menu");
    triggerDownload(`/api/runs/${S.runId}/download/jsonl`);
  });
  document.getElementById("btn-dl-artifacts").addEventListener("click", () => {
    hide("export-menu");
    triggerDownload(`/api/runs/${S.runId}/download/artifacts`);
  });
  document.getElementById("btn-dl-full").addEventListener("click", () => {
    hide("export-menu");
    triggerDownload(`/api/runs/${S.runId}/download/full`);
  });
  document.addEventListener("click", () => {
    document.getElementById("export-menu").classList.add("hidden");
  });

  // Close modals on overlay click
  for (const id of ["run-modal","validate-modal","output-modal","diag-modal"]) {
    document.getElementById(id).addEventListener("click", e => {
      if (e.target === e.currentTarget) hide(id);
    });
  }
}

function wireRunForm() {
  // run-mode radio toggles main-class field
  document.querySelectorAll("input[name=run-mode]").forEach(r =>
    r.addEventListener("change", () => {
      const mc = document.getElementById("main-class-row");
      mc.style.display = r.value === "main-class" && r.checked ? "block" : "none";
    })
  );
  document.getElementById("run-modal-submit").addEventListener("click", submitRun);
}

function wireTabs() {
  document.querySelectorAll(".tab-btn").forEach(btn =>
    btn.addEventListener("click", () => switchTab(btn.dataset.tab))
  );
}

function wireClassFilter() {
  document.getElementById("class-filter").addEventListener("input", e => {
    S.classFilter = e.target.value.toLowerCase();
    renderClassList();
  });
}

// ── Run selector ──────────────────────────────────────────────────────────
async function selectRun(runId) {
  if (!runId) return;
  S.runId   = runId;
  S.selectedClass = null;
  S.selectedIdx   = null;

  clearMiddle();
  clearRight();

  setEl("run-stats-label", "");
  setEl("run-status-badge", "");

  const run = await api(`/api/runs/${runId}`).catch(() => null);
  if (!run) return;

  S.userPrefixes = run.user_prefixes_list || [];

  updateRunBadge(run);
  showRunStats(run);

  if (run.status === "done" || run.has_jsonl) {
    await loadClasses();
    document.getElementById("btn-validate").disabled = false;
    document.getElementById("btn-output").disabled   = false;
    document.getElementById("btn-diags").disabled    = false;
    document.getElementById("btn-export").disabled   = false;
  } else if (run.status === "running") {
    startPolling();
    document.getElementById("btn-output").disabled = false;
  }
}

function startPolling() {
  if (S.pollTimer) return;
  S.pollTimer = setInterval(async () => {
    if (!S.runId) { stopPolling(); return; }
    const run = await api(`/api/runs/${S.runId}`).catch(() => null);
    if (!run) return;
    updateRunBadge(run);
    showRunStats(run);
    if (run.status !== "running") {
      stopPolling();
      if (run.status === "done") {
        await loadClasses();
        document.getElementById("btn-validate").disabled = false;
        document.getElementById("btn-diags").disabled    = false;
        document.getElementById("btn-export").disabled   = false;
      }
    }
  }, 2000);
}

function stopPolling() {
  clearInterval(S.pollTimer);
  S.pollTimer = null;
}

function updateRunBadge(run) {
  const badge = document.getElementById("run-status-badge");
  badge.className = `status-badge ${run.status}`;
  badge.textContent = run.status.toUpperCase() +
    (run.timed_out ? " (TIMEOUT)" : "") +
    (run.exit_code != null && run.status === "done" ? ` exit=${run.exit_code}` : "") +
    (run.error ? " ERROR" : "");
}

function showRunStats(run) {
  const st = run.stats;
  if (!st) return;
  const es = st.export_summary;
  const complete = es ? (es.complete === false ? "⚠ INCOMPLETE" : "✓ complete") : "";
  setEl("run-stats-label",
    `${st.class_count} classes · ${st.callsite_count} callsites · ${st.diagnostic_count} diags · ${st.total_records} records ${complete}`);
}

// ── Class list ────────────────────────────────────────────────────────────
async function loadClasses() {
  S.classes = await api(`/api/runs/${S.runId}/classes`).catch(() => []);
  renderClassList();
}

function renderClassList() {
  const el = document.getElementById("class-list");
  const f  = S.classFilter;

  const filtered = f
    ? S.classes.filter(c => c.name.toLowerCase().includes(f))
    : S.classes;

  if (!filtered.length) {
    el.innerHTML = `<div class="empty-state"><span class="icon">🔍</span>${f ? "no matches" : "no classes"}</div>`;
    return;
  }

  el.innerHTML = filtered.map(c => {
    const isApp = S.userPrefixes.length > 0 && S.userPrefixes.some(p => c.name.startsWith(p));
    const badges = [
      isApp                ? `<span class="badge badge-app" title="matches user/app prefix">APP</span>` : "",
      c.has_callsites_src  ? `<span class="badge badge-cs" title="has callsites (source)">CS</span>` : "",
      c.has_artifacts      ? `<span class="badge badge-art" title="has bytecode artifacts">ART</span>` : "",
      c.has_diagnostics    ? `<span class="badge badge-diag" title="has diagnostics">⚠</span>` : "",
      c.generated          ? `<span class="badge badge-gen" title="runtime-generated">GEN</span>` : "",
    ].join("");
    const sel = c.name === S.selectedClass ? " selected" : "";
    return `<div class="class-item${sel}" data-class="${esc(c.name)}">
      <span class="name">${esc(shortName(c.name))}</span>
      <span class="badges">${badges}</span>
    </div>`;
  }).join("");

  el.querySelectorAll(".class-item").forEach(row =>
    row.addEventListener("click", () => onClassClick(row.dataset.class))
  );
}

async function onClassClick(className) {
  if (className === S.selectedClass) return;
  S.selectedClass = className;
  S.selectedIdx   = null;
  renderClassList();
  clearRight();
  await loadCallsites(className);
}

// ── Callsite list ─────────────────────────────────────────────────────────
async function loadCallsites(className) {
  const el = document.getElementById("callsite-list");
  el.innerHTML = `<div class="loading"><span class="spinner"></span> loading…</div>`;
  setEl("middle-title", shortName(className));

  S.callsites = await api(`/api/runs/${S.runId}/classes/${encodeURIComponent(className)}/callsites`)
    .catch(() => []);
  renderCallsiteList();
}

function renderCallsiteList() {
  const el = document.getElementById("callsite-list");
  if (!S.callsites.length) {
    el.innerHTML = `<div class="empty-state"><span class="icon">—</span>No callsites for this class</div>`;
    return;
  }

  // Group by method
  const byMethod = new Map();
  for (const cs of S.callsites) {
    const key = `${cs.source_method}||${cs.source_descriptor||""}`;
    if (!byMethod.has(key)) byMethod.set(key, []);
    byMethod.get(key).push(cs);
  }

  let html = "";
  for (const [key, group] of byMethod) {
    const [mname, mdesc] = key.split("||");
    html += `<div class="method-group">
      <div class="method-header">
        <span class="method-name">${esc(mname)}</span>
        <span class="method-desc">${esc(shortDesc(mdesc))}</span>
      </div>`;
    for (const cs of group) {
      html += renderCallsiteRow(cs);
    }
    html += `</div>`;
  }

  el.innerHTML = html;
  el.querySelectorAll(".callsite-row").forEach(row =>
    row.addEventListener("click", () => onCallsiteClick(parseInt(row.dataset.idx)))
  );
}

function renderCallsiteRow(cs) {
  const tag = csTypeTag(cs.record);
  const sel = cs.idx === S.selectedIdx ? " selected" : "";
  const summary = csOneliner(cs);
  const bciStr  = cs.source_bci >= 0 ? `${cs.source_bci}` : "?";
  const opcode  = cs.source_opcode || "";
  return `<div class="callsite-row${sel}" data-idx="${cs.idx}">
    <span class="cs-bci">${esc(bciStr)}</span>
    <span class="cs-opcode">${esc(opcode)}</span>
    <span class="cs-summary">${esc(summary)}</span>
    <span class="cs-type-tag ${tag.cls}">${tag.label}</span>
  </div>`;
}

// ── Callsite detail (right panel) ─────────────────────────────────────────
async function onCallsiteClick(idx) {
  S.selectedIdx = idx;
  renderCallsiteList(); // refresh selection highlight

  const cs = S.callsites.find(c => c.idx === idx);
  if (!cs) return;

  setEl("right-title", `BCI ${cs.source_bci} · ${cs.source_method}`);
  document.getElementById("targets-view").innerHTML = renderTargetsView(cs);
  document.getElementById("bytecode-view").innerHTML =
    `<div class="empty-state"><span class="icon">📄</span>Click "View Bytecode" on a target</div>`;

  switchTab("targets");
  wireTargetViewButtons(cs);
}

function renderTargetsView(cs) {
  let html = `<div class="target-section">
    <h4>Callsite Info</h4>
    <div class="kv-grid">
      <span class="kv-key">Record</span>      <span class="kv-val">${esc(cs.record)}</span>
      <span class="kv-key">Category</span>    <span class="kv-val">${esc(cs.category||"—")}</span>
      <span class="kv-key">Source</span>       <span class="kv-val">${esc(cs.source_class)}.${esc(cs.source_method)}</span>
      <span class="kv-key">BCI</span>          <span class="kv-val">${cs.source_bci}</span>
      <span class="kv-key">Opcode</span>       <span class="kv-val">${esc(cs.source_opcode||"—")}</span>
    </div>
  </div>`;

  if (cs.record === "callsite_target") {
    html += renderSingleTarget(cs);
  } else if (cs.record === "callsite_target_set") {
    html += renderTargetSet(cs);
  } else if (cs.record === "callsite_adapter_graph") {
    html += renderAdapterGraph(cs);
  } else if (cs.record === "diagnostic") {
    html += renderDiagDetail(cs);
  }

  return html;
}

function renderSingleTarget(cs) {
  const t = cs.target || {};
  const ev = cs.evidence || "";
  let html = `<div class="target-section">
    <h4>Target</h4>
    <div class="kv-grid">
      <span class="kv-key">Class</span>       <span class="kv-val">${esc(t.class||"—")}</span>
      <span class="kv-key">Method</span>      <span class="kv-val">${esc(t.method||"—")}</span>
      <span class="kv-key">Descriptor</span>  <span class="kv-val">${esc(t.descriptor||"—")}</span>
      <span class="kv-key">Loader</span>      <span class="kv-val">${esc(t.loader_id||"—")}</span>
      <span class="kv-key">Evidence</span>    <span class="kv-val">${esc(ev||"—")}</span>
    </div>`;

  if (cs.lmf_impl) {
    const li = cs.lmf_impl;
    html += `<div style="margin-top:10px">
      <div class="kv-grid">
        <span class="kv-key" style="grid-column:1/-1;font-weight:700;color:#7c3aed">λ LMF impl</span>
        <span class="kv-key">Class</span>      <span class="kv-val">${esc(li.class||"—")}</span>
        <span class="kv-key">Method</span>     <span class="kv-val">${esc(li.method||"—")}</span>
        <span class="kv-key">Descriptor</span><span class="kv-val">${esc(li.descriptor||"—")}</span>
      </div>
      <div style="margin-top:6px">
        <button class="btn-code btn-sm" data-bc-class="${esc(li.class)}" data-bc-loader="" data-bc-method="${esc(li.method)}">View λ impl bytecode</button>
      </div>
    </div>`;
  }

  if (t.class) {
    html += `<div style="margin-top:8px">
      <button class="btn-code btn-sm" data-bc-class="${esc(t.class)}" data-bc-loader="${esc(t.loader_id||"")}" data-bc-method="${esc(t.method||"")}">View bytecode →</button>
    </div>`;
  }
  html += `</div>`;
  return html;
}

function renderTargetSet(cs) {
  const targets = cs.targets || [];
  const shape   = cs.adapter_shape || "";
  let html = `<div class="target-section">
    <h4>Target Set — ${esc(shape)}</h4>
    <table class="target-set-table">
      <thead><tr><th>Role</th><th>Class</th><th>Method</th><th>Descriptor</th><th></th></tr></thead>
      <tbody>`;
  for (const t of targets) {
    const valid = t.valid !== false;
    html += `<tr>
      <td>${esc(t.role||"—")}</td>
      <td>${esc(shortName(t.class||""))}</td>
      <td>${esc(t.method||"—")}</td>
      <td>${esc(shortDesc(t.descriptor||""))}</td>
      <td>${valid && t.class ? `<button class="btn-code btn-sm" data-bc-class="${esc(t.class)}" data-bc-loader="${esc(t.loader_id||"")}" data-bc-method="${esc(t.method||"")}">View</button>` : ""}</td>
    </tr>`;
  }
  html += `</tbody></table></div>`;
  return html;
}

function renderAdapterGraph(cs) {
  const nodes = cs.nodes || [];
  const kind  = cs.adapter_kind || "";
  const allEx = cs.all_exact;

  let html = `<div class="target-section">
    <h4>Adapter Graph — ${esc(kind)} <span style="font-size:10px;font-weight:400;color:${allEx?"#15803d":"#b45309"}">${allEx?"all_exact=true":"all_exact=false"}</span></h4>
    <div class="node-list">`;

  for (const n of nodes) {
    const cls = n.classification || "unknown";
    const badgeClass = { user_target:"node-badge-user", internal_jdk:"node-badge-jdk", helper_boxing:"node-badge-boxing", helper_adapter:"node-badge-adapter", helper_invoker:"node-badge-adapter" }[cls] || "node-badge-adapter";
    const cardClass  = cls.replace(/_/g,"-").replace("internal-jdk","internal_jdk");
    const exactStr   = n.exact === false ? `<span style="color:#b45309;font-size:10px"> exact=false${n.exact_false_reason ? " ("+n.exact_false_reason+")" : ""}</span>` : "";

    html += `<div class="node-card ${cls}">
      <div style="display:flex;align-items:center;gap:6px;margin-bottom:5px">
        <span class="node-badge ${badgeClass}">${esc(cls)}</span>
        <span style="color:#64748b;font-size:10px">node ${n.id}</span>
        ${exactStr}
      </div>
      <div class="kv-grid">
        <span class="kv-key">Class</span>      <span class="kv-val node-class">${esc(n.class||n.node_adapter_class||"—")}</span>
        <span class="kv-key">Method</span>     <span class="kv-val node-method">${esc(n.method||"—")}</span>
        <span class="kv-key">Descriptor</span><span class="kv-val">${esc(shortDesc(n.descriptor||""))}</span>
      </div>`;

    if (n.class && n.method && n.exact !== false) {
      html += `<div style="margin-top:6px">
        <button class="btn-code btn-sm" data-bc-class="${esc(n.class)}" data-bc-loader="${esc(n.loader_id||"")}" data-bc-method="${esc(n.method||"")}">View bytecode</button>
      </div>`;
    }
    html += `</div>`;
  }

  html += `</div></div>`;
  return html;
}

function renderDiagDetail(cs) {
  return `<div class="target-section">
    <h4>Diagnostic</h4>
    <div class="kv-grid">
      <span class="kv-key">Reason</span>  <span class="kv-val" style="color:#dc2626">${esc(cs.reason||"—")}</span>
      <span class="kv-key">Category</span><span class="kv-val">${esc(cs.category||"—")}</span>
      <span class="kv-key">BCI</span>     <span class="kv-val">${cs.source_bci >= 0 ? cs.source_bci : "—"}</span>
      <span class="kv-key">Opcode</span>  <span class="kv-val">${esc(cs.source_opcode||"—")}</span>
    </div>
    <p style="margin-top:10px;font-size:11px;color:#64748b">This callsite could not be fully resolved. No exact target was recorded for this site.</p>
  </div>`;
}

function wireTargetViewButtons(cs) {
  document.querySelectorAll("[data-bc-class]").forEach(btn => {
    btn.addEventListener("click", async () => {
      const cls    = btn.dataset.bcClass;
      const loader = btn.dataset.bcLoader;
      const method = btn.dataset.bcMethod;
      await loadBytecode(cls, loader, method);
    });
  });
}

// ── Bytecode ──────────────────────────────────────────────────────────────
async function loadBytecode(className, loaderId, targetMethod) {
  switchTab("bytecode");
  const bv = document.getElementById("bytecode-view");
  bv.innerHTML = `<div class="loading"><span class="spinner"></span> fetching artifact…</div>`;

  // Step 1: find artifact
  const artifact = await api(
    `/api/runs/${S.runId}/artifact?class=${encodeURIComponent(className)}&loader_id=${encodeURIComponent(loaderId||"")}`
  ).catch(() => null);

  if (!artifact || !artifact.artifact_path) {
    bv.innerHTML = artifactMissingView(className, artifact);
    return;
  }

  // Step 2: run javap
  const result = await api(
    `/api/runs/${S.runId}/bytecode?artifact_path=${encodeURIComponent(artifact.artifact_path)}`
  ).catch(e => ({ error: e.message }));

  if (result.error) {
    bv.innerHTML = `<div style="padding:12px;color:#dc2626;font-size:12px">javap error: ${esc(result.error)}</div>`;
    return;
  }

  // Artifact metadata header
  let meta = `<div class="target-section">
    <h4>Artifact</h4>
    <div class="kv-grid">
      <span class="kv-key">Class</span>    <span class="kv-val">${esc(className)}</span>
      <span class="kv-key">Method</span>   <span class="kv-val" style="color:#3b82f6">${esc(targetMethod||"(all)")}</span>
      <span class="kv-key">Kind</span>     <span class="kv-val">${esc(artifact.kind||"—")}</span>
      <span class="kv-key">CRC</span>      <span class="kv-val">${esc(artifact.crc||"—")}</span>
      <span class="kv-key">Size</span>     <span class="kv-val">${artifact.size||"?"} bytes</span>
      <span class="kv-key">Path</span>     <span class="kv-val" style="font-size:9px">${esc(artifact.artifact_path)}</span>
    </div>
  </div>`;

  if (artifact.kind === "original") {
    meta += `<div style="margin:0 0 10px;padding:6px 10px;background:#fff7ed;border-radius:4px;font-size:11px;color:#92400e">
      ⚠ This is the <strong>original</strong> (pre-transformation) bytecode. The JVM ran the <strong>final</strong> version.
    </div>`;
  }

  const highlighted = highlightBytecode(result.output, targetMethod);
  bv.innerHTML = meta + `<div id="bytecode-container">${highlighted}</div>`;

  // Scroll to target method
  if (targetMethod) {
    const highlightEl = bv.querySelector(".bc-highlight");
    if (highlightEl) highlightEl.scrollIntoView({ behavior: "smooth", block: "center" });
  }
}

function artifactMissingView(className, artifact) {
  if (!artifact) {
    return `<div style="padding:12px">
      <div style="color:#dc2626;font-weight:600;margin-bottom:8px">No artifact record found</div>
      <p style="font-size:11px;color:#64748b">No bytecode_artifact record exists for class <code>${esc(className)}</code>.</p>
    </div>`;
  }
  return `<div style="padding:12px">
    <div style="color:#d97706;font-weight:600;margin-bottom:8px">Artifact metadata only — file not on disk</div>
    <div class="kv-grid" style="font-size:11px">
      <span class="kv-key">Class</span>  <span class="kv-val">${esc(className)}</span>
      <span class="kv-key">CRC</span>    <span class="kv-val">${esc(artifact.crc||"—")}</span>
      <span class="kv-key">Kind</span>   <span class="kv-val">${esc(artifact.kind||"—")}</span>
    </div>
    <p style="margin-top:8px;font-size:11px;color:#64748b">
      artifact_bytes_dumped=false. The class was recorded in the JSONL but its bytes were not written to disk.<br>
      Check that SOROUSH_CAPTURE_FINAL_BYTECODE=1 was set and SOROUSH_BYTECODE_DUMP_DIR is writable.
    </p>
  </div>`;
}

// Simple bytecode highlighter for javap -verbose output
function highlightBytecode(raw, targetMethod) {
  const lines = raw.split("\n");
  let inTarget = false;
  const out = [];

  for (let i = 0; i < lines.length; i++) {
    let line = escHtml(lines[i]);

    // Detect method headers (lines like "  public static int add(int, int);")
    const isMethodHeader = /^\s{0,2}[\w<(]/.test(lines[i]) && lines[i].includes("(") && !lines[i].trim().startsWith("//") && !lines[i].trim().startsWith("Code:");
    if (isMethodHeader) {
      inTarget = targetMethod && lines[i].includes(targetMethod);
    }

    // Colorise
    if (isMethodHeader) {
      line = `<span class="bc-method-header">${line}</span>`;
    } else if (/^\s+\d+:/.test(lines[i])) {
      // bytecode instruction line
      line = line.replace(/^(\s+)(\d+:)(\s+)(\S+)(.*)$/, (_, sp, off, sp2, instr, rest) =>
        `${sp}<span class="bc-offset">${off}</span>${sp2}<span class="bc-instruction">${instr}</span>` +
        rest.replace(/(\/\/.*)$/, `<span class="bc-comment">$1</span>`)
      );
    }

    if (inTarget) {
      line = `<span class="bc-highlight">${line}</span>`;
    }
    out.push(line);
  }
  return out.join("\n");
}

function escHtml(s) {
  return s.replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
}

// ── Tab switching ─────────────────────────────────────────────────────────
function switchTab(tab) {
  S.activeTab = tab;
  document.querySelectorAll(".tab-btn").forEach(b => b.classList.toggle("active", b.dataset.tab === tab));
  document.querySelectorAll(".tab-content").forEach(c => c.classList.toggle("active", c.id === `${tab}-view`));
}

// ── Run form ──────────────────────────────────────────────────────────────
function openRunModal() { show("run-modal"); }
function closeRunModal() { hide("run-modal"); }

async function submitRun() {
  const jarInput = document.getElementById("jar-input");
  if (!jarInput.files.length) { alert("Please select a JAR file."); return; }

  const runMode = document.querySelector("input[name=run-mode]:checked")?.value || "java-jar";
  const config = {
    run_mode:         runMode,
    main_class:       val("main-class-input"),
    program_args:     val("prog-args-input"),
    jvm_args:         val("jvm-args-input"),
    extra_classpath:  val("extra-cp-input"),
    env_vars:         val("env-vars-input"),
    user_prefixes:    val("user-prefixes-input"),
    mh_walk_depth:    val("mh-depth-input"),
    working_dir:      val("working-dir-input"),
    jdk_path:         val("jdk-path-input"),
    javap_path:       val("javap-path-input"),
    timeout:          parseInt(val("timeout-input") || "60"),
  };

  const fd = new FormData();
  fd.append("jar", jarInput.files[0]);
  fd.append("config", JSON.stringify(config));

  closeRunModal();

  let data;
  try {
    const resp = await fetch("/api/run", { method: "POST", body: fd });
    data = await resp.json();
  } catch (e) {
    alert(`Failed to start run: ${e.message}`);
    return;
  }

  if (data.error) { alert(`Run error: ${data.error}`); return; }

  // Add to selector and select it
  const sel = document.getElementById("run-selector");
  const opt = new Option(`${data.run_id} — running`, data.run_id);
  sel.appendChild(opt);
  sel.value = data.run_id;
  await selectRun(data.run_id);
}

// ── Validate ──────────────────────────────────────────────────────────────
async function openValidate() {
  show("validate-modal");
  document.getElementById("validate-content").innerHTML =
    `<div class="loading"><span class="spinner"></span> running checks…</div>`;
  const result = await api(`/api/runs/${S.runId}/validate`).catch(e => ({error: e.message}));
  document.getElementById("validate-content").innerHTML = renderValidation(result);
}

function renderValidation(result) {
  if (result.error) return `<div style="color:#dc2626">${esc(result.error)}</div>`;
  const cls   = result.overall ? "pass" : "fail";
  const label = result.overall ? `✓ PASS (${result.pass}/${result.pass+result.fail} checks)` : `✗ FAIL (${result.fail} failures)`;
  let html = `<div class="validate-overall ${cls}">${label}</div>`;
  for (const r of result.results||[]) {
    html += `<div class="check-row">
      <span class="check-icon">${r.passed ? "✅" : "❌"}</span>
      <span class="check-name">${esc(r.name)}</span>
      <span class="check-detail">${esc(r.detail||"")}</span>
    </div>`;
  }
  return `<div class="validate-result">${html}</div>`;
}

// ── Output modal ──────────────────────────────────────────────────────────
async function openOutput() {
  show("output-modal");
  const [out, err] = await Promise.all([
    getText(`/api/runs/${S.runId}/stdout`),
    getText(`/api/runs/${S.runId}/stderr`),
  ]);
  const limit = s => s.length > 40000 ? `… [truncated, ${s.length} chars total]\n` + s.slice(-40000) : s;
  document.getElementById("stdout-content").textContent = out || "(empty)";
  document.getElementById("stderr-content").textContent = limit(err) || "(empty)";
}

// ── Diagnostics modal ─────────────────────────────────────────────────────
async function openDiags() {
  show("diag-modal");
  document.getElementById("diag-content").innerHTML =
    `<div class="loading"><span class="spinner"></span> loading…</div>`;
  const diags = await api(`/api/runs/${S.runId}/diagnostics`).catch(() => []);
  document.getElementById("diag-content").innerHTML = renderDiags(diags);
}

function renderDiags(diags) {
  if (!diags.length) return `<div class="loading">No diagnostics.</div>`;

  // Group by source class
  const byClass = new Map();
  for (const d of diags) {
    const k = d.source_class || "(unknown)";
    if (!byClass.has(k)) byClass.set(k, []);
    byClass.get(k).push(d);
  }

  let html = `<p style="margin-bottom:12px;font-size:11px;color:#64748b">${diags.length} diagnostic(s) total. Diagnostics indicate callsites where exact targets could not be resolved.</p>`;

  for (const [cls, group] of byClass) {
    const isDemo = S.userPrefixes.length > 0
      ? S.userPrefixes.some(p => cls.startsWith(p))
      : !cls.startsWith("java/") && !cls.startsWith("jdk/") && !cls.startsWith("sun/");
    html += `<div class="diag-section-header">${esc(shortName(cls))} ${isDemo ? "⚠ user code" : "(JDK internal)"}</div>`;
    for (const d of group) {
      html += `<div class="diag-row">
        <div class="diag-reason">${esc(d.reason||"—")}</div>
        <div class="diag-source">${esc(d.source_method||"?")} · BCI ${d.source_bci} · ${esc(d.category||"")}</div>
      </div>`;
    }
  }
  return html;
}

// ── Helpers ───────────────────────────────────────────────────────────────
function val(id) {
  const el = document.getElementById(id);
  return el ? el.value.trim() : "";
}

function esc(s) {
  return String(s||"").replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;");
}

function setEl(id, html) {
  const el = document.getElementById(id);
  if (el) el.innerHTML = html;
}

function show(id) { document.getElementById(id).classList.remove("hidden"); }
function hide(id) { document.getElementById(id).classList.add("hidden"); }

function clearMiddle() {
  setEl("callsite-list", `<div class="empty-state"><span class="icon">🔍</span>Select a class to see callsites</div>`);
  setEl("middle-title", "Select a class");
  S.callsites = [];
}

function clearRight() {
  setEl("targets-view",  `<div class="empty-state"><span class="icon">🎯</span>Select a callsite to see its targets</div>`);
  setEl("bytecode-view", `<div class="empty-state"><span class="icon">📄</span>Click "View Bytecode" on a target</div>`);
  setEl("right-title", "Select a callsite");
  switchTab("targets");
}

function shortName(s) {
  // java/lang/String -> String (keep last segment)
  const parts = (s||"").split("/");
  return parts[parts.length - 1] || s;
}

function shortDesc(desc) {
  // Abbreviate common descriptor parts for display
  return (desc||"")
    .replace(/Ljava\/lang\//g, "L")
    .replace(/Ljava\/util\//g, "Lutil.")
    .replace(/;/g, "")
    .replace(/\[/g, "[]");
}

function csTypeTag(record) {
  switch (record) {
    case "callsite_target":        return { cls: "ct-tag",   label: "CT"  };
    case "callsite_adapter_graph": return { cls: "cag-tag",  label: "CAG" };
    case "callsite_target_set":    return { cls: "cts-tag",  label: "CTS" };
    case "diagnostic":             return { cls: "diag-tag", label: "DIAG"};
    default:                       return { cls: "ct-tag",   label: record.slice(0,4).toUpperCase() };
  }
}

function triggerDownload(url) {
  const a = document.createElement("a");
  a.href = url;
  a.style.display = "none";
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
}

function csOneliner(cs) {
  if (cs.record === "callsite_target") {
    const t = cs.target || {};
    return t.method ? `→ ${shortName(t.class||"")}.${t.method}` : cs.category || "—";
  }
  if (cs.record === "callsite_adapter_graph") {
    const ns = cs.nodes||[];
    const userNodes = ns.filter(n => n.classification === "user_target").map(n => n.method).filter(Boolean);
    return userNodes.length ? `→ ${userNodes.join(", ")}` : (cs.adapter_kind || "adapter");
  }
  if (cs.record === "callsite_target_set") {
    const ts = cs.targets||[];
    return ts.map(t => t.method||"?").filter(Boolean).join(" / ") || cs.adapter_shape || "target set";
  }
  if (cs.record === "diagnostic") {
    return `⚠ ${cs.reason||"diagnostic"}`;
  }
  return cs.category || cs.record;
}
