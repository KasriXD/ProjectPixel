// The platform's browser page (docs/platform.md#browsers): log in, pick a
// game, play it with the WebAssembly player against the same game servers
// as the desktop app. All text from the platform goes in with textContent.
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const sections = ['login', 'catalog', 'game', 'unavailable'];
  const store = {
    get: (k) => { try { return sessionStorage.getItem(k); } catch { return null; } },
    set: (k, v) => { try { sessionStorage.setItem(k, v); } catch { /* private mode */ } },
    del: (k) => { try { sessionStorage.removeItem(k); } catch { /* private mode */ } },
  };
  let token = store.get('px.token');
  let user = null;
  let searchTimer = 0;

  function show(name) {
    for (const s of sections) $(s).hidden = s !== name;
  }

  function message(text) {
    $('message').textContent = text || '';
    $('message').hidden = !text;
  }

  async function api(method, path, body) {
    const headers = { Accept: 'application/json' };
    if (token) headers.Authorization = 'Bearer ' + token;
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    const res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
    let data = {};
    try { data = await res.json(); } catch { /* not JSON */ }
    if (!res.ok) {
      const err = new Error((data.error && data.error.message) || `The platform answered ${res.status}`);
      err.status = res.status;
      throw err;
    }
    return data;
  }

  function signedIn(u, t) {
    user = u;
    if (t) {
      token = t;
      store.set('px.token', t);
    }
    $('who-name').textContent = u.username;
    $('who').hidden = false;
  }

  async function logIn(register) {
    message('');
    const username = $('username').value.trim();
    const password = $('password').value;
    if (!username || !password) {
      message('Enter a name and a password.');
      return;
    }
    try {
      const data = await api('POST', register ? '/api/v1/accounts' : '/api/v1/sessions', { username, password });
      $('password').value = '';
      signedIn(data.user, data.token);
      route();
    } catch (e) {
      message(e.message);
    }
  }

  async function logOut() {
    try { await api('DELETE', '/api/v1/sessions/current'); } catch { /* gone already */ }
    token = null;
    user = null;
    store.del('px.token');
    location.href = '/play';
  }

  function gameIdFromHash() {
    const m = /^#game=(\d{1,18})$/.exec(location.hash);
    return m ? m[1] : null;
  }

  function route() {
    if (!user) {
      show('login');
      $('username').focus();
      return;
    }
    const id = gameIdFromHash();
    if (id) play(id);
    else catalog();
  }

  async function catalog() {
    show('catalog');
    const search = $('search').value.trim();
    try {
      const q = new URLSearchParams({ sort: 'popular', limit: '100' });
      if (search) q.set('search', search);
      const { games } = await api('GET', '/api/v1/games?' + q.toString());
      const list = $('games');
      list.replaceChildren();
      for (const g of games) {
        if (!g.latestVersion) continue;
        const li = document.createElement('li');
        const title = document.createElement('span');
        title.className = 'title';
        title.textContent = g.title;
        const meta = document.createElement('span');
        meta.className = 'meta';
        const playing = g.playing ? `, ${g.playing} playing now` : '';
        meta.textContent = `by ${g.owner.username} · ${g.plays} plays${playing} · up to ${g.maxPlayers} players`;
        li.append(title, meta);
        if (g.description) {
          const d = document.createElement('span');
          d.className = 'description';
          d.textContent = g.description;
          li.append(d);
        }
        const button = document.createElement('button');
        button.type = 'button';
        button.textContent = 'Play';
        button.dataset.game = String(g.id);
        button.addEventListener('click', () => {
          history.replaceState(null, '', location.pathname + location.search + '#game=' + g.id);
          play(String(g.id));
        });
        li.append(button);
        list.append(li);
      }
      $('no-games').hidden = list.children.length > 0;
    } catch (e) {
      message(e.message);
    }
  }

  // Extra player arguments for automated tests, from the page's address.
  // Only these: the platform decides what to connect to.
  function testArguments() {
    const out = [];
    const q = new URLSearchParams(location.search);
    const frames = q.get('frames');
    if (frames && /^\d{1,7}$/.test(frames)) out.push('--frames', frames);
    const script = q.get('inputScript');
    if (script && /^\/examples\/[A-Za-z0-9_\-/]{1,100}\.input$/.test(script) && !script.includes('..')) {
      out.push('--input-script', script);
    }
    if (q.get('debug') === '1') out.push('--debug');
    return out;
  }

  let started = false;

  function gameStatus(text) {
    $('game-status').textContent = text;
  }

  function playerLine(text) {
    console.log(text); // tests read the console
    const line = String(text);
    if (/Joined/.test(line)) gameStatus('Playing. Have fun!');
    else if (/WARN|ERROR/.test(line)) gameStatus(line.replace(/^\[\s*[\d.]+\]\s*/, ''));
  }

  async function play(id) {
    if (started) return;
    started = true;
    message('');
    show('game');
    $('game-title').textContent = 'Game ' + id;
    gameStatus('Finding a game server…');
    try {
      const { game } = await api('GET', `/api/v1/games/${id}`);
      $('game-title').textContent = game.title;
      document.title = game.title;
      const { ticket } = await api('POST', `/api/v1/games/${id}/join`);
      if (!ticket.webSocket) throw new Error('The servers of this platform do not accept browsers yet.');
      if (location.protocol === 'https:' && ticket.webSocket.startsWith('ws:')) {
        throw new Error('This page is secure but the game server is not (ws://); ask the platform to turn on TLS.');
      }
      // The package downloads while the player loads; the player waits for it.
      const pkg = fetch('/api/v1/packages/' + encodeURIComponent(ticket.packageHash), {
        headers: { Authorization: 'Bearer ' + token },
      }).then((r) => {
        if (!r.ok) throw new Error(`Could not download the game (${r.status})`);
        return r.arrayBuffer();
      });
      gameStatus('Loading the player…');
      const canvas = $('canvas');
      canvas.addEventListener('contextmenu', (e) => e.preventDefault());
      window.Module = {
        canvas,
        // px-player.wasm and .data next to px-player.js, not next to this page.
        locateFile: (path) => '/play/player/' + path,
        noUrlArguments: true,
        pxPackage: pkg,
        arguments: ['--package', '/game.pxpkg', '--connect', ticket.webSocket, '--token', ticket.token,
          '--name', user.username, ...testArguments()],
        print: playerLine,
        printErr: playerLine,
        setStatus: (s) => { if (s) gameStatus(s); },
        onAbort: (what) => gameStatus('The player stopped: ' + what),
        onExit: () => gameStatus('The game has ended.'),
      };
      const script = document.createElement('script');
      script.src = '/play/player/px-player.js';
      script.onerror = () => gameStatus('Could not load the player.');
      document.body.append(script);
      canvas.focus();
    } catch (e) {
      gameStatus('');
      message(e.message);
      if (e.status === 401) {
        store.del('px.token');
        token = null;
      }
    }
  }

  function fullscreen() {
    const stage = $('stage');
    const req = stage.requestFullscreen || stage.webkitRequestFullscreen;
    if (!req) return;
    Promise.resolve(req.call(stage)).then(() => {
      if (screen.orientation && screen.orientation.lock) screen.orientation.lock('landscape').catch(() => {});
      $('canvas').focus();
    }).catch(() => {});
  }

  async function start() {
    $('login-form').addEventListener('submit', (e) => { e.preventDefault(); logIn(false); });
    $('register').addEventListener('click', () => logIn(true));
    $('logout').addEventListener('click', logOut);
    $('back').addEventListener('click', () => { location.href = '/play' + location.search; });
    $('fullscreen').addEventListener('click', fullscreen);
    $('search').addEventListener('input', () => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(catalog, 250);
    });
    document.addEventListener('securitypolicyviolation', (e) => {
      console.log('CSP blocked ' + e.blockedURI + ' (' + e.violatedDirective + ')');
    });
    window.addEventListener('hashchange', () => { if (!started) route(); });
    try {
      const info = await api('GET', '/api/v1/platform');
      $('platform-name').textContent = info.name;
      document.title = 'Play · ' + info.name;
      if (!info.webPlayer) {
        show('unavailable');
        return;
      }
    } catch (e) {
      message('Could not reach the platform: ' + e.message);
      return;
    }
    if (token) {
      try {
        signedIn((await api('GET', '/api/v1/me')).user);
      } catch {
        token = null;
        store.del('px.token');
      }
    }
    route();
  }

  start();
})();
