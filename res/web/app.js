/* Over The Hare, the browser half.
 *
 * No framework and no build step: this file is served off the card exactly as
 * it is written, so it can be edited on the device and reloaded. That is worth
 * more here than any convenience a toolchain would add - the whole point of
 * the feature is not needing a card reader.
 *
 * Uploads go through XMLHttpRequest rather than fetch, for one reason:
 * upload progress. fetch still has no way to report how much of a request body
 * has gone out, and a 900 MB ROM with no progress bar is indistinguishable
 * from a hang.
 */
'use strict';

const $ = (id) => document.getElementById(id);

let cwd = '';                 /* the path the listing is showing, "" = roots */
let entries = [];
let roots = [];               /* remembered from the root listing, for crumbs */

/* ---- talking to the device -------------------------------------------- */

/* Every non-200 is an error with the server's own words in it. The routes
 * answer in plain text precisely so this can show them without a schema. */
async function api(method, path, body) {
	const r = await fetch(path, { method, body, credentials: 'same-origin' });
	if (r.status === 401) { showGate('Session ended. Enter the PIN again.'); throw new Error('unauthorized'); }
	if (!r.ok) throw new Error((await r.text()).split('\n')[1] || r.statusText);
	return r;
}

const enc = encodeURIComponent;

/* ---- the gate ---------------------------------------------------------- */

function showGate(msg) {
	$('app').hidden = true;
	$('gate').hidden = false;
	$('gatemsg').textContent = msg || '';
	$('gatemsg').classList.remove('ok');
	$('pin').value = '';
	$('pin').focus();
}

$('pinform').addEventListener('submit', async (e) => {
	e.preventDefault();
	const pin = $('pin').value.trim();
	if (pin.length !== 4) { $('gatemsg').textContent = 'Four digits.'; return; }
	try {
		const r = await fetch('/api/auth', { method: 'POST', body: pin });
		if (r.status === 429) {
			$('gatemsg').textContent = 'Too many wrong PINs. Wait half a minute.';
			return;
		}
		if (!r.ok) { $('gatemsg').textContent = 'Wrong PIN.'; $('pin').value = ''; return; }
		$('gate').hidden = true;
		$('app').hidden = false;
		await start();
	} catch (err) {
		$('gatemsg').textContent = 'Could not reach the device.';
	}
});

/* ---- listing ----------------------------------------------------------- */

/* Decimal, not 1024.
 *
 * These were 1024-based and labeled KB/MB/GB, which is the one combination
 * that is wrong on every platform: macOS has quoted decimal since 10.6, so a
 * 15,528,261-byte ROM read 15.53 MB in Finder and 14.8 MB here. The whole job
 * of this page is to agree with the machine at the other end about what is on
 * the card, and a size that disagrees with the file manager beside it reads as
 * a failed copy. Relabeling to KiB would have been true and no help. */
function human(n) {
	if (n < 1000) return n + ' B';
	const u = ['KB', 'MB', 'GB'];
	let i = -1;
	do { n /= 1000; i++; } while (n >= 1000 && i < u.length - 1);
	/* A decimal for MB and GB, whole numbers for KB. Rounding 15,528,261 to
	 * "16 MB" put the units right and still disagreed with the file manager
	 * next to it by half a megabyte, which was the original complaint. */
	return (i === 0 ? Math.round(n) : n.toFixed(1)) + ' ' + u[i];
}

function crumbs() {
	const nav = $('crumbs');
	nav.textContent = '';
	const parts = cwd ? cwd.split('/') : [];
	const mk = (label, path, last) => {
		if (last) {
			const s = document.createElement('span');
			s.className = 'here';
			s.textContent = label;
			nav.append(s);
			return;
		}
		const b = document.createElement('button');
		b.textContent = label;
		b.onclick = () => go(path);
		nav.append(b);
		const sep = document.createElement('span');
		sep.className = 'sep';
		sep.textContent = '/';
		nav.append(sep);
	};
	mk('Device', '', parts.length === 0);
	parts.forEach((p, i) => {
		/* The first segment is a root's URL name - "roms" - and the device
		 * calls it "ROMs" everywhere else. Show what it is called. */
		const root = i === 0 && roots.find((r) => r.path === p);
		mk(root ? root.name : p, parts.slice(0, i + 1).join('/'),
		   i === parts.length - 1);
	});
}

