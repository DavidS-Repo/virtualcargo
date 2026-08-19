(() => {
  'use strict';

  const state = {
    csrf: '',
    editingEnabled: false,
    identity: '',
    route: 'overview',
    containerId: '',
    currentContainer: null,
    health: null,
    searchTimer: null,
    searchAbort: null,
    activityTimer: null,
    paletteOpen: false,
    pendingRootId: '',
    itemSelections: new Map()
  };

  const app = document.getElementById('app');
  const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const fmtInt = value => new Intl.NumberFormat().format(Number(value || 0));
  const fmtNum = value => Number.isFinite(Number(value)) ? new Intl.NumberFormat(undefined,{maximumFractionDigits:3}).format(Number(value)) : '0';
  const fmtBytes = value => {
    let n = Number(value || 0);
    const units = ['B','KB','MB','GB','TB'];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
    return `${n.toFixed(i ? 1 : 0)} ${units[i]}`;
  };
  const fmtTime = value => value ? new Date(Number(value)).toLocaleString() : 'Never';
  const fmtLocalInput = value => { if(!value) return ''; const d=new Date(Number(value)); const pad=n=>String(n).padStart(2,'0'); return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`; };
  const localInputMs = value => value ? new Date(value).getTime() : 0;
  const chip = (text, kind='') => `<span class="chip ${kind}">${esc(text)}</span>`;
  const button = (label, cls='button', attrs='') => `<button type="button" class="${cls}" ${attrs}>${esc(label)}</button>`;

  async function api(path, options={}) {
    const headers = new Headers(options.headers || {});
    if (options.body && !headers.has('Content-Type')) headers.set('Content-Type', 'application/json');
    if (options.method && options.method !== 'GET' && state.csrf) headers.set('X-Clippy-CSRF', state.csrf);
    const response = await fetch(path, {...options, headers, credentials:'same-origin', cache:'no-store'});
    const body = await response.json().catch(() => ({}));
    if (!response.ok || body.ok === false) {
      const message = body?.error?.message || `Request failed (${response.status})`;
      const error = new Error(message);
      error.code = body?.error?.code || 'request_failed';
      error.retryable = Boolean(body?.error?.retryable);
      throw error;
    }
    return body.data ?? body;
  }

  const writeApi = (path, body) => api(path, {method:'POST', body:JSON.stringify(body || {})});

  async function bootstrap() {
    const match = location.hash.match(/^#bootstrap=([0-9a-fA-F]{64,128})$/);
    try {
      let session;
      if (match) {
        session = await api('/api/session/bootstrap', {method:'POST', body:JSON.stringify({token:match[1]})});
        history.replaceState(null, '', location.pathname + location.search);
      } else {
        session = await api('/api/session');
      }
      state.csrf = session.csrf || '';
      state.editingEnabled = Boolean(session.editing_enabled);
      state.identity = session.identity || '';
      return true;
    } catch (error) {
      renderLogin(match ? `Bootstrap failed: ${error.message}` : 'Open this panel with OPEN-CLIPPY-ADMIN.bat or the server manager admin command.');
      return false;
    }
  }

  function renderLogin(message) {
    app.innerHTML = `<main class="login"><section class="login-card"><h1>Local session required</h1><p class="subtitle">${esc(message)}</p><p class="muted">The panel accepts a short-lived local bootstrap token. PostgreSQL passwords and Clippy service secrets are never sent to this page.</p></section></main>`;
  }

  function navButton(route, label) {
    return `<button type="button" data-route="${route}" ${state.route===route?'aria-current="page"':''}>${label}</button>`;
  }

  function shell() {
    app.innerHTML = `<div class="shell">
      <aside class="sidebar">
        <nav class="nav" aria-label="Admin pages">
          ${navButton('overview','Overview')}
          ${navButton('containers','Containers')}
          ${navButton('items','Items')}
          ${navButton('activity','Activity')}
          ${navButton('sessions','Sessions')}
          ${navButton('recovery','Recovery')}
          ${navButton('maintenance','Maintenance')}
          ${navButton('backups','Backups')}
          ${navButton('quarantine','Quarantine')}
          ${navButton('audit','Audit Log')}
          ${navButton('database','Database')}
          ${navButton('reports','Reports')}
          ${navButton('players','Players')}
          ${navButton('settings','Settings')}
        </nav>
        <div class="side-note">${state.editingEnabled ? 'Safe editing enabled' : 'Read-only mode'}<br>Localhost only${state.identity ? `<br>${esc(state.identity)}` : ''}</div>
      </aside>
      <section class="workspace">
        <header class="topbar">
          <div class="global-search"><input id="globalSearch" aria-label="Search or open command palette" placeholder="Search anything..." autocomplete="off"><span class="keyhint">Ctrl K</span></div>
          <div class="statuses" id="statuses"></div>
        </header>
        <main class="content" id="content"></main>
      </section>
    </div>`;
    document.querySelectorAll('[data-route]').forEach(el => el.addEventListener('click', () => navigate(el.dataset.route)));
    const gs = document.getElementById('globalSearch');
    gs.addEventListener('focus', openPalette);
    gs.addEventListener('keydown', event => { if (event.key === 'Enter' && gs.value.trim()) command(gs.value.trim()); });
  }

  function pageHead(title, subtitle, actions='') {
    const badge = state.editingEnabled ? '<span class="mode-badge edit">EDITING ENABLED</span>' : '<span class="mode-badge">READ ONLY</span>';
    return `<div class="page-head"><div><h1>${esc(title)}</h1><div class="subtitle">${esc(subtitle)}</div></div><div class="page-actions">${actions}${badge}</div></div>`;
  }

  function setContent(html) {
    const content = document.getElementById('content');
    if (content) content.innerHTML = html;
  }

  function showError(error) {
    setContent(`<div class="error"><strong>Request failed</strong><div>${esc(error.message || error)}</div></div>`);
  }

  function toast(message, kind='good') {
    let host = document.getElementById('toasts');
    if (!host) {
      host = document.createElement('div');
      host.id = 'toasts';
      host.className = 'toasts';
      host.setAttribute('aria-live','polite');
      host.setAttribute('aria-relevant','additions');
      document.body.appendChild(host);
    }
    const el = document.createElement('div');
    el.className = `toast ${kind}`;
    if(kind==='danger') el.setAttribute('role','alert'); else el.setAttribute('role','status');
    el.textContent = message;
    host.appendChild(el);
    setTimeout(() => el.remove(), 4500);
  }

  async function refreshHealth() {
    try {
      state.health = await api('/api/health');
      const pg = state.health.postgres?.ok ? '<span><i class="status-dot good"></i>PostgreSQL</span>' : '<span><i class="status-dot warn"></i>PostgreSQL</span>';
      const host = state.health.storage_host_reachable ? '<span><i class="status-dot good"></i>Storage Host</span>' : '<span><i class="status-dot warn"></i>Storage Host</span>';
      const dayz = state.health.dayz_server_running ? '<span><i class="status-dot good"></i>DayZ</span>' : '<span><i class="status-dot"></i>DayZ stopped</span>';
      const edit = state.editingEnabled ? '<span><i class="status-dot edit"></i>Editing</span>' : '';
      const el = document.getElementById('statuses');
      if (el) el.innerHTML = pg + host + dayz + edit;
    } catch (_) {}
  }

  async function navigate(route, data={}) {
    if (state.activityTimer) { clearInterval(state.activityTimer); state.activityTimer = null; }
    if (state.searchAbort) { state.searchAbort.abort(); state.searchAbort = null; }
    state.route = route;
    if (data.containerId) state.containerId = data.containerId;
    shell();
    refreshHealth();
    try {
      if (route === 'overview') await renderOverview();
      else if (route === 'containers') await renderContainers(data.query ? {q:data.query} : {});
      else if (route === 'container') await renderContainer(state.containerId);
      else if (route === 'items') await renderItems(data.query || '');
      else if (route === 'activity') await renderActivity();
      else if (route === 'sessions') await renderSessions();
      else if (route === 'recovery') await renderRecovery();
      else if (route === 'maintenance') await renderMaintenance();
      else if (route === 'backups') await renderBackups();
      else if (route === 'quarantine') await renderQuarantine();
      else if (route === 'audit') await renderAudit();
      else if (route === 'database') await renderDatabase();
      else if (route === 'reports') await renderReports();
      else if (route === 'players') await renderPlayers(data.query || '');
      else if (route === 'settings') await renderSettings();
    } catch (error) { showError(error); }
  }

  function card(label, value, detail='') {
    return `<div class="card"><div class="card-label">${esc(label)}</div><div class="card-value">${esc(value)}</div><div class="card-detail">${esc(detail)}</div></div>`;
  }

  async function renderOverview() {
    const [data,activity,changes,backups] = await Promise.all([
      api('/api/overview'), api('/api/activity?limit=8'), api('/api/admin/changes?limit=8'), api('/api/backups')
    ]);
    const latestBackup=(backups.rows||[])[0];
    const actions='<button class="button" id="overviewIntegrity">Run integrity check</button> <button class="button" id="overviewBackup">Create backup</button> <button class="button" id="overviewStuck">View stuck workflows</button> <button class="button primary" id="overviewRecovery">Open recovery</button>';
    setContent(`${pageHead('Overview','Current storage health, maintenance state, and admin recovery state.',actions)}
      <div class="cards">
        ${card('PostgreSQL', data.postgres_ok ? 'Healthy' : 'Unavailable', data.postgres_version || '')}
        ${card('Storage Host', data.storage_host_reachable ? 'Healthy' : 'Unavailable', '127.0.0.1:27815')}
        ${card('DayZ Server', data.dayz_server_running ? 'Running' : 'Stopped', 'Local process status')}
        ${card('Database size', fmtBytes(data.database_size_bytes), `Schema v${data.schema_version}`)}
        ${card('Containers', fmtInt(data.containers_estimated), 'Estimated from PostgreSQL statistics')}
        ${card('Virtual roots', fmtInt(data.roots_estimated), 'Estimated from PostgreSQL statistics')}
        ${card('Item nodes', fmtInt(data.item_nodes_estimated), data.item_index_complete?'Estimated indexed nodes':'Index backfill not complete')}
        ${card('Open sessions', fmtInt(data.active_sessions), 'OPEN, MATERIALIZED, or COMMITTED')}
        ${card('Pending workflow', fmtInt(Number(data.incomplete_operations)+Number(data.incomplete_migrations)), 'Operations and migrations')}
        ${card('Pending cleanup', fmtInt(data.pending_cleanup), 'Physical reconciliation records')}
        ${card('Admin locks', fmtInt(data.admin_locks), 'Unexpired maintenance locks')}
        ${card('Quarantine', fmtInt(data.quarantine_items), 'Items available for restore')}
        ${card('Applied changes', fmtInt(data.applied_admin_changes), 'Revision-checked undo candidates')}
        ${card('Known players', fmtInt(data.known_players), `${fmtInt(data.recent_players)} seen recently`)}
        ${card('Live commands', fmtInt(data.pending_player_commands), 'Pending or claimed')}
        ${card('Player quarantine', fmtInt(data.player_quarantine_items), 'Live items available for restore')}
        ${card('Last backup', latestBackup ? fmtTime(latestBackup.created_ms) : 'None found', latestBackup ? fmtBytes(latestBackup.bytes) : 'Create one before major maintenance')}
        ${card('Item index', data.item_index_complete ? 'Ready' : 'Backfill pending', data.item_index_complete ? 'Nested search enabled' : 'Safe fallback search active')}
      </div>
      <section class="panel"><div class="panel-head"><h2>Safety state</h2></div><div class="panel-body">
        ${chip('Localhost only','good')} ${chip('Dedicated read pool','good')} ${chip('Domain writes only','good')} ${state.editingEnabled ? chip('Maintenance-lock edits','edit') : chip('Editing disabled','good')}
        <p class="muted">cargo_roots.tree_json remains authoritative. Edits acquire a short maintenance lock, check active workflows and the current revision, write recoverable before-state data, update the tree and derived index in one transaction, increment the revision, and record the change.</p>
      </div></section>
      <section class="panel"><div class="panel-head"><h2>Recent activity</h2></div>${activityTable(activity.rows||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Recent admin changes</h2></div>${changesTable(changes.rows||[])}</section>`);
    document.getElementById('overviewIntegrity').onclick=runIntegrity;
    document.getElementById('overviewBackup').onclick=createBackup;
    document.getElementById('overviewStuck').onclick=()=>navigate('recovery');
    document.getElementById('overviewRecovery').onclick=()=>navigate('recovery');
    wireChangeButtons();
  }

  async function renderContainers(filters={}, after='') {
    if (typeof filters === 'string') filters={q:filters};
    const params=new URLSearchParams({limit:'50',after});
    for(const key of ['q','contains','status','min_nodes','stale_days'])if(filters[key])params.set(key,filters[key]);
    const data = await api(`/api/containers?${params}`);
    setContent(`${pageHead('Containers','Browse and safely maintain virtual cargo containers.')}
      <section class="panel"><div class="panel-body"><div class="search-grid">
        <label class="field wide"><span>Container</span><input id="containerSearch" aria-label="Filter containers" placeholder="storage ID, provider key, or display name" value="${esc(filters.q||'')}"></label>
        <label class="field"><span>Contains item class</span><input id="containerContains" maxlength="128" placeholder="M4A1 prefix" value="${esc(filters.contains||'')}"></label>
        <label class="field"><span>Status</span><select id="containerStatus"><option value="">All</option><option value="session" ${filters.status==='session'?'selected':''}>Active session</option><option value="recovery" ${filters.status==='recovery'?'selected':''}>Pending recovery</option><option value="locked" ${filters.status==='locked'?'selected':''}>Admin locked</option><option value="idle" ${filters.status==='idle'?'selected':''}>Idle</option></select></label>
        <label class="field"><span>Minimum nodes</span><input id="containerMinNodes" type="number" min="0" max="1000000000" step="1" value="${esc(filters.min_nodes||'')}"></label>
        <label class="field"><span>Not seen for days</span><input id="containerStaleDays" type="number" min="0" max="36500" step="1" value="${esc(filters.stale_days||'')}" placeholder="30"></label>
        <button class="button primary search-submit" id="containerSearchButton">Apply</button>
      </div><div class="muted">Search also matches the DayZ-reported container class and map. Item-class filtering uses cargo_item_index after its backfill completes.</div></div></section>
      <section class="panel"><div class="table-wrap"><table><thead><tr><th>Container</th><th>Class / Map</th><th>Position</th><th>Roots</th><th>Nodes</th><th>Revision</th><th>Last seen</th><th>Status</th></tr></thead><tbody>
      ${data.rows.map(r => `<tr><td><button class="link-button open-container" data-id="${esc(r.storage_id)}">${esc(r.display_name)}</button><div class="mono muted">${esc(r.storage_id)}</div></td><td>${esc(r.container_class||'Unknown')}<div class="muted">${esc(r.map_name||'Unknown map')}</div></td><td class="mono">${r.world_position_x===undefined?'Unknown':`${fmtNum(r.world_position_x)}, ${fmtNum(r.world_position_y)}, ${fmtNum(r.world_position_z)}`}</td><td>${fmtInt(r.root_count)}</td><td>${fmtInt(r.node_count)}</td><td>${fmtInt(r.revision)}</td><td>${esc(fmtTime(r.last_seen_ms||r.updated_ms))}</td><td>${r.active_session?chip('session','warn'):''}${r.active_operation?chip('operation','warn'):''}${r.active_migration?chip('migration','warn'):''}${r.admin_locked?chip('admin lock','edit'):''}${(!r.active_session&&!r.active_operation&&!r.active_migration&&!r.admin_locked)?chip('idle','good'):''}</td></tr>`).join('')}
      </tbody></table></div>${data.rows.length?'':'<div class="empty">No containers matched.</div>'}</section>
      <div class="pager"><button class="button" id="containerNext" ${data.next_after?'':'disabled'}>Next page</button></div>`);
    const current=()=>({q:document.getElementById('containerSearch').value.trim(),contains:document.getElementById('containerContains').value.trim(),status:document.getElementById('containerStatus').value,min_nodes:document.getElementById('containerMinNodes').value.trim(),stale_days:document.getElementById('containerStaleDays').value.trim()});
    document.getElementById('containerSearchButton').onclick = () => renderContainers(current(), '').catch(showError);
    for(const id of ['containerSearch','containerContains','containerMinNodes','containerStaleDays'])document.getElementById(id).addEventListener('keydown', e => { if (e.key === 'Enter') renderContainers(current(), '').catch(showError); });
    document.querySelectorAll('.open-container').forEach(b => b.onclick = () => navigate('container',{containerId:b.dataset.id}));
    document.getElementById('containerNext').onclick = () => renderContainers(filters, data.next_after || '').catch(showError);
  }

  const pair = (k,v) => `<div class="detail-pair"><div class="k">${esc(k)}</div><div class="v">${esc(v)}</div></div>`;

  function workflowSummary(d) {
    const total = d.active_sessions.length + d.active_operations.length + d.active_migrations.length;
    if (!total && !d.admin_lock) return '<span class="muted">No active operation, cargo session, migration, or maintenance lock is blocking this container.</span>';
    return `${d.active_sessions.map(x=>chip(`session ${x.status}`,'warn')).join('')}${d.active_operations.map(x=>chip(`operation ${x.status}`,'warn')).join('')}${d.active_migrations.map(x=>chip(`migration ${x.status}`,'warn')).join('')}${d.admin_lock?chip('admin maintenance lock','edit'):''}`;
  }

  async function renderContainer(id, after='') {
    const [detail, roots, changes, snapshots] = await Promise.all([
      api(`/api/containers/${encodeURIComponent(id)}`),
      api(`/api/containers/${encodeURIComponent(id)}/roots?limit=50&after=${encodeURIComponent(after)}`),
      api(`/api/admin/changes?storage_id=${encodeURIComponent(id)}&limit=25`),
      api(`/api/snapshots?storage_id=${encodeURIComponent(id)}&limit=15`)
    ]);
    state.currentContainer = detail;
    const busyWorkflow = detail.active_sessions.length || detail.active_operations.length || detail.active_migrations.length;
    const ownLock = Boolean(detail.admin_lock?.owned_by_current_session);
    const otherLock = Boolean(detail.admin_lock && !ownLock);
    const editBlocked = Boolean(busyWorkflow || otherLock);
    const lockButtons = state.editingEnabled ? (ownLock
      ? `<button class="button" id="lockContainer">Renew lock</button><button class="button warn" id="unlockContainer">Release lock</button>`
      : `<button class="button" id="lockContainer" ${editBlocked?'disabled':''}>Lock for maintenance</button>`) : '';
    const actionBar = `<button class="button" id="backContainers">Back</button><button class="button" id="exportContainer">Export inventory</button><button class="button" id="containerIntegrity">Run integrity check</button>${lockButtons}${state.editingEnabled ? `<button class="button primary" id="snapshotContainer" ${editBlocked?'disabled':''}>Create snapshot</button>` : ''}`;
    setContent(`${pageHead(detail.display_name || 'Container', detail.storage_id, actionBar)}
      ${otherLock && state.editingEnabled ? '<div class="notice warn">Editing is blocked because another local admin session holds the maintenance lock.</div>' : ''}
      ${busyWorkflow && state.editingEnabled ? '<div class="notice warn">Editing is blocked while this container has an active DayZ operation, cargo session, or migration.</div>' : ''}
      ${ownLock ? `<div class="notice">This local admin session holds the maintenance lock until ${esc(fmtTime(detail.admin_lock.expires_ms))}. DayZ cannot open this virtual cargo while the lock is active.</div>` : ''}
      <section class="panel"><div class="panel-head"><h2>Container detail</h2></div><div class="panel-body"><div class="detail-grid">
        ${pair('Provider',detail.provider_id)}${pair('Provider key',detail.provider_key)}${pair('Container class',detail.container_class||'Unknown')}${pair('Map',detail.map_name||'Unknown')}
        ${pair('Position',detail.world_position_x===undefined?'Unknown':`${fmtNum(detail.world_position_x)}, ${fmtNum(detail.world_position_y)}, ${fmtNum(detail.world_position_z)}`)}${pair('First seen',fmtTime(detail.first_seen_ms))}${pair('Last seen',fmtTime(detail.last_seen_ms))}${pair('Capacity',fmtInt(detail.capacity_slots))}${pair('Revision',fmtInt(detail.revision))}
        ${pair('Root items',fmtInt(detail.root_count))}${pair('Total nodes',fmtInt(detail.node_count))}${pair('Created',fmtTime(detail.created_ms))}${pair('Updated',fmtTime(detail.updated_ms))}
      </div></div></section>
      <section class="panel"><div class="panel-head"><h2>Inventory roots</h2><span class="muted">Trees load only when opened</span></div><div class="table-wrap"><table><thead><tr><th>Class</th><th>Root ID</th><th>Qty</th><th>Health</th><th>Nodes</th><th></th></tr></thead><tbody>
        ${roots.rows.map(r => `<tr><td>${esc(r.class_name)}</td><td class="mono">${esc(r.root_item_id)}</td><td>${fmtNum(r.quantity)}</td><td>${fmtNum(r.health)}</td><td>${fmtInt(r.node_count)}</td><td><button class="button load-tree" data-root="${esc(r.root_item_id)}">Open tree</button></td></tr>`).join('')}
      </tbody></table></div>${roots.rows.length?'':'<div class="empty">This container has no virtual roots.</div>'}<div class="panel-body"><button class="button" id="rootsNext" ${roots.next_after?'':'disabled'}>Next roots</button></div></section>
      <section class="panel" id="treePanel" hidden><div class="panel-head"><h2>Inventory tree</h2><div><button class="button" id="treeExport">Export tree</button> <button class="button" id="rawToggle">Raw JSON</button></div></div><div class="panel-body" id="treeBody"></div></section>
      <section class="panel"><div class="panel-head"><h2>Active workflows</h2></div><div class="panel-body">${workflowSummary(detail)}</div></section>
      <section class="panel"><div class="panel-head"><h2>Admin change history</h2></div>${changesTable(changes.rows, true)}</section>
      <section class="panel"><div class="panel-head"><h2>Snapshots</h2></div>${snapshotsTable(snapshots.rows)}</section>`);
    document.getElementById('backContainers').onclick = () => navigate('containers');
    document.getElementById('exportContainer').onclick = () => exportContainer(detail);
    document.getElementById('containerIntegrity').onclick = runIntegrity;
    if (document.getElementById('snapshotContainer')) document.getElementById('snapshotContainer').onclick = () => snapshotContainer(detail);
    if (document.getElementById('lockContainer')) document.getElementById('lockContainer').onclick = () => lockContainer(detail);
    if (document.getElementById('unlockContainer')) document.getElementById('unlockContainer').onclick = () => unlockContainer(detail);
    document.querySelectorAll('.load-tree').forEach(b => b.onclick = () => loadTree(detail,b.dataset.root));
    document.getElementById('rootsNext').onclick = () => renderContainer(id, roots.next_after || '');
    wireChangeButtons();
    document.querySelectorAll('.compare-snapshot').forEach(el=>el.onclick=()=>compareSnapshot(el.dataset.snapshot));
    if(state.pendingRootId){const root=state.pendingRootId;state.pendingRootId='';loadTree(detail,root);}
  }

  async function loadTree(detail, rootId) {
    const panel = document.getElementById('treePanel');
    const body = document.getElementById('treeBody');
    panel.hidden = false;
    body.innerHTML = '<div class="muted">Loading tree...</div>';
    try {
      const data = await api(`/api/containers/${encodeURIComponent(detail.storage_id)}/roots/${encodeURIComponent(rootId)}/tree`);
      let raw = false;
      const render = () => {
        body.replaceChildren();
        if (raw) {
          const pre = document.createElement('pre');
          pre.className = 'json';
          pre.textContent = JSON.stringify(data.tree, null, 2);
          body.appendChild(pre);
        } else {
          const tree = document.createElement('div');
          tree.className = 'tree';
          tree.appendChild(treeNodeElement(data.tree, {storageId:detail.storage_id,rootId,revision:detail.revision,busy:Boolean(detail.active_sessions.length||detail.active_operations.length||detail.active_migrations.length||(detail.admin_lock&&!detail.admin_lock.owned_by_current_session))}, true));
          body.appendChild(tree);
        }
      };
      render();
      document.getElementById('rawToggle').onclick = () => {
        raw = !raw;
        document.getElementById('rawToggle').textContent = raw ? 'Tree view' : 'Raw JSON';
        render();
      };
      document.getElementById('treeExport').onclick = () => downloadJson(`${safeFileName(data.tree.class_name || 'item')}-${safeFileName(rootId)}.json`, data.tree);
      panel.scrollIntoView({block:'start'});
    } catch (error) { body.innerHTML = `<div class="error">${esc(error.message)}</div>`; }
  }

  function treeNodeElement(node, context, expanded=false) {
    const wrapper = document.createElement('div');
    wrapper.className = 'tree-node';
    const line = document.createElement('div');
    line.className = 'tree-line';
    const children = Array.isArray(node.children) ? node.children : [];
    const toggle = document.createElement('button');
    toggle.type = 'button';
    toggle.className = 'tree-toggle';
    toggle.disabled = children.length === 0;
    toggle.textContent = children.length ? (expanded ? '▾' : '▸') : '·';
    line.appendChild(toggle);
    const name = document.createElement('span'); name.className = 'tree-name'; name.textContent = node.class_name || '(unknown class)'; line.appendChild(name);
    const item = document.createElement('span'); item.className = 'mono muted'; item.textContent = node.item_id || ''; line.appendChild(item);
    for (const text of [`qty ${fmtNum(node.quantity)}`, `health ${fmtNum(node.health)}`]) { const tag=document.createElement('span'); tag.className='chip'; tag.textContent=text; line.appendChild(tag); }
    const actions = document.createElement('span'); actions.className = 'tree-actions';
    const make = (label, cls, handler, blocked=false) => { const b=document.createElement('button'); b.type='button'; b.className=cls; b.textContent=label; b.disabled=blocked; b.onclick=event=>{event.stopPropagation();handler();}; actions.appendChild(b); };
    make('History','mini-button',()=>viewItemHistory(node.item_id||''));
    make('Export','mini-button',()=>downloadJson(`${safeFileName(node.class_name||'item')}-${safeFileName(node.item_id||'item')}.json`,node));
    if (state.editingEnabled) {
      make('Edit','mini-button',()=>editItem(context,node),context.busy);
      make('Move / Copy','mini-button',()=>moveCopyItem(context,node),context.busy);
      if ((node.item_id||'') !== context.rootId) make('Detach to root','mini-button',()=>detachToRoot(context,node),context.busy);
      if ((node.item_id||'') === context.rootId) make('Duplicate root','mini-button',()=>duplicateRoot(context,node),context.busy);
      make('Quarantine','mini-button warn',()=>quarantineItem(context,node),context.busy);
      make('Remove','mini-button danger',()=>removeItem(context,node),context.busy);
    }
    line.appendChild(actions);
    wrapper.appendChild(line);
    const childHost = document.createElement('div'); childHost.className='tree-children'; wrapper.appendChild(childHost);
    let materialized = false;
    const setExpanded = value => {
      expanded = value; toggle.setAttribute('aria-expanded',String(value)); toggle.textContent=children.length?(value?'▾':'▸'):'·'; childHost.hidden=!value;
      if (value && !materialized) { for (const child of children) childHost.appendChild(treeNodeElement(child,context,false)); materialized=true; }
    };
    toggle.onclick = () => setExpanded(!expanded);
    setExpanded(expanded);
    return wrapper;
  }

  function safeFileName(value) { return String(value || 'export').replace(/[^A-Za-z0-9._-]+/g,'_').slice(0,100); }
  function downloadJson(name, value) {
    const blob = new Blob([JSON.stringify(value,null,2)],{type:'application/json'});
    const url = URL.createObjectURL(blob); const a=document.createElement('a'); a.href=url; a.download=name; document.body.appendChild(a); a.click(); a.remove(); setTimeout(()=>URL.revokeObjectURL(url),1000);
  }

  function openDialog({title,body,confirm='Save',danger=false,onSubmit}) {
    const previousFocus=document.activeElement;
    const dialog = document.createElement('dialog');
    const titleId=`modal-title-${Math.random().toString(16).slice(2)}`;
    dialog.className='modal';dialog.setAttribute('aria-labelledby',titleId);
    dialog.innerHTML=`<form class="modal-card"><div class="modal-head"><h2 id="${titleId}">${esc(title)}</h2><button type="button" class="icon-button close-dialog" aria-label="Close">×</button></div><div class="modal-body">${body}</div><div class="modal-actions"><button type="button" class="button cancel-dialog">Cancel</button><button type="submit" class="button ${danger?'danger':'primary'}">${esc(confirm)}</button></div></form>`;
    document.body.appendChild(dialog);
    const close=()=>{if(dialog.open)dialog.close();dialog.remove();if(previousFocus&&previousFocus.isConnected&&typeof previousFocus.focus==='function')previousFocus.focus();};
    dialog.querySelector('.close-dialog').onclick=close; dialog.querySelector('.cancel-dialog').onclick=close;
    dialog.addEventListener('cancel',event=>{event.preventDefault();close();});
    dialog.querySelector('form').addEventListener('submit', async event => {
      event.preventDefault();
      const submit=dialog.querySelector('button[type=submit]'); submit.disabled=true;
      try { await onSubmit(dialog); close(); } catch (error) { submit.disabled=false; const host=dialog.querySelector('.modal-error') || dialog.querySelector('.modal-body').appendChild(Object.assign(document.createElement('div'),{className:'modal-error',role:'alert'})); host.textContent=error.message; }
    });
    dialog.showModal();
    const first=dialog.querySelector('.modal-body input:not([disabled]),.modal-body textarea:not([disabled]),.modal-body select:not([disabled]),button[type=submit]'); if(first) first.focus();
  }

  const field = (dialog,name) => dialog.querySelector(`[name="${name}"]`)?.value ?? '';
  const reasonField = (required=true) => `<label class="field"><span>Reason${required?' *':''}</span><textarea name="reason" maxlength="512" ${required?'required':''} placeholder="Why is this change needed?"></textarea></label>`;

  function editItem(context,node) {
    openDialog({title:`Edit ${node.class_name || 'item'}`,confirm:'Save change',body:`
      <div class="notice">Only generic quantity and health values are editable here. Adapter-specific state is intentionally not exposed.</div>
      <div class="form-grid"><label class="field"><span>Quantity</span><input name="quantity" type="number" min="0" step="any" value="${esc(node.quantity ?? 0)}"></label><label class="field"><span>Health</span><input name="health" type="number" min="0" step="any" value="${esc(node.health ?? 0)}"></label></div>${reasonField(true)}`,
      onSubmit: async dialog => {
        const quantity=Number(field(dialog,'quantity')), health=Number(field(dialog,'health'));
        if(!Number.isFinite(quantity)||quantity<0||!Number.isFinite(health)||health<0) throw new Error('Quantity and health must be non-negative numbers.');
        await writeApi(`/api/items/${encodeURIComponent(node.item_id)}/edit`,{storage_id:context.storageId,root_item_id:context.rootId,expected_revision:context.revision,patch:{quantity,health},reason:field(dialog,'reason').trim()});
        toast('Item updated.'); await navigate('container',{containerId:context.storageId});
      }});
  }

  function quarantineItem(context,node) {
    openDialog({title:`Quarantine ${node.class_name || 'item'}`,confirm:'Quarantine',danger:true,body:`<div class="notice warn">The selected item and its nested children will be removed from virtual cargo and stored in the admin quarantine table so they can be restored later.</div>${reasonField(true)}`,
      onSubmit:async dialog=>{await writeApi(`/api/items/${encodeURIComponent(node.item_id)}/quarantine`,{storage_id:context.storageId,root_item_id:context.rootId,expected_revision:context.revision,reason:field(dialog,'reason').trim()});toast('Item moved to quarantine.');await navigate('container',{containerId:context.storageId});}});
  }

  function removeItem(context,node) {
    openDialog({title:`Remove ${node.class_name || 'item'}`,confirm:'Remove permanently',danger:true,body:`<div class="notice danger">This removes the selected item tree without placing it in quarantine. The change still has a revision-checked undo record, but quarantine is safer for normal removals.</div>${reasonField(true)}<label class="field"><span>Type REMOVE to confirm</span><input name="confirm" autocomplete="off" required></label>`,
      onSubmit:async dialog=>{if(field(dialog,'confirm')!=='REMOVE')throw new Error('Type REMOVE exactly to confirm.');await writeApi(`/api/items/${encodeURIComponent(node.item_id)}/remove`,{storage_id:context.storageId,root_item_id:context.rootId,expected_revision:context.revision,reason:field(dialog,'reason').trim()});toast('Item removed.','warn');await navigate('container',{containerId:context.storageId});}});
  }

  function moveCopyItem(context,node) {
    openDialog({title:`Move or copy ${node.class_name || 'item'}`,confirm:'Apply',body:`<div class="notice">The selected subtree becomes a virtual root in the target container. Copies get new item IDs. Moves keep the existing IDs.</div><label class="field"><span>Action</span><select name="action"><option value="move">Move</option><option value="copy">Copy</option></select></label><label class="field"><span>Target storage ID</span><input name="target" maxlength="128" required placeholder="storage ID"></label>${reasonField(true)}`,
      onSubmit:async dialog=>{
        const target=field(dialog,'target').trim(); if(!target)throw new Error('Target storage ID is required.');
        const targetDetail=await api(`/api/containers/${encodeURIComponent(target)}`);
        const action=field(dialog,'action');
        await writeApi(`/api/items/${encodeURIComponent(node.item_id)}/${action}`,{storage_id:context.storageId,root_item_id:context.rootId,expected_revision:context.revision,target_storage_id:target,target_expected_revision:targetDetail.revision,reason:field(dialog,'reason').trim()});
        toast(`${action==='copy'?'Copied':'Moved'} item to ${target}.`); await navigate('container',{containerId:context.storageId});
      }});
  }

  function detachToRoot(context,node) {
    openDialog({title:`Detach ${node.class_name || 'item'} to a root`,confirm:'Detach to root',body:`<div class="notice">The selected nested subtree will be removed from its parent and stored as a new virtual root in this same container. Item IDs are preserved.</div>${reasonField(true)}`,
      onSubmit:async dialog=>{await writeApi(`/api/items/${encodeURIComponent(node.item_id)}/move`,{storage_id:context.storageId,root_item_id:context.rootId,expected_revision:context.revision,target_storage_id:context.storageId,target_expected_revision:context.revision,reason:field(dialog,'reason').trim()});toast('Nested item detached to a virtual root.');await navigate('container',{containerId:context.storageId});}});
  }

  function duplicateRoot(context,node) {
    openDialog({title:`Duplicate ${node.class_name || 'root'}`,confirm:'Duplicate root',body:`<div class="notice">The duplicate stays in this container and receives fresh item IDs for the full copied tree.</div>${reasonField(true)}`,
      onSubmit:async dialog=>{await writeApi(`/api/items/${encodeURIComponent(node.item_id)}/copy`,{storage_id:context.storageId,root_item_id:context.rootId,expected_revision:context.revision,target_storage_id:context.storageId,target_expected_revision:context.revision,reason:field(dialog,'reason').trim()});toast('Root duplicated.');await navigate('container',{containerId:context.storageId});}});
  }

  function exportContainer(detail) {
    openDialog({title:'Export container inventory',confirm:'Create export',body:`<div class="notice">The server writes a JSON Lines export in bounded 50-root database batches. This avoids loading a large container into one browser response. The export contains virtual cargo data only and does not include database passwords or service secrets.</div><div class="detail-grid">${pair('Container',detail.display_name||detail.storage_id)}${pair('Revision',fmtInt(detail.revision))}${pair('Roots',fmtInt(detail.root_count))}${pair('Nodes',fmtInt(detail.node_count))}</div>`,
      onSubmit:async()=>{const result=await writeApi(`/api/containers/${encodeURIComponent(detail.storage_id)}/export`,{});toast(`Exported ${fmtInt(result.roots)} roots to ${result.file}.`);try{await navigator.clipboard.writeText(result.path);}catch(_){};openDialog({title:'Container export complete',confirm:'Close',body:`<div class="notice">The export was written on the DayZ server. Its path was copied to the clipboard when browser permissions allowed it.</div><div class="detail-grid">${pair('File',result.file)}${pair('Roots',fmtInt(result.roots))}${pair('Nodes',fmtInt(result.nodes))}${pair('Size',fmtBytes(result.bytes))}</div><div class="mono">${esc(result.path)}</div><div class="modal-actions-inline"><button type="button" class="button" id="openExportFolder">Open export folder</button><button type="button" class="button" id="copyExportPath">Copy path</button></div>`,onSubmit:async()=>{}});const open=document.getElementById('openExportFolder');if(open)open.onclick=async()=>{try{await writeApi('/api/exports/open-folder',{});}catch(error){toast(error.message,'danger');}};const copy=document.getElementById('copyExportPath');if(copy)copy.onclick=async()=>{try{await navigator.clipboard.writeText(result.path);toast('Export path copied.');}catch(_){toast('Clipboard access was blocked by the browser.','warn');}};}});
  }

  function snapshotContainer(detail) {
    openDialog({title:'Create container snapshot',confirm:'Create snapshot',body:`<div class="notice">This stores a server-side before-state snapshot of every virtual root in this container at revision ${fmtInt(detail.revision)}.</div>${reasonField(false)}`,
      onSubmit:async dialog=>{const result=await writeApi(`/api/containers/${encodeURIComponent(detail.storage_id)}/snapshot`,{expected_revision:detail.revision,reason:field(dialog,'reason').trim()});toast(`Snapshot ${result.snapshot_id} created.`);await navigate('container',{containerId:detail.storage_id});}});
  }

  function lockContainer(detail) {
    const renewing=Boolean(detail.admin_lock?.owned_by_current_session);
    openDialog({title:renewing?'Renew maintenance lock':'Lock container for maintenance',confirm:renewing?'Renew lock':'Acquire lock',body:`<div class="notice warn">While this lock is active, DayZ cannot start a new virtual-cargo workflow for this container. The lock expires automatically if this browser session disappears.</div>${reasonField(true)}`,
      onSubmit:async dialog=>{const result=await writeApi(`/api/containers/${encodeURIComponent(detail.storage_id)}/lock`,{expected_revision:detail.revision,reason:field(dialog,'reason').trim()});toast(`${result.renewed?'Maintenance lock renewed':'Container locked'} until ${fmtTime(result.expires_ms)}.`);await navigate('container',{containerId:detail.storage_id});}});
  }

  function unlockContainer(detail) {
    openDialog({title:'Release maintenance lock',confirm:'Release lock',body:`<div class="notice">This allows new DayZ virtual-cargo workflows to start for this container again. Releasing a lock does not change stored cargo.</div>${reasonField(false)}`,
      onSubmit:async dialog=>{const result=await writeApi(`/api/containers/${encodeURIComponent(detail.storage_id)}/unlock`,{reason:field(dialog,'reason').trim()});toast(result.released?'Maintenance lock released.':'The maintenance lock had already expired or been released.','good');await navigate('container',{containerId:detail.storage_id});}});
  }

  function changesTable(rows, containerContext=false) {
    if(!rows.length)return '<div class="empty">No admin changes recorded.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Time</th><th>Action</th><th>Item</th><th>Revision</th><th>Status</th><th>Reason</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td>${esc(r.action_type)}</td><td class="mono">${esc(r.item_id||'')}</td><td>${fmtInt(r.before_revision)} → ${fmtInt(r.after_revision)}</td><td>${chip(r.status,r.status==='APPLIED'?'good':'')}</td><td>${esc(r.reason||'')}</td><td><button class="button view-change" data-change="${esc(r.change_id)}">Compare / export</button> ${state.editingEnabled&&r.status==='APPLIED'&&r.action_type!=='undo'?`<button class="button undo-change" data-change="${esc(r.change_id)}">Undo</button>`:''}</td></tr>`).join('')}</tbody></table></div>`;
  }

  function snapshotsTable(rows) {
    if(!rows.length)return '<div class="empty">No snapshots recorded for this container.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Time</th><th>Snapshot</th><th>Revision</th><th>Roots</th><th>Nodes</th><th>Reason</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td class="mono">${esc(r.snapshot_id)}</td><td>${fmtInt(r.revision)}</td><td>${fmtInt(r.root_count)}</td><td>${fmtInt(r.node_count)}</td><td>${esc(r.reason||'')}</td><td><button class="button compare-snapshot" data-snapshot="${esc(r.snapshot_id)}">Compare current</button></td></tr>`).join('')}</tbody></table></div>`;
  }

  async function compareSnapshot(snapshotId) {
    try {
      const data=await api(`/api/snapshots/${encodeURIComponent(snapshotId)}/compare?limit=75`);
      const changed=Number(data.added_roots||0)+Number(data.removed_roots||0)+Number(data.changed_roots||0);
      const rows=data.differences||[];
      const body=`<div class="detail-grid">${pair('Snapshot revision',fmtInt(data.snapshot_revision))}${pair('Current revision',fmtInt(data.current_revision))}${pair('Added roots',fmtInt(data.added_roots))}${pair('Removed roots',fmtInt(data.removed_roots))}${pair('Changed roots',fmtInt(data.changed_roots))}${pair('Snapshot time',fmtTime(data.snapshot_created_ms))}</div>${changed===0?'<div class="notice">The current virtual roots match this snapshot.</div>':`<div class="table-wrap"><table><thead><tr><th>Change</th><th>Class</th><th>Root ID</th></tr></thead><tbody>${rows.map(r=>`<tr><td>${chip(r.kind,r.kind==='removed'?'warn':'')}</td><td>${esc(r.class_name||'')}</td><td class="mono">${esc(r.root_item_id)}</td></tr>`).join('')}</tbody></table></div>${data.next_after_root?'<div class="muted">Showing the first 75 changed roots. The comparison is bounded so a large container cannot freeze the admin page.</div>':''}`}`;
      openDialog({title:`Snapshot comparison`,confirm:'Close',body,onSubmit:async()=>{}});
    } catch(error){toast(error.message,'danger');}
  }

  function wireChangeButtons() {
    document.querySelectorAll('.undo-change').forEach(el=>el.onclick=()=>undoChange(el.dataset.change));
    document.querySelectorAll('.view-change').forEach(el=>el.onclick=()=>viewChange(el.dataset.change));
  }

  async function viewChange(changeId) {
    try {
      const data=await api(`/api/admin/changes/${encodeURIComponent(changeId)}`);
      const entries=data.entries||[];
      const body=`<div class="detail-grid">${pair('Action',data.action_type)}${pair('Status',data.status)}${pair('Container',data.storage_id)}${pair('Created',fmtTime(data.created_ms))}${pair('Before revision',fmtInt(data.before_revision))}${pair('After revision',fmtInt(data.after_revision))}</div>
        <div class="notice">Before and after states are the exact server-side change record used by revision-safe undo.</div>
        ${entries.map((e,i)=>`<section class="diff-block"><h3>Entry ${i+1}: ${esc(e.item_id||e.root_item_id)}</h3><div class="diff-grid"><div><div class="muted">Before</div><pre class="json">${esc(JSON.stringify(e.before_state??null,null,2))}</pre></div><div><div class="muted">After</div><pre class="json">${esc(JSON.stringify(e.after_state??null,null,2))}</pre></div></div></section>`).join('')}
        <button type="button" class="button" id="exportChangeRecord">Export change record</button>`;
      openDialog({title:`Change ${changeId}`,confirm:'Close',body,onSubmit:async()=>{}});
      const button=document.getElementById('exportChangeRecord'); if(button)button.onclick=()=>downloadJson(`clippy-change-${safeFileName(changeId)}.json`,data);
    } catch(error){toast(error.message,'danger');}
  }

  async function viewItemHistory(itemId) {
    try {
      const data=await api(`/api/items/${encodeURIComponent(itemId)}/history?limit=75`);
      const body=(data.rows||[]).length?changesTable(data.rows):'<div class="empty">No admin history for this exact item ID.</div>';
      openDialog({title:`Item history: ${itemId}`,confirm:'Close',body,onSubmit:async()=>{}});
      wireChangeButtons();
    } catch(error){toast(error.message,'danger');}
  }

  function undoChange(changeId) {
    openDialog({title:'Undo admin change',confirm:'Undo change',danger:true,body:`<div class="notice warn">Undo is allowed only if every affected container is still at the revision written by the original change. Newer changes are never overwritten.</div>${reasonField(true)}`,
      onSubmit:async dialog=>{await writeApi(`/api/admin/changes/${encodeURIComponent(changeId)}/undo`,{reason:field(dialog,'reason').trim()});toast('Change undone.');if(state.route==='container')await navigate('container',{containerId:state.containerId});else await navigate('audit');}});
  }

  async function renderItems(query='', cursor={}) {
    const q = query;
    const params = new URLSearchParams({q,limit:'50'});
    for(const [k,v] of Object.entries(cursor)) if(v!==''&&v!=null)params.set(k,v);
    if (state.searchAbort) state.searchAbort.abort();
    const controller = new AbortController();
    state.searchAbort = controller;
    let data;
    try {
      data = await api(`/api/items/search?${params.toString()}`,{signal:controller.signal});
    } catch (error) {
      if (error?.name === 'AbortError') return;
      throw error;
    }
    if (state.searchAbort !== controller) return;
    const selectedCount=state.itemSelections.size;
    setContent(`${pageHead('Items','Search virtual cargo without materializing it into DayZ.',`<button class="button" id="exportItemPage">Export page</button>`)}
      <section class="panel"><div class="panel-body"><div class="search-grid">
        <label class="field wide"><span>Class prefix or exact item ID</span><input id="itemQuery" value="${esc(q)}" placeholder="M4A1, id:exact-item-id, or leave blank for filters"></label>
        <label class="field"><span>Adapter ID</span><input id="adapterId" maxlength="128" value="${esc(cursor.adapter_id||'')}" placeholder="exact adapter"></label>
        <label class="field"><span>Location</span><input id="locationType" maxlength="64" value="${esc(cursor.location_type||'')}" placeholder="cargo / attachment"></label>
        <label class="field"><span>Min qty</span><input id="minQty" type="number" min="0" step="any" value="${esc(cursor.min_quantity||'')}"></label>
        <label class="field"><span>Max qty</span><input id="maxQty" type="number" min="0" step="any" value="${esc(cursor.max_quantity||'')}"></label>
        <label class="field"><span>Min health</span><input id="minHealth" type="number" min="0" step="any" value="${esc(cursor.min_health||'')}"></label>
        <label class="field"><span>Max health</span><input id="maxHealth" type="number" min="0" step="any" value="${esc(cursor.max_health||'')}"></label>
        <button class="button primary search-submit" id="itemSearchButton">Search</button>
      </div><div class="muted">${data.nested_class_search_available?'Nested class search is using cargo_item_index.':'Index backfill is incomplete. Search is limited to root class prefixes and exact item IDs.'}${data.filter_requires_index?' Adapter and location filters require the completed item index.':''}</div></div></section>
      ${state.editingEnabled?`<section class="panel bulk-bar"><div class="panel-body"><strong id="bulkCount">${selectedCount} root${selectedCount===1?'':'s'} selected</strong> <span class="muted">Bulk changes are limited to 25 roots and always run a conflict preview first. Move/copy requires all selected roots to come from one source container.</span><div class="page-actions"><button class="button" id="bulkExport" ${selectedCount?'':'disabled'}>Export selected</button> <button class="button" id="bulkMove" ${selectedCount?'':'disabled'}>Preview move</button> <button class="button" id="bulkCopy" ${selectedCount?'':'disabled'}>Preview copy</button> <button class="button" id="bulkQuarantine" ${selectedCount?'':'disabled'}>Preview quarantine</button> <button class="button danger" id="bulkRemove" ${selectedCount?'':'disabled'}>Preview remove</button> <button class="button" id="bulkClear" ${selectedCount?'':'disabled'}>Clear</button></div></div></section>`:''}
      <section class="panel"><div class="table-wrap"><table><thead><tr>${state.editingEnabled?'<th>Select</th>':''}<th>Class</th><th>Container</th><th>Item ID</th><th>Parent</th><th>Depth</th><th>Qty</th><th>Health</th><th>Adapter</th><th>Location</th><th>Updated</th><th></th></tr></thead><tbody>${data.rows.map(r=>{const key=`${r.storage_id}|${r.root_item_id}`;const root=r.item_id===r.root_item_id;const rowData=esc(encodeURIComponent(JSON.stringify(r)));return `<tr>${state.editingEnabled?`<td>${root?`<input type="checkbox" class="bulk-select" data-row="${rowData}" ${state.itemSelections.has(key)?'checked':''} aria-label="Select ${esc(r.class_name)} for bulk action">`:'<span class="muted">nested</span>'}</td>`:''}<td>${esc(r.class_name)}</td><td><button class="link-button open-container" data-id="${esc(r.storage_id)}">${esc(r.storage_id)}</button></td><td class="mono">${esc(r.item_id)}</td><td class="mono">${esc(r.parent_item_id||'')}</td><td>${fmtInt(r.depth)}</td><td>${fmtNum(r.quantity)}</td><td>${fmtNum(r.health)}</td><td class="mono">${esc(r.adapter_id||'')}</td><td>${esc(r.location_type||'')}</td><td>${esc(fmtTime(r.updated_ms))}</td><td><button class="mini-button open-item-tree" data-row="${rowData}">Open tree</button> <button class="mini-button item-history" data-id="${esc(r.item_id)}">History</button> <button class="mini-button export-search-row" data-row="${rowData}">Export</button>${state.editingEnabled?` <button class="mini-button item-search-edit" data-row="${rowData}">Edit</button> <button class="mini-button warn item-search-quarantine" data-row="${rowData}">Quarantine</button>`:''}</td></tr>`;}).join('')}</tbody></table></div>${data.rows.length?'':'<div class="empty">No items matched.</div>'}</section>
      <div class="pager"><button class="button" id="itemsNext" ${data.next_after_storage?'':'disabled'}>Next page</button></div>`);
    const currentFilters=()=>{const filters={};for(const [id,key] of [['minQty','min_quantity'],['maxQty','max_quantity'],['minHealth','min_health'],['maxHealth','max_health'],['adapterId','adapter_id'],['locationType','location_type']]){const v=document.getElementById(id).value.trim();if(v)filters[key]=v;}return filters;};
    const run=()=>{state.itemSelections.clear();renderItems(document.getElementById('itemQuery').value.trim(),currentFilters()).catch(showError);};
    document.getElementById('itemSearchButton').onclick=run;
    document.getElementById('itemQuery').addEventListener('input',()=>{clearTimeout(state.searchTimer);state.searchTimer=setTimeout(run,200);});
    document.getElementById('itemQuery').addEventListener('keydown',e=>{if(e.key==='Enter')run();});
    document.querySelectorAll('.open-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
    document.querySelectorAll('.open-item-tree').forEach(b=>b.onclick=()=>{const r=JSON.parse(decodeURIComponent(b.dataset.row));state.pendingRootId=r.root_item_id;navigate('container',{containerId:r.storage_id});});
    document.querySelectorAll('.item-history').forEach(b=>b.onclick=()=>viewItemHistory(b.dataset.id));
    document.querySelectorAll('.export-search-row').forEach(b=>b.onclick=()=>{const r=JSON.parse(decodeURIComponent(b.dataset.row));downloadJson(`clippy-item-${safeFileName(r.item_id)}.json`,r);});
    document.querySelectorAll('.item-search-edit').forEach(b=>b.onclick=()=>actionFromSearch(b,'edit'));
    document.querySelectorAll('.item-search-quarantine').forEach(b=>b.onclick=()=>actionFromSearch(b,'quarantine'));
    document.querySelectorAll('.bulk-select').forEach(b=>b.onchange=()=>{const r=JSON.parse(decodeURIComponent(b.dataset.row));const key=`${r.storage_id}|${r.root_item_id}`;if(b.checked){if(state.itemSelections.size>=25){b.checked=false;toast('Bulk batches are limited to 25 roots.','warn');return;}state.itemSelections.set(key,{storage_id:r.storage_id,root_item_id:r.root_item_id,item_id:r.item_id,expected_revision:r.revision});}else state.itemSelections.delete(key);const n=state.itemSelections.size;document.getElementById('bulkCount').textContent=`${n} root${n===1?'':'s'} selected`;for(const id of ['bulkExport','bulkMove','bulkCopy','bulkQuarantine','bulkRemove','bulkClear'])document.getElementById(id).disabled=!n;});
    if(document.getElementById('bulkExport'))document.getElementById('bulkExport').onclick=exportSelectedRoots;
    if(document.getElementById('bulkMove'))document.getElementById('bulkMove').onclick=()=>previewBulkTransfer('move');
    if(document.getElementById('bulkCopy'))document.getElementById('bulkCopy').onclick=()=>previewBulkTransfer('copy');
    if(document.getElementById('bulkQuarantine'))document.getElementById('bulkQuarantine').onclick=()=>previewBulk('quarantine');
    if(document.getElementById('bulkRemove'))document.getElementById('bulkRemove').onclick=()=>previewBulk('remove');
    if(document.getElementById('bulkClear'))document.getElementById('bulkClear').onclick=()=>{state.itemSelections.clear();renderItems(q,cursor).catch(showError);};
    document.getElementById('exportItemPage').onclick=()=>downloadJson(`clippy-item-search-${safeFileName(q||'results')}.json`,{query:q,filters:cursor,rows:data.rows});
    document.getElementById('itemsNext').onclick=()=>renderItems(q,{...cursor,after_class:data.next_after_class||'',after_storage:data.next_after_storage||'',after_root:data.next_after_root||'',after_item:data.next_after_item||''});
  }

  function exportSelectedRoots() {
    const items=[...state.itemSelections.values()];if(!items.length)return;
    openDialog({title:'Export selected roots',confirm:'Export JSON',body:`<div class="notice">Up to 25 selected root trees will be loaded one at a time and downloaded as one local JSON file. This is read-only and does not materialize cargo into DayZ.</div><div class="detail-grid">${pair('Roots',fmtInt(items.length))}${pair('Containers',fmtInt(new Set(items.map(x=>x.storage_id)).size))}</div>`,
      onSubmit:async()=>{const roots=[];for(const item of items){const data=await api(`/api/containers/${encodeURIComponent(item.storage_id)}/roots/${encodeURIComponent(item.root_item_id)}/tree`);roots.push({storage_id:item.storage_id,root_item_id:item.root_item_id,tree:data.tree});}downloadJson(`clippy-selected-roots-${Date.now()}.json`,{created_ms:Date.now(),roots});toast(`Exported ${roots.length} selected root trees.`);}});
  }

  async function previewBulk(action) {
    const items=[...state.itemSelections.values()];
    if(!items.length)return;
    try {
      const preview=await writeApi('/api/bulk/preview',{items});
      const conflicts=preview.conflicts||[];
      const body=`<div class="detail-grid">${pair('Roots',fmtInt(preview.roots_affected))}${pair('Containers',fmtInt(preview.containers_affected))}${pair('Nodes',fmtInt(preview.nodes_affected))}${pair('Conflicts',fmtInt(conflicts.length))}</div>${conflicts.length?`<div class="notice danger">This batch cannot run until every conflict is resolved.</div><pre class="json">${esc(JSON.stringify(conflicts,null,2))}</pre>`:`<div class="notice warn">Dry run passed. The final request will acquire maintenance locks and check every revision and workflow again before changing any root.</div>${reasonField(true)}`}`;
      openDialog({title:`Bulk ${action} preview`,confirm:conflicts.length?'Close':(action==='quarantine'?'Quarantine roots':'Remove roots'),danger:action==='remove',body,onSubmit:async dialog=>{if(conflicts.length)return;const result=await writeApi('/api/bulk/roots',{items,action,reason:field(dialog,'reason').trim()});state.itemSelections.clear();toast(`Bulk ${action} completed for ${result.roots_affected} roots.`);await navigate('items');}});
    } catch(error){toast(error.message,'danger');}
  }

  function previewBulkTransfer(action) {
    const items=[...state.itemSelections.values()];
    if(!items.length)return;
    const sources=new Set(items.map(item=>item.storage_id));
    if(sources.size!==1){toast('Bulk move/copy requires all selected roots to come from one source container.','warn');return;}
    const source=[...sources][0];
    openDialog({title:`Bulk ${action} roots`,confirm:'Run conflict preview',body:`<div class="notice">Selected roots will remain bounded to one source and one target container. The final request rechecks source and target revisions, active workflows, maintenance locks, target capacity, and item ID conflicts inside one transaction.</div><div class="detail-grid">${pair('Source',source)}${pair('Selected roots',fmtInt(items.length))}</div><label class="field"><span>Target storage ID *</span><input name="target" maxlength="128" required placeholder="storage ID"></label>${reasonField(true)}`,
      onSubmit:async dialog=>{
        const target=field(dialog,'target').trim();const reason=field(dialog,'reason').trim();if(!target)throw new Error('Target storage ID is required.');if(target===source)throw new Error('Choose a different target container.');
        const targetDetail=await api(`/api/containers/${encodeURIComponent(target)}`);
        const preview=await writeApi('/api/bulk/transfer/preview',{items,target_storage_id:target,target_expected_revision:targetDetail.revision});
        setTimeout(()=>showBulkTransferPreview(action,items,target,targetDetail.revision,reason,preview),0);
      }});
  }

  function showBulkTransferPreview(action,items,target,targetRevision,reason,preview) {
    const conflicts=preview.conflicts||[];const targetInfo=preview.target||{};
    const body=`<div class="detail-grid">${pair('Roots',fmtInt(preview.roots_affected))}${pair('Nodes',fmtInt(preview.nodes_affected))}${pair('Source',items[0]?.storage_id||'')}${pair('Target',targetInfo.display_name||target)}${pair('Target revision',fmtInt(targetInfo.current_revision??targetRevision))}${pair('Conflicts',fmtInt(conflicts.length))}</div>${conflicts.length?`<div class="notice danger">This transfer cannot run until every conflict is resolved.</div><pre class="json">${esc(JSON.stringify(conflicts,null,2))}</pre>`:`<div class="notice warn">Dry run passed. ${action==='move'?'The selected roots will leave the source container.':'Copies receive new item IDs and the source roots stay unchanged.'} The final request acquires maintenance locks and repeats every safety check.</div><div class="detail-pair"><div class="k">Reason</div><div class="v">${esc(reason)}</div></div>`}`;
    openDialog({title:`Bulk ${action} preview`,confirm:conflicts.length?'Close':(action==='move'?'Move roots':'Copy roots'),danger:false,body,onSubmit:async()=>{if(conflicts.length)return;const result=await writeApi('/api/bulk/transfer',{items,action,target_storage_id:target,target_expected_revision:targetRevision,reason});state.itemSelections.clear();toast(`Bulk ${action} completed for ${result.roots_affected} roots.`);await navigate('items');}});
  }

  async function actionFromSearch(buttonEl,action) {
    const row=JSON.parse(decodeURIComponent(buttonEl.dataset.row));
    try { const detail=await api(`/api/containers/${encodeURIComponent(row.storage_id)}`); const ctx={storageId:row.storage_id,rootId:row.root_item_id,revision:detail.revision,busy:Boolean(detail.active_sessions.length||detail.active_operations.length||detail.active_migrations.length||(detail.admin_lock&&!detail.admin_lock.owned_by_current_session))}; if(action==='edit')editItem(ctx,row);else quarantineItem(ctx,row); }
    catch(error){toast(error.message,'danger');}
  }

  async function renderActivity(filters={}) {
    const params=()=>{const p=new URLSearchParams({limit:'75'});for(const key of ['target','event','source','from_ms','to_ms'])if(filters[key])p.set(key,filters[key]);return p;};
    setContent(`${pageHead('Activity','Recent storage and successful admin events. This bounded view refreshes every five seconds.')}
      <section class="panel"><div class="panel-body"><div class="search-grid"><label class="field"><span>Target</span><input id="activityTarget" maxlength="128" value="${esc(filters.target||'')}" placeholder="container, operation, item"></label><label class="field"><span>Event type</span><input id="activityEvent" maxlength="128" value="${esc(filters.event||'')}" placeholder="event type"></label><label class="field"><span>Source</span><select id="activitySource"><option value="">All</option><option value="storage" ${filters.source==='storage'?'selected':''}>Storage</option><option value="admin" ${filters.source==='admin'?'selected':''}>Admin</option></select></label><label class="field"><span>From</span><input id="activityFrom" type="datetime-local" value="${esc(fmtLocalInput(filters.from_ms))}"></label><label class="field"><span>To</span><input id="activityTo" type="datetime-local" value="${esc(fmtLocalInput(filters.to_ms))}"></label><button class="button primary search-submit" id="activityApply">Apply</button></div></div></section><div id="activityHost"></div>`);
    const load=async()=>{const data=await api(`/api/activity?${params()}`);if(state.route!=='activity')return;const host=document.getElementById('activityHost');if(host)host.innerHTML=activityTable(data.rows||[]);};
    document.getElementById('activityApply').onclick=()=>{const from=localInputMs(document.getElementById('activityFrom').value);const to=localInputMs(document.getElementById('activityTo').value);if(from&&to&&from>to){toast('Activity From must be before To.','warn');return;}renderActivity({target:document.getElementById('activityTarget').value.trim(),event:document.getElementById('activityEvent').value.trim(),source:document.getElementById('activitySource').value,from_ms:from,to_ms:to}).catch(showError);};
    await load();
    state.activityTimer=setInterval(()=>load().catch(()=>{}),5000);
  }

  function activityTable(rows) {
    return `<section class="panel">${rows.length?`<div class="table-wrap"><table><thead><tr><th>Time</th><th>Source</th><th>Event</th><th>Target</th><th>Detail</th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td>${chip(r.source,r.source==='admin'?'edit':'')}</td><td>${esc(r.event_type)}</td><td class="mono">${esc(r.target_id||'')}</td><td><code>${esc(JSON.stringify(r.detail||{}))}</code></td></tr>`).join('')}</tbody></table></div>`:'<div class="empty">No activity recorded.</div>'}</section>`;
  }

  async function renderSessions(cursor={}) {
    const params=new URLSearchParams({limit:'75'});if(cursor.before_ms)params.set('before_ms',cursor.before_ms);if(cursor.before_id)params.set('before_id',cursor.before_id);
    const data=await api(`/api/sessions?${params}`);
    setContent(`${pageHead('Sessions','Cargo sessions and their recovery state.')}
      <section class="panel"><div class="table-wrap"><table><thead><tr><th>Updated</th><th>Container</th><th>Session</th><th>Player</th><th>Status</th><th>Expected rev</th><th>Cleanup</th><th></th></tr></thead><tbody>${data.rows.map(r=>`<tr><td>${esc(fmtTime(r.updated_ms))}</td><td><button class="link-button open-container" data-id="${esc(r.storage_id)}">${esc(r.container)}</button></td><td class="mono">${esc(r.session_id)}</td><td class="mono">${esc(r.player_id)}</td><td>${chip(r.status,r.status==='OPEN'?'warn':'')}</td><td>${fmtInt(r.expected_revision)}</td><td>${fmtInt(r.pending_cleanup)}</td><td>${state.editingEnabled&&r.status==='OPEN'?`<button class="button abort-session" data-id="${esc(r.session_id)}">Abort safely</button>`:''}</td></tr>`).join('')}</tbody></table></div>${data.rows.length?'':'<div class="empty">No sessions recorded.</div>'}</section><div class="pager"><button class="button" id="sessionsNext" ${data.next_before_id?'':'disabled'}>Older</button></div>`);
    document.querySelectorAll('.open-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
    document.querySelectorAll('.abort-session').forEach(b=>b.onclick=()=>abortSession(b.dataset.id));
    document.getElementById('sessionsNext').onclick=()=>renderSessions({before_ms:data.next_before_ms,before_id:data.next_before_id});
  }

  function abortSession(id) {
    openDialog({title:'Abort open cargo session',confirm:'Abort session',danger:true,body:`<div class="notice warn">StorageHost will reject this if the session was already materialized. The browser does not rewrite session status directly.</div>${reasonField(true)}`,
      onSubmit:async d=>{await writeApi(`/api/recovery/sessions/${encodeURIComponent(id)}/abort`,{reason:field(d,'reason').trim()});toast('Session abort request completed.');await navigate('sessions');}});
  }

  async function renderRecovery() {
    const data=await api('/api/recovery');
    const actions='<button class="button" id="runIntegrity">Run integrity check</button> <button class="button" id="exportRecovery">Export diagnostics</button>';
    setContent(`${pageHead('Recovery','Inspect unfinished storage workflows and call existing StorageHost recovery logic.',actions)}
      <div class="cards">${card('Incomplete operations',fmtInt((data.operations||[]).length),'Bounded current view')}${card('Active sessions',fmtInt((data.sessions||[]).length),'OPEN, MATERIALIZED, COMMITTED')}${card('Stale sessions',fmtInt((data.stale_sessions||[]).length),'No update for 30+ minutes')}${card('Failed migrations',fmtInt((data.failed_migrations||[]).length),'Incomplete migrations with errors')}${card('Revision conflicts',fmtInt(data.revision_conflict_failures),'Recorded admin failures')}${card('Orphan roots',fmtInt(data.orphaned_virtual_roots),'Roots without a container row')}${card('Blocked containers',fmtInt(data.blocked_containers),'Unfinished workflow state')}</div>
      ${data.last_integrity?`<section class="panel"><div class="panel-head"><h2>Latest integrity check</h2><span>${esc(fmtTime(data.last_integrity.created_ms))}</span></div><div class="panel-body"><pre class="json">${esc(JSON.stringify(data.last_integrity.detail,null,2))}</pre></div></section>`:''}
      <section class="panel"><div class="panel-head"><h2>Operations</h2></div>${recoveryOperations(data.operations||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Active sessions</h2></div>${recoverySessions(data.sessions||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Stale sessions</h2></div>${recoveryStaleSessions(data.stale_sessions||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Migrations</h2></div>${recoveryMigrations(data.migrations||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Failed migrations</h2></div>${recoveryFailedMigrations(data.failed_migrations||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Pending cleanup</h2></div><div class="panel-body detail-grid">${pair('Operations',fmtInt(data.pending_cleanup.operations))}${pair('Sessions',fmtInt(data.pending_cleanup.sessions))}${pair('Migrations',fmtInt(data.pending_cleanup.migrations))}</div><div class="panel-body muted">Cleanup acknowledgements stay DayZ-aware. The panel does not mark physical cleanup complete by changing database status fields.</div></section>`);
    document.getElementById('runIntegrity').onclick=runIntegrity;
    document.getElementById('exportRecovery').onclick=()=>downloadJson(`clippy-recovery-${Date.now()}.json`,data);
    document.querySelectorAll('.abort-operation').forEach(b=>b.onclick=()=>abortOperation(b.dataset.id,b.dataset.status));
    document.querySelectorAll('.abort-session-recovery').forEach(b=>b.onclick=()=>abortSession(b.dataset.id));
    document.querySelectorAll('.inspect-json').forEach(b=>b.onclick=()=>inspectJson(b.dataset.title,JSON.parse(decodeURIComponent(b.dataset.row))));
  }

  function inspectJson(title,value){openDialog({title,confirm:'Close',body:`<pre class="json">${esc(JSON.stringify(value,null,2))}</pre>`,onSubmit:async()=>{}});}

  function recoveryOperations(rows) {
    if(!rows.length)return '<div class="empty">No incomplete operations.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Created</th><th>ID</th><th>Kind</th><th>Status</th><th>Cleanup</th><th>Storage</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td class="mono">${esc(r.operation_id)}</td><td>${esc(r.kind)}</td><td>${chip(r.status,'warn')}</td><td>${esc(r.cleanup_state)}</td><td class="mono">${esc(r.storage_id)}</td><td><button class="button inspect-json" data-title="Operation ${esc(r.operation_id)}" data-row="${esc(encodeURIComponent(JSON.stringify(r)))}">Inspect</button> ${state.editingEnabled&&['PREPARED','QUARANTINED'].includes(r.status)?`<button class="button abort-operation" data-id="${esc(r.operation_id)}" data-status="${esc(r.status)}">Abort safely</button>`:''}</td></tr>`).join('')}</tbody></table></div>`;
  }
  function recoverySessions(rows) {
    if(!rows.length)return '<div class="empty">No active sessions.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Created</th><th>ID</th><th>Status</th><th>Storage</th><th>Player</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td class="mono">${esc(r.session_id)}</td><td>${chip(r.status,'warn')}</td><td class="mono">${esc(r.storage_id)}</td><td class="mono">${esc(r.player_id)}</td><td><button class="button inspect-json" data-title="Session ${esc(r.session_id)}" data-row="${esc(encodeURIComponent(JSON.stringify(r)))}">Inspect</button> ${state.editingEnabled&&r.status==='OPEN'?`<button class="button abort-session-recovery" data-id="${esc(r.session_id)}">Release safely</button>`:''}</td></tr>`).join('')}</tbody></table></div>`;
  }
  function recoveryStaleSessions(rows){if(!rows.length)return '<div class="empty">No stale sessions in the current diagnostic window.</div>';return `<div class="table-wrap"><table><thead><tr><th>Updated</th><th>ID</th><th>Status</th><th>Storage</th><th>Error</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.updated_ms))}</td><td class="mono">${esc(r.session_id)}</td><td>${chip(r.status,'warn')}</td><td class="mono">${esc(r.storage_id)}</td><td>${esc(r.error||'')}</td><td><button class="button inspect-json" data-title="Stale session ${esc(r.session_id)}" data-row="${esc(encodeURIComponent(JSON.stringify(r)))}">Inspect</button> ${state.editingEnabled&&r.status==='OPEN'?`<button class="button abort-session-recovery" data-id="${esc(r.session_id)}">Release safely</button>`:''}</td></tr>`).join('')}</tbody></table></div>`;}
  function recoveryMigrations(rows) {
    if(!rows.length)return '<div class="empty">No incomplete migrations.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Created</th><th>ID</th><th>Status</th><th>Storage</th><th>Class</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td class="mono">${esc(r.migration_id)}</td><td>${chip(r.status,'warn')}</td><td class="mono">${esc(r.storage_id)}</td><td>${esc(r.container_class)}</td><td><button class="button inspect-json" data-title="Migration ${esc(r.migration_id)}" data-row="${esc(encodeURIComponent(JSON.stringify(r)))}">Inspect</button></td></tr>`).join('')}</tbody></table></div>`;
  }
  function recoveryFailedMigrations(rows){if(!rows.length)return '<div class="empty">No recent failed migrations.</div>';return `<div class="table-wrap"><table><thead><tr><th>Updated</th><th>ID</th><th>Storage</th><th>Class</th><th>Error</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.updated_ms))}</td><td class="mono">${esc(r.migration_id)}</td><td class="mono">${esc(r.storage_id)}</td><td>${esc(r.container_class)}</td><td>${esc(r.error||'')}</td><td><button class="button inspect-json" data-title="Failed migration ${esc(r.migration_id)}" data-row="${esc(encodeURIComponent(JSON.stringify(r)))}">Inspect</button></td></tr>`).join('')}</tbody></table></div>`;}

  function abortOperation(id,status) {
    const extra=status==='QUARANTINED'?'This operation already quarantined a physical item. StorageHost will move it into its normal pending-cleanup recovery state instead of pretending cleanup is complete.':'';
    openDialog({title:'Abort storage operation',confirm:'Abort operation',danger:true,body:`<div class="notice warn">${esc(extra||'StorageHost will abort only if its current operation state allows it.')}</div>${reasonField(true)}`,
      onSubmit:async d=>{await writeApi(`/api/recovery/operations/${encodeURIComponent(id)}/abort`,{reason:field(d,'reason').trim()});toast('Operation recovery request completed.');await navigate('recovery');}});
  }

  async function runIntegrity() {
    openDialog({title:'Run integrity check',confirm:'Run check',body:'<div class="notice">This calls the existing StorageHost logical integrity checks. It does not run browser-supplied SQL.</div>',onSubmit:async()=>{const result=await writeApi('/api/recovery/integrity',{});toast(result.healthy?'Integrity check passed.':'Integrity check reported a problem.',result.healthy?'good':'warn');await navigate('recovery');}});
  }

  async function renderMaintenance(cursor={}) {
    const params=new URLSearchParams({limit:'75'});if(cursor.before_expiry_ms)params.set('before_expiry_ms',cursor.before_expiry_ms);if(cursor.before_storage_id)params.set('before_storage_id',cursor.before_storage_id);
    const data=await api(`/api/locks?${params}`);const rows=data.rows||[];
    setContent(`${pageHead('Maintenance','Unexpired container locks that coordinate admin work with DayZ storage workflows.',`<button class="button" id="maintenanceRecovery">Open recovery</button>`)}
      <div class="cards">${card('Active locks',fmtInt(rows.length),data.next_before_storage_id?'More locks on older pages':'Current page')}${card('Lock lifetime','Auto-expiring','Renew from container detail')}${card('DayZ behavior','New opens blocked','Existing workflows are checked before lock acquisition')}</div>
      <section class="panel"><div class="panel-head"><h2>Active maintenance locks</h2><span class="muted">Session IDs are intentionally not exposed</span></div>${rows.length?`<div class="table-wrap"><table><thead><tr><th>Expires</th><th>Container</th><th>Storage ID</th><th>Reason</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.expires_ms))}</td><td>${esc(r.display_name||r.storage_id)}</td><td class="mono">${esc(r.storage_id)}</td><td>${esc(r.reason||'')}</td><td><button class="button open-maintenance-container" data-id="${esc(r.storage_id)}">Open container</button></td></tr>`).join('')}</tbody></table></div>`:'<div class="empty">No active maintenance locks.</div>'}</section>
      <div class="notice">Locks expire automatically after the configured maintenance-lock period. Open a container to see whether the current browser session owns its lock and to renew or release it safely.</div>
      <div class="pager"><button class="button" id="maintenanceNext" ${data.next_before_storage_id?'':'disabled'}>Older expirations</button></div>`);
    document.getElementById('maintenanceRecovery').onclick=()=>navigate('recovery');
    document.querySelectorAll('.open-maintenance-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
    document.getElementById('maintenanceNext').onclick=()=>renderMaintenance({before_expiry_ms:data.next_before_expiry_ms,before_storage_id:data.next_before_storage_id});
  }

  async function renderBackups() {
    const [data,creates,verifies]=await Promise.all([api('/api/backups'),api('/api/audit?action=create_backup&limit=100'),api('/api/audit?action=verify_backup&limit=100')]);
    const leaf=value=>String(value||'').split(/[\\/]/).pop()||'';
    const createByFile=new Map();for(const row of creates.rows||[]){const file=leaf(row.detail?.path);if(file&&!createByFile.has(file))createByFile.set(file,row);}
    const verifyByFile=new Map();for(const row of verifies.rows||[]){const file=row.target_id||leaf(row.detail?.path);if(file&&!verifyByFile.has(file))verifyByFile.set(file,row);}
    const action='<button class="button" id="openBackupFolder">Open backup folder</button> <button class="button primary" id="createBackup">Create backup now</button>';
    setContent(`${pageHead('Backups','PostgreSQL custom-format backups created by StorageHost.',action)}<section class="panel">${data.rows.length?`<div class="table-wrap"><table><thead><tr><th>Created</th><th>File</th><th>Reason</th><th>Size</th><th>Verification</th><th></th></tr></thead><tbody>${data.rows.map(r=>{const created=createByFile.get(r.file);const verified=verifyByFile.get(r.file);return `<tr><td>${esc(fmtTime(r.created_ms))}</td><td class="mono">${esc(r.file)}</td><td>${esc(created?.reason||'')}</td><td>${fmtBytes(r.bytes)}</td><td>${chip(verified?`verified ${fmtTime(verified.created_ms)}`:(r.verification||'verified when created'),'good')}</td><td><button class="button verify-backup" data-file="${esc(r.file)}">Verify</button> <button class="button restore-backup-help" data-file="${esc(r.file)}">Restore</button> <button class="button copy-backup-path" data-path="${esc(r.path||r.file)}">Copy path</button></td></tr>`;}).join('')}</tbody></table></div>`:'<div class="empty">No generated PostgreSQL backups found.</div>'}</section>`);
    document.getElementById('createBackup').onclick=createBackup;
    document.getElementById('openBackupFolder').onclick=async()=>{try{await writeApi('/api/backups/open-folder',{});toast('Opened backup folder.');}catch(error){toast(error.message,'danger');}};
    document.querySelectorAll('.verify-backup').forEach(b=>b.onclick=()=>verifyBackup(b.dataset.file));
    document.querySelectorAll('.restore-backup-help').forEach(b=>b.onclick=()=>showRestoreBackupInstructions(b.dataset.file));
    document.querySelectorAll('.copy-backup-path').forEach(b=>b.onclick=async()=>{try{await navigator.clipboard.writeText(b.dataset.path);toast('Backup path copied.');}catch(_){toast('Clipboard access was blocked by the browser.','warn');}});
  }

  function showRestoreBackupInstructions(file) {
    const command=`powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\\ClippyServerManager.ps1 restore-backup "${String(file).replace(/"/g,'')}"`;
    openDialog({title:'Restore PostgreSQL backup',confirm:'Copy restore command',danger:true,body:`<div class="notice danger">Restore is intentionally manager-controlled. DayZ must be stopped. The manager closes ClippyAdminHost and ClippyStorageHost, verifies this archive, creates and verifies a fresh safety backup, restores PostgreSQL, runs schema migration and integrity validation, then leaves the server stopped.</div><div class="detail-pair"><div class="k">Backup</div><div class="v mono">${esc(file)}</div></div><label class="field"><span>Run from the DayZ server root</span><textarea name="restore_command" rows="3" readonly>${esc(command)}</textarea></label><div class="notice">The manager requires you to type the exact backup name again before it changes the database.</div>`,onSubmit:async d=>{const text=field(d,'restore_command');try{await navigator.clipboard.writeText(text);toast('Restore command copied. Close the panel before running it.');}catch(_){toast('Clipboard access was blocked. Copy the command from the dialog.','warn');}}});
  }

  function createBackup() {
    openDialog({title:'Create PostgreSQL backup',confirm:'Create backup',body:`<div class="notice">StorageHost runs pg_dump and verifies the custom archive with pg_restore --list before reporting success. This does not change virtual cargo.</div>${reasonField(false)}`,onSubmit:async d=>{const result=await writeApi('/api/backups',{reason:field(d,'reason').trim()});toast(`Backup created: ${result.path || 'complete'}`);await navigate('backups');}});
  }

  function verifyBackup(file) {
    openDialog({title:'Verify PostgreSQL backup',confirm:'Verify archive',body:`<div class="notice">StorageHost will run pg_restore --list against this existing Clippy backup. No restore is performed.</div><div class="mono">${esc(file)}</div>${reasonField(false)}`,onSubmit:async d=>{await writeApi(`/api/backups/${encodeURIComponent(file)}/verify`,{reason:field(d,'reason').trim()});toast('Backup archive verified.');await navigate('backups');}});
  }

  async function renderQuarantine(cursor={}) {
    const params=new URLSearchParams({limit:'75'});if(cursor.before_ms)params.set('before_ms',cursor.before_ms);if(cursor.before_id)params.set('before_id',cursor.before_id);
    const data=await api(`/api/quarantine?${params}`);
    setContent(`${pageHead('Quarantine','Recoverable item trees removed by admin actions.')}<section class="panel"><div class="table-wrap"><table><thead><tr><th>Time</th><th>Class</th><th>Container</th><th>Item ID</th><th>State</th><th>Reason</th><th></th></tr></thead><tbody>${data.rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td>${esc(r.class_name)}</td><td><button class="link-button open-container" data-id="${esc(r.storage_id)}">${esc(r.container)}</button></td><td class="mono">${esc(r.item_id)}</td><td>${r.restored_ms?chip('restored','good'):chip('quarantined','warn')}</td><td>${esc(r.reason||'')}</td><td>${state.editingEnabled&&!r.restored_ms?`<button class="button restore-quarantine" data-q="${esc(r.quarantine_id)}" data-storage="${esc(r.storage_id)}">Restore</button>`:''}</td></tr>`).join('')}</tbody></table></div>${data.rows.length?'':'<div class="empty">No quarantine entries.</div>'}</section><div class="pager"><button class="button" id="quarantineNext" ${data.next_before_id?'':'disabled'}>Older</button></div>`);
    document.querySelectorAll('.open-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
    document.querySelectorAll('.restore-quarantine').forEach(b=>b.onclick=()=>restoreQuarantine(b.dataset.q,b.dataset.storage));
    document.getElementById('quarantineNext').onclick=()=>renderQuarantine({before_ms:data.next_before_ms,before_id:data.next_before_id});
  }

  function restoreQuarantine(id,storageId) {
    openDialog({title:'Restore quarantined item',confirm:'Restore',body:`<div class="notice warn">Restore succeeds only if the original root or parent is still compatible and the container revision has not changed after this confirmation starts.</div>${reasonField(true)}`,
      onSubmit:async d=>{const detail=await api(`/api/containers/${encodeURIComponent(storageId)}`);await writeApi(`/api/quarantine/${encodeURIComponent(id)}/restore`,{expected_revision:detail.revision,reason:field(d,'reason').trim()});toast('Quarantined item restored.');await navigate('quarantine');}});
  }

  async function renderAudit(stateFilter={}) {
    const params=new URLSearchParams({limit:'75'});
    for(const key of ['before_ms','before_id','from_ms','to_ms','admin','action','target_type','target_id','result'])if(stateFilter[key])params.set(key,stateFilter[key]);
    const [audit,changes]=await Promise.all([api(`/api/audit?${params}`),api('/api/admin/changes?limit=30')]);
    setContent(`${pageHead('Audit Log','Durable admin action results and revision-safe change history.',`<button class="button" id="exportAudit">Export page</button>`)}
      <section class="panel"><div class="panel-body"><div class="search-grid"><label class="field"><span>Admin</span><input id="auditAdmin" maxlength="128" value="${esc(stateFilter.admin||'')}"></label><label class="field"><span>Action</span><input id="auditAction" maxlength="128" value="${esc(stateFilter.action||'')}"></label><label class="field"><span>Target type</span><input id="auditTargetType" maxlength="64" value="${esc(stateFilter.target_type||'')}"></label><label class="field"><span>Target ID</span><input id="auditTargetId" maxlength="128" value="${esc(stateFilter.target_id||'')}"></label><label class="field"><span>Result</span><select id="auditResult"><option value="">All</option><option value="SUCCESS" ${stateFilter.result==='SUCCESS'?'selected':''}>Success</option><option value="FAILURE" ${stateFilter.result==='FAILURE'?'selected':''}>Failure</option></select></label><label class="field"><span>From</span><input id="auditFrom" type="datetime-local" value="${esc(fmtLocalInput(stateFilter.from_ms))}"></label><label class="field"><span>To</span><input id="auditTo" type="datetime-local" value="${esc(fmtLocalInput(stateFilter.to_ms))}"></label><button class="button primary search-submit" id="auditApply">Apply</button></div></div></section>
      <section class="panel"><div class="panel-head"><h2>Recent changes</h2></div>${changesTable(changes.rows)}</section>
      <section class="panel"><div class="panel-head"><h2>Action audit</h2></div>${audit.rows.length?`<div class="table-wrap"><table><thead><tr><th>Time</th><th>Result</th><th>Action</th><th>Target</th><th>Admin</th><th>Reason / error</th><th>Request</th></tr></thead><tbody>${audit.rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td>${chip(r.result,r.result==='SUCCESS'?'good':'danger')}</td><td>${esc(r.action)}</td><td><span>${esc(r.target_type)}</span><div class="mono muted">${esc(r.target_id)}</div></td><td>${esc(r.windows_identity||'')}</td><td>${esc(r.error||r.reason||'')}</td><td class="mono">${esc(r.request_id||'')}</td></tr>`).join('')}</tbody></table></div>`:'<div class="empty">No admin audit records matched.</div>'}</section><div class="pager"><button class="button" id="auditNext" ${audit.next_before_id?'':'disabled'}>Older audit events</button></div>`);
    wireChangeButtons();
    document.getElementById('auditApply').onclick=()=>{const from=localInputMs(document.getElementById('auditFrom').value);const to=localInputMs(document.getElementById('auditTo').value);if(from&&to&&from>to){toast('Audit From must be before To.','warn');return;}renderAudit({admin:document.getElementById('auditAdmin').value.trim(),action:document.getElementById('auditAction').value.trim(),target_type:document.getElementById('auditTargetType').value.trim(),target_id:document.getElementById('auditTargetId').value.trim(),result:document.getElementById('auditResult').value,from_ms:from,to_ms:to}).catch(showError);};
    document.getElementById('exportAudit').onclick=()=>downloadJson('clippy-admin-audit-page.json',{filters:stateFilter,rows:audit.rows});
    document.getElementById('auditNext').onclick=()=>renderAudit({...stateFilter,before_ms:audit.next_before_ms,before_id:audit.next_before_id});
  }

  async function renderDatabase() {
    const data=await api('/api/database/info');
    setContent(`${pageHead('Database','Safe inspection of the private Clippy PostgreSQL database.')}
      <div class="cards">${card('PostgreSQL',data.postgres_version,'127.0.0.1 only')}${card('Database',data.database,fmtBytes(data.size_bytes))}${card('Schema',`v${data.schema_version}`,data.item_index_complete?'Item index ready':'Index backfill pending')}${card('Read role',data.role,data.transaction_read_only?'transaction_read_only=on':'read role check failed')}${card('Connections',fmtInt(data.connections),'Current database backends')}${card('Schemas',(data.schemas||[]).join(', ')||'clippy','Visible to the admin read role')}</div>
      <section class="panel"><div class="panel-head"><h2>Clippy tables</h2><span class="muted">Select a table for a bounded read-only preview</span></div><div class="table-wrap"><table><thead><tr><th>Table</th><th>Estimated rows</th><th>Total</th><th>Table</th><th>Indexes</th><th></th></tr></thead><tbody>${data.tables.map(r=>`<tr><td class="mono">${esc(r.name)}</td><td>${fmtInt(r.estimated_rows)}</td><td>${fmtBytes(r.total_bytes)}</td><td>${fmtBytes(r.table_bytes)}</td><td>${fmtBytes(r.index_bytes)}</td><td><button class="button browse-db-table" data-table="${esc(r.name)}">Browse</button></td></tr>`).join('')}</tbody></table></div></section>
      <section class="panel" id="dbPreview" hidden><div class="panel-head"><h2 id="dbPreviewTitle">Table preview</h2></div><div id="dbPreviewBody"></div></section>
      <div class="notice">There is no writable SQL console. Table names come from a fixed server-side allowlist, values are parameterized, large tree/change fields are omitted from previews, and browser writes use domain endpoints only.</div>`);
    document.querySelectorAll('.browse-db-table').forEach(b=>b.onclick=()=>browseDatabaseTable(b.dataset.table,''));
  }

  async function browseDatabaseTable(table,after='') {
    try {
      const data=await api(`/api/database/table/${encodeURIComponent(table)}?limit=50&after=${encodeURIComponent(after)}`);
      const panel=document.getElementById('dbPreview');const body=document.getElementById('dbPreviewBody');const title=document.getElementById('dbPreviewTitle');
      panel.hidden=false;title.textContent=`${table} preview`;body.innerHTML=(data.rows||[]).length?`<div class="table-wrap"><table><thead><tr><th>CTID</th><th>Row</th><th></th></tr></thead><tbody>${data.rows.map(r=>`<tr><td class="mono">${esc(r.ctid)}</td><td><code>${esc(JSON.stringify(r.row))}</code></td><td><button class="button inspect-db-row" data-row="${esc(encodeURIComponent(JSON.stringify(r.row)))}">Inspect</button></td></tr>`).join('')}</tbody></table></div><div class="panel-body"><span class="muted">Omitted large fields: ${(data.omitted_large_fields||[]).map(esc).join(', ')}</span> <button class="button" id="dbNext" ${data.next_after?'':'disabled'}>Next page</button></div>`:'<div class="empty">No rows in this table.</div>';
      document.querySelectorAll('.inspect-db-row').forEach(b=>b.onclick=()=>inspectJson(`${table} row`,JSON.parse(decodeURIComponent(b.dataset.row))));
      const next=document.getElementById('dbNext');if(next)next.onclick=()=>browseDatabaseTable(table,data.next_after||'');panel.scrollIntoView({block:'start'});
    } catch(error){toast(error.message,'danger');}
  }


  async function renderReports() {
    const reports=[
      ['top_classes','Most stored item classes'],
      ['container_types','Containers by class'],
      ['largest_containers','Largest virtual containers'],
      ['stale_containers','Stale container counts'],
      ['player_classes','Most carried item classes'],
      ['duplicate_item_ids','Duplicate virtual item IDs']
    ];
    setContent(`${pageHead('Reports','On-demand economy and forensic reports. Nothing on this page runs until you choose a report.')}
      <div class="notice warn">Reports can scan derived indexes or aggregate container rows. They use the admin read pool and short statement timeout, but you should still run the included EXPLAIN ANALYZE benchmarks before using large reports during peak gameplay.</div>
      <section class="panel"><div class="panel-body"><div class="toolbar">${reports.map(([k,l])=>`<button class="button run-report" data-kind="${k}">${esc(l)}</button>`).join(' ')}</div></div></section>
      <section class="panel" id="reportOutput"><div class="empty">Choose a report above.</div></section>`);
    document.querySelectorAll('.run-report').forEach(b=>b.onclick=()=>runReport(b.dataset.kind,b.textContent).catch(showError));
  }

  async function runReport(kind,title) {
    const host=document.getElementById('reportOutput');host.innerHTML='<div class="empty">Running report…</div>';
    const data=await api(`/api/reports?kind=${encodeURIComponent(kind)}&limit=25`);
    const rows=data.rows||[];
    if(!rows.length){host.innerHTML=`<div class="panel-head"><h2>${esc(title)}</h2></div><div class="empty">No rows matched.</div>`;return;}
    const keys=Object.keys(rows[0]);
    host.innerHTML=`<div class="panel-head"><h2>${esc(title)}</h2><span class="muted">Top ${fmtInt(rows.length)}</span></div><div class="table-wrap"><table><thead><tr>${keys.map(k=>`<th>${esc(k.replaceAll('_',' '))}</th>`).join('')}</tr></thead><tbody>${rows.map(r=>`<tr>${keys.map(k=>`<td>${k.endsWith('_ms')?esc(fmtTime(r[k])):typeof r[k]==='number'?esc(fmtNum(r[k])):esc(r[k]??'')}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`;
  }

  async function renderPlayers(query='', beforeMs=0, beforeId='') {
    const settings=await api('/api/settings');
    const params=new URLSearchParams({limit:'50',q:query});
    if(beforeMs)params.set('before_ms',String(beforeMs));if(beforeId)params.set('before_id',beforeId);
    const [players,items]=await Promise.all([api(`/api/players?${params}`),api(`/api/player-items/search?limit=50&q=${encodeURIComponent(query)}`)]);
    const telemetry=settings.player_telemetry_enabled;
    const live=telemetry&&settings.live_player_control_enabled&&state.editingEnabled;
    const banner=telemetry
      ? `<div class="notice">Player telemetry is enabled. Inventory shown here comes from server-side DayZ snapshots, not from virtual-cargo guesses.${live?' Live commands are enabled and execute inside DayZ.':' Live inventory control is disabled.'}</div>`
      : '<div class="notice warn">Player telemetry is currently disabled in ClippyServerManager.json. Historical snapshots remain readable, but they may be stale.</div>';
    setContent(`${pageHead('Players','Server-side player registry, inventory snapshots, item search, and optional live controls.')}
      ${banner}
      <section class="panel"><div class="panel-body"><div class="search-grid"><label class="field wide"><span>Player</span><input id="playerSearch" maxlength="128" placeholder="name or player ID" value="${esc(query)}"></label><button class="button primary search-submit" id="playerSearchButton">Search</button></div></div></section>
      <section class="panel"><div class="panel-head"><h2>Player registry</h2><span class="muted">Online is based on recent telemetry</span></div><div class="table-wrap"><table><thead><tr><th>Player</th><th>Status</th><th>Latest inventory</th><th>Network</th><th>Map</th><th>Last seen</th><th>Aliases</th><th></th></tr></thead><tbody>${(players.rows||[]).map(r=>`<tr><td><strong>${esc(r.display_name||r.player_id)}</strong><div class="mono muted">${esc(r.player_id)}</div></td><td>${r.online?chip('online','good'):chip('offline')}</td><td>${fmtInt(r.last_inventory_count)} items<div class="muted">${esc(fmtTime(r.last_snapshot_ms))}</div></td><td>${r.last_ping_ms==null?'Unavailable':`${fmtInt(r.last_ping_ms)} ms`}<div class="muted">${r.last_bandwidth_kbps==null?'':`${fmtInt(r.last_bandwidth_kbps)} kbps`}</div></td><td>${esc(r.last_map_name||'')}</td><td>${esc(fmtTime(r.last_seen_ms))}</td><td>${fmtInt(r.alias_count)}</td><td><button class="button open-player" data-player="${esc(r.player_id)}">Open</button></td></tr>`).join('')}</tbody></table></div>${(players.rows||[]).length?'':'<div class="empty">No players matched.</div>'}</section>
      <div class="pager"><button class="button" id="playersNext" ${players.next_before_id?'':'disabled'}>Next page</button></div>
      <section class="panel"><div class="panel-head"><h2>Latest carried items</h2><span class="muted">Searches the latest stored snapshot for each player</span></div><div class="table-wrap"><table><thead><tr><th>Class</th><th>Player</th><th>Item ID</th><th>Depth</th><th>Qty</th><th>Health</th><th>Snapshot</th></tr></thead><tbody>${(items.rows||[]).map(r=>`<tr><td>${esc(r.class_name)}</td><td><button class="link-button open-player" data-player="${esc(r.player_id)}">${esc(r.display_name||r.player_id)}</button></td><td class="mono">${esc(r.item_id)}</td><td>${fmtInt(r.depth)}</td><td>${fmtNum(r.quantity)}</td><td>${fmtNum(r.health)}</td><td>${esc(fmtTime(r.captured_ms))}</td></tr>`).join('')}</tbody></table></div>${(items.rows||[]).length?'':'<div class="empty">No carried items matched this search.</div>'}</section>`);
    const apply=()=>renderPlayers(document.getElementById('playerSearch').value.trim()).catch(showError);
    document.getElementById('playerSearchButton').onclick=apply;document.getElementById('playerSearch').addEventListener('keydown',e=>{if(e.key==='Enter')apply();});
    document.querySelectorAll('.open-player').forEach(b=>b.onclick=()=>renderPlayerDetail(b.dataset.player).catch(showError));
    document.getElementById('playersNext').onclick=()=>renderPlayers(query,players.next_before_ms||0,players.next_before_id||'').catch(showError);
  }

  async function renderPlayerDetail(playerId) {
    const settings=await api('/api/settings');
    const [detail,commands,quarantine]=await Promise.all([
      api(`/api/players/${encodeURIComponent(playerId)}`),
      api(`/api/player-commands?player_id=${encodeURIComponent(playerId)}&limit=25`),
      api(`/api/player-quarantine?player_id=${encodeURIComponent(playerId)}&limit=25`)
    ]);
    const live=Boolean(settings.player_telemetry_enabled&&settings.live_player_control_enabled&&state.editingEnabled);
    const canCommand=live&&detail.online;
    const actions=`<button class="button" id="backPlayers">Back</button>${live?` <button class="button primary" id="requestPlayerSnapshot" ${canCommand?'':'disabled'}>Request fresh snapshot</button> <button class="button" id="givePlayerItem" ${canCommand?'':'disabled'}>Give item</button>`:''}`;
    setContent(`${pageHead(detail.display_name||'Player',detail.player_id,actions)}
      ${live&&!detail.online?'<div class="notice warn">This player is not currently reporting telemetry. Live commands are disabled until the player is online.</div>':''}
      <div class="cards">${card('Status',detail.online?'Online':'Offline',`Last seen ${fmtTime(detail.last_seen_ms)}`)}${card('Latest inventory',`${fmtInt(detail.last_inventory_count)} items`,fmtTime(detail.last_snapshot_ms))}${card('Network',detail.last_ping_ms==null?'Unavailable':`${fmtInt(detail.last_ping_ms)} ms ping`,detail.last_bandwidth_kbps==null?'No bandwidth estimate':`${fmtInt(detail.last_bandwidth_kbps)} kbps average`)}${card('Position',detail.last_map_name||'Unavailable',detail.last_position_x==null?'No position snapshot':`${fmtNum(detail.last_position_x)}, ${fmtNum(detail.last_position_y)}, ${fmtNum(detail.last_position_z)}`)}${card('Profile',detail.full_name||detail.plain_name||detail.display_name||'',detail.last_session_player_id==null?'No session ID':`Session ID ${fmtInt(detail.last_session_player_id)}`)}${card('Aliases',fmtInt((detail.aliases||[]).length),'Names reported by DayZ')}</div>
      <div class="notice">Player IP addresses are not collected by this mod because the supported DayZ server script API does not expose them. Network telemetry is limited to ping, bandwidth estimates, and output throttle reported by PlayerIdentity.</div>
      <section class="panel"><div class="panel-head"><h2>Snapshots</h2><span class="muted">Latest 25</span></div><div class="table-wrap"><table><thead><tr><th>Captured</th><th>Items</th><th>Network</th><th>Position</th><th>Equipment summary</th><th></th></tr></thead><tbody>${(detail.snapshots||[]).map((r,i)=>`<tr><td>${esc(fmtTime(r.captured_ms))}</td><td>${fmtInt(r.item_count)}</td><td>${r.network?.available?`${fmtInt(r.network.ping_avg_ms)} ms / ${fmtInt(r.network.bandwidth_avg_kbps)} kbps`:'Unavailable'}</td><td>${r.position?.available?`${esc(r.position.map_name||'')} ${fmtNum(r.position.world_position_x)}, ${fmtNum(r.position.world_position_y)}, ${fmtNum(r.position.world_position_z)}`:'Unavailable'}</td><td><code>${esc(JSON.stringify(r.equipment||{}))}</code></td><td><button class="mini-button view-player-snapshot" data-snapshot="${esc(r.snapshot_id)}">View</button>${i+1<(detail.snapshots||[]).length?` <button class="mini-button compare-player-snapshots" data-new="${esc(r.snapshot_id)}" data-old="${esc(detail.snapshots[i+1].snapshot_id)}">Compare previous</button>`:''}</td></tr>`).join('')}</tbody></table></div>${(detail.snapshots||[]).length?'':'<div class="empty">No inventory snapshot has been received yet.</div>'}</section>
      <section class="panel" id="playerSnapshotPanel" hidden><div class="panel-head"><h2 id="playerSnapshotTitle">Inventory snapshot</h2><button class="button" id="playerSnapshotExport">Export</button></div><div class="panel-body" id="playerSnapshotBody"></div></section>
      <section class="panel"><div class="panel-head"><h2>Recently accessed virtual cargo</h2></div><div class="table-wrap"><table><thead><tr><th>Container</th><th>Class</th><th>Map</th><th>Status</th><th>Updated</th></tr></thead><tbody>${(detail.recent_containers||[]).map(r=>`<tr><td><button class="link-button open-player-container" data-id="${esc(r.storage_id)}">${esc(r.display_name||r.storage_id)}</button></td><td>${esc(r.container_class||'')}</td><td>${esc(r.map_name||'')}</td><td>${esc(r.status)}</td><td>${esc(fmtTime(r.updated_ms))}</td></tr>`).join('')}</tbody></table></div>${(detail.recent_containers||[]).length?'':'<div class="empty">No recent Clippy cargo sessions for this player.</div>'}</section>
      <section class="panel"><div class="panel-head"><h2>Live command history</h2><span class="muted">Commands expire and execute inside DayZ</span></div>${playerCommandsTable(commands.rows||[])}</section>
      <section class="panel"><div class="panel-head"><h2>Live quarantine</h2></div>${playerQuarantineTable(quarantine.rows||[],live&&detail.online)}</section>
      <section class="panel"><div class="panel-head"><h2>Recent player events</h2></div><div class="table-wrap"><table><thead><tr><th>Time</th><th>Event</th><th>Detail</th></tr></thead><tbody>${(detail.recent_events||[]).map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td>${esc(r.event_type)}</td><td><code>${esc(JSON.stringify(r.detail||{}))}</code></td></tr>`).join('')}</tbody></table></div></section>
      <section class="panel"><div class="panel-head"><h2>Aliases</h2></div><div class="table-wrap"><table><thead><tr><th>Name</th><th>First seen</th><th>Last seen</th></tr></thead><tbody>${(detail.aliases||[]).map(r=>`<tr><td>${esc(r.display_name)}</td><td>${esc(fmtTime(r.first_seen_ms))}</td><td>${esc(fmtTime(r.last_seen_ms))}</td></tr>`).join('')}</tbody></table></div></section>`);
    document.getElementById('backPlayers').onclick=()=>navigate('players');
    if(document.getElementById('requestPlayerSnapshot'))document.getElementById('requestPlayerSnapshot').onclick=()=>enqueuePlayerCommand(detail,'REQUEST_SNAPSHOT',{});
    if(document.getElementById('givePlayerItem'))document.getElementById('givePlayerItem').onclick=()=>givePlayerItem(detail);
    document.querySelectorAll('.view-player-snapshot').forEach(b=>b.onclick=()=>showPlayerSnapshot(detail,b.dataset.snapshot,canCommand).catch(showError));
    document.querySelectorAll('.compare-player-snapshots').forEach(b=>b.onclick=()=>comparePlayerSnapshots(detail,b.dataset.new,b.dataset.old).catch(showError));
    document.querySelectorAll('.open-player-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
    document.querySelectorAll('.restore-player-quarantine').forEach(b=>b.onclick=()=>enqueuePlayerCommand(detail,'RESTORE_QUARANTINE',{quarantine_id:b.dataset.id},'Restore quarantined live item'));
  }

  function playerCommandsTable(rows) {
    if(!rows.length)return '<div class="empty">No live player commands recorded.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Time</th><th>Action</th><th>Status</th><th>Reason</th><th>Result</th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(fmtTime(r.created_ms))}</td><td>${esc(r.action)}</td><td>${chip(r.status,r.status==='SUCCEEDED'?'good':r.status==='FAILED'||r.status==='EXPIRED'?'warn':'edit')}</td><td>${esc(r.reason||'')}</td><td>${r.error?`<span class="error-text">${esc(r.error)}</span>`:`<code>${esc(JSON.stringify(r.result||{}))}</code>`}</td></tr>`).join('')}</tbody></table></div>`;
  }

  function playerQuarantineTable(rows,canRestore) {
    if(!rows.length)return '<div class="empty">No live player items have been quarantined.</div>';
    return `<div class="table-wrap"><table><thead><tr><th>Item</th><th>Item ID</th><th>Created</th><th>Status</th><th></th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(r.class_name||'')}</td><td class="mono">${esc(r.item_id)}</td><td>${esc(fmtTime(r.created_ms))}</td><td>${r.restored?chip('restored','good'):chip('quarantined','warn')}</td><td>${!r.restored?`<button class="mini-button restore-player-quarantine" data-id="${esc(r.quarantine_id)}" ${canRestore?'':'disabled'}>Restore</button>`:''}</td></tr>`).join('')}</tbody></table></div>`;
  }

  async function showPlayerSnapshot(detail,snapshotId,canCommand) {
    const data=await api(`/api/players/${encodeURIComponent(detail.player_id)}/snapshots/${encodeURIComponent(snapshotId)}`);
    const panel=document.getElementById('playerSnapshotPanel'),body=document.getElementById('playerSnapshotBody');
    panel.hidden=false;document.getElementById('playerSnapshotTitle').textContent=`Inventory snapshot ${fmtTime(data.captured_ms)}`;body.replaceChildren();
    const roots=Array.isArray(data.inventory)?data.inventory:[];
    if(!roots.length)body.innerHTML='<div class="empty">Snapshot contains no inventory items.</div>';else {const tree=document.createElement('div');tree.className='tree';for(const root of roots)tree.appendChild(playerTreeNodeElement(root,detail,canCommand,true));body.appendChild(tree);}
    document.getElementById('playerSnapshotExport').onclick=()=>downloadJson(`clippy-player-${safeFileName(detail.player_id)}-${safeFileName(snapshotId)}.json`,data);
    panel.scrollIntoView({block:'start'});
  }

  function playerTreeNodeElement(node,detail,canCommand,expanded=false) {
    const wrapper=document.createElement('div');wrapper.className='tree-node';const line=document.createElement('div');line.className='tree-line';const children=Array.isArray(node.children)?node.children:[];
    const toggle=document.createElement('button');toggle.type='button';toggle.className='tree-toggle';toggle.disabled=!children.length;line.appendChild(toggle);
    const name=document.createElement('span');name.className='tree-name';name.textContent=node.class_name||'(unknown class)';line.appendChild(name);
    const id=document.createElement('span');id.className='mono muted';id.textContent=node.item_id||'';line.appendChild(id);
    for(const text of [`qty ${fmtNum(node.quantity)}`,`health ${fmtNum(node.health)}`]){const tag=document.createElement('span');tag.className='chip';tag.textContent=text;line.appendChild(tag);}
    if(canCommand&&node.item_id){const actions=document.createElement('span');actions.className='tree-actions';const add=(label,cls,fn)=>{const b=document.createElement('button');b.type='button';b.className=cls;b.textContent=label;b.onclick=e=>{e.stopPropagation();fn();};actions.appendChild(b);};add('Repair','mini-button',()=>repairPlayerItem(detail,node));add('Move','mini-button',()=>movePlayerItem(detail,node));add('Quarantine','mini-button warn',()=>enqueuePlayerCommand(detail,'QUARANTINE_ITEM',{item_id:node.item_id},`Quarantine ${node.class_name||'live item'}`));add('Remove','mini-button danger',()=>removePlayerItem(detail,node));line.appendChild(actions);}
    wrapper.appendChild(line);const host=document.createElement('div');host.className='tree-children';wrapper.appendChild(host);let made=false;const set=value=>{expanded=value;toggle.textContent=children.length?(value?'▾':'▸'):'·';toggle.setAttribute('aria-expanded',String(value));host.hidden=!value;if(value&&!made){for(const child of children)host.appendChild(playerTreeNodeElement(child,detail,canCommand,false));made=true;}};toggle.onclick=()=>set(!expanded);set(expanded);return wrapper;
  }

  function flattenPlayerInventory(inventory) {const rows=[];const walk=(n)=>{if(!n||typeof n!=='object')return;rows.push(n);for(const c of Array.isArray(n.children)?n.children:[])walk(c);};for(const r of Array.isArray(inventory)?inventory:[])walk(r);return rows;}

  async function comparePlayerSnapshots(detail,newId,oldId) {
    const [newer,older]=await Promise.all([api(`/api/players/${encodeURIComponent(detail.player_id)}/snapshots/${encodeURIComponent(newId)}`),api(`/api/players/${encodeURIComponent(detail.player_id)}/snapshots/${encodeURIComponent(oldId)}`)]);
    const counts=inv=>{const m=new Map();for(const n of flattenPlayerInventory(inv)){const k=n.class_name||'(unknown)';m.set(k,(m.get(k)||0)+1);}return m;};const n=counts(newer.inventory),o=counts(older.inventory),keys=[...new Set([...n.keys(),...o.keys()])].sort();const changed=keys.map(k=>({class_name:k,before:o.get(k)||0,after:n.get(k)||0})).filter(x=>x.before!==x.after);
    openDialog({title:'Compare player snapshots',confirm:'Close',body:`<div class="detail-grid">${pair('Older',fmtTime(older.captured_ms))}${pair('Newer',fmtTime(newer.captured_ms))}${pair('Items before',fmtInt(older.item_count))}${pair('Items after',fmtInt(newer.item_count))}</div><div class="table-wrap"><table><thead><tr><th>Class</th><th>Before</th><th>After</th><th>Change</th></tr></thead><tbody>${changed.map(r=>`<tr><td>${esc(r.class_name)}</td><td>${fmtInt(r.before)}</td><td>${fmtInt(r.after)}</td><td>${fmtInt(r.after-r.before)}</td></tr>`).join('')}</tbody></table></div>${changed.length?'':'<div class="empty">No class-count changes.</div>'}`,onSubmit:async d=>d.close()});
  }

  function enqueuePlayerCommand(detail,action,payload,defaultReason='') {
    openDialog({title:`${action.replaceAll('_',' ')}: ${detail.display_name||detail.player_id}`,confirm:'Queue command',danger:action==='REMOVE_ITEM',body:`<div class="notice warn">This command executes server-side against the live DayZ player. It expires quickly and will fail if the player or item is no longer available.</div><label class="field"><span>Reason</span><textarea name="reason" maxlength="512" placeholder="Why is this command needed?">${esc(defaultReason)}</textarea></label>`,
      onSubmit:async dialog=>{const result=await writeApi(`/api/players/${encodeURIComponent(detail.player_id)}/commands`,{action,payload,reason:field(dialog,'reason').trim()});toast(`Queued ${action}. Command ${result.command_id}.`);await renderPlayerDetail(detail.player_id);}});
  }

  function givePlayerItem(detail) {openDialog({title:`Give item to ${detail.display_name||detail.player_id}`,confirm:'Queue give',body:`<div class="notice warn">The item is created inside DayZ. Class names are validated server-side.</div><label class="field"><span>Class name</span><input name="class_name" maxlength="128" required></label><label class="field"><span>Quantity</span><input name="quantity" type="number" min="0" max="1000000000" step="any" value="1"></label><label class="field"><span>Health 0 to 1</span><input name="health" type="number" min="0" max="1" step="0.01" value="1"></label>${reasonField(false)}`,onSubmit:async d=>{const payload={class_name:field(d,'class_name').trim(),quantity:Number(field(d,'quantity')),health:Number(field(d,'health'))};await writeApi(`/api/players/${encodeURIComponent(detail.player_id)}/commands`,{action:'GIVE_ITEM',payload,reason:field(d,'reason').trim()});toast('Give-item command queued.');await renderPlayerDetail(detail.player_id);}});}
  function repairPlayerItem(detail,node) {openDialog({title:`Repair ${node.class_name||'item'}`,confirm:'Queue repair',body:`<label class="field"><span>Health 0 to 1</span><input name="health" type="number" min="0" max="1" step="0.01" value="1"></label>${reasonField(false)}`,onSubmit:async d=>{await writeApi(`/api/players/${encodeURIComponent(detail.player_id)}/commands`,{action:'REPAIR_ITEM',payload:{item_id:node.item_id,health:Number(field(d,'health'))},reason:field(d,'reason').trim()});toast('Repair command queued.');await renderPlayerDetail(detail.player_id);}});}
  function movePlayerItem(detail,node) {openDialog({title:`Move ${node.class_name||'item'} to another player`,confirm:'Queue move',body:`<label class="field"><span>Target player ID</span><input name="target" maxlength="128" required></label>${reasonField(true)}`,onSubmit:async d=>{await writeApi(`/api/players/${encodeURIComponent(detail.player_id)}/commands`,{action:'MOVE_ITEM',payload:{item_id:node.item_id,target_player_id:field(d,'target').trim()},reason:field(d,'reason').trim()});toast('Move command queued.');await renderPlayerDetail(detail.player_id);}});}
  function removePlayerItem(detail,node) {openDialog({title:`Remove ${node.class_name||'item'} from live player`,confirm:'Queue remove',danger:true,body:`<div class="notice danger">This live removal is not quarantined. Use Quarantine when recovery may be needed.</div><label class="field"><span>Type REMOVE to confirm</span><input name="confirm" required></label>${reasonField(true)}`,onSubmit:async d=>{if(field(d,'confirm')!=='REMOVE')throw new Error('Type REMOVE exactly to confirm.');await writeApi(`/api/players/${encodeURIComponent(detail.player_id)}/commands`,{action:'REMOVE_ITEM',payload:{item_id:node.item_id},reason:field(d,'reason').trim()});toast('Remove command queued.','warn');await renderPlayerDetail(detail.player_id);}});}

  async function renderSettings() {
    const data=await api('/api/settings');
    setContent(`${pageHead('Settings','Effective safe AdminHost settings. Secrets are never returned by this API.')}
      <section class="panel"><div class="panel-head"><h2>Local services</h2></div><div class="panel-body detail-grid">${pair('Admin listener',`${data.listen_address}:${data.port}`)}${pair('Storage Host',`${data.storage_host_address}:${data.storage_host_port}`)}${pair('PostgreSQL',`${data.postgres_host}:${data.postgres_port}`)}${pair('Database',data.postgres_database)}${pair('Read role',data.postgres_read_role)}${pair('Read pool',fmtInt(data.postgres_read_pool_size))}${pair('Idle shutdown',`${fmtInt(data.idle_shutdown_minutes)} minutes`)}${pair('Maintenance lock',`${fmtInt(data.maintenance_lock_seconds)} seconds`)}${pair('Export folder',data.export_directory||'exports')}${pair('Editing',data.editing_enabled?'Enabled':'Disabled')}${pair('Player telemetry',data.player_telemetry_enabled?'Enabled':'Disabled')}${pair('Network telemetry',data.player_network_telemetry_enabled?'Enabled':'Disabled')}${pair('Position telemetry',data.player_position_telemetry_enabled?'Enabled':'Disabled')}${pair('Snapshot interval',`${fmtInt(data.player_snapshot_interval_seconds)} seconds`)}${pair('Telemetry retention',`${fmtInt(data.player_telemetry_retention_days)} days`)}${pair('Snapshot history cap',fmtInt(data.player_snapshot_history_limit))}${pair('Admin audit retention',`${fmtInt(data.admin_audit_retention_days)} days`)}${pair('Player IP collection',data.player_ip_collection_supported?'Supported':'Not exposed by DayZ API')}${pair('Live player control',data.live_player_control_enabled?'Enabled':'Disabled')}${pair('Command expiry',`${fmtInt(data.player_command_expiry_seconds)} seconds`)}</div></section>
      <div class="notice">Server-specific ClippyServerManager.json remains the source for these settings. The panel does not expose database passwords, StorageHost tokens, or signing material.</div>`);
  }

  function openPalette() {
    if(state.paletteOpen)return;
    state.paletteOpen=true;
    const overlay=document.createElement('div');overlay.className='palette-overlay';overlay.id='palette';overlay.innerHTML=`<div class="palette"><div class="palette-title">Go to</div>${[['overview','Overview'],['containers','Containers'],['items','Items'],['activity','Activity'],['sessions','Sessions'],['recovery','Recovery'],['maintenance','Maintenance'],['backups','Backups'],['quarantine','Quarantine'],['audit','Audit Log'],['database','Database'],['reports','Reports'],['players','Players'],['settings','Settings']].map(([r,l])=>`<button data-go="${r}">${l}</button>`).join('')}<div class="palette-help">Type item:AKM, container:barrel, storage:&lt;id&gt;, or id:&lt;item-id&gt; in the top search.</div></div>`;document.body.appendChild(overlay);overlay.querySelectorAll('[data-go]').forEach(b=>b.onclick=()=>{closePalette();navigate(b.dataset.go);});overlay.onclick=e=>{if(e.target===overlay)closePalette();};
  }
  function closePalette(){document.getElementById('palette')?.remove();state.paletteOpen=false;}

  function command(text) {
    closePalette();
    const lower=text.toLowerCase();
    if(lower.startsWith('storage:')){const id=text.slice(text.indexOf(':')+1).trim();if(id)navigate('container',{containerId:id});return;}
    if(lower.startsWith('container:')){const query=text.slice(text.indexOf(':')+1).trim();navigate('containers',{query});return;}
    if(lower.startsWith('item:')){navigate('items',{query:text.slice(5).trim()});return;}
    if(lower.startsWith('id:')){navigate('items',{query:text.slice(3).trim()});return;}
    if(lower.startsWith('player:')){navigate('players',{query:text.slice(7).trim()});return;}
    navigate('items',{query:text});
  }

  document.addEventListener('keydown',event=>{
    if((event.ctrlKey||event.metaKey)&&event.key.toLowerCase()==='k'){event.preventDefault();const input=document.getElementById('globalSearch');if(input){input.focus();openPalette();}}
    if(event.key==='Escape'&&state.paletteOpen)closePalette();
  });

  bootstrap().then(ok=>{if(ok)navigate('overview');});
})();
