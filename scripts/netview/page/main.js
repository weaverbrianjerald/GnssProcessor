'use strict';

// A failure is shown on the page, never silent: the note names the message for a report.
window.addEventListener('error', ev => {
  const box = document.getElementById('missing');
  if (!box) return;
  box.hidden = false;
  box.insertAdjacentHTML('beforeend', '<div style="color:var(--release)">Something on this page failed: ' +
    esc(ev.message || String(ev.error)) + '. The rest of the page still works.</div>');
});

start().catch(err => {
  document.getElementById('loading').textContent = 'The page could not open its data: ' + err.message;
  console.error(err);
  if (STILL.on) stillFail(err.message);
});