/* `hist` says what this navigation does to browser history:
 *
 *   'push'    a real navigation - a crumb or a folder, back should undo it
 *   'replace' the first listing, which is where back should stop
 *   'none'    a refresh of where you already are, after a rename, a delete,
 *             a mkdir or a finished queue. These are not navigations and
 *             pushing them would make back replay the same folder repeatedly.
 *
 * The path also goes in the fragment, so a reload lands where you were and a
 * link can be sent to another device on the LAN. */
async function go(path, hist) {
	const r = await api('GET', '/api/list' + (path ? '?p=' + enc(path) : ''));
	const data = await r.json();
	cwd = data.path;
	if (cwd === '') {
		/* The roots keep the order the device gave them: ROMs is the reason
		 * anyone opened this, and alphabetical put BIOS above it. */
		entries = data.entries;
		roots = data.entries.slice();
	} else {
		/* Inside a folder, sorted: vfat's readdir order is creation order,
		 * which is no order at all to somebody looking for a game. */
		entries = data.entries.sort((a, b) =>
			(b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true }));
	}
	draw();

	const url = '#' + enc(cwd);
	if (hist === 'replace') history.replaceState({ path: cwd }, '', url);
	else if (hist !== 'none') history.pushState({ path: cwd }, '', url);
}

/* Back and forward. Always 'none': the browser has already moved its own
 * pointer through the stack, and pushing here would append a duplicate entry
 * and make forward unreachable.
 *
 * A queue in flight is unaffected, because each job carries the folder it was
 * bound to at enqueue. That ordering was deliberate - popstate fires with no
 * click and no confirmation, so shipping this while the destination was still
 * read from the live `cwd` would have turned an occasional misfile into a
 * routine one. */
addEventListener('popstate', (e) => {
	const path = e.state && typeof e.state.path === 'string' ? e.state.path : '';
	go(path, 'none').catch(() => {});
});

/* The first listing: whatever the fragment names, else the roots. Always
 * 'replace', so the entry the session opens on is the one back stops at
 * instead of leaving the page.
 *
 * A fragment naming a folder that has since been renamed or deleted is not a
 * dead session, so that falls back to the roots. An expired cookie IS, and is
 * rethrown - otherwise the retry would 401 as well and the gate would be
 * raised twice. */
async function start() {
	const want = location.hash ? decodeURIComponent(location.hash.slice(1)) : '';
	if (!want) { await go('', 'replace'); return; }
	try {
		await go(want, 'replace');
	} catch (err) {
		if (err.message === 'unauthorized') throw err;
		await go('', 'replace');
	}
}

function draw() {
	crumbs();
	/* The wording differs at the roots, where nothing takes a drop, so it has
	 * to be rebuilt whenever the listing changes rather than written once. */
	refreshDropHint();
	const ul = $('list');
	ul.textContent = '';
	$('empty').hidden = entries.length > 0;
	$('logs').hidden = cwd !== '';

	for (const e of entries) {
		const li = document.createElement('li');
		li.className = 'row' + (e.dir ? ' dir' : '');

		const mark = document.createElement('span');
		mark.className = 'mark';
		/* Text, not an icon font and not an SVG sprite: two characters that
		 * every system font already has, and nothing more to serve. */
		mark.textContent = e.dir ? '▸' : '·';
		li.append(mark);

		/* Drop straight onto a folder, so uploading into eleven shelves is not
		 * eleven round trips through each one.
		 *
		 * Only inside a root, never on the roots themselves. Roms/ is the
		 * reason: lib_scan is always called with a specific system folder and
		 * opens <roms_root>/<folder>, so nothing ever reads Roms/ itself - a
		 * file dropped there is never seen by any shelf again. Bios/ and
		 * Saves/ are flat and would be safe, but one uniform rule beats three
		 * special cases, and the row simply not lighting up says so without
		 * anyone having to know it. */
		if (e.dir && cwd) {
			li.classList.add('drops');
			li.addEventListener('dragover', (ev) => {
				ev.preventDefault();
				ev.stopPropagation();
				li.classList.add('over');
			});
			/* dragleave also fires crossing into a child, and relatedTarget is
			 * where the pointer went: still inside means it never left. */
			li.addEventListener('dragleave', (ev) => {
				if (li.contains(ev.relatedTarget)) return;
				li.classList.remove('over');
			});
			li.addEventListener('drop', (ev) => {
				ev.preventDefault();
				ev.stopPropagation();
				li.classList.remove('over');
				dragDepth = 0;
				/* Through dragUI, not by hiding the frame alone: the header is
				 * showing the drop message in the crumbs' place, and stopping
				 * at the frame leaves it there for good. */
				dragUI(false);
				if (ev.dataTransfer.files.length)
					enqueue(ev.dataTransfer.files, e.path);
			});
		}

		if (e.dir) {
			const b = document.createElement('button');
			b.className = 'name';
			b.textContent = e.name;
			b.onclick = () => go(e.path);
			li.append(b);
		} else {
			const s = document.createElement('span');
			s.className = 'name';
			s.textContent = e.name;
			li.append(s);
			const sz = document.createElement('span');
			sz.className = 'size';
			sz.textContent = human(e.size);
			li.append(sz);
		}

		const acts = document.createElement('div');
		acts.className = 'acts';
		if (!e.dir) {
			const a = document.createElement('a');
			a.href = '/api/file?p=' + enc(e.path);
			a.textContent = 'Get';
			a.setAttribute('download', e.name);
			acts.append(a);
		}
		if (cwd) {                       /* the roots themselves are not editable */
			acts.append(mkbtn('Rename', () => rename(e)));
			const d = mkbtn('Delete', () => del(e));
			d.className = 'danger';
			acts.append(d);
		}
		li.append(acts);
		ul.append(li);
	}
}

