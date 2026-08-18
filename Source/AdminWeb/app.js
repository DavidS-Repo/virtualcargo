(() => {
  'use strict';

  const state = {
    csrf: '',
    route: 'overview',
    containerId: '',
    health: null,
    abort: null,
    searchTimer: null,
    paletteOpen: false
  };
  const app = document.getElementById('app');

  const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const fmtInt = value => new Intl.NumberFormat().format(Number(value || 0));
  const fmtBytes = value => {
    let n = Number(value || 0);
    const units = ['B','KB','MB','GB','TB'];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
    return `${n.toFixed(i ? 1 : 0)} ${units[i]}`;
  };
  const fmtTime = value => value ? new Date(Number(value)).toLocaleString() : 'Never';
  const chip = (text, kind='') => `<span class="chip ${kind}">${esc(text)}</span>`;

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
      throw error;
    }
    return body.data ?? body;
  }

  async function bootstrap() {
    const match = location.hash.match(/^#bootstrap=([0-9a-fA-F]{64,128})$/);
    if (match) {
      try {
        const result = await api('/api/session/bootstrap', {method:'POST', body:JSON.stringify({token:match[1]})});
        state.csrf = result.csrf;
        history.replaceState(null, '', location.pathname + location.search);
        return true;
      } catch (error) {
        renderLogin(`Bootstrap failed: ${error.message}`);
        return false;
      }
    }
    try {
      const session = await api('/api/session');
      state.csrf = session.csrf || '';
      return true;
    } catch (_) {
      renderLogin('Open this panel with OPEN-CLIPPY-ADMIN.bat or the server manager admin command.');
      return false;
    }
  }

  function renderLogin(message) {
    app.innerHTML = `<main class="login"><section class="login-card"><div class="brand"><span class="brand-mark">C</span><span>Clippy Admin Panel</span></div><h1>Local session required</h1><p class="subtitle">${esc(message)}</p><p class="muted">The panel only accepts a short-lived local bootstrap token. PostgreSQL credentials and Clippy service secrets are never sent to this page.</p></section></main>`;
  }

  function shell() {
    app.innerHTML = `<div class="shell">
      <aside class="sidebar">
        <div class="brand"><span class="brand-mark">C</span><span>Clippy Admin</span></div>
        <nav class="nav" aria-label="Admin pages">
          ${navButton('overview','Overview')}${navButton('containers','Containers')}${navButton('items','Items')}${navButton('sessions','Sessions')}${navButton('recovery','Recovery')}${navButton('database','Database')}
        </nav>
        <div class="side-note">Read-only Alpha<br>Localhost only<br>Editing disabled</div>
      </aside>
      <section class="workspace">
        <header class="topbar">
          <div class="global-search"><input id="globalSearch" aria-label="Search or open command palette" placeholder="Search anything..." autocomplete="off"><span class="keyhint">Ctrl K</span></div>
          <div class="statuses" id="statuses"></div>
        </header>
        <main class="content" id="content"></main>
      </section>
    </div>`;
    document.querySelectorAll('[data-route]').forEach(button => button.addEventListener('click', () => navigate(button.dataset.route)));
    const gs = document.getElementById('globalSearch');
    gs.addEventListener('focus', openPalette);
    gs.addEventListener('keydown', event => {
      if (event.key === 'Enter' && gs.value.trim()) command(gs.value.trim());
    });
  }

  function navButton(route, label) {
    return `<button type="button" data-route="${route}" ${state.route===route?'aria-current="page"':''}>${label}</button>`;
  }

  async function refreshHealth() {
    try {
      state.health = await api('/api/health');
      const pg = state.health.postgres?.ok ? '<span><i class="status-dot good"></i>PostgreSQL</span>' : '<span><i class="status-dot warn"></i>PostgreSQL</span>';
      const host = state.health.storage_host_reachable ? '<span><i class="status-dot good"></i>Storage Host</span>' : '<span><i class="status-dot warn"></i>Storage Host</span>';
      const el = document.getElementById('statuses');
      if (el) el.innerHTML = pg + host;
    } catch (_) {}
  }

  function pageHead(title, subtitle) {
    return `<div class="page-head"><div><h1>${esc(title)}</h1><div class="subtitle">${esc(subtitle)}</div></div><span class="readonly">READ ONLY</span></div>`;
  }

  function setContent(html) { document.getElementById('content').innerHTML = html; }
  function showError(error) { setContent(`<div class="error">${esc(error.message || error)}</div>`); }

  async function navigate(route, data={}) {
    state.route = route;
    if (data.containerId) state.containerId = data.containerId;
    shell();
    refreshHealth();
    try {
      if (route === 'overview') await renderOverview();
      else if (route === 'containers') await renderContainers();
      else if (route === 'container') await renderContainer(state.containerId);
      else if (route === 'items') await renderItems(data.query || '');
      else if (route === 'sessions') await renderSessions();
      else if (route === 'recovery') await renderRecovery();
      else if (route === 'database') await renderDatabase();
    } catch (error) { showError(error); }
  }

  async function renderOverview() {
    const data = await api('/api/overview');
    setContent(`${pageHead('Overview','Current Clippy storage health and recovery state.')}
      <div class="cards">
        ${card('PostgreSQL', data.postgres_ok ? 'Healthy' : 'Unavailable', data.postgres_version || '')}
        ${card('Database size', fmtBytes(data.database_size_bytes), `Schema v${data.schema_version}`)}
        ${card('Containers', fmtInt(data.containers_estimated), 'Estimated from PostgreSQL statistics')}
        ${card('Virtual roots', fmtInt(data.roots_estimated), 'Estimated from PostgreSQL statistics')}
        ${card('Open sessions', fmtInt(data.active_sessions), 'OPEN, MATERIALIZED, or COMMITTED')}
        ${card('Incomplete operations', fmtInt(data.incomplete_operations), 'Needs normal recovery flow')}
        ${card('Incomplete migrations', fmtInt(data.incomplete_migrations), 'Needs normal migration recovery')}
        ${card('Pending cleanup', fmtInt(data.pending_cleanup), 'Cleanup records across workflows')}
        ${card('Item index', data.item_index_complete ? 'Ready' : 'Backfill pending', data.item_index_complete ? 'Nested item search enabled' : 'Safe fallback search active')}
      </div>
      <section class="panel"><div class="panel-head"><h2>Safety state</h2></div><div class="panel-body">
        ${chip('Localhost only','good')} ${chip('Read-only PostgreSQL role','good')} ${chip('Editing disabled','good')}
        <p class="muted">cargo_item_index is derived from cargo_roots.tree_json. When backfill is complete, nested class search uses the index. If it is incomplete, the panel falls back to root class search and exact item-ID lookup without claiming missing nested results.</p>
      </div></section>`);
  }

  function card(label, value, detail='') {
    return `<div class="card"><div class="card-label">${esc(label)}</div><div class="card-value">${esc(value)}</div><div class="card-detail">${esc(detail)}</div></div>`;
  }

  async function renderContainers(search='', after='') {
    const data = await api(`/api/containers?q=${encodeURIComponent(search)}&after=${encodeURIComponent(after)}&limit=50`);
    setContent(`${pageHead('Containers','Browse virtual cargo containers without materializing them into DayZ.')}
      <div class="search-row"><input id="containerSearch" aria-label="Filter containers" placeholder="Filter by storage ID, provider key, or display name" value="${esc(search)}"><button class="button" id="containerSearchButton">Search</button></div>
      <section class="panel"><div class="table-wrap"><table><thead><tr><th>Container</th><th>Storage ID</th><th>Roots</th><th>Nodes</th><th>Revision</th><th>Updated</th><th>Status</th></tr></thead><tbody>
      ${data.rows.map(r => `<tr><td><button class="link-button open-container" data-id="${esc(r.storage_id)}">${esc(r.display_name)}</button><div class="muted">${esc(r.provider_key)}</div></td><td class="mono">${esc(r.storage_id)}</td><td>${fmtInt(r.root_count)}</td><td>${fmtInt(r.node_count)}</td><td>${fmtInt(r.revision)}</td><td>${esc(fmtTime(r.updated_ms))}</td><td>${r.active_session?chip('session','warn'):''}${r.active_operation?chip('operation','warn'):''}${r.active_migration?chip('migration','warn'):''}${(!r.active_session&&!r.active_operation&&!r.active_migration)?chip('idle','good'):''}</td></tr>`).join('')}
      </tbody></table></div>${data.rows.length?'':'<div class="empty">No containers matched.</div>'}</section>
      <div><button class="button" id="containerNext" ${data.next_after?'':'disabled'}>Next page</button></div>`);
    document.getElementById('containerSearchButton').onclick = () => renderContainers(document.getElementById('containerSearch').value.trim(), '');
    const input = document.getElementById('containerSearch');
    input.addEventListener('keydown', e => { if (e.key === 'Enter') renderContainers(input.value.trim(), ''); });
    document.querySelectorAll('.open-container').forEach(b => b.onclick = () => navigate('container',{containerId:b.dataset.id}));
    document.getElementById('containerNext').onclick = () => renderContainers(search, data.next_after || '');
  }

  async function renderContainer(id, after='') {
    const [detail, roots] = await Promise.all([
      api(`/api/containers/${encodeURIComponent(id)}`),
      api(`/api/containers/${encodeURIComponent(id)}/roots?limit=50&after=${encodeURIComponent(after)}`)
    ]);
    setContent(`${pageHead(detail.display_name || 'Container', detail.storage_id)}
      <section class="panel"><div class="panel-head"><h2>Container detail</h2><button class="button" id="backContainers">Back to containers</button></div><div class="panel-body"><div class="detail-grid">
        ${pair('Provider',detail.provider_id)}${pair('Provider key',detail.provider_key)}${pair('Capacity',fmtInt(detail.capacity_slots))}${pair('Revision',fmtInt(detail.revision))}
        ${pair('Root items',fmtInt(detail.root_count))}${pair('Total nodes',fmtInt(detail.node_count))}${pair('Created',fmtTime(detail.created_ms))}${pair('Updated',fmtTime(detail.updated_ms))}
      </div></div></section>
      <section class="panel"><div class="panel-head"><h2>Inventory roots</h2><span class="muted">Trees load only when opened</span></div><div class="table-wrap"><table><thead><tr><th>Class</th><th>Root ID</th><th>Qty</th><th>Health</th><th>Nodes</th><th></th></tr></thead><tbody>
        ${roots.rows.map(r => `<tr><td>${esc(r.class_name)}</td><td class="mono">${esc(r.root_item_id)}</td><td>${esc(r.quantity)}</td><td>${esc(r.health)}</td><td>${fmtInt(r.node_count)}</td><td><button class="button load-tree" data-root="${esc(r.root_item_id)}">Open tree</button></td></tr>`).join('')}
      </tbody></table></div>${roots.rows.length?'':'<div class="empty">This container has no virtual roots.</div>'}<div class="panel-body"><button class="button" id="rootsNext" ${roots.next_after?'':'disabled'}>Next roots</button></div></section>
      <section class="panel" id="treePanel" hidden><div class="panel-head"><h2>Inventory tree</h2><button class="button" id="rawToggle">Raw JSON</button></div><div class="panel-body" id="treeBody"></div></section>
      <section class="panel"><div class="panel-head"><h2>Active workflows</h2></div><div class="panel-body">${workflowSummary(detail)}</div></section>`);
    document.getElementById('backContainers').onclick = () => navigate('containers');
    document.querySelectorAll('.load-tree').forEach(b => b.onclick = () => loadTree(id,b.dataset.root));
    document.getElementById('rootsNext').onclick = () => renderContainer(id, roots.next_after || '');
  }

  function pair(k,v) { return `<div class="detail-pair"><div class="k">${esc(k)}</div><div class="v">${esc(v)}</div></div>`; }
  function workflowSummary(d) {
    const total = d.active_sessions.length + d.active_operations.length + d.active_migrations.length;
    if (!total) return '<span class="muted">No active operation, cargo session, or migration is blocking this container.</span>';
    return `${d.active_sessions.map(x=>chip(`session ${x.status}`,'warn')).join('')}${d.active_operations.map(x=>chip(`operation ${x.status}`,'warn')).join('')}${d.active_migrations.map(x=>chip(`migration ${x.status}`,'warn')).join('')}`;
  }

  async function loadTree(storageId, rootId) {
    const panel = document.getElementById('treePanel');
    const body = document.getElementById('treeBody');
    panel.hidden = false;
    body.innerHTML = '<div class="muted">Loading tree...</div>';
    try {
      const data = await api(`/api/containers/${encodeURIComponent(storageId)}/roots/${encodeURIComponent(rootId)}/tree`);
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
          tree.appendChild(treeNodeElement(data.tree, true));
          body.appendChild(tree);
        }
      };
      render();
      document.getElementById('rawToggle').onclick = () => {
        raw = !raw;
        document.getElementById('rawToggle').textContent = raw ? 'Tree view' : 'Raw JSON';
        render();
      };
    } catch (error) { body.innerHTML = `<div class="error">${esc(error.message)}</div>`; }
  }

  function treeNodeElement(node, expanded=false) {
    const wrapper = document.createElement('div');
    wrapper.className = 'tree-node';
    const line = document.createElement('div');
    line.className = 'tree-line';
    const children = Array.isArray(node.children) ? node.children : [];
    const toggle = document.createElement('button');
    toggle.type = 'button';
    toggle.className = 'tree-toggle';
    toggle.setAttribute('aria-label', children.length ? 'Expand nested items' : 'No nested items');
    toggle.disabled = children.length === 0;
    toggle.textContent = children.length ? (expanded ? '▾' : '▸') : '·';
    line.appendChild(toggle);
    const name = document.createElement('span');
    name.className = 'tree-name';
    name.textContent = node.class_name || '(unknown class)';
    line.appendChild(name);
    const item = document.createElement('span');
    item.className = 'mono muted';
    item.textContent = node.item_id || '';
    line.appendChild(item);
    for (const text of [`qty ${node.quantity ?? 0}`, `health ${node.health ?? 0}`]) {
      const tag = document.createElement('span');
      tag.className = 'chip';
      tag.textContent = text;
      line.appendChild(tag);
    }
    wrapper.appendChild(line);
    const childHost = document.createElement('div');
    wrapper.appendChild(childHost);
    let materialized = false;
    const setExpanded = value => {
      expanded = value;
      toggle.setAttribute('aria-expanded', String(value));
      toggle.textContent = value ? '▾' : '▸';
      if (value && !materialized) {
        const fragment = document.createDocumentFragment();
        children.forEach(child => fragment.appendChild(treeNodeElement(child, false)));
        childHost.appendChild(fragment);
        materialized = true;
      }
      childHost.hidden = !value;
    };
    if (children.length) {
      toggle.onclick = () => setExpanded(!expanded);
      setExpanded(expanded);
    }
    return wrapper;
  }

  async function renderItems(initial='') {
    setContent(`${pageHead('Items','Search virtual cargo. When the Phase 3 index is complete, class searches include nested attachments and cargo.')}
      <div class="search-row"><input id="itemSearch" aria-label="Search virtual items" placeholder="M4A1, item:AKM, or id:0123..." value="${esc(initial)}"><button class="button" id="itemSearchButton">Search</button></div>
      <div class="item-filters" aria-label="Item numeric filters">
        <label>Min quantity<input id="minQuantity" inputmode="decimal" type="number" min="0" step="any" placeholder="0"></label>
        <label>Max quantity<input id="maxQuantity" inputmode="decimal" type="number" min="0" step="any" placeholder="Any"></label>
        <label>Min health<input id="minHealth" inputmode="decimal" type="number" min="0" step="any" placeholder="0"></label>
        <label>Max health<input id="maxHealth" inputmode="decimal" type="number" min="0" step="any" placeholder="Any"></label>
      </div>
      <div id="itemResults" class="panel"><div class="empty">Enter a class prefix or exact item ID.</div></div>`);
    const input = document.getElementById('itemSearch');
    const filterValue = id => document.getElementById(id).value.trim();
    const currentFilters = () => ({
      minQuantity: filterValue('minQuantity'),
      maxQuantity: filterValue('maxQuantity'),
      minHealth: filterValue('minHealth'),
      maxHealth: filterValue('maxHealth')
    });
    const run = () => runItemSearch(input.value.trim(), {}, currentFilters());
    document.getElementById('itemSearchButton').onclick = run;
    input.addEventListener('keydown', e => { if (e.key === 'Enter') run(); });
    input.addEventListener('input', () => {
      clearTimeout(state.searchTimer);
      state.searchTimer = setTimeout(run, 200);
    });
    document.querySelectorAll('.item-filters input').forEach(el => el.addEventListener('change', run));
    if (initial) run();
  }

  async function runItemSearch(q, cursor={}, filters={}) {
    if (!q) return;
    if (state.abort) state.abort.abort();
    state.abort = new AbortController();
    const target = document.getElementById('itemResults');
    target.innerHTML = '<div class="empty">Searching...</div>';
    const params = new URLSearchParams({
      q,
      limit: '50',
      after_class: cursor.afterClass || '',
      after_storage: cursor.afterStorage || '',
      after_root: cursor.afterRoot || '',
      after_item: cursor.afterItem || ''
    });
    if (filters.minQuantity) params.set('min_quantity', filters.minQuantity);
    if (filters.maxQuantity) params.set('max_quantity', filters.maxQuantity);
    if (filters.minHealth) params.set('min_health', filters.minHealth);
    if (filters.maxHealth) params.set('max_health', filters.maxHealth);
    try {
      const data = await api(`/api/items/search?${params.toString()}`, {signal:state.abort.signal});
      const indexMessage = data.nested_class_search_available
        ? 'Nested class index active'
        : 'Index backfill incomplete. Search is limited to roots plus exact item IDs.';
      target.innerHTML = `<div class="panel-head"><h2>Results</h2><span class="muted">${esc(indexMessage)}</span></div><div class="table-wrap"><table><thead><tr><th>Class</th><th>Container</th><th>Item ID</th><th>Parent</th><th>Depth</th><th>Qty</th><th>Health</th><th>Location</th><th>Adapter</th></tr></thead><tbody>${data.rows.map(r=>`<tr><td>${esc(r.class_name)}</td><td><button class="link-button item-container" data-id="${esc(r.storage_id)}">${esc(r.storage_id)}</button></td><td class="mono">${esc(r.item_id)}</td><td class="mono">${esc(r.parent_item_id || '')}</td><td>${r.depth}</td><td>${esc(r.quantity)}</td><td>${esc(r.health)}</td><td>${esc(r.location_type || '')}</td><td>${esc(r.adapter_id || '')}</td></tr>`).join('')}</tbody></table></div>${data.rows.length?'':'<div class="empty">No matches.</div>'}<div class="panel-body"><button class="button" id="itemNext" ${data.next_after_storage?'':'disabled'}>Next results</button></div>`;
      document.querySelectorAll('.item-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
      document.getElementById('itemNext').onclick=()=>runItemSearch(q,{
        afterClass:data.next_after_class||'',
        afterStorage:data.next_after_storage||'',
        afterRoot:data.next_after_root||'',
        afterItem:data.next_after_item||''
      },filters);
    } catch (error) {
      if (error.name !== 'AbortError') target.innerHTML = `<div class="error">${esc(error.message)}</div>`;
    }
  }

  async function renderSessions(beforeMs='', beforeId='') {
    const data = await api(`/api/sessions?limit=75&before_ms=${encodeURIComponent(beforeMs)}&before_id=${encodeURIComponent(beforeId)}`);
    setContent(`${pageHead('Sessions','Recent cargo sessions. Active states are shown without changing recovery state.')}
      <section class="panel"><div class="table-wrap"><table><thead><tr><th>Status</th><th>Session</th><th>Container</th><th>Player ID</th><th>Expected rev</th><th>Updated</th><th>Cleanup</th></tr></thead><tbody>${data.rows.map(r=>`<tr><td>${chip(r.status,['OPEN','MATERIALIZED','COMMITTED'].includes(r.status)?'warn':'good')}</td><td class="mono">${esc(r.session_id)}</td><td><button class="link-button session-container" data-id="${esc(r.storage_id)}">${esc(r.container)}</button></td><td class="mono">${esc(r.player_id)}</td><td>${fmtInt(r.expected_revision)}</td><td>${esc(fmtTime(r.updated_ms))}</td><td>${fmtInt(r.pending_cleanup)}</td></tr>`).join('')}</tbody></table></div><div class="panel-body"><button class="button" id="sessionNext" ${data.next_before_id?'':'disabled'}>Next sessions</button></div></section>`);
    document.querySelectorAll('.session-container').forEach(b=>b.onclick=()=>navigate('container',{containerId:b.dataset.id}));
    document.getElementById('sessionNext').onclick=()=>renderSessions(data.next_before_ms||'',data.next_before_id||'');
  }

  async function renderRecovery() {
    const data = await api('/api/recovery');
    setContent(`${pageHead('Recovery','Read-only view of unfinished operations, sessions, migrations, and cleanup records.')}
      <div class="cards">${card('Operations',data.operations.length,'Incomplete')}${card('Sessions',data.sessions.length,'Active workflow states')}${card('Migrations',data.migrations.length,'Incomplete')}${card('Pending cleanup',fmtInt(data.pending_cleanup.operations+data.pending_cleanup.sessions+data.pending_cleanup.migrations),'Across cleanup tables')}</div>
      ${recoveryTable('Operations',data.operations,'operation_id')}${recoveryTable('Sessions',data.sessions,'session_id')}${recoveryTable('Migrations',data.migrations,'migration_id')}
      ${data.truncated?'<div class="error">At least one recovery list reached the 100-row safety cap. Use the existing host recovery flow or inspect the database page. No unbounded query was run.</div>':''}`);
  }

  function recoveryTable(title, rows, idKey) {
    return `<section class="panel"><div class="panel-head"><h2>${esc(title)}</h2></div>${rows.length?`<div class="table-wrap"><table><thead><tr><th>ID</th><th>Storage</th><th>Status</th><th>Updated</th><th>Error</th></tr></thead><tbody>${rows.map(r=>`<tr><td class="mono">${esc(r[idKey])}</td><td class="mono">${esc(r.storage_id)}</td><td>${chip(r.status||r.cleanup_state||'pending','warn')}</td><td>${esc(fmtTime(r.updated_ms))}</td><td>${esc(r.error||'')}</td></tr>`).join('')}</tbody></table></div>`:'<div class="empty">None.</div>'}</section>`;
  }

  async function renderDatabase() {
    const data = await api('/api/database/info');
    setContent(`${pageHead('Database','Safe PostgreSQL inspection using a dedicated read-only login.')}
      <div class="cards">${card('PostgreSQL',data.postgres_version,`Database ${data.database}`)}${card('Database size',fmtBytes(data.size_bytes),`Schema v${data.schema_version}`)}${card('Connections',fmtInt(data.connections),'Current database')}${card('Transaction mode',data.transaction_read_only?'Read only':'Unexpected','Admin connection')}${card('Item index',data.item_index_complete?'Ready':'Backfill pending','Derived from tree_json')}</div>
      <section class="panel"><div class="panel-head"><h2>Clippy tables</h2></div><div class="table-wrap"><table><thead><tr><th>Table</th><th>Estimated rows</th><th>Total size</th><th>Table</th><th>Indexes</th></tr></thead><tbody>${data.tables.map(t=>`<tr><td class="mono">${esc(t.name)}</td><td>${fmtInt(t.estimated_rows)}</td><td>${fmtBytes(t.total_bytes)}</td><td>${fmtBytes(t.table_bytes)}</td><td>${fmtBytes(t.index_bytes)}</td></tr>`).join('')}</tbody></table></div></section>`);
  }

  function openPalette() {
    if (state.paletteOpen) return;
    state.paletteOpen = true;
    const node = document.createElement('div');
    node.className = 'palette-backdrop';
    node.id = 'paletteBackdrop';
    node.innerHTML = `<div class="palette" role="dialog" aria-modal="true" aria-label="Command palette"><input id="paletteInput" placeholder="Search or type a command" autocomplete="off"><div class="palette-help">Commands: overview, containers, items, sessions, recovery, database<br>Search: item:AKM, id:&lt;item-id&gt;, container:&lt;text&gt;</div></div>`;
    document.body.appendChild(node);
    const input = document.getElementById('paletteInput');
    input.focus();
    input.onkeydown = event => {
      if (event.key === 'Escape') closePalette();
      if (event.key === 'Enter') { command(input.value.trim()); closePalette(); }
    };
    node.addEventListener('mousedown', event => { if (event.target === node) closePalette(); });
  }
  function closePalette() { document.getElementById('paletteBackdrop')?.remove(); state.paletteOpen=false; }

  function command(text) {
    if (!text) return;
    const lower = text.toLowerCase();
    if (['overview','containers','items','sessions','recovery','database'].includes(lower)) return navigate(lower);
    if (lower.startsWith('container:')) { navigate('containers'); setTimeout(()=>renderContainers(text.slice(10).trim()),0); return; }
    if (lower.startsWith('item:')) return navigate('items',{query:text.slice(5).trim()});
    if (lower.startsWith('id:')) return navigate('items',{query:text});
    return navigate('items',{query:text});
  }

  document.addEventListener('keydown', event => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') { event.preventDefault(); openPalette(); }
    if (event.key === 'Escape' && state.paletteOpen) closePalette();
  });

  bootstrap().then(ok => { if (ok) navigate('overview'); });
})();
