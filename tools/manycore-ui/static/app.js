/* Runtime Truth UI — frontend logic */
"use strict";

// ── Class category classification constants ────────────────────────────────
const JDK_PREFIXES = ['java/', 'jdk/', 'sun/', 'com/sun/', 'javax/'];
const FRAMEWORK_PREFIXES = [
  'org/springframework/', 'org/hibernate/', 'com/fasterxml/', 'org/apache/',
  'io/netty/', 'org/slf4j/', 'ch/qos/', 'com/google/', 'io/micrometer/',
  'org/aopalliance/', 'net/bytebuddy/', 'org/objenesis/', 'javassist/',
  'reactor/', 'io/projectreactor/', 'org/reactivestreams/',
];
const PROXY_RE = /\$\$EnhancerBy|\$\$SpringCGLIB|\$\$FastClassBy|\$Proxy\d|HibernateProxy/;

// ── State ─────────────────────────────────────────────────────────────────
const S = {
  runId:           null,
  runStatus:       null,
  pollTimer:       null,
  classes:         [],
  classFilter:     "",
  selectedClass:   null,
  callsites:       [],
  selectedIdx:     null,
  activeTab:       "targets",
  userPrefixes:    [],
  showHelperNodes: true,
  classFilters:    new Set(['app', 'other', 'generated', 'hidden', 'proxy']),
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
  wireJarBrowser();
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

  // Helper-node toggle
  document.getElementById("show-helper-nodes").addEventListener("change", e => {
    S.showHelperNodes = e.target.checked;
    renderCurrentTargets();
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
  const csLabel = st.user_callsite_count != null
    ? `${st.user_callsite_count} user callsites (${st.callsite_count} total)`
    : `${st.callsite_count} callsites`;
  setEl("run-stats-label",
    `${st.class_count} classes · ${csLabel} · ${st.diagnostic_count} diags · ${st.total_records} records ${complete}`);
}

// ── Class list ────────────────────────────────────────────────────────────
async function loadClasses() {
  S.classes = await api(`/api/runs/${S.runId}/classes`).catch(() => []);
  // Default: app + other (user code) + generated + hidden + proxy (direct dispatch targets).
  // Only jdk and framework stay off — those are the real noise (thousands of internal classes).
  S.classFilters = new Set(['app', 'other', 'generated', 'hidden', 'proxy']);
  renderCategoryBar();
  renderClassList();
}

// ── Class category classification ─────────────────────────────────────────

const CAT_META = [
  { key: 'app',       label: 'APP',   title: 'Application / user-prefix classes'             },
  { key: 'generated', label: 'GEN',   title: 'Runtime-generated classes (lambdas, CGLIB)'    },
  { key: 'hidden',    label: 'HID',   title: 'Hidden class instances (+0x…)'                 },
  { key: 'proxy',     label: 'PROXY', title: 'Dynamic proxy classes (CGLIB, $Proxy, ByteBuddy)' },
  { key: 'framework', label: 'FW',    title: 'Framework classes (Spring, Hibernate, etc.)'   },
  { key: 'jdk',       label: 'JDK',   title: 'JDK / platform classes (java.*, jdk.*, sun.*)' },
  { key: 'other',     label: 'Other', title: 'Uncategorized (likely user code without a prefix set)' },
];

function classifyClass(c) {
  const name = c.name;
  if (/\+0x[0-9a-fA-F]+$/.test(name)) return 'hidden';
  if (c.generated) return 'generated';
  if (S.userPrefixes.length > 0 && S.userPrefixes.some(p => name.startsWith(p))) return 'app';
  if (JDK_PREFIXES.some(p => name.startsWith(p))) return 'jdk';
  if (FRAMEWORK_PREFIXES.some(p => name.startsWith(p))) return 'framework';
  if (PROXY_RE.test(name)) return 'proxy';
  return 'other';
}

function renderCategoryBar() {
  const bar = document.getElementById('class-category-bar');
  if (!bar) return;

  // Count per category across all classes (unfiltered by text)
  const counts = {};
  for (const c of S.classes) {
    const cat = classifyClass(c);
    c._cat = cat;
    counts[cat] = (counts[cat] || 0) + 1;
  }

  let html = `<div class="class-filters">`;
  for (const { key, label, title } of CAT_META) {
    const n = counts[key] || 0;
    if (n === 0) continue;
    const active = S.classFilters.has(key) ? ' active' : '';
    html += `<button class="cat-chip cat-${key}${active}" data-cat="${key}" title="${esc(title)}">${esc(label)} <span class="cat-count">${n}</span></button>`;
  }
  const totalVisible = S.classes.filter(c => S.classFilters.has(c._cat || classifyClass(c))).length;
  const totalAll     = S.classes.length;
  if (totalVisible < totalAll) {
    html += `<button class="cat-chip cat-all" id="cat-all-btn" title="Show all ${totalAll} classes">All</button>`;
  }
  html += `</div>`;
  bar.innerHTML = html;

  bar.querySelectorAll('.cat-chip[data-cat]').forEach(btn => {
    btn.addEventListener('click', () => {
      const cat = btn.dataset.cat;
      if (S.classFilters.has(cat)) {
        if (S.classFilters.size > 1) S.classFilters.delete(cat); // keep at least one
      } else {
        S.classFilters.add(cat);
      }
      renderCategoryBar();
      renderClassList();
    });
  });

  const allBtn = bar.querySelector('#cat-all-btn');
  if (allBtn) {
    allBtn.addEventListener('click', () => {
      for (const c of S.classes) S.classFilters.add(c._cat || classifyClass(c));
      renderCategoryBar();
      renderClassList();
    });
  }
}

function renderClassList() {
  const el = document.getElementById("class-list");
  const f  = S.classFilter.toLowerCase();

  const visible = S.classes.filter(c => {
    const cat = c._cat || classifyClass(c);
    c._cat = cat;
    if (!S.classFilters.has(cat)) return false;
    if (f && !c.name.toLowerCase().includes(f)) return false;
    return true;
  });

  if (!visible.length) {
    const reason = f ? "no matches for filter" : "no classes in selected categories";
    el.innerHTML = `<div class="empty-state"><span class="icon">🔍</span>${reason}</div>`;
    return;
  }

  const rows = visible.map(c => {
    const cat   = c._cat;
    const isApp = cat === 'app';
    const scoped = S.userPrefixes.length > 0;
    const showSrc = scoped ? c.has_user_callsites_src : c.has_callsites_src;
    const showTgt = scoped ? c.has_user_callsites_tgt : c.has_callsites_tgt;
    const badges = [
      isApp               ? `<span class="badge badge-app" title="matches user prefix">APP</span>` : "",
      showSrc             ? `<span class="badge badge-src" title="source of callsites">SRC</span>` : "",
      showTgt             ? `<span class="badge badge-tgt" title="dispatch target">TGT</span>` : "",
      c.has_artifacts     ? `<span class="badge badge-art" title="has bytecode artifact">ART</span>` : "",
      c.has_diagnostics   ? `<span class="badge badge-diag" title="has unresolved callsites">⚠</span>` : "",
    ].join("");

    const short = shortName(c.name);
    const pkg   = c.name.includes('/') ? c.name.slice(0, c.name.lastIndexOf('/')) : '';
    const sel   = c.name === S.selectedClass ? " selected" : "";

    return `<div class="class-item${sel}" data-class="${esc(c.name)}" title="${esc(c.name)}">
      <div class="class-item-main">
        <span class="name">${esc(short)}</span>
        <span class="badges">${badges}</span>
      </div>
      ${pkg ? `<div class="pkg">${esc(pkg)}</div>` : ""}
    </div>`;
  }).join("");

  el.innerHTML = rows;

  // Scroll selected class into view without moving the panel
  const selEl = el.querySelector('.class-item.selected');
  if (selEl) selEl.scrollIntoView({ block: 'nearest' });

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

  if (!S.callsites.length) {
    const cls = S.classes.find(c => c.name === className);
    if (cls && cls.has_artifacts) {
      await showArtifactClass(cls);
      return;
    }
    if (cls && !cls.has_artifacts && (cls.record_types || []).includes("runtime_target_ref")) {
      await showLambdaInstanceClass(cls);
      return;
    }
  }
  renderCallsiteList();
}

async function showArtifactClass(cls) {
  const el = document.getElementById("callsite-list");

  const kindBadges = [
    cls.generated       ? `<span class="badge badge-gen" title="runtime-generated">GEN</span>` : "",
    cls.has_callsites_tgt || cls.has_user_callsites_tgt
      ? `<span class="badge badge-tgt" title="appears as dispatch target">TGT</span>` : "",
    `<span class="badge badge-art" title="has bytecode artifact">ART</span>`,
  ].join("");

  el.innerHTML = `<div class="artifact-info-panel">
    <div class="artifact-info-class">${esc(cls.name)}<span class="badges" style="margin-left:6px">${kindBadges}</span></div>
    <div class="artifact-info-note">No source callsites — runtime-generated artifact.<br>Loading bytecode…</div>
  </div>`;

  // Right panel: set title, clear targets placeholder, auto-load bytecode
  setEl("right-title", shortName(cls.name));
  document.getElementById("targets-view").innerHTML =
    `<div class="empty-state"><span class="icon">🔧</span>Runtime-generated class — no source callsites</div>`;
  document.getElementById("bytecode-view").innerHTML =
    `<div class="loading"><span class="spinner"></span> loading artifact…</div>`;
  const toggleWrap = document.getElementById("helper-toggle-wrap");
  if (toggleWrap) toggleWrap.classList.add("hidden");
  switchTab("bytecode");

  await loadBytecode(cls.name, "", null);
}

async function showLambdaInstanceClass(cls) {
  const isInstance = /\+0x[0-9a-fA-F]+$/.test(cls.name);

  const el = document.getElementById("callsite-list");
  const tgtBadge = cls.has_callsites_tgt || cls.has_user_callsites_tgt
    ? `<span class="badge badge-tgt" title="appears as dispatch target">TGT</span>` : "";

  if (isInstance) {
    el.innerHTML = `<div class="artifact-info-panel">
      <div class="artifact-info-class">${esc(cls.name)}<span class="badges" style="margin-left:6px">${tgtBadge}</span></div>
      <div class="artifact-info-note">
        Hidden lambda instance — loading exact bytecode via identity record…
      </div>
    </div>`;
  } else {
    el.innerHTML = `<div class="artifact-info-panel">
      <div class="artifact-info-class">${esc(cls.name)}<span class="badges" style="margin-left:6px">${tgtBadge}</span></div>
      <div class="artifact-info-note">Runtime-only target — no source callsites and no artifact.</div>
    </div>`;
  }

  setEl("right-title", shortName(cls.name));
  const toggleWrap = document.getElementById("helper-toggle-wrap");
  if (toggleWrap) toggleWrap.classList.add("hidden");

  if (isInstance) {
    document.getElementById("targets-view").innerHTML =
      `<div class="empty-state"><span class="icon">λ</span>Hidden lambda instance — exact artifact resolved via identity record</div>`;
    document.getElementById("bytecode-view").innerHTML =
      `<div class="loading"><span class="spinner"></span> loading artifact…</div>`;
    switchTab("bytecode");
    // Use cls.name (+0x... form) directly — backend resolves to the exact CRC-suffixed artifact
    // via the hidden_class_identity JSONL record. No fallback guessing.
    await loadBytecode(cls.name, "", null);
  } else {
    document.getElementById("targets-view").innerHTML =
      `<div class="empty-state"><span class="icon">—</span>Runtime-only target — no artifact</div>`;
    document.getElementById("bytecode-view").innerHTML =
      `<div class="empty-state"><span class="icon">—</span>No bytecode available</div>`;
  }
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
  const tag     = csTypeTag(cs.record);
  const sel     = cs.idx === S.selectedIdx ? " selected" : "";
  const summary = csOneliner(cs);
  const bciStr  = cs.source_bci >= 0 ? `${cs.source_bci}` : "?";
  const opcode  = cs.source_opcode || "";
  const verdict = computeVerdict(cs);
  return `<div class="callsite-row${sel}" data-idx="${cs.idx}">
    <span class="cs-verdict-dot v-${verdict.status}" title="${esc(verdict.status)}"></span>
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

  // Show helper-node toggle only for adapter graphs
  const toggleWrap = document.getElementById("helper-toggle-wrap");
  if (toggleWrap) {
    toggleWrap.classList.toggle("hidden", cs.record !== "callsite_adapter_graph");
  }

  document.getElementById("targets-view").innerHTML = renderTargetsView(cs);
  document.getElementById("bytecode-view").innerHTML =
    `<div class="empty-state"><span class="icon">📄</span>Click "View bytecode" or "View details" on a node</div>`;

  switchTab("targets");
  wireTargetViewButtons(cs);
}

// Re-render the targets view in-place (used when toggle changes without re-selecting the callsite)
function renderCurrentTargets() {
  if (S.selectedIdx == null) return;
  const cs = S.callsites.find(c => c.idx === S.selectedIdx);
  if (!cs) return;
  document.getElementById("targets-view").innerHTML = renderTargetsView(cs);
  wireTargetViewButtons(cs);
}

function renderTargetsView(cs) {
  let html = renderVerdictBanner(cs);

  // Callsite metadata in a collapsible details element
  html += `<details class="verdict-details-toggle">
    <summary>Callsite metadata</summary>
    <div class="kv-grid" style="margin-top:6px">
      <span class="kv-key">Record</span>   <span class="kv-val">${esc(cs.record)}</span>
      <span class="kv-key">Category</span> <span class="kv-val">${esc(cs.category||"—")}</span>
      <span class="kv-key">Source</span>   <span class="kv-val">${esc(cs.source_class)}.${esc(cs.source_method)}</span>
      <span class="kv-key">BCI</span>      <span class="kv-val">${cs.source_bci}</span>
      <span class="kv-key">Opcode</span>   <span class="kv-val">${esc(cs.source_opcode||"—")}</span>
    </div>
  </details>`;

  if (cs.record === "callsite_target") {
    if (cs.category === "invokedynamic") {
      html += renderIndyTargetView(cs);
    } else {
      html += renderSingleTarget(cs);
    }
  } else if (cs.record === "callsite_target_set") {
    html += renderTargetSet(cs);
  } else if (cs.record === "callsite_adapter_graph") {
    html += renderAdapterGraph(cs);
  } else if (cs.record === "diagnostic") {
    html += renderDiagDetail(cs);
  }

  return html;
}

function renderIndyTargetView(cs) {
  const bsm       = cs.bootstrap_method || "";
  const indyName  = cs.indy_name || "";
  const indyDesc  = cs.indy_descriptor || "";
  const isLMF     = cs.lmf_impl != null;
  const isSCF     = cs.semantic_op === "string_concat" || indyName === "makeConcatWithConstants";
  const loader    = cs.source_loader_id || "";

  let html = `<div class="target-section">
    <h4>InvokeDynamic</h4>
    <div class="kv-grid">
      <span class="kv-key">Name</span>        <span class="kv-val">${esc(indyName || "—")}</span>
      <span class="kv-key">Descriptor</span>  <span class="kv-val">${esc(indyDesc || "—")}</span>
      <span class="kv-key">Bootstrap</span>   <span class="kv-val" style="word-break:break-all;font-size:10px">${esc(bsm || "—")}</span>
      <span class="kv-key">Evidence</span>    <span class="kv-val">${esc(cs.evidence || "LINKAGE_GUARANTEED")}</span>
    </div>
  </div>`;

  if (isLMF) {
    const li = cs.lmf_impl;
    html += `<div class="target-section indy-lmf-section">
      <h4>λ LambdaMetafactory Implementation</h4>
      <div class="kv-grid">
        <span class="kv-key">Class</span>      <span class="kv-val node-class">${esc(li.class || "—")}</span>
        <span class="kv-key">Method</span>     <span class="kv-val">${esc(li.method || "—")}</span>
        <span class="kv-key">Descriptor</span><span class="kv-val">${esc(shortDesc(li.descriptor || ""))}</span>
      </div>`;
    if (li.class && li.method) {
      html += `<div style="margin-top:8px">
        <button class="btn-code btn-sm" data-bc-class="${esc(li.class)}" data-bc-loader="${esc(loader)}" data-bc-method="${esc(li.method)}">View impl bytecode</button>
      </div>`;
    }
    html += `</div>`;

  } else if (isSCF) {
    const recipe    = cs.string_concat_recipe;
    const consts    = cs.string_concat_constants;
    const isRecon   = cs.reconstructable === true;
    const isStatic  = cs.staticizable === true;
    const blockers  = Array.isArray(cs.staticization_blockers) ? cs.staticization_blockers : [];

    const recipeDisplay = recipe != null
      ? esc(recipe).replace(//g, '<span class="indy-scf-arg">\\u0001</span>')
                   .replace(//g, '<span class="indy-scf-const">\\u0002</span>')
      : `<span style="color:#94a3b8">not captured</span>`;

    let constsDisplay = `<span style="color:#94a3b8">none</span>`;
    if (Array.isArray(consts) && consts.length > 0) {
      constsDisplay = consts.map(c =>
        typeof c === "string" ? `<code>${esc(c)}</code>` :
        (c && typeof c === "object" && c.type) ? `<span style="color:#94a3b8">[${esc(c.type)}]</span>` :
        String(c)
      ).join(", ");
    } else if (consts != null && !Array.isArray(consts)) {
      constsDisplay = esc(String(consts));
    }

    const blockerNote = blockers.length > 0 ? " — " + blockers.map(esc).join(", ") : "";
    const staticNote  = isStatic ? " — staticizable" : "";

    html += `<div class="target-section indy-scf-section">
      <h4>String Concatenation <span class="node-semantic-op">string_concat</span></h4>
      <div class="kv-grid">
        <span class="kv-key">Recipe</span>    <span class="kv-val indy-recipe">${recipeDisplay}</span>
        <span class="kv-key">Constants</span> <span class="kv-val">${constsDisplay}</span>
        <span class="kv-key">Reconstructable</span>
        <span class="kv-val" style="color:${isRecon?"#15803d":"#dc2626"}">${isRecon ? "✓ yes" + staticNote : "✗ no" + blockerNote}</span>
      </div>
      <p class="indy-note">Composite runtime behavior: StringConcatFactory assembles a MH chain at linkage time. The adapter graph below shows the full helper structure.</p>
    </div>`;

  } else {
    // Generic indy: unknown BSM
    html += `<div class="target-section indy-generic-section">
      <h4>Bootstrap-Composed Callsite</h4>
      <p class="indy-note">No single direct target method. Runtime behavior is entirely determined by the bootstrap method above. See adapter graph (if emitted) for runtime helper structure.</p>
    </div>`;
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

// A node is "structural-only" if it has no bytecode artifact (no class+method).
// These nodes represent runtime MH chain slots: bound values, nested adapters, etc.
function nodeIsStructural(n) {
  if (n.class && n.method) return false;
  if (n.artifact_path)     return false;
  return true;
}

// nodeIsHideable: kept for backward compat with toggle; hides structural nodes when
// S.showHelperNodes is false.
function nodeIsHideable(n) {
  if ((n.classification || "") === "user_target") return false;
  return nodeIsStructural(n);
}

const NODE_BADGE_MAP = {
  user_target:      "node-badge-user",
  internal_jdk:     "node-badge-jdk",
  helper_boxing:    "node-badge-boxing",
  helper_adapter:   "node-badge-adapter",
  helper_invoker:   "node-badge-adapter",
  helper_reflection:"node-badge-helper",
  bound_data:       "node-badge-data",
};

function renderAdapterGraph(cs) {
  const allNodes    = cs.nodes || [];
  const kind        = cs.adapter_kind || "";
  const allEx       = cs.all_exact;
  const nodes       = S.showHelperNodes ? allNodes : allNodes.filter(n => !nodeIsHideable(n));
  const hiddenCount = allNodes.length - nodes.length;

  let html = `<div class="target-section">
    <h4>Adapter Graph — ${esc(kind)} <span style="font-size:10px;font-weight:400;color:${allEx?"#15803d":"#b45309"}">${allEx?"all_exact=true":"all_exact=false"}</span></h4>
    <div class="node-list">`;

  if (nodes.length === 0 && hiddenCount > 0) {
    const structCls = [...new Set(allNodes.filter(nodeIsStructural).map(n => n.classification||"unknown"))];
    html += `<div class="empty-state" style="margin:8px 0">
      <span class="icon">📦</span>
      ${hiddenCount} structural node(s) hidden (${structCls.join(", ")}) — enable "Show all" to view runtime-structure details
    </div>`;
  } else {
    for (const n of nodes) {
      html += renderNodeCard(n, cs);
    }
  }

  if (hiddenCount > 0 && !S.showHelperNodes) {
    const structCls = [...new Set(allNodes.filter(nodeIsStructural).map(n => n.classification||"unknown"))];
    html += `<div class="hidden-nodes-bar">Collapsed: ${hiddenCount} structural node(s) — ${structCls.join(", ")} (enable "Show all" to view)</div>`;
  }

  html += `</div></div>`;
  return html;
}

function renderNodeCard(n, cs) {
  const cls        = n.classification || "unknown";
  const isStruct   = nodeIsStructural(n);
  const badgeClass = NODE_BADGE_MAP[cls] || "node-badge-helper";
  // helper_adapter nodes are BMH containers with no standalone bytecode; their
  // node-level exact=false is structural and does not indicate graph inexactness.
  const exactStr = (cls === "helper_adapter")
    ? `<span style="color:#64748b;font-size:10px"> · container</span>`
    : (n.exact === false
      ? `<span style="color:#b45309;font-size:10px"> ✗ exact=false${n.exact_false_reason ? " ("+n.exact_false_reason+")" : ""}</span>`
      : "");
  const semOp  = n.semantic_op
    ? `<span class="node-semantic-op">${esc(n.semantic_op)}</span>` : "";
  const typeOp = (n.from_type && n.to_type)
    ? `<span class="node-type-cast">${esc(n.from_type)} → ${esc(n.to_type)}</span>` : "";

  // ── kv rows depend on node type ──────────────────────────────────────────
  let kvRows = "";
  if (!isStruct) {
    // Has bytecode — standard class/method/descriptor
    kvRows = `
      <span class="kv-key">Class</span>      <span class="kv-val node-class">${esc(n.class||"—")}</span>
      <span class="kv-key">Method</span>     <span class="kv-val node-method">${esc(n.method||"—")}</span>
      <span class="kv-key">Descriptor</span><span class="kv-val">${esc(shortDesc(n.descriptor||""))}</span>`;
    if (n.to_descriptor)
      kvRows += `<span class="kv-key">Outer type</span><span class="kv-val" style="color:#64748b">${esc(n.to_descriptor)}</span>`;
  } else if (cls === "bound_data") {
    const species = shortCls(n.adapter_class || n.node_adapter_class || cs.adapter_class || "");
    kvRows = `
      <span class="kv-key">Type</span>       <span class="kv-val">bound non-MH argument</span>
      ${species ? `<span class="kv-key">BMH species</span><span class="kv-val node-class">${esc(species)}</span>` : ""}
      ${n.exact_false_reason ? `<span class="kv-key">Reason</span><span class="kv-val" style="color:#b45309">${esc(n.exact_false_reason)}</span>` : ""}`;
  } else if (cls === "helper_adapter") {
    const species = shortCls(n.adapter_class || n.node_adapter_class || "");
    kvRows = `
      <span class="kv-key">Type</span><span class="kv-val" style="color:#64748b">Container node: no standalone bytecode; behavior lives in child nodes.</span>
      ${species ? `<span class="kv-key">BMH species</span><span class="kv-val node-class">${esc(species)}</span>` : ""}
      ${n.to_descriptor ? `<span class="kv-key">Inner type</span><span class="kv-val" style="font-family:ui-monospace,monospace;font-size:10px">${esc(n.to_descriptor)}</span>` : ""}`;
  } else {
    kvRows = `
      <span class="kv-key">Role</span>  <span class="kv-val">${esc(n.role||"—")}</span>
      ${n.exact_false_reason ? `<span class="kv-key">Reason</span><span class="kv-val" style="color:#b45309">${esc(n.exact_false_reason)}</span>` : ""}`;
  }

  // ── action buttons ────────────────────────────────────────────────────────
  let btns = "";
  if (!isStruct && n.exact !== false) {
    btns = `<button class="btn-code btn-sm" data-bc-class="${esc(n.class)}" data-bc-loader="${esc(n.loader_id||"")}" data-bc-method="${esc(n.method||"")}">View bytecode</button>`;
  }
  if (isStruct) {
    btns = `<button class="btn-details btn-sm" data-detail-nodeid="${n.id}" data-callsite-idx="${cs.idx}">View details</button>`;
  }

  return `<div class="node-card ${cls}${isStruct ? " structural" : ""}">
    <div style="display:flex;align-items:center;gap:6px;margin-bottom:5px;flex-wrap:wrap">
      <span class="node-badge ${badgeClass}">${esc(cls)}</span>
      <span style="color:#64748b;font-size:10px">node ${n.id} · ${esc(n.role||"—")}</span>
      ${semOp}${typeOp}${exactStr}
    </div>
    <div class="kv-grid">${kvRows}</div>
    ${btns ? `<div style="margin-top:6px">${btns}</div>` : ""}
  </div>`;
}

function renderDiagDetail(cs) {
  let primaryHtml = "";
  if (cs.linked_primary_bci != null && cs.linked_primary_bci >= 0) {
    const t = cs.linked_primary_target;
    const tStr = t
      ? `${esc(t.class)}.${esc(t.method)}${esc(t.descriptor)}`
      : "(unknown)";
    primaryHtml = `
      <div style="margin-top:10px;padding:8px;background:#f0fdf4;border:1px solid #86efac;border-radius:4px;font-size:11px">
        <strong>Sibling callsite</strong> — shares CP entry #${cs.source_cp_index >= 0 ? cs.source_cp_index : "?"} with BCI ${cs.linked_primary_bci}.<br>
        Target previously resolved as: <code>${tStr}</code><br>
        <span style="color:#64748b">This BCI executed after the CP cache entry was already populated; exact receiver not re-captured.</span>
      </div>`;
  }
  return `<div class="target-section">
    <h4>Diagnostic</h4>
    <div class="kv-grid">
      <span class="kv-key">Reason</span>   <span class="kv-val" style="color:#dc2626">${esc(cs.reason||"—")}</span>
      <span class="kv-key">Category</span> <span class="kv-val">${esc(cs.category||"—")}</span>
      <span class="kv-key">BCI</span>      <span class="kv-val">${cs.source_bci >= 0 ? cs.source_bci : "—"}</span>
      <span class="kv-key">Opcode</span>   <span class="kv-val">${esc(cs.source_opcode||"—")}</span>
      <span class="kv-key">CP index</span> <span class="kv-val">${cs.source_cp_index >= 0 ? cs.source_cp_index : "—"}</span>
    </div>
    ${primaryHtml}
    <p style="margin-top:10px;font-size:11px;color:#64748b">This callsite could not be fully resolved. No exact target was recorded for this site.</p>
  </div>`;
}

function wireTargetViewButtons(cs) {
  document.querySelectorAll("[data-bc-class]").forEach(btn => {
    btn.addEventListener("click", async () => {
      await loadBytecode(btn.dataset.bcClass, btn.dataset.bcLoader, btn.dataset.bcMethod);
    });
  });
  document.querySelectorAll("[data-detail-nodeid]").forEach(btn => {
    btn.addEventListener("click", () => {
      loadNodeDetails(parseInt(btn.dataset.detailNodeid, 10), parseInt(btn.dataset.callsiteIdx, 10));
    });
  });
}

// ── Node details (structural/runtime-structure nodes) ─────────────────────
function loadNodeDetails(nodeId, csIdx) {
  switchTab("bytecode");
  const bv = document.getElementById("bytecode-view");
  const cs = S.callsites.find(c => c.idx === csIdx);
  if (!cs) { bv.innerHTML = `<div style="padding:12px;color:#dc2626">Callsite ${csIdx} not found</div>`; return; }
  const n = (cs.nodes || []).find(nd => nd.id === nodeId);
  if (!n) { bv.innerHTML = `<div style="padding:12px;color:#dc2626">Node ${nodeId} not found</div>`; return; }
  bv.innerHTML = renderNodeDetailsPanel(n, cs);
}

function renderNodeDetailsPanel(n, cs) {
  const cls = n.classification || "unknown";
  const badgeClass = NODE_BADGE_MAP[cls] || "node-badge-helper";

  const CLS_EXPLAIN = {
    bound_data:
      "A bound non-MethodHandle argument captured in a BoundMethodHandle slot. " +
      "This node holds a concrete value (primitive or object) fixed at linkage time — it has no bytecode.",
    helper_adapter:
      "Container node: a BoundMethodHandle Species wrapper with no standalone bytecode. " +
      "Its node-level exact=false is structural — it means this wrapper has no direct DMH target, " +
      "not that the graph is inexact. Behavior lives in child nodes reachable via the edges from this container.",
    helper_invoker:
      "A JVM-internal MH invoker trampoline. Dispatches calls through the MH chain but is " +
      "internal JDK infrastructure with no user-visible bytecode.",
    helper_boxing:
      "A boxing/unboxing helper inserted by the MH framework to convert between primitive and " +
      "reference types in the adapter chain.",
    helper_reflection:
      "A reflective dispatch helper (e.g. java.lang.reflect.Method or MethodHandle.invoke internals).",
    internal_jdk:
      "A JDK-internal helper method. Bytecode exists but the class was classified as JDK infrastructure.",
  };
  const explain = CLS_EXPLAIN[cls] || `Runtime-structure node (${cls}). No bytecode artifact.`;

  // Runtime payload rows
  let payloadRows = "";
  const addP = (k, v) => { payloadRows += `<span class="kv-key">${k}</span><span class="kv-val">${v}</span>`; };
  addP("Node ID",        esc(String(n.id)));
  addP("Role",           esc(n.role || "—"));
  if (cls === "helper_adapter") {
    addP("Node type", `<span style="color:#64748b">Container node: no standalone bytecode; behavior lives in child nodes.</span>`);
  } else {
    addP("Exact",
      n.exact === false
        ? `<span style="color:#b45309">✗ false${n.exact_false_reason ? " — " + esc(n.exact_false_reason) : ""}</span>`
        : `<span style="color:#15803d">✓ true</span>`);
  }
  if (n.semantic_op)
    addP("Semantic op", `<span class="node-semantic-op">${esc(n.semantic_op)}</span>`);
  if (n.from_type && n.to_type)
    addP("Type cast", `<span class="node-type-cast">${esc(n.from_type)} → ${esc(n.to_type)}</span>`);
  const species = n.adapter_class || n.node_adapter_class || (cls === "bound_data" ? cs.adapter_class : "") || "";
  if (species)
    addP("BMH species", `<span class="node-class">${esc(shortCls(species))}</span>`);
  if (n.to_descriptor)
    addP("Inner type", `<code style="font-family:ui-monospace,monospace;font-size:10px">${esc(n.to_descriptor)}</code>`);

  // Graph context
  let graphRows = "";
  const addG = (k, v) => { graphRows += `<span class="kv-key">${k}</span><span class="kv-val">${v}</span>`; };
  addG("Kind",             esc(cs.adapter_kind || "—"));
  if (cs.adapter_class) addG("Top-level species", esc(shortCls(cs.adapter_class)));
  addG("Graph all_exact",      cs.all_exact    ? `<span style="color:#15803d">✓ yes</span>` : `<span style="color:#b45309">✗ no</span>`);
  addG("Graph staticizable",   cs.staticizable ? `<span style="color:#15803d">✓ yes</span>` : `<span style="color:#b45309">✗ no</span>`);

  // Edges involving this node
  const edges = cs.edges || [];
  const myEdges = edges.filter(e => {
    const f = e.from_node_id ?? e.from;
    const t = e.to_node_id   ?? e.to;
    return f === n.id || t === n.id;
  });
  let edgeHtml;
  if (myEdges.length > 0) {
    edgeHtml = `<table style="width:100%;border-collapse:collapse;font-size:10px;font-family:ui-monospace,monospace">
      <thead><tr>
        <th style="text-align:left;padding:3px 6px;color:#64748b;border-bottom:1px solid #e2e8f0">From</th>
        <th style="text-align:left;padding:3px 6px;color:#64748b;border-bottom:1px solid #e2e8f0">To</th>
        <th style="text-align:left;padding:3px 6px;color:#64748b;border-bottom:1px solid #e2e8f0">Label</th>
      </tr></thead><tbody>`;
    for (const e of myEdges) {
      const f  = e.from_node_id ?? e.from;
      const t  = e.to_node_id   ?? e.to;
      const lb = e.label || e.edge_label || "—";
      const fN = (cs.nodes || []).find(nd => nd.id === f);
      const tN = (cs.nodes || []).find(nd => nd.id === t);
      const fStr = f === n.id ? `<strong>node ${f}</strong>` : `node ${f}${fN ? " (" + esc(fN.role||"") + ")" : ""}`;
      const tStr = t === n.id ? `<strong>node ${t}</strong>` : `node ${t}${tN ? " (" + esc(tN.role||"") + ")" : ""}`;
      edgeHtml += `<tr style="border-bottom:1px solid #f1f5f9">
        <td style="padding:3px 6px">${fStr}</td>
        <td style="padding:3px 6px">${tStr}</td>
        <td style="padding:3px 6px;color:#64748b">${esc(lb)}</td>
      </tr>`;
    }
    edgeHtml += `</tbody></table>`;
  } else {
    edgeHtml = `<p style="font-size:11px;color:#94a3b8;font-style:italic">No edge data recorded for this node.</p>`;
  }

  // Sibling nodes list
  const allNodes = cs.nodes || [];
  let siblingsHtml = `<div style="display:flex;flex-direction:column;gap:3px">`;
  for (const sib of allNodes) {
    const sibCls   = sib.classification || "unknown";
    const sibBadge = NODE_BADGE_MAP[sibCls] || "node-badge-helper";
    const isCur    = sib.id === n.id;
    const sibStyle = isCur
      ? "background:#dbeafe;border:1px solid #93c5fd"
      : "background:#f8fafc;border:1px solid #e2e8f0";
    siblingsHtml += `<div style="${sibStyle};border-radius:4px;padding:4px 8px;font-size:10px;font-family:ui-monospace,monospace">
      <span class="node-badge ${sibBadge}" style="font-size:8px;padding:1px 4px;border-radius:2px">${esc(sibCls)}</span>
      <span style="color:#64748b;margin-left:4px">node ${sib.id}</span>
      <span style="color:#334155;margin-left:4px">${esc(sib.role || sib.method || "—")}</span>
      ${isCur ? `<span style="color:#1d4ed8;margin-left:6px;font-weight:700">← this node</span>` : ""}
    </div>`;
  }
  siblingsHtml += `</div>`;

  return `
    <div class="target-section">
      <h4><span class="node-badge ${badgeClass}" style="font-size:9px;padding:1px 5px;border-radius:3px;vertical-align:middle">${esc(cls)}</span>&nbsp; Node Details</h4>
      <p style="margin-bottom:8px;font-size:11px;color:#64748b;font-style:italic;line-height:1.5">${explain}</p>
    </div>
    <div class="target-section">
      <h4>Runtime Payload</h4>
      <div class="kv-grid">${payloadRows}</div>
    </div>
    <div class="target-section">
      <h4>Graph Context</h4>
      <div class="kv-grid">${graphRows}</div>
    </div>
    <div class="target-section">
      <h4>Graph Edges (this node)</h4>
      ${edgeHtml}
    </div>
    <div class="target-section">
      <h4>All Nodes in Graph</h4>
      ${siblingsHtml}
    </div>`;
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
    const isHidden = /\+0x[0-9a-fA-F]+$/.test(className);
    if (isHidden) {
      return `<div style="padding:12px">
        <div style="color:#dc2626;font-weight:600;margin-bottom:8px">Capture/export bug — no identity record</div>
        <p style="font-size:11px;color:#64748b">
          No <code>hidden_class_identity</code> record was emitted for <code>${esc(className)}</code>.<br>
          The JVM did not record the CRC mapping for this hidden class instance.<br>
          Check that the provenance export ran after class load and that <code>soroush_graph_hidden_class_id()</code> was called.
        </p>
      </div>`;
    }
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
function openRunModal() { resetJarBrowser(); show("run-modal"); }
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
  setEl("bytecode-view", `<div class="empty-state"><span class="icon">📄</span>Click "View bytecode" or "View details" on a node</div>`);
  setEl("right-title", "Select a callsite");
  switchTab("targets");
}

function shortName(s) {
  // java/lang/String -> String (keep last segment)
  const parts = (s||"").split("/");
  return parts[parts.length - 1] || s;
}

function shortCls(s) {
  if (!s) return "—";
  const p = s.lastIndexOf("/");
  return p >= 0 ? s.slice(p + 1) : s;
}

function shortDesc(desc) {
  // Abbreviate common descriptor parts for display
  return (desc||"")
    .replace(/Ljava\/lang\//g, "L")
    .replace(/Ljava\/util\//g, "Lutil.")
    .replace(/;/g, "")
    .replace(/\[/g, "[]");
}

// ── Staticization verdict ─────────────────────────────────────────────────

function computeVerdict(cs) {
  const rec = cs.record;

  if (rec === "diagnostic") {
    return { status: "unknown", evidence: cs.reason || "could not resolve target", targets: 0, blockers: [], note: "" };
  }

  if (rec === "callsite_target") {
    // Indy path has explicit staticizable flag from the JVM
    if (cs.staticizable === true) {
      return { status: "staticizable", evidence: cs.evidence || "exact", targets: 1, blockers: [], note: "" };
    }
    if (cs.staticizable === false) {
      const bl = Array.isArray(cs.staticization_blockers) ? cs.staticization_blockers : [];
      return { status: "blocked", evidence: cs.evidence || "", targets: 1, blockers: bl.length ? bl : ["not staticizable"], note: "" };
    }
    // Non-indy: use all_exact to distinguish guaranteed vs observed-only
    if (cs.category !== "invokedynamic") {
      if (cs.all_exact) {
        return { status: "staticizable", evidence: cs.evidence || "exact", targets: 1, blockers: [], note: "" };
      }
      // exact=false on a single callsite_target record means one target was observed but
      // resolution was not exact (profile-based / observed-only). This is not the same as
      // blocked — it means the single-target assumption has not been formally proven.
      return {
        status:   "likely",
        evidence: cs.evidence || "observed",
        targets:  1,
        blockers: [],
        note:     "1 target observed; resolution not exact — may be polymorphic at other call sites",
      };
    }
    // Indy without explicit staticizable: unknown
    return { status: "unknown", evidence: cs.evidence || "", targets: 1, blockers: [], note: "" };
  }

  if (rec === "callsite_target_set") {
    const n = (cs.targets || []).length;
    if (n === 1) return { status: "staticizable", evidence: "single target", targets: 1, blockers: [], note: "" };
    return { status: "blocked", evidence: "", targets: n, blockers: [`${n} observed targets — polymorphic dispatch`], note: "" };
  }

  if (rec === "callsite_adapter_graph") {
    const userTargets = (cs.nodes || []).filter(n => n.classification === "user_target").length;
    if (cs.staticizable === true) {
      return { status: "staticizable", evidence: cs.adapter_kind || "adapter graph", targets: userTargets, blockers: [], note: "" };
    }
    if (cs.staticizable === false) {
      const bl = Array.isArray(cs.staticization_blockers) ? cs.staticization_blockers : ["not staticizable"];
      return { status: "blocked", evidence: cs.adapter_kind || "", targets: userTargets, blockers: bl, note: "" };
    }
    if (cs.all_exact) {
      return { status: "staticizable", evidence: "all_exact", targets: userTargets, blockers: [], note: "" };
    }
    return {
      status:   "likely",
      evidence: cs.adapter_kind || "adapter graph",
      targets:  userTargets,
      blockers: [],
      note:     "adapter graph resolved but not all nodes exact",
    };
  }

  return { status: "unknown", evidence: "", targets: 0, blockers: [], note: "" };
}

function renderVerdictBanner(cs) {
  const v = computeVerdict(cs);
  const icons  = { staticizable: "✓", likely: "~", blocked: "✗", unknown: "?" };
  const labels = {
    staticizable: "STATICIZABLE",
    likely:       "LIKELY STATICIZABLE",
    blocked:      "BLOCKED",
    unknown:      "UNKNOWN",
  };

  const parts = [];
  if (v.targets > 0) parts.push(`${v.targets} target${v.targets !== 1 ? "s" : ""}`);
  if (v.evidence)    parts.push(`evidence: ${v.evidence}`);
  const opMeta = cs.source_opcode ? `${cs.source_opcode} @ BCI ${cs.source_bci >= 0 ? cs.source_bci : "?"}` : "";
  if (opMeta) parts.push(opMeta);

  const blockersHtml = v.blockers.length
    ? `<div class="verdict-blockers">${v.blockers.map(b => `<span class="verdict-blocker">${esc(b)}</span>`).join("")}</div>`
    : "";

  const noteHtml = v.note
    ? `<div class="verdict-note">${esc(v.note)}</div>`
    : "";

  return `<div class="verdict-banner verdict-${v.status}">
    <span class="verdict-icon">${icons[v.status]}</span>
    <div class="verdict-body">
      <div class="verdict-title">${labels[v.status]}</div>
      <div class="verdict-meta">${esc(parts.join(" · "))}</div>
      ${noteHtml}
      ${blockersHtml}
    </div>
  </div>`;
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

// ── JAR Class Browser ─────────────────────────────────────────────────────

// Isolated state for the JAR browser (separate from run-time S).
const Jar = {
  classes:  [],        // [{full, pkg, short}]  — from last inspected JAR
  selected: new Set(), // full class names currently checked
  filter:   "",        // live search string
};

function wireJarBrowser() {
  document.getElementById("jar-input").addEventListener("change", e => {
    const f = e.target.files[0];
    if (f) onJarFileSelected(f); else resetJarBrowser();
  });
  document.getElementById("jar-class-filter").addEventListener("input", e => {
    Jar.filter = e.target.value.toLowerCase();
    renderJarTree();
  });
  document.getElementById("jar-select-all").addEventListener("click", jarSelectAllVisible);
  document.getElementById("jar-clear-all").addEventListener("click",   jarClearAllVisible);

  const treeEl = document.getElementById("jar-class-tree");
  // Toggle package expand/collapse on header click (but not on the checkbox)
  treeEl.addEventListener("click", e => {
    const hdr = e.target.closest(".pkg-header");
    if (!hdr || e.target.closest("input")) return;
    const body = hdr.nextElementSibling;
    if (!body) return;
    body.classList.toggle("collapsed");
    const icon = hdr.querySelector(".pkg-toggle");
    if (icon) icon.textContent = body.classList.contains("collapsed") ? "▸" : "▾";
  });
  // Checkbox state changes via event delegation
  treeEl.addEventListener("change", e => {
    if (e.target.classList.contains("cls-cb")) {
      const full = e.target.dataset.cls;
      if (e.target.checked) Jar.selected.add(full);
      else                  Jar.selected.delete(full);
      const c = Jar.classes.find(x => x.full === full);
      if (c) refreshPkgCheckbox(c.pkg);
      updateJarPreview();
    } else if (e.target.classList.contains("pkg-cb")) {
      const pkg     = e.target.dataset.pkg;
      const checked = e.target.checked;
      // Toggle all VISIBLE class-cbs that belong to this package
      treeEl.querySelectorAll(`.cls-cb`).forEach(cb => {
        const c = Jar.classes.find(x => x.full === cb.dataset.cls);
        if (c && c.pkg === pkg) {
          cb.checked = checked;
          if (checked) Jar.selected.add(c.full);
          else         Jar.selected.delete(c.full);
        }
      });
      e.target.indeterminate = false;
      updateJarPreview();
    }
  });
}

async function onJarFileSelected(file) {
  resetJarBrowser();
  show("jar-classes-section");
  document.getElementById("jar-class-tree").innerHTML =
    `<div class="loading"><span class="spinner"></span> reading JAR…</div>`;

  const fd = new FormData();
  fd.append("jar", file);
  try {
    const resp = await fetch("/api/jar/classes", { method: "POST", body: fd });
    const data = await resp.json();
    if (data.error) {
      document.getElementById("jar-class-tree").innerHTML =
        `<div class="jar-error">⚠ ${esc(data.error)}</div>`;
      return;
    }
    Jar.classes = data.classes.map(full => ({
      full,
      pkg:   full.includes("/") ? full.slice(0, full.lastIndexOf("/")) : "",
      short: full.includes("/") ? full.slice(full.lastIndexOf("/") + 1) : full,
    }));
    document.getElementById("jar-browser-title").textContent =
      `${Jar.classes.length} classes in JAR`;
    renderJarTree();
  } catch (e) {
    document.getElementById("jar-class-tree").innerHTML =
      `<div class="jar-error">⚠ Could not read JAR: ${esc(e.message)}</div>`;
  }
}

function resetJarBrowser() {
  Jar.classes = [];
  Jar.selected.clear();
  Jar.filter = "";
  const fi = document.getElementById("jar-class-filter");
  if (fi) fi.value = "";
  const t = document.getElementById("jar-class-tree");
  if (t) t.innerHTML = "";
  const p = document.getElementById("jar-prefix-preview");
  if (p) p.textContent = "";
  syncPrefixesToTextarea();
  hide("jar-classes-section");
}

function renderJarTree() {
  const treeEl = document.getElementById("jar-class-tree");
  const f = Jar.filter;

  // Group visible classes by package
  const pkgMap = new Map();
  for (const c of Jar.classes) {
    if (f && !c.full.toLowerCase().includes(f)) continue;
    if (!pkgMap.has(c.pkg)) pkgMap.set(c.pkg, []);
    pkgMap.get(c.pkg).push(c);
  }

  if (pkgMap.size === 0) {
    treeEl.innerHTML = `<div class="loading">No classes match.</div>`;
    updateJarPreview();
    return;
  }

  const pkgs = [...pkgMap.keys()].sort();
  let html = "";
  for (const pkg of pkgs) {
    const classes   = pkgMap.get(pkg);
    const selCount  = classes.filter(c => Jar.selected.has(c.full)).length;
    const allSel    = selCount === classes.length;
    const someSel   = selCount > 0 && selCount < classes.length;
    const pkgLabel  = pkg ? pkg.replace(/\//g, ".") : "(default package)";
    const pkgAttr   = esc(pkg);

    html += `<div class="pkg-group">
      <div class="pkg-header">
        <input type="checkbox" class="pkg-cb" data-pkg="${pkgAttr}"
          ${allSel ? "checked" : ""} ${someSel ? "data-indet" : ""}>
        <span class="pkg-name" title="${pkgAttr}">${esc(pkgLabel)}</span>
        <span class="pkg-count">${selCount}/${classes.length}</span>
        <span class="pkg-toggle">▾</span>
      </div>
      <div class="pkg-body">`;

    for (const c of classes) {
      html += `<label class="cls-row">
        <input type="checkbox" class="cls-cb" data-cls="${esc(c.full)}"
          ${Jar.selected.has(c.full) ? "checked" : ""}>
        <span class="cls-name" title="${esc(c.full)}">${esc(c.short)}</span>
      </label>`;
    }
    html += `</div></div>`;
  }

  treeEl.innerHTML = html;

  // Apply indeterminate state (can't be set via HTML attribute)
  treeEl.querySelectorAll(".pkg-cb[data-indet]").forEach(cb => {
    cb.indeterminate = true;
  });

  updateJarPreview();
}

// Recompute a single package checkbox's tri-state from current DOM
function refreshPkgCheckbox(pkg) {
  const treeEl = document.getElementById("jar-class-tree");
  const pkgCb  = treeEl.querySelector(`.pkg-cb[data-pkg="${CSS.escape(pkg)}"]`);
  if (!pkgCb) return;

  const clsCbs    = [...treeEl.querySelectorAll(".cls-cb")].filter(cb => {
    const c = Jar.classes.find(x => x.full === cb.dataset.cls);
    return c && c.pkg === pkg;
  });
  const nChecked  = clsCbs.filter(cb => cb.checked).length;
  pkgCb.checked       = nChecked === clsCbs.length && clsCbs.length > 0;
  pkgCb.indeterminate = nChecked > 0 && nChecked < clsCbs.length;

  const countEl = pkgCb.closest(".pkg-header")?.querySelector(".pkg-count");
  if (countEl) countEl.textContent = `${nChecked}/${clsCbs.length}`;
}

function jarSelectAllVisible() {
  const treeEl = document.getElementById("jar-class-tree");
  treeEl.querySelectorAll(".cls-cb").forEach(cb => {
    cb.checked = true;
    Jar.selected.add(cb.dataset.cls);
  });
  treeEl.querySelectorAll(".pkg-cb").forEach(cb => {
    cb.checked = true;
    cb.indeterminate = false;
    const countEl = cb.closest(".pkg-header")?.querySelector(".pkg-count");
    if (countEl) {
      const total = cb.closest(".pkg-group")?.querySelectorAll(".cls-cb").length || 0;
      countEl.textContent = `${total}/${total}`;
    }
  });
  updateJarPreview();
}

function jarClearAllVisible() {
  const treeEl = document.getElementById("jar-class-tree");
  treeEl.querySelectorAll(".cls-cb").forEach(cb => {
    cb.checked = false;
    Jar.selected.delete(cb.dataset.cls);
  });
  treeEl.querySelectorAll(".pkg-cb").forEach(cb => {
    cb.checked = false;
    cb.indeterminate = false;
    const countEl = cb.closest(".pkg-header")?.querySelector(".pkg-count");
    if (countEl) {
      const total = cb.closest(".pkg-group")?.querySelectorAll(".cls-cb").length || 0;
      countEl.textContent = `0/${total}`;
    }
  });
  updateJarPreview();
}

function syncPrefixesToTextarea() {
  const ta = document.getElementById("user-prefixes-input");
  if (!ta) return;
  ta.value = computeMinimalPrefixes().join("\n");
}

function updateJarPreview() {
  const el = document.getElementById("jar-prefix-preview");
  if (!el) return;
  const n = Jar.selected.size;
  const prefixes = computeMinimalPrefixes();
  if (n === 0) {
    el.textContent = "0 classes selected.";
  } else {
    const preview = prefixes.slice(0, 4).join(", ");
    const more    = prefixes.length > 4 ? ` +${prefixes.length - 4} more` : "";
    el.textContent = `${n} selected → ${preview}${more}`;
  }
  syncPrefixesToTextarea();
}

// Compute the minimal set of slash-notation prefixes that cover all selected classes.
// Rules:
//   • If every class in a package is selected → use the package path as prefix.
//   • If only some classes in a package are selected → list individual class paths.
//   • Root-package classes (no "/") always use the full class path.
//   • Deduplicate: drop any prefix already covered by a shorter one already in the result.
function computeMinimalPrefixes() {
  if (!Jar.selected.size) return [];

  // Count total and selected per package across ALL classes (not just visible ones)
  const totals = new Map();
  const sels   = new Map();
  for (const c of Jar.classes) {
    totals.set(c.pkg, (totals.get(c.pkg) || 0) + 1);
    if (Jar.selected.has(c.full))
      sels.set(c.pkg, (sels.get(c.pkg) || 0) + 1);
  }

  const raw = [];
  for (const [pkg, total] of totals) {
    const sel = sels.get(pkg) || 0;
    if (sel === 0) continue;
    if (sel === total && pkg !== "") {
      raw.push(pkg);   // whole named package → use package as prefix
    } else {
      // partial, or root package → individual class names
      for (const c of Jar.classes) {
        if (c.pkg === pkg && Jar.selected.has(c.full))
          raw.push(c.full);
      }
    }
  }

  // Sort shortest first, then alphabetically
  raw.sort((a, b) => a.length - b.length || a.localeCompare(b));

  // Remove entries already covered by an earlier (shorter) prefix
  const result = [];
  for (const p of raw) {
    const covered = result.some(r => p === r || p.startsWith(r + "/"));
    if (!covered) result.push(p);
  }
  return result;
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