function mkbtn(label, fn) {
	const b = document.createElement('button');
	b.textContent = label;
	b.onclick = fn;
	return b;
}

/* ---- the operations ---------------------------------------------------- */

async function rename(e) {
	const to = prompt('Rename to', e.name);
	if (!to || to === e.name) return;
	try {
		await api('POST', '/api/rename?p=' + enc(e.path) + '&to=' + enc(to));
		toast('Renamed');
		await go(cwd, 'none');
	} catch (err) { toast(err.message, true); }
}

async function del(e) {
	if (!confirm('Delete ' + e.name + '?')) return;
	try {
		await api('POST', '/api/delete?p=' + enc(e.path));
		toast('Deleted');
		await go(cwd, 'none');
	} catch (err) { toast(err.message, true); }
}

$('newfolder').onclick = async () => {
	if (!cwd) { toast('Pick a folder first', true); return; }
	const name = prompt('New folder name');
	if (!name) return;
	try {
		await api('POST', '/api/mkdir?p=' + enc(cwd + '/' + name));
		await go(cwd, 'none');
	} catch (err) { toast(err.message, true); }
};

/* ---- uploading --------------------------------------------------------- */

const queue = [];
let sending = false;

/* `dest` is the folder these files are going to, defaulting to the one on
 * screen. It is bound HERE, into each job, rather than read at send time.
 *
 * Reading it later was a real bug: `next()` built every URL from the live
 * `cwd`, so navigating during a multi-file upload sent the rest of the queue
 * wherever you had gone. A 40-file batch, walked away from after file 5, put
 * 35 files somewhere nobody chose - and then the refresh at the end of the
 * queue listed the folder you had moved to, so the files that landed there
 * looked like the ones you meant. Binding it per job also makes a per-file
 * destination possible, which is what dropping onto a folder row uses. */
function enqueue(files, dest) {
	const dir = dest === undefined ? cwd : dest;
	if (!dir) { toast('Open a folder first', true); return; }
	for (const f of files) queue.push({ file: f, dir, pct: 0, state: 'waiting' });
	drawQueue();
	if (!sending) next();
}

function next() {
	const job = queue.find((j) => j.state === 'waiting');
	if (!job) {
		sending = false;
		/* Left on screen for a moment so the last line is readable, rather
		 * than the panel vanishing the instant the final byte lands. */
		setTimeout(() => { if (!queue.some((j) => j.state === 'sending')) {
			queue.length = 0; drawQueue();
		} }, 2500);
		go(cwd, 'none').catch(() => {});
		return;
	}
	sending = true;
	job.state = 'sending';
	drawQueue();

	const xhr = new XMLHttpRequest();
	xhr.open('PUT', '/api/file?p=' + enc(job.dir + '/' + job.file.name));
	xhr.upload.onprogress = (ev) => {
		if (!ev.lengthComputable) return;
		job.pct = Math.round(ev.loaded / ev.total * 100);
		drawQueue();
	};
	xhr.onload = () => {
		if (xhr.status === 200) { job.state = 'done'; job.pct = 100; }
		else {
			job.state = 'failed';
			job.why = (xhr.responseText || '').split('\n')[1] || ('HTTP ' + xhr.status);
		}
		drawQueue();
		next();
	};
	xhr.onerror = () => {
		job.state = 'failed';
		job.why = 'connection lost';
		drawQueue();
		next();
	};
	xhr.send(job.file);
}

