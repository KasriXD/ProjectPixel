// The platform's browser page (docs/platform.md#browsers, docs/launcher.md):
// log in, browse games and their leaderboards, see friends, play with the
// WebAssembly player against the same game servers as the apps, join
// friends and invite them. All text from the platform goes in with
// textContent.
//
// Pages (the address's #): games, game=ID (a game's page), play=ID,
// join=NAME (a friend's game), friends, you. play= and join= start the
// player only after a click on this page (see route()). Links in the
// platform's email: verify=TOKEN confirms an address, reset=TOKEN chooses a
// new password (docs/accounts.md); the token leaves the address at once.
// Without a browser player the page still has games, friends and accounts.
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const sections = ['login', 'forgot-page', 'reset-page', 'catalog', 'details', 'game', 'friends', 'you'];
  const store = {
    get: (k) => { try { return sessionStorage.getItem(k); } catch { return null; } },
    set: (k, v) => { try { sessionStorage.setItem(k, v); } catch { /* private mode */ } },
    del: (k) => { try { sessionStorage.removeItem(k); } catch { /* private mode */ } },
  };
  const SOCIAL_POLL_MS = 15000;
  let token = store.get('px.token');
  let user = null; // { id, username, ... }
  let profile = null; // your own profile
  let social = { friends: [], incoming: [], outgoing: [], invitations: [] };
  let started = false; // the player is running (it cannot be unloaded: leaving reloads the page)
  let joined = false;
  let searchTimer = 0;
  let suggestTimer = 0;
  let pollTimer = 0;
  let boards = [];
  let platform = { webPlayer: false, mail: false };
  let resetToken = null;
  const pictures = new Map(); // "id:hash" -> object URL
  const invited = new Set();

  // --- Helpers -------------------------------------------------------------------------------

  // el('li', { className: 'x' }, child, 'text', ...): elements without HTML parsing.
  function el(tag, props, ...children) {
    const e = document.createElement(tag);
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (k === 'dataset') Object.assign(e.dataset, v);
        else if (k === 'onclick') e.addEventListener('click', v);
        else e[k] = v;
      }
    }
    for (const c of children) {
      if (c === null || c === undefined || c === false) continue;
      e.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return e;
  }

  function button(text, onclick, className) {
    return el('button', { type: 'button', className: className || '', onclick }, text);
  }

  function avatar(index, big) {
    const i = Number.isInteger(index) && index >= 0 && index < 16 ? index : 0;
    return el('span', { className: `avatar av${i}${big ? ' big' : ''}`, ariaHidden: 'true' });
  }

  let shown = '';
  function show(name) {
    if (name !== shown) notice(''); // a notice is about the page it was shown on
    shown = name;
    for (const s of sections) $(s).hidden = s !== name;
    const page = name === 'details' || name === 'catalog' ? 'games' : name;
    for (const a of document.querySelectorAll('#nav a')) a.classList.toggle('current', a.dataset.page === page);
    $('invitations').hidden = !(name === 'catalog' || name === 'friends') || social.invitations.length === 0;
  }

  function message(text) {
    $('message').textContent = text || '';
    $('message').hidden = !text;
    if (text) $('notice').hidden = true;
  }

  function notice(text) {
    $('notice').textContent = text || '';
    $('notice').hidden = !text;
    if (text) $('message').hidden = true;
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

  // Runs an action; errors go to the message line (an earlier one goes away).
  async function act(fn) {
    message('');
    try {
      await fn();
    } catch (e) {
      message(e.message);
    }
  }

  const name = (n) => encodeURIComponent(n);
  const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;

  // As project::formatScore: whole numbers as they are, others with two
  // decimals; times as m:ss.cc.
  function scoreText(value, format) {
    if (format !== 'time') return Number.isInteger(value) ? String(value) : value.toFixed(2);
    const hundredths = Math.round(Math.max(0, Math.min(value, 1e12)) * 100);
    const m = Math.floor(hundredths / 6000);
    const s = Math.floor((hundredths % 6000) / 100);
    const h = hundredths % 100;
    return `${m}:${String(s).padStart(2, '0')}.${String(h).padStart(2, '0')}`;
  }

  function presenceText(p) {
    if (p.state === 'playing') return p.gameTitle ? 'Playing ' + p.gameTitle : 'Playing';
    return p.state === 'online' ? 'Online' : 'Offline';
  }

  // A game's picture: fetched with the login (private games need it), shown from a blob.
  async function picture(game) {
    if (!game.thumbnail) return null;
    const key = `${game.id}:${game.thumbnail}`;
    if (pictures.has(key)) return pictures.get(key);
    const res = await fetch(`/api/v1/games/${game.id}/thumbnail`, { headers: token ? { Authorization: 'Bearer ' + token } : {} });
    if (!res.ok || res.status === 204) return null;
    const url = URL.createObjectURL(await res.blob());
    pictures.set(key, url);
    return url;
  }

  function pictureBox(game, className) {
    const box = el('div', { className }, el('span', { className: 'type' }, game.gameType || 'game'));
    box.dataset.tone = String(game.id % 6);
    picture(game).then((url) => {
      if (!url) return;
      box.replaceChildren(el('img', { src: url, alt: '', loading: 'lazy' }));
    }).catch(() => {});
    return box;
  }

  // --- Logging in ------------------------------------------------------------------------------

  async function signedIn(u, t) {
    user = u;
    if (t) {
      token = t;
      store.set('px.token', t);
    }
    $('who-name').textContent = u.username;
    $('who').hidden = false;
    $('nav').hidden = false;
    await loadProfile().catch(() => {});
    await refreshSocial().catch(() => {});
    clearInterval(pollTimer);
    pollTimer = setInterval(() => refreshSocial().catch(() => {}), SOCIAL_POLL_MS);
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
      await signedIn(data.user, data.token);
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

  // Logged out by the platform (a new password, a deleted account): back to
  // logging in on this page, keeping what it says.
  function loggedOut() {
    token = null;
    user = null;
    profile = null;
    store.del('px.token');
    clearInterval(pollTimer);
    $('who').hidden = true;
    $('nav').hidden = true;
    $('invitations').hidden = true;
    history.replaceState(null, '', location.pathname + location.search);
    show('login');
  }

  // --- Pages ------------------------------------------------------------------------------------

  function route() {
    const hash = decodeURIComponent(location.hash.slice(1));
    let link;
    if ((link = /^verify=([A-Za-z0-9_-]{16,100})$/.exec(hash))) {
      confirmEmail(link[1]);
      return;
    }
    if ((link = /^reset=([A-Za-z0-9_-]{16,100})$/.exec(hash))) {
      resetPage(link[1]);
      return;
    }
    if (!user) {
      show('login');
      $('forgot-line').hidden = !platform.mail;
      $('username').focus();
      return;
    }
    // Playing and joining start only from a click on this page: a link from
    // elsewhere opens the game's page or the friends page instead.
    const asked = store.get('px.intent') === location.hash;
    store.del('px.intent');
    let m;
    if ((m = /^play=(\d{1,18})$/.exec(hash))) {
      if (asked && platform.webPlayer) play(m[1]);
      else details(m[1]);
    } else if ((m = /^join=([A-Za-z0-9_]{1,32})$/.exec(hash))) {
      if (asked && platform.webPlayer) joinFriend(m[1]);
      else friendsPage();
    } else if ((m = /^game=(\d{1,18})$/.exec(hash))) details(m[1]);
    else if (hash === 'friends') friendsPage();
    else if (hash === 'you') youPage();
    else catalog();
  }

  // Goes to a page from a click on this page. While the player runs, other
  // pages need a fresh page (the player cannot be unloaded).
  function go(hash) {
    store.set('px.intent', hash);
    if (started) {
      history.replaceState(null, '', location.pathname + location.search + hash);
      location.reload();
    } else if (location.hash === hash) {
      route();
    } else {
      location.hash = hash;
    }
  }

  async function catalog() {
    show('catalog');
    message('');
    const search = $('search').value.trim();
    try {
      const q = new URLSearchParams({ sort: $('sort').value, limit: '100' });
      if (search) q.set('search', search);
      const { games } = await api('GET', '/api/v1/games?' + q.toString());
      const list = $('games');
      list.replaceChildren();
      for (const g of games) {
        if (!g.latestVersion) continue;
        const open = el('a', { href: `#game=${g.id}`, className: 'open' },
          pictureBox(g, 'picture'),
          el('span', { className: 'title' }, g.title));
        const playing = g.playing ? `, ${g.playing} playing now` : '';
        const meta = el('span', { className: 'meta' }, `by ${g.owner.username} · ${plural(g.plays, 'play', 'plays')}${playing}`);
        const play = el('button', { type: 'button', className: 'needs-player', dataset: { game: String(g.id) } }, 'Play');
        play.addEventListener('click', () => go(`#play=${g.id}`));
        list.append(el('li', null, open, meta, play));
      }
      $('no-games').hidden = list.children.length > 0;
    } catch (e) {
      message(e.message);
    }
  }

  async function details(id) {
    show('details');
    message('');
    $('details-title').textContent = '';
    $('boards').replaceChildren();
    try {
      const [{ game }, { leaderboards }] = await Promise.all([
        api('GET', `/api/v1/games/${id}`),
        api('GET', `/api/v1/games/${id}/leaderboards?limit=10`),
      ]);
      document.title = game.title;
      $('details-picture').replaceWith(Object.assign(pictureBox(game, 'picture'), { id: 'details-picture' }));
      $('details-title').textContent = game.title;
      $('details-owner').textContent = 'by ' + game.owner.username;
      $('details-description').textContent = game.description || '';
      $('details-stats').textContent = `${plural(game.playing, 'player', 'players')} now · ` +
        `${plural(game.plays, 'play', 'plays')} · version ${game.latestVersion}`;
      $('details-play').hidden = !game.latestVersion;
      $('details-play').onclick = () => go(`#play=${game.id}`);
      const here = $('details-friends');
      here.replaceChildren();
      for (const f of social.friends) {
        if (f.presence.state !== 'playing' || f.presence.gameId !== game.id) continue;
        here.append(el('li', null, avatar(f.avatar), el('span', { className: 'playing-text' }, `${f.username} is playing`),
          f.presence.joinable ? button('Join', () => go(`#join=${name(f.username)}`), 'secondary needs-player') : null));
      }
      boards = leaderboards;
      drawBoards();
    } catch (e) {
      message(e.message);
    }
  }

  function drawBoards() {
    const scope = document.querySelector('input[name=board-scope]:checked').value;
    const box = $('boards');
    box.replaceChildren();
    $('no-boards').hidden = boards.length > 0;
    for (const b of boards) {
      const rows = scope === 'friends' ? b.friends : b.top;
      const table = el('table', null, el('caption', null, b.title + (b.order === 'lowest' ? ' (lowest wins)' : '')));
      const body = el('tbody');
      const row = (e) => {
        const tr = el('tr', { className: user && e.username === user.username ? 'mine' : '' },
          el('td', { className: 'rank' }, String(e.rank)),
          el('td', null, avatar(e.avatar), ' ', e.username),
          el('td', { className: 'score' }, scoreText(e.value, b.format)));
        body.append(tr);
      };
      for (const e of rows) row(e);
      if (scope === 'top' && b.you && !rows.some((e) => e.username === b.you.username)) row(b.you);
      if (!body.children.length) {
        body.append(el('tr', null, el('td', { colSpan: 3, className: 'meta' },
          scope === 'friends' ? 'None of your friends has a score yet.' : 'No scores yet: be the first!')));
      }
      table.append(body);
      box.append(table);
    }
  }

  // --- Friends -----------------------------------------------------------------------------------

  async function refreshSocial() {
    if (!user) return;
    ({ social } = await api('GET', '/api/v1/me/social'));
    const order = { playing: 0, online: 1, offline: 2 };
    social.friends.sort((a, b) => order[a.presence.state] - order[b.presence.state]);
    const alerts = social.incoming.length + social.invitations.length;
    $('friends-badge').textContent = String(alerts);
    $('friends-badge').hidden = alerts === 0;
    drawInvitations();
    if (!$('friends').hidden) drawFriends();
    if (joined) drawInvitePanel();
  }

  function drawInvitations() {
    const list = $('invitations');
    list.replaceChildren();
    for (const inv of social.invitations) {
      list.append(el('li', null, avatar(inv.fromAvatar),
        el('span', { className: 'grow' }, `${inv.from} invites you to play ${inv.gameTitle}`),
        button('Join', () => go(`#join=${name(inv.from)}`)),
        button('Not now', () => act(async () => {
          await api('DELETE', `/api/v1/invitations/${inv.id}`);
          await refreshSocial();
        }), 'secondary')));
    }
    const page = $('catalog').hidden && $('friends').hidden;
    list.hidden = page || social.invitations.length === 0;
  }

  function personMenu(username, isFriend) {
    const menu = el('details', { className: 'menu' }, el('summary', { ariaLabel: 'More' }, '…'));
    const items = el('div', { className: 'menu-items' });
    if (isFriend) {
      items.append(button('Remove from friends', () => act(async () => {
        await api('DELETE', `/api/v1/friends/${name(username)}`);
        notice(`${username} is no longer your friend.`);
        await refreshSocial();
      }), 'quiet'));
    }
    items.append(button('Block', () => act(async () => {
      await api('POST', '/api/v1/blocks', { username });
      notice(`You blocked ${username}: you will not meet in games. Unblock them on your page.`);
      await refreshSocial();
    }), 'quiet'));
    items.append(button('Report…', () => openReport(username), 'quiet'));
    menu.append(items);
    return menu;
  }

  function drawFriends() {
    const incoming = $('incoming');
    incoming.replaceChildren();
    for (const r of social.incoming) {
      incoming.append(el('li', null, avatar(r.avatar), el('span', { className: 'grow' }, r.username),
        button('Accept', () => act(async () => {
          await api('POST', `/api/v1/friends/requests/${name(r.username)}/accept`);
          notice(`You and ${r.username} are friends now.`);
          await refreshSocial();
        })),
        button('Decline', () => act(async () => {
          await api('DELETE', `/api/v1/friends/requests/${name(r.username)}`);
          await refreshSocial();
        }), 'secondary'),
        personMenu(r.username, false)));
    }
    $('incoming-box').hidden = social.incoming.length === 0;

    const list = $('friend-list');
    list.replaceChildren();
    for (const f of social.friends) {
      const state = el('span', { className: 'presence ' + f.presence.state }, presenceText(f.presence));
      let action = null;
      if (f.presence.joinable && platform.webPlayer) action = button('Join', () => go(`#join=${name(f.username)}`));
      else if (f.presence.state === 'playing' && f.presence.gameId) {
        // (Without a browser player: the game's page says to use the app.)
        action = button('See the game', () => go(`#game=${f.presence.gameId}`), 'secondary');
      }
      list.append(el('li', null, avatar(f.avatar, true),
        el('span', { className: 'grow stack' }, el('span', null, f.username), state), action, personMenu(f.username, true)));
    }
    $('friends-heading').textContent = social.friends.length ? `Friends (${social.friends.length})` : 'Friends';
    $('no-friends').hidden = social.friends.length > 0;

    const outgoing = $('outgoing');
    outgoing.replaceChildren();
    for (const r of social.outgoing) {
      outgoing.append(el('li', null, avatar(r.avatar), el('span', { className: 'grow' }, r.username),
        button('Take back', () => act(async () => {
          await api('DELETE', `/api/v1/friends/requests/${name(r.username)}`);
          await refreshSocial();
        }), 'secondary')));
    }
    $('outgoing-box').hidden = social.outgoing.length === 0;
  }

  async function friendsPage() {
    show('friends');
    message('');
    drawFriends();
    await act(refreshSocial);
  }

  async function addFriend(username) {
    username = username.trim();
    if (!username) return;
    await act(async () => {
      const { status } = await api('POST', '/api/v1/friends/requests', { username });
      $('add-name').value = '';
      notice(status === 'friends' ? `You and ${username} are friends now.` : `Friend request sent to ${username}.`);
      await refreshSocial();
    });
  }

  async function suggest() {
    const prefix = $('add-name').value.trim();
    const list = $('suggestions');
    if (prefix.length < 2) {
      list.replaceChildren();
      return;
    }
    try {
      const { users } = await api('GET', '/api/v1/users?search=' + encodeURIComponent(prefix));
      list.replaceChildren(...users.filter((u) => u.username !== user.username)
        .map((u) => el('option', { value: u.username })));
    } catch { /* suggestions are optional */ }
  }

  let reportName = '';
  function openReport(username) {
    reportName = username;
    $('report-title').textContent = `Report ${username}`;
    $('report-reason').value = '';
    $('report-dialog').showModal();
  }

  // --- You ---------------------------------------------------------------------------------------

  async function loadProfile() {
    ({ profile } = await api('GET', `/api/v1/users/${name(user.username)}`));
    $('who-avatar').className = `avatar av${profile.avatar}`;
  }

  let chosenAvatar = 0;
  function drawProfile() {
    $('you-avatar').className = `avatar big av${chosenAvatar}`;
    $('you-name').textContent = profile.user.username;
    const joinedOn = new Date(profile.user.created * 1000).toISOString().slice(0, 10);
    $('you-meta').textContent = `Joined ${joinedOn} · ${plural(profile.friends, 'friend', 'friends')}`;
    const picker = $('avatar-picker');
    picker.replaceChildren();
    for (let i = 0; i < 16; ++i) {
      const b = el('button', { type: 'button', className: 'pick' + (i === chosenAvatar ? ' chosen' : ''), ariaLabel: `Portrait ${i + 1}` },
        avatar(i, true));
      b.setAttribute('aria-pressed', String(i === chosenAvatar));
      b.addEventListener('click', () => {
        chosenAvatar = i;
        drawProfile();
      });
      picker.append(b);
    }
    $('bio-count').textContent = `${$('bio').value.length} / 200`;
  }

  async function youPage() {
    show('you');
    message('');
    await act(async () => {
      await loadProfile();
      chosenAvatar = profile.avatar;
      $('bio').value = profile.bio || '';
      for (const r of document.querySelectorAll('input[name=join-policy]')) r.checked = r.value === profile.joinPolicy;
      drawProfile();
      const { blocked } = await api('GET', '/api/v1/blocks');
      const list = $('blocked-list');
      list.replaceChildren(...blocked.map((b) => el('li', null, avatar(b.avatar), el('span', { className: 'grow' }, b.username),
        button('Unblock', () => act(async () => {
          await api('DELETE', `/api/v1/blocks/${name(b.username)}`);
          notice(`${b.username} is no longer blocked.`);
          await youPage();
        }), 'secondary'))));
      $('no-blocked').hidden = blocked.length > 0;
      await loadAccount();
    });
  }

  async function saveProfile() {
    const policy = document.querySelector('input[name=join-policy]:checked');
    await act(async () => {
      ({ profile } = await api('PATCH', '/api/v1/me', {
        bio: $('bio').value, avatar: chosenAvatar, joinPolicy: policy ? policy.value : 'friends',
      }));
      $('who-avatar').className = `avatar av${profile.avatar}`;
      notice('Saved.');
    });
  }

  // --- Your account (docs/accounts.md) -------------------------------------------------------

  // The address without its #token, so it stays out of the history.
  function forgetLink() {
    history.replaceState(null, '', location.pathname + location.search + (user ? '#you' : ''));
  }

  async function confirmEmail(t) {
    forgetLink();
    try {
      const data = await api('POST', '/api/v1/email-verifications', { token: t });
      route();
      notice(`Thank you: ${data.email} is confirmed for ${data.username}.`);
    } catch (e) {
      route();
      message(e.message);
    }
  }

  function resetPage(t) {
    resetToken = t;
    forgetLink();
    show('reset-page');
    message('');
    $('reset-password').focus();
  }

  async function completeReset() {
    const password = $('reset-password').value;
    if (password !== $('reset-repeat').value) {
      message('The two passwords are not the same.');
      return;
    }
    await act(async () => {
      const data = await api('POST', '/api/v1/password-resets/complete', { token: resetToken, password });
      resetToken = null;
      $('reset-password').value = '';
      $('reset-repeat').value = '';
      loggedOut(); // every session ended, this page's too
      $('forgot-line').hidden = !platform.mail;
      $('username').value = data.username;
      $('password').focus();
      notice('Your password is changed. Log in with the new one.');
    });
  }

  async function requestReset() {
    const n = $('forgot-name').value.trim();
    if (!n) {
      message('Enter your name or email address.');
      return;
    }
    await act(async () => {
      const data = await api('POST', '/api/v1/password-resets', { name: n });
      notice(data.status);
    });
  }

  function drawAccount(acc) {
    const state = $('email-state');
    state.className = 'meta';
    if (!acc.email) {
      state.textContent = 'No email address. With a confirmed one you can reset a forgotten password.';
    } else if (acc.emailVerified) {
      state.textContent = `${acc.email} · confirmed`;
      state.classList.add('confirmed');
    } else {
      state.textContent = `${acc.email} · not confirmed yet: open the link sent there`;
      state.classList.add('unconfirmed');
    }
    if (acc.emailRequiredToPublish && !acc.emailVerified) {
      state.textContent += '. Publishing games on this platform needs a confirmed address.';
    }
    $('resend-email').hidden = !acc.mail || !acc.email || acc.emailVerified;
    $('email-form').hidden = !acc.mail && !acc.email;
    $('no-mail').hidden = acc.mail;
    $('email').value = acc.email;
    $('email-password').value = '';
  }

  // Only the newest answer is drawn: an older request (the page opening)
  // must not overwrite what a change just showed.
  let accountRequests = 0;
  async function loadAccount() {
    const mine = ++accountRequests;
    const acc = await api('GET', '/api/v1/me/account');
    if (mine === accountRequests) drawAccount(acc);
  }

  async function saveEmail() {
    const email = $('email').value.trim();
    await act(async () => {
      await api('PUT', '/api/v1/me/email', { email, password: $('email-password').value });
      await loadAccount();
      notice(email ? `A link is on its way to ${email.toLowerCase()}: open it to confirm the address.` : 'Your email address is removed.');
    });
  }

  async function changePassword() {
    await act(async () => {
      await api('PUT', '/api/v1/me/password', { current: $('current-password').value, password: $('new-password').value });
      $('current-password').value = '';
      $('new-password').value = '';
      notice('Your password is changed. Everywhere else, you are logged out.');
    });
  }

  async function exportData() {
    await act(async () => {
      const res = await fetch('/api/v1/me/export', { headers: { Authorization: 'Bearer ' + token } });
      if (!res.ok) throw new Error(`Could not download your data (${res.status})`);
      const url = URL.createObjectURL(await res.blob());
      const a = el('a', { href: url, download: `${user.username}-account.json` });
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    });
  }

  async function deleteAccount() {
    const password = $('delete-password').value;
    message('');
    try {
      const data = await api('DELETE', '/api/v1/me', { password });
      $('delete-dialog').close();
      $('delete-password').value = '';
      loggedOut();
      $('forgot-line').hidden = !platform.mail;
      notice(data.games ? `Your account and its ${plural(data.games, 'game', 'games')} are deleted.` : 'Your account is deleted.');
    } catch (e) {
      $('delete-dialog').close();
      message(e.message);
    }
  }

  // --- Playing -----------------------------------------------------------------------------------

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

  function gameStatus(text) {
    $('game-status').textContent = text;
  }

  function playerLine(text) {
    console.log(text); // tests read the console
    const line = String(text);
    if (/Joined/.test(line)) {
      gameStatus('Playing. Have fun!');
      joined = true;
      drawInvitePanel();
    } else if (/WARN|ERROR/.test(line)) {
      gameStatus(line.replace(/^\[\s*[\d.]+\]\s*/, ''));
    }
  }

  // Friends to invite to the game server you are on.
  function drawInvitePanel() {
    $('invite-panel').hidden = false;
    const list = $('invite-list');
    list.replaceChildren();
    for (const f of social.friends) {
      const done = invited.has(f.username);
      const b = button(done ? 'Invited' : 'Invite', () => act(async () => {
        b.disabled = true;
        await api('POST', `/api/v1/friends/${name(f.username)}/invite`);
        invited.add(f.username);
        b.textContent = 'Invited';
        console.log(`Invited ${f.username}`);
      }), 'secondary');
      b.disabled = done;
      list.append(el('li', null, avatar(f.avatar),
        el('span', { className: 'grow stack' }, el('span', null, f.username),
          el('span', { className: 'presence ' + f.presence.state }, presenceText(f.presence))), b));
    }
    $('no-invites').hidden = social.friends.length > 0;
  }

  async function play(id) {
    if (started) return;
    show('game');
    message('');
    $('game-title').textContent = 'Game ' + id;
    gameStatus('Finding a game server…');
    try {
      const { game } = await api('GET', `/api/v1/games/${id}`);
      $('game-title').textContent = game.title;
      document.title = game.title;
      const { ticket } = await api('POST', `/api/v1/games/${id}/join`);
      startPlayer(ticket);
    } catch (e) {
      failedToStart(e);
    }
  }

  async function joinFriend(friend) {
    if (started) return;
    show('game');
    message('');
    $('game-title').textContent = `${friend}'s game`;
    gameStatus(`Joining ${friend}…`);
    try {
      const { ticket } = await api('POST', `/api/v1/friends/${name(friend)}/join`);
      const f = social.friends.find((x) => x.username === friend);
      if (f && f.presence.gameTitle) {
        $('game-title').textContent = f.presence.gameTitle;
        document.title = f.presence.gameTitle;
      }
      console.log(`Joining ${friend}`);
      startPlayer(ticket);
    } catch (e) {
      failedToStart(e);
    }
  }

  function failedToStart(e) {
    gameStatus('');
    message(e.message);
    if (e.status === 401) {
      store.del('px.token');
      token = null;
    }
  }

  function startPlayer(ticket) {
    if (!ticket.webSocket) throw new Error('The servers of this platform do not accept browsers yet.');
    if (location.protocol === 'https:' && ticket.webSocket.startsWith('ws:')) {
      throw new Error('This page is secure but the game server is not (ws://); ask the platform to turn on TLS.');
    }
    started = true;
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
        ...(ticket.serverKey ? ['--server-key', ticket.serverKey] : []),
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

  // --- Start -------------------------------------------------------------------------------------

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
    $('sort').addEventListener('change', catalog);
    for (const r of document.querySelectorAll('input[name=board-scope]')) r.addEventListener('change', drawBoards);
    for (const a of document.querySelectorAll('#nav a, a.back')) {
      a.addEventListener('click', (e) => {
        if (!started) return;
        e.preventDefault();
        go(a.getAttribute('href'));
      });
    }
    $('add-friend').addEventListener('submit', (e) => { e.preventDefault(); addFriend($('add-name').value); });
    $('add-name').addEventListener('input', () => {
      clearTimeout(suggestTimer);
      suggestTimer = setTimeout(suggest, 300);
    });
    $('profile').addEventListener('submit', (e) => { e.preventDefault(); saveProfile(); });
    $('bio').addEventListener('input', () => { $('bio-count').textContent = `${$('bio').value.length} / 200`; });
    $('report-cancel').addEventListener('click', () => $('report-dialog').close());
    $('forgot').addEventListener('click', () => {
      show('forgot-page');
      message('');
      $('forgot-name').value = $('username').value.trim();
      $('forgot-name').focus();
    });
    $('forgot-back').addEventListener('click', () => route());
    $('forgot-form').addEventListener('submit', (e) => { e.preventDefault(); requestReset(); });
    $('reset-form').addEventListener('submit', (e) => { e.preventDefault(); completeReset(); });
    $('email-form').addEventListener('submit', (e) => { e.preventDefault(); saveEmail(); });
    $('resend-email').addEventListener('click', () => act(async () => {
      await api('POST', '/api/v1/me/email/verification', {});
      notice('The link is sent again.');
    }));
    $('password-form').addEventListener('submit', (e) => { e.preventDefault(); changePassword(); });
    $('export').addEventListener('click', exportData);
    $('delete-account').addEventListener('click', () => {
      $('delete-password').value = '';
      $('delete-dialog').showModal();
    });
    $('delete-cancel').addEventListener('click', () => $('delete-dialog').close());
    $('delete-form').addEventListener('submit', (e) => { e.preventDefault(); deleteAccount(); });
    $('report-form').addEventListener('submit', () => {
      const reason = $('report-reason').value.trim();
      if (!reason) return;
      act(async () => {
        await api('POST', `/api/v1/users/${name(reportName)}/reports`, { reason });
        notice('Thank you: a moderator will look at it.');
      });
    });
    document.addEventListener('securitypolicyviolation', (e) => {
      console.log('CSP blocked ' + e.blockedURI + ' (' + e.violatedDirective + ')');
    });
    window.addEventListener('hashchange', () => { if (!started) route(); });
    try {
      const info = await api('GET', '/api/v1/platform');
      platform = { webPlayer: !!info.webPlayer, mail: !!info.mail };
      $('platform-name').textContent = info.name;
      document.title = (info.webPlayer ? 'Play · ' : '') + info.name;
      document.body.classList.toggle('no-player', !info.webPlayer);
      if (!info.webPlayer) $('login').querySelector('.lead').textContent =
        'Log in, or make an account, to see the games here, your friends and their best scores. Play the games in the Project Pixel app.';
    } catch (e) {
      message('Could not reach the platform: ' + e.message);
      return;
    }
    if (token) {
      try {
        await signedIn((await api('GET', '/api/v1/me')).user);
      } catch {
        token = null;
        store.del('px.token');
      }
    }
    route();
  }

  start();
})();