function drawQueue() {
	const ul = $('queuelist');
	$('queue').hidden = queue.length === 0;
	ul.textContent = '';
	for (const j of queue) {
		const li = document.createElement('li');
		if (j.state === 'failed') li.className = 'failed';
		const top = document.createElement('div');
		top.className = 'top';
		const n = document.createElement('span');
		n.className = 'n';
		n.textContent = j.file.name;
		/* Where it is going, shown only when that is no longer what is on
		 * screen - after navigating away, or after a drop onto a folder row.
		 * Silent in the ordinary case, and there exactly when the answer has
		 * stopped being obvious. */
		if (j.dir !== cwd) {
			const d = document.createElement('span');
			d.className = 'dest';
			d.textContent = '→ ' + (j.dir.split('/').pop() || j.dir);
			d.title = j.dir;
			n.append(' ', d);
		}
		const p = document.createElement('span');
		p.className = 'pct';
		p.textContent = j.state === 'failed' ? j.why
		              : j.state === 'done' ? 'done'
		              : j.state === 'waiting' ? 'waiting'
		              : j.pct + '%';
		top.append(n, p);
		const bar = document.createElement('div');
		bar.className = 'bar';
		const fill = document.createElement('span');
		fill.style.width = (j.state === 'failed' ? 100 : j.pct) + '%';
		bar.append(fill);
		li.append(top, bar);
		ul.append(li);
	}
}

$('picker').onchange = (e) => { enqueue(e.target.files); e.target.value = ''; };

/* Drag and drop, counted rather than toggled: dragenter and dragleave fire for
 * every child element the pointer crosses, so a boolean flickers the overlay
 * off the moment the pointer moves over a row inside it. */
let dragDepth = 0;

/* "here" stopped having one meaning once folder rows took drops, so the banner
 * says which of the three situations you are actually in. At the roots nothing
 * accepts a drop at all - enqueue refuses with the same words - so say that
 * before the file is let go rather than after. */
function dropLabel() {
	return cwd ? 'Drop files to upload' : 'Open a folder to upload';
}

/* The hint is permanent, not summoned by the drag.
 *
 * It used to appear only on dragenter, which meant the only person who ever saw
 * it was somebody who had already worked out that dropping was possible. A
 * capability advertised solely to people who have already found it is not
 * advertised. It sits beside the crumbs rather than replacing them, because
 * when the pointer is NOT over a folder row the crumbs are the only thing
 * saying where a drop would land.
 *
 * Dragging changes emphasis, not words. Same string lit differently: nothing
 * to keep in sync, and no flicker as the text is swapped under the pointer. */
function dragUI(on) {
	$('drop').hidden = !on;
	$('dropmsg').classList.toggle('armed', on);
}

/* Kept current as the listing changes, since the wording differs at the roots
 * where nothing accepts a drop. */
function refreshDropHint() {
	$('dropmsg').textContent = dropLabel();
}

addEventListener('dragenter', (e) => {
	e.preventDefault();
	if (++dragDepth === 1 && !$('app').hidden) dragUI(true);
});
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; dragUI(false); } });
addEventListener('drop', (e) => {
	e.preventDefault();
	dragDepth = 0;
	dragUI(false);
	if (e.dataTransfer.files.length) enqueue(e.dataTransfer.files);
});

/* ---- toast ------------------------------------------------------------- */

let toastTimer = 0;
function toast(msg, bad) {
	const t = $('toast');
	t.textContent = msg;
	t.classList.toggle('bad', !!bad);
	t.classList.add('show');
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => t.classList.remove('show'), 2600);
}

/* A reload should not always mean re-entering the PIN: the cookie may still be
 * good. Ask for the roots, and only show the gate if that is refused. */
(async () => {
	try {
		const r = await fetch('/api/list', { credentials: 'same-origin' });
		if (!r.ok) throw new Error();
		$('gate').hidden = true;
		$('app').hidden = false;
		await start();
	} catch (e) {
		showGate('');
	}
})();
